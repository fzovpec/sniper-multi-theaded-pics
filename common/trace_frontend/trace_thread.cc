#include "trace_thread.h"
#include "trace_manager.h"
#include "simulator.h"
#include "core_manager.h"
#include "thread_manager.h"
#include "thread.h"
#include "dvfs_manager.h"
#include "instruction.h"
#include "dynamic_instruction.h"
#include "performance_model.h"
#include "instruction_decoder_wlib.h"
#include "micro_op.h"
#include "config.hpp"
#include "syscall_model.h"
#include "core.h"
#include "magic_client.h"
#include "branch_predictor.h"
#include "rng.h"
#include "routine_tracer.h"
#include "sim_api.h"
#include "mimicos.h"
#include "stats.h"
#include "parametric_dram_directory_msi/memory_manager.h"
#include "lock_state_tracker.h"
#include "spin_loop_detector.h"

#include <unistd.h>
#include <sys/syscall.h>
#include <sched.h>

#include <x86_decoder.h>  // TODO remove when the decode function in microop perf model is adapted

#include "debug_config.h"
#include <sstream>
#include <vector>
#include <chrono>
#include <algorithm>

// ---------------------------------------------------------------------------
// Pin calling thread to a distinct host CPU chosen from the process's
// available set (typically constrained by SLURM's cgroup).  Thread-safe:
// uses a static atomic counter so each caller gets a different CPU.
// ---------------------------------------------------------------------------
static void pinTraceThreadToCPU(int thread_id)
{
   cpu_set_t available;
   CPU_ZERO(&available);
   if (sched_getaffinity(0, sizeof(available), &available) != 0)
      return;  // cannot query – leave scheduling to the OS

   // Collect the list of CPUs we are allowed to use
   std::vector<int> cpus;
   for (int c = 0; c < CPU_SETSIZE; ++c)
      if (CPU_ISSET(c, &available))
         cpus.push_back(c);

   if (cpus.empty())
      return;

   // Pick a CPU using the thread id (wraps if more threads than CPUs)
   int target = cpus[thread_id % cpus.size()];

   cpu_set_t mask;
   CPU_ZERO(&mask);
   CPU_SET(target, &mask);
   sched_setaffinity(0, sizeof(mask), &mask);
}

int TraceThread::m_isa = 0;

TraceThread::TraceThread(Thread *thread, SubsecondTime time_start, String tracefile, String responsefile, app_id_t app_id, bool cleanup)
   : m__thread(NULL)
   , m_thread(thread)
   , m_time_start(time_start)
   , m_trace(tracefile.c_str(), responsefile.c_str(), thread->getId())
   , m_kernel_trace(NULL)
   , m_app_trace(NULL)
   , m_current_sift_reader(NULL)
   , m_trace_has_pa(false)
   , m_champsim_trace(false)
   , m_champsim_access_size(Sift::CHAMPSIM_DEFAULT_MEM_SIZE)
   , m_address_randomization(Sim()->getCfg()->getBool("traceinput/address_randomization"))
   , m_appid_from_coreid(Sim()->getCfg()->getString("scheduler/type") == "sequential" ? true : false)
   , m_stop(false)
   , m_bbv_base(0)
   , m_bbv_count(0)
   , m_bbv_last(0)
   , m_bbv_end(false)
   , m_output_leftover_size(0)
   , m_tracefile(tracefile)
   , m_responsefile(responsefile)
   , m_app_id(app_id)
   , m_blocked(false)
   , m_cleanup(cleanup)
   , m_started(false)
   , m_stopped(false)
   , m_champsim_icache_hits(0)
   , m_champsim_icache_misses(0)
   , m_is_spinning(false)
   , m_other_thread_holds_lock(false)
   , m_claim_load_pc(0)
   , m_claim_addr(0)
   , m_check_pc(0)
   , m_check_base(0)
   , m_wait_base(0)
   , m_wait_check_pc(0)
   , m_wait_reported(false)
   , m_sync_addr(0)
   , m_sync_check_pc(0)
   , m_sync_loop_pc(0)
   , m_sync_passed(false)
   , m_sync_provisional(false)
   , m_sync_branch_since_check(false)
   , m_sync_wait_reported(false)
   , m_path_var(0)
   , m_path_pc(0)
   , m_path_overflow(false)
   , m_synthetic_left(0)
   , m_path_clean(false)
   , m_path_replays(0)
   , m_replays(0)
   , m_injected_insns(0)
   , m_skipped_insns(0)
   , m_sync_skips(0)
{

   bool userspace_mimicos_enabled = Sim()->getCfg()->getBool("general/enable_userspace_mimicos");

   if (userspace_mimicos_enabled)
   {
      m_current_run_func = &TraceThread::m_run_func_with_userpace_mimicos;
   }
   else
   {
      m_current_run_func = &TraceThread::m_run_func_default;
   }

   if (Sim()->getCfg()->hasKey("traceinput/champsim_mem_access_bytes"))
   {
      SInt64 configured_size = Sim()->getCfg()->getInt("traceinput/champsim_mem_access_bytes");
      if (configured_size > 0)
         m_champsim_access_size = static_cast<UInt32>(configured_size);
      else
         m_champsim_access_size = Sift::CHAMPSIM_DEFAULT_MEM_SIZE;
   }

   m_trace.setHandleInstructionCountFunc(TraceThread::__handleInstructionCountFunc, this);
   m_trace.setHandleCacheOnlyFunc(TraceThread::__handleCacheOnlyFunc, this);
   if (Sim()->getCfg()->getBool("traceinput/mirror_output"))
      m_trace.setHandleOutputFunc(TraceThread::__handleOutputFunc, this);
   m_trace.setHandleSyscallFunc(TraceThread::__handleSyscallFunc, this);
   m_trace.setHandleNewThreadFunc(TraceThread::__handleNewThreadFunc, this);
   m_trace.setHandleJoinFunc(TraceThread::__handleJoinFunc, this);
   m_trace.setHandleMagicFunc(TraceThread::__handleMagicFunc, this);
   m_trace.setHandleEmuFunc(TraceThread::__handleEmuFunc, this);
   m_trace.setHandleForkFunc(TraceThread::__handleForkFunc, this);
   if (Sim()->getRoutineTracer())
      m_trace.setHandleRoutineFunc(TraceThread::__handleRoutineChangeFunc, TraceThread::__handleRoutineAnnounceFunc, this);

   if (m_address_randomization)
   {
      // Fisher-Yates shuffle, simultaneously initializing array to m_address_randomization_table[i] = i
      // See http://en.wikipedia.org/wiki/Fisher%E2%80%93Yates_shuffle#The_.22inside-out.22_algorithm
      // By using the app_id as a random seed, we get an app_id-specific pseudo-random permutation of 0..255
      UInt64 state = rng_seed(app_id);
      m_address_randomization_table[0] = 0;
      for (unsigned int i = 1; i < 256; ++i)
      {
         uint8_t j = rng_next(state) % (i + 1);
         m_address_randomization_table[i] = m_address_randomization_table[j];
         m_address_randomization_table[j] = i;
      }
   }

   thread->setVa2paFunc(_va2pa, (UInt64)this);
   stats.kernel_time = SubsecondTime::Zero();

   // Guard against duplicate registration when a trace is restarted
   // (--sim-end=last-restart creates a new TraceThread with the same app_id).
   StatsMetricBase *existing = Sim()->getStatsManager()->getMetricObject("trace_thread", m_app_id, "kernel_time");
   if (existing == NULL)
      registerStatsMetric("trace_thread", m_app_id, "kernel_time", &stats.kernel_time);
   else
      static_cast<StatsMetric<SubsecondTime>*>(existing)->metric = &stats.kernel_time;
}

TraceThread::~TraceThread()
{
   delete m__thread;
   if (m_cleanup)
   {
      unlink(m_tracefile.c_str());
      unlink(m_responsefile.c_str());
   }
   for (std::unordered_map<IntPtr, const dl::DecodedInst *>::iterator i = m_decoder_cache.begin(); i != m_decoder_cache.end(); ++i)
   {
      delete (*i).second;
   }
   
   // Clean up ChampSim instruction cache (since TraceManager doesn't delete TraceThreads)
   cleanupChampSimCache();
}

void TraceThread::cleanupChampSimCache()
{
   // Already cleaned up?
   if (m_champsim_icache.empty()) {
      return;
   }
   
   // Print ChampSim instruction cache statistics
   UInt64 total_accesses = m_champsim_icache_hits + m_champsim_icache_misses;
   if (total_accesses > 0 || m_champsim_icache.size() > 0) {
      double hit_rate = (total_accesses > 0) ? (100.0 * m_champsim_icache_hits / total_accesses) : 0.0;
      printf("[CHAMPSIM_ICACHE] Stats: hits=%lu misses=%lu total=%lu hit_rate=%.2f%% cache_size=%zu\n",
             m_champsim_icache_hits, m_champsim_icache_misses, total_accesses, hit_rate, m_champsim_icache.size());
      fflush(stdout);
   }
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] cleanupChampSimCache: Cleaning up %zu cached ChampSim instructions\n", m_champsim_icache.size());
   #endif
   
   // Clean up ChampSim instruction cache
   size_t cleanup_count = 0;
   for (auto& kv : m_champsim_icache)
   {
      Instruction* instr = kv.second;
      if (instr) {
         const std::vector<const MicroOp*>* uops = instr->getMicroOps();
         #if DEBUG_CHAMPSIM_CACHE >= DEBUG_DETAILED
         printf("[CHAMPSIM_CACHE] FREE Instruction*=%p with %zu MicroOps\n", 
                (void*)instr, uops ? uops->size() : 0);
         #endif
         if (uops) {
            for (const MicroOp* uop : *uops) {
               delete uop;
            }
            delete uops;
         }
         delete instr;
         cleanup_count++;
      }
   }
   m_champsim_icache.clear();
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] cleanupChampSimCache: Freed %zu instructions\n", cleanup_count);
   #endif
}

UInt64 TraceThread::va2pa(UInt64 va, bool *noMapping)
{
   if (m_trace_has_pa)
   {
      UInt64 pa = m_trace.va2pa(va);
      if (pa != 0)
      {
         return pa;
      }
      else
      {
         if (noMapping)
            *noMapping = true;
         // else
         //    LOG_PRINT_WARNING("No mapping found for logical address %lx", va);
         //  Fall through to construct an address with our thread id in the upper bits (assume address is private)
      }
   }

   UInt64 haddr;

   // When the scheduler is set to sequential, every thread with same core affinity
   // will be considered chunks of the same process, therefore they have the same
   // physical address space.
   if (m_appid_from_coreid)
   {
      haddr = UInt64(m_thread->getCore()->getId());
   }
   else
   {
      haddr = UInt64(m_thread->getAppId());
   }

   if (m_address_randomization)
   {
      // Set 16 bits to app_id | remap middle 36 bits using app_id-specific mapping | keep lower 12 bits (page offset)
      return (haddr << pa_core_shift) | (remapAddress(va >> va_page_shift) << va_page_shift) | (va & va_page_mask);
   }
   else
   {
      // Set 16 bits to app_id | keep lower 48 bits
      return (haddr << pa_core_shift) | (va & pa_va_mask);
   }
}

UInt64 TraceThread::remapAddress(UInt64 va_page)
{
   // va is the virtual address shifted right by the page size
   // By randomly remapping the lower 24 bits of va_page, addresses will be distributed
   // over a 1<<(16+3*8) = 64 GB range which should avoid artificial set contention in all cache levels.
   // Of course we want the remapping to be invertible so we never map different incoming addresses
   // onto the same outgoing address. This is guaranteed since m_address_randomization_table
   // contains each 0..255 number only once.
   UInt64 result = va_page;
   uint8_t *array = (uint8_t *)&result;
   array[0] = m_address_randomization_table[array[0]];
   array[1] = m_address_randomization_table[array[1]];
   array[2] = m_address_randomization_table[array[2]];
   return result;
}

void TraceThread::handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size)
{
   FILE *fp;
   if (fd == 1)
      fp = stdout;
   else if (fd == 2)
      fp = stderr;
   else
      return;

   while (size)
   {
      const uint8_t *ptr = data;
      while (ptr < data + size && *ptr != '\r' && *ptr != '\n')
         ++ptr;
      if (ptr == data + size)
      {
         if (size > sizeof(m_output_leftover))
            size = sizeof(m_output_leftover);
         memcpy(m_output_leftover, data, size);
         m_output_leftover_size = size;
         break;
      }

      fprintf(fp, "[TRACE:%u] ", m_thread->getId());
      if (m_output_leftover_size)
      {
         fwrite(m_output_leftover, m_output_leftover_size, 1, fp);
         m_output_leftover_size = 0;
      }
      fwrite(data, ptr - data, 1, fp);
      fprintf(fp, "\n");

      while (ptr < data + size && (*ptr == '\r' || *ptr == '\n'))
         ++ptr;
      size -= (ptr - data);
      data = ptr;
   }
}

uint64_t TraceThread::handleSyscallFunc(uint16_t syscall_number, const uint8_t *data, uint32_t size)
{
   // We may have been blocked in a system call, if we start executing instructions again that means we're continuing
   if (m_blocked)
   {
      unblock();
   }

   LOG_ASSERT_ERROR(m_thread->getCore(), "Cannot execute while not on a core");
   uint64_t ret = 0;

   switch (syscall_number)
   {
   case SYS_exit_group:
      Sim()->getTraceManager()->endApplication(this, getCurrentTime());
      break;

   default:
   {
      LOG_ASSERT_ERROR(size == sizeof(SyscallMdl::syscall_args_t), "Syscall arguments not the correct size");

      SyscallMdl::syscall_args_t *args = (SyscallMdl::syscall_args_t *)data;

      m_blocked = m_thread->getSyscallMdl()->runEnter(syscall_number, *args);
      if (m_blocked == false)
      {
         ret = m_thread->getSyscallMdl()->runExit(ret);
      }
      break;
   }
   }

   return ret;
}

int32_t TraceThread::handleNewThreadFunc()
{
   return Sim()->getTraceManager()->createThread(m_app_id, getCurrentTime(), m_thread->getId());
}

int32_t TraceThread::handleForkFunc()
{
   return Sim()->getTraceManager()->createApplication(getCurrentTime(), m_thread->getId());
}

int32_t TraceThread::handleJoinFunc(int32_t join_thread_id)
{
   Sim()->getThreadManager()->joinThread(m_thread->getId(), join_thread_id);
   return 0;
}

uint64_t TraceThread::handleMagicFunc(uint64_t a, uint64_t b, uint64_t c)
{
   return handleMagicInstruction(m_thread->getId(), a, b, c);
}

void TraceThread::handleRoutineChangeFunc(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip)
{
   switch (event)
   {
   case Sift::RoutineEnter:
      m_thread->getRoutineTracer()->routineEnter(eip, esp, callEip);
      break;
   case Sift::RoutineExit:
      m_thread->getRoutineTracer()->routineExit(eip, esp);
      break;
   case Sift::RoutineAssert:
      m_thread->getRoutineTracer()->routineAssert(eip, esp);
      break;
   default:
      LOG_PRINT_ERROR("Invalid Sift::RoutineOpType %d", event);
   }
}

bool TraceThread::handleEmuFunc(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res)
{
   // We may have been blocked in a system call, if we start executing instructions again that means we're continuing
   if (m_blocked)
   {
      unblock();
   }

   LOG_ASSERT_ERROR(m_thread->getCore(), "Cannot execute while not on a core");

   switch (type)
   {
   case Sift::EmuTypeRdtsc:
   {
      SubsecondTime cycles_fs = getCurrentTime();
      // Convert SubsecondTime to cycles in global clock domain
      const ComponentPeriod *dom_global = Sim()->getDvfsManager()->getGlobalDomain();
      UInt64 cycles = SubsecondTime::divideRounded(cycles_fs, *dom_global);

      res.rdtsc.cycles = cycles;
      return true;
   }
   case Sift::EmuTypeGetProcInfo:
   {
      res.getprocinfo.procid = m_thread->getCore()->getId();
      res.getprocinfo.nprocs = Sim()->getConfig()->getApplicationCores();
      res.getprocinfo.emunprocs = Sim()->getConfig()->getOSEmuNprocs() ? Sim()->getConfig()->getOSEmuNprocs() : Sim()->getConfig()->getApplicationCores();
      return true;
   }
   case Sift::EmuTypeGetTime:
   {
      res.gettime.time_ns = Sim()->getConfig()->getOSEmuTimeStart() * 1000000000 + getCurrentTime().getNS();
      return true;
   }
   case Sift::EmuTypeCpuid:
   {
      cpuid_result_t result;
      m_thread->getCore()->emulateCpuid(req.cpuid.eax, req.cpuid.ecx, result);
      res.cpuid.eax = result.eax;
      res.cpuid.ebx = result.ebx;
      res.cpuid.ecx = result.ecx;
      res.cpuid.edx = result.edx;
      return true;
   }
   case Sift::EmuTypeSetThreadInfo:
   {
      m_thread->m_os_info.tid = req.setthreadinfo.tid;
      return true;
   }
   case Sift::EmuTypePAPIstart:
   {
      m_papi_counters = new long long[NUM_PAPI_COUNTERS];
      for (unsigned int i = 0; i < NUM_PAPI_COUNTERS; i++)
         m_papi_counters[i] = 0;
      return true;
   }
   case Sift::EmuTypePAPIread:
   {
      m_papi_counters[PAPI_TOT_INS] = m_thread->getCore()->getPerformanceModel()->getInstructionCount();

      SubsecondTime cycles_fs = getCurrentTime();
      // Convert SubsecondTime to cycles in global clock domain
      const ComponentPeriod *dom_global = Sim()->getDvfsManager()->getGlobalDomain();
      UInt64 cycles = SubsecondTime::divideRounded(cycles_fs, *dom_global);

      m_papi_counters[PAPI_TOT_CYC] = cycles;

      UInt64 load_misses_l1d = Sim()->getStatsManager()->getMetricObject("L1-D", m_thread->getCore()->getId(), "load-misses")->recordMetric();
      UInt64 store_misses_l1d = Sim()->getStatsManager()->getMetricObject("L1-D", m_thread->getCore()->getId(), "store-misses")->recordMetric();

      UInt64 load_misses_l2 = Sim()->getStatsManager()->getMetricObject("L2", m_thread->getCore()->getId(), "load-misses")->recordMetric();
      UInt64 store_misses_l2 = Sim()->getStatsManager()->getMetricObject("L2", m_thread->getCore()->getId(), "store-misses")->recordMetric();

      UInt64 load_misses_l3 = Sim()->getStatsManager()->getMetricObject("L3", m_thread->getCore()->getId(), "load-misses")->recordMetric();
      UInt64 store_misses_l3 = Sim()->getStatsManager()->getMetricObject("L3", m_thread->getCore()->getId(), "store-misses")->recordMetric();

      m_papi_counters[PAPI_L1_DCM] = load_misses_l1d + store_misses_l1d;
      m_papi_counters[PAPI_L2_DCM] = load_misses_l2 + store_misses_l2;
      m_papi_counters[PAPI_L3_TCM] = load_misses_l3 + store_misses_l3;

      m_papi_counters[PAPI_BR_MSP] = m_thread->getCore()->getPerformanceModel()->getBranchPredictor()->getNumIncorrectPredictions();

      for (unsigned i = 0; i < NUM_PAPI_COUNTERS; i++)
         res.papi.values[i] = m_papi_counters[i];

      return true;
   }
   default:
      // Not emulated
      return false;
   }
}

void TraceThread::handleRoutineAnnounceFunc(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
{
   Sim()->getRoutineTracer()->addRoutine(eip, name, imgname, offset, column, line, filename);
}

SubsecondTime TraceThread::getCurrentTime() const
{
   LOG_ASSERT_ERROR(m_thread->getCore() != NULL, "Cannot get time while not on a core");
   return m_thread->getCore()->getPerformanceModel()->getElapsedTime();
}

Instruction *TraceThread::decode(Sift::Instruction &inst)
{

   // printf("PC: %lx Size: %d num_addresses=%d is_branch=%d\n", inst.sinst->addr, inst.sinst->size, inst.num_addresses, inst.is_branch);
   if (m_decoder_cache.count(inst.sinst->addr) == 0)
      m_decoder_cache[inst.sinst->addr] = staticDecode(inst);

   const dl::DecodedInst &dec_inst = *(m_decoder_cache[inst.sinst->addr]);

   OperandList list;

   // Ignore memory-referencing operands in NOP instructions
   if (!(dec_inst.is_nop()))
   {
      for (uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         if (Sim()->getDecoder()->op_read_mem(&dec_inst, mem_idx))
            list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));

      for (uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         if (Sim()->getDecoder()->op_write_mem(&dec_inst, mem_idx))
            list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));
   }

   Instruction *instruction;
   if (inst.is_branch)
      instruction = new BranchInstruction(list);

   else
      instruction = new GenericInstruction(list);

   instruction->setVaddress(inst.sinst->addr);
   instruction->setAddress(va2pa(inst.sinst->addr));
   instruction->setSize(inst.sinst->size);
   instruction->setAtomic(dec_inst.is_atomic());
   instruction->setDisassembly(dec_inst.disassembly_to_str().c_str());
   instruction->setName(Sim()->getDecoder()->inst_name(dec_inst.inst_num_id()));
   instruction->setDynamic(false);

   const std::vector<const MicroOp *> *uops = InstructionDecoder::decode(inst.sinst->addr, &dec_inst, instruction);
   instruction->setMicroOps(uops);

   return instruction;
}

Instruction* TraceThread::decodeChampsim(Sift::Instruction &inst)
{
   // Build cache key from instruction signature
   ChampSimCacheKey cache_key;
   cache_key.pc = inst.sinst->addr;
   cache_key.is_branch = inst.is_branch ? 1 : 0;
   cache_key.num_src_regs = inst.num_src_registers;
   cache_key.num_dest_regs = inst.num_dest_registers;
   cache_key.num_loads = inst.num_src_addresses;
   cache_key.num_stores = inst.num_dest_addresses;
   
   // Check cache first
   auto it = m_champsim_icache.find(cache_key);
   if (it != m_champsim_icache.end()) {
      // Cache hit - return cached instruction
      m_champsim_icache_hits++;
      #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
      printf("[CHAMPSIM_CACHE] HIT  pc=0x%lx branch=%d src_regs=%d dest_regs=%d loads=%d stores=%d -> Instruction*=%p (cache_size=%zu)\n",
             cache_key.pc, cache_key.is_branch, cache_key.num_src_regs, cache_key.num_dest_regs,
             cache_key.num_loads, cache_key.num_stores, (void*)it->second, m_champsim_icache.size());
      #endif
      return it->second;
   }
   
   m_champsim_icache_misses++;
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] MISS pc=0x%lx branch=%d src_regs=%d dest_regs=%d loads=%d stores=%d (cache_size=%zu) -> ALLOCATING\n",
          cache_key.pc, cache_key.is_branch, cache_key.num_src_regs, cache_key.num_dest_regs,
          cache_key.num_loads, cache_key.num_stores, m_champsim_icache.size());
   #endif
   
   // Cache miss - create new instruction
   OperandList list;

   for (uint8_t idx = 0; idx < inst.num_src_registers; ++idx)
      list.push_back(Operand(Operand::REG, inst.src_registers[idx], Operand::READ));

   for (uint8_t idx = 0; idx < inst.num_dest_registers; ++idx)
      list.push_back(Operand(Operand::REG, inst.dest_registers[idx], Operand::WRITE));

   for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
      list.push_back(Operand(Operand::MEMORY, 0, Operand::READ));

   for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
      list.push_back(Operand(Operand::MEMORY, 0, Operand::WRITE));

   Instruction *instruction = inst.is_branch ? static_cast<Instruction*>(new BranchInstruction(list))
                                             : static_cast<Instruction*>(new GenericInstruction(list));

   instruction->setAddress(va2pa(inst.sinst->addr));
   instruction->setSize(inst.sinst->size);
   instruction->setAtomic(false);
   instruction->setDisassembly("champsim-trace");
   // Cached instructions are NOT dynamic - they persist and are reused
   instruction->setDynamic(false);

   UInt32 mem_size = m_champsim_access_size ? m_champsim_access_size : Sift::CHAMPSIM_DEFAULT_MEM_SIZE;

   size_t num_loads = inst.num_src_addresses;
   size_t num_stores = inst.num_dest_addresses;
   size_t num_execs = 1;
   size_t total_microops = num_loads + num_execs + num_stores;
   if (total_microops == 0)
   {
      num_execs = 1;
      total_microops = 1;
   }

   auto *uops = new std::vector<const MicroOp*>();
   uops->reserve(total_microops);

   for (size_t idx = 0; idx < total_microops; ++idx)
   {
      MicroOp *uop = new MicroOp();
      uop->setInstructionPointer(Memory::make_access(inst.sinst->addr));
      uop->setOperandSize(mem_size * 8);
      uop->setInstruction(instruction);
      uop->setDecodedInstruction(NULL);

      if (idx < num_loads)
      {
         uop->makeLoad(idx, dl::Decoder::DL_OPCODE_INVALID, "champsim-load", mem_size);
      }
      else if (idx < num_loads + num_execs)
      {
         size_t exec_index = idx - num_loads;
         uop->makeExecute(exec_index, num_loads, dl::Decoder::DL_OPCODE_INVALID, "champsim-exec", inst.is_branch);
      }
      else
      {
         size_t store_index = idx - num_loads - num_execs;
         uop->makeStore(store_index, num_execs, dl::Decoder::DL_OPCODE_INVALID, "champsim-store", mem_size);
      }

      if (idx == 0)
         uop->setFirst(true);
      if (idx == total_microops - 1)
         uop->setLast(true);

      uops->push_back(uop);
   }

   instruction->setMicroOps(uops);
   
   // Add to cache for future reuse
   m_champsim_icache[cache_key] = instruction;
   
   #if DEBUG_CHAMPSIM_CACHE >= DEBUG_BASIC
   printf("[CHAMPSIM_CACHE] ALLOC pc=0x%lx Instruction*=%p with %zu MicroOps (new cache_size=%zu)\n",
          cache_key.pc, (void*)instruction, uops->size(), m_champsim_icache.size());
   #endif
   
   return instruction;
}

Sift::Mode TraceThread::handleInstructionCountFunc(uint32_t icount)
{
   if (!m_started)
   {
      // Received first instruction, let TraceManager know our SIFT connection is up and running
      Sim()->getTraceManager()->signalStarted();
      m_started = true;
   }

   // We may have been blocked in a system call, if we start executing instructions again that means we're continuing
   if (m_blocked)
   {
      unblock();
   }

   Core *core = m_thread->getCore();
   LOG_ASSERT_ERROR(core, "We cannot execute instructions while not on a core");
   SubsecondTime time = core->getPerformanceModel()->getElapsedTime();
   core->countInstructions(0, icount);

   if (Sim()->getInstrumentationMode() == InstMode::DETAILED && icount)
   {
      // We're in detailed mode, but our SIFT recorder doesn't know it yet
      // Do something to advance time
      core->getPerformanceModel()->queuePseudoInstruction(new UnknownInstruction(icount * core->getDvfsDomain()->getPeriod()));
      core->getPerformanceModel()->iterate();
   }

   // We may have been rescheduled
   if (m_thread->reschedule(time, core))
   {
      core = m_thread->getCore();
      core->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(time, SyncInstruction::UNSCHEDULED));
   }

   switch (Sim()->getInstrumentationMode())
   {
   case InstMode::FAST_FORWARD:
      return Sift::ModeIcount;
   case InstMode::CACHE_ONLY:
      return Sift::ModeMemory;
   case InstMode::DETAILED:
      return Sift::ModeDetailed;
   case InstMode::INVALID:
      return Sift::ModeUnknown;
   }
   assert(false);
}

void TraceThread::handleCacheOnlyFunc(uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address)
{
   Core *core = m_thread->getCore();
   if (!core)
   {
      // LOG_PRINT_WARNING("Ignoring warmup while not on a core");
      return;
   }
   // LOG_ASSERT_ERROR(core, "We cannot perform warmup while not on a core");

   if (icount)
      core->countInstructions(0, icount);

   switch (type)
   {
   case Sift::CacheOnlyBranchTaken:
   case Sift::CacheOnlyBranchNotTaken:
   {
      bool taken = (type == Sift::CacheOnlyBranchTaken);
      bool mispredict = core->accessBranchPredictor(va2pa(eip), taken, false, va2pa(address));
      if (mispredict)
         core->getPerformanceModel()->handleBranchMispredict();
      break;
   }

   case Sift::CacheOnlyMemRead:
   case Sift::CacheOnlyMemWrite:
      core->accessMemory(
          Core::NONE,
          type == Sift::CacheOnlyMemRead ? Core::READ : Core::WRITE,
          va2pa(address),
          NULL,
          4,
          Core::MEM_MODELED_COUNT,
          va2pa(eip));
      break;

   case Sift::CacheOnlyMemIcache:
      if (Sim()->getConfig()->getEnableICacheModeling())
         core->readInstructionMemory(va2pa(eip), address);
      break;
   }
}

const dl::DecodedInst *TraceThread::staticDecode(Sift::Instruction &inst)
{
   dl::DecodedInst *dec_inst = m_factory->CreateInstruction(Sim()->getDecoder(), inst.sinst->data,
                                                            inst.sinst->size, inst.sinst->addr);
   Sim()->getDecoder()->decode(dec_inst, (dl::dl_isa)inst.isa);
   return dec_inst;
}

void TraceThread::handleInstructionWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core, bool do_icache_warmup, UInt64 icache_warmup_addr, UInt64 icache_warmup_size)
{
   if (m_decoder_cache.count(inst.sinst->addr) == 0)
      m_decoder_cache[inst.sinst->addr] = staticDecode(inst);

   const dl::DecodedInst &dec_inst = *(m_decoder_cache[inst.sinst->addr]);

   // Warmup instruction caches

   if (do_icache_warmup && Sim()->getConfig()->getEnableICacheModeling())
   {
      core->readInstructionMemory(va2pa(icache_warmup_addr), icache_warmup_size);
   }

   // Warmup branch predictor

   if (inst.is_branch)
   {
      bool mispredict = core->accessBranchPredictor(va2pa(inst.sinst->addr), inst.taken, dec_inst.is_indirect_branch(), va2pa(next_inst.sinst->addr));
      if (mispredict)
         core->getPerformanceModel()->handleBranchMispredict();
   }

   // Warmup data caches

   if (inst.executed)
   {
      const bool is_atomic_update = dec_inst.is_atomic();
      const bool is_prefetch = dec_inst.is_prefetch();

      // Ignore memory-referencing operands in NOP instructions
      if (!dec_inst.is_nop())
      {
         for (uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         {
            if (Sim()->getDecoder()->op_read_mem(&dec_inst, mem_idx))
            {
               UInt64 mem_address;
               // LDP ARM instructions, second element to be loaded, using the address of the first element
               if (dec_inst.is_mem_pair() && ((int)mem_idx == (inst.num_addresses + 1)))
               {
                  LOG_ASSERT_ERROR((int)mem_idx < (inst.num_addresses + 1), "Did not receive enough data addresses");

                  mem_address = inst.addresses[mem_idx - 1] + Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx);
               }
               else
               {
                  LOG_ASSERT_ERROR(mem_idx < inst.num_addresses, "Did not receive enough data addresses");

                  mem_address = inst.addresses[mem_idx];
               }

               bool no_mapping = false;
               UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
               if (no_mapping)
                  continue;

               core->accessMemory(
                   /*(is_atomic_update) ? Core::LOCK :*/ Core::NONE,
                   (is_atomic_update) ? Core::READ_EX : Core::READ,
                   pa,
                   NULL,
                   Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx),
                   Core::MEM_MODELED_COUNT,
                   va2pa(inst.sinst->addr));
            }
         }

         for (uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
         {
            if (Sim()->getDecoder()->op_write_mem(&dec_inst, mem_idx))
            {
               UInt64 mem_address;
               // STP ARM instructions, second element to be stored, using the address of the first element
               if (dec_inst.is_mem_pair() && ((int)mem_idx == (inst.num_addresses + 1)))
               {
                  LOG_ASSERT_ERROR((int)mem_idx < (inst.num_addresses + 1), "Did not receive enough data addresses");

                  mem_address = inst.addresses[mem_idx - 1] + Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx);
               }
               else
               {
                  LOG_ASSERT_ERROR(mem_idx < inst.num_addresses, "Did not receive enough data addresses");

                  mem_address = inst.addresses[mem_idx];
               }

               bool no_mapping = false;
               UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
               if (no_mapping)
                  continue;

               if (is_atomic_update)
                  core->logMemoryHit(false, Core::WRITE, pa, Core::MEM_MODELED_COUNT, va2pa(inst.sinst->addr));
               else
                  core->accessMemory(
                      /*(is_atomic_update) ? Core::UNLOCK :*/ Core::NONE,
                      Core::WRITE,
                      pa,
                      NULL,
                      Sim()->getDecoder()->size_mem_op(&dec_inst, mem_idx),
                      Core::MEM_MODELED_COUNT,
                      va2pa(inst.sinst->addr));
            }
         }
   }
   }
}

void TraceThread::handleChampSimWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core)
{
   // Warmup instruction cache
   if (Sim()->getConfig()->getEnableICacheModeling())
   {
      core->readInstructionMemory(va2pa(inst.sinst->addr), inst.sinst->size);
   }

   // Warmup branch predictor
   if (inst.is_branch)
   {
      bool mispredict = core->accessBranchPredictor(va2pa(inst.sinst->addr), inst.taken, false, va2pa(next_inst.sinst->addr));
      if (mispredict)
         core->getPerformanceModel()->handleBranchMispredict();
   }

   if (!inst.executed)
      return;

   // Warmup data caches
   UInt32 mem_size = m_champsim_access_size ? m_champsim_access_size : Sift::CHAMPSIM_DEFAULT_MEM_SIZE;

   for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
   {
      bool no_mapping = false;
      UInt64 pa = va2pa(inst.src_addresses[idx], &no_mapping);
      if (no_mapping)
         continue;

      core->accessMemory(
            Core::NONE,
            Core::READ,
            pa,
            NULL,
            mem_size,
            Core::MEM_MODELED_COUNT,
            va2pa(inst.sinst->addr));
   }

   for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
   {
      bool no_mapping = false;
      UInt64 pa = va2pa(inst.dest_addresses[idx], &no_mapping);
      if (no_mapping)
         continue;

      core->accessMemory(
            Core::NONE,
            Core::WRITE,
            pa,
            NULL,
            mem_size,
            Core::MEM_MODELED_COUNT,
            va2pa(inst.sinst->addr));
   }
}

void TraceThread::handleInstructionDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl)
{

   // Set up instruction

   if (m_icache.count(inst.sinst->addr) == 0)
      m_icache[inst.sinst->addr] = decode(inst);
   // Here get the decoder instruction without checking, because we must have it for sure
   const dl::DecodedInst &dec_inst = *(m_decoder_cache[inst.sinst->addr]);
   Instruction *ins = m_icache[inst.sinst->addr];
   DynamicInstruction *dynins = prfmdl->createDynamicInstruction(ins, va2pa(inst.sinst->addr));

   // Add dynamic instruction info

   if (inst.is_branch)
   {
      dynins->addBranch(inst.taken, va2pa(next_inst.sinst->addr), dec_inst.is_indirect_branch());
   }

   // Ignore memory-referencing operands in NOP instructions
   if (!dec_inst.is_nop())
   {
      const bool is_prefetch = dec_inst.is_prefetch();

      for (uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
      {
         if (Sim()->getDecoder()->op_read_mem(&dec_inst, mem_idx))
         {
            addDetailedMemoryInfo(dynins, inst, dec_inst, mem_idx, Operand::READ, is_prefetch, prfmdl);
         }
      }

      for (uint32_t mem_idx = 0; mem_idx < Sim()->getDecoder()->num_memory_operands(&dec_inst); ++mem_idx)
      {
         if (Sim()->getDecoder()->op_write_mem(&dec_inst, mem_idx))
         {
            addDetailedMemoryInfo(dynins, inst, dec_inst, mem_idx, Operand::WRITE, is_prefetch, prfmdl);
         }
      }
   }


   prfmdl->queueInstruction(dynins);

   // simulate
   prfmdl->iterate();
}

void TraceThread::handleChampSimDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl)
{
   #if DEBUG_TRACE_CHAMPSIM >= DEBUG_BASIC
      std::cout << "[TraceThread] Handling ChampSim detailed instruction at address " << std::hex << inst.sinst->addr << std::dec << std::endl;
   #endif 

   // ChampSim instructions are cached by (PC, is_branch, num_src_regs, num_dest_regs, num_loads, num_stores).
   // If the same PC appears with a different operand signature, a new Instruction is created.
   // This avoids unbounded memory growth while handling variable operand counts.
   Instruction *ins = decodeChampsim(inst);
   DynamicInstruction *dynins = prfmdl->createDynamicInstruction(ins, va2pa(inst.sinst->addr));

   #if DEBUG_TRACE_CHAMPSIM >= DEBUG_BASIC
   
      std::ostringstream oss;
      oss << "[TraceThread] ChampSim instruction info: addr=0x" << std::hex << inst.sinst->addr
          << " size=" << std::dec << static_cast<uint32_t>(inst.sinst->size)
          << " branch=" << inst.is_branch
          << " taken=" << inst.taken
          << " predicate=" << inst.is_predicate
          << " executed=" << inst.executed
          << " isa=" << inst.isa
          << " is_champsim=" << inst.is_champsim
          << " num_addresses=" << static_cast<uint32_t>(inst.num_addresses)
          << " addresses=[";
      for (uint8_t idx = 0; idx < inst.num_addresses; ++idx)
      {
         if (idx)
            oss << ',';
         oss << "0x" << std::hex << inst.addresses[idx];
      }
      oss << std::dec << "] src=[";
      for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
      {
         if (idx)
            oss << ',';
         oss << "0x" << std::hex << inst.src_addresses[idx];
      }
      oss << "] dest=[";
      for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
      {
         if (idx)
            oss << ',';
         oss << "0x" << std::hex << inst.dest_addresses[idx];
      }
      oss << std::dec << "] src_regs=[";
      for (uint8_t idx = 0; idx < inst.num_src_registers; ++idx)
      {
         if (idx)
            oss << ',';
         oss << static_cast<uint32_t>(inst.src_registers[idx]);
      }
      oss << "] dest_regs=[";
      for (uint8_t idx = 0; idx < inst.num_dest_registers; ++idx)
      {
         if (idx)
            oss << ',';
         oss << static_cast<uint32_t>(inst.dest_registers[idx]);
      }
      oss << "]";
      std::cout << oss.str() << std::endl;

      std::ostringstream opss;
      const OperandList &ops = ins->getOperands();
      opss << "[TraceThread] ChampSim operand check: src_regs=[";
      for (uint8_t idx = 0; idx < inst.num_src_registers; ++idx)
      {
         if (idx)
            opss << ',';
         opss << static_cast<uint32_t>(inst.src_registers[idx]);
      }
      opss << "] dest_regs=[";
      for (uint8_t idx = 0; idx < inst.num_dest_registers; ++idx)
      {
         if (idx)
            opss << ',';
         opss << static_cast<uint32_t>(inst.dest_registers[idx]);
      }
      opss << "] operands=[";
      for (size_t i = 0; i < ops.size(); ++i)
      {
         if (i)
            opss << ';';
         opss << ops[i].toString();
      }
      opss << "]";
      std::cout << opss.str() << std::endl;
   
   #endif 

   if (inst.is_branch)
   {
      dynins->addBranch(inst.taken, va2pa(next_inst.sinst->addr), false);
   }

   for (uint8_t idx = 0; idx < inst.num_src_addresses; ++idx)
   {
      addChampSimMemoryInfo(dynins, inst.src_addresses[idx], Operand::READ, inst.executed, false, prfmdl);
   }

   for (uint8_t idx = 0; idx < inst.num_dest_addresses; ++idx)
   {
      addChampSimMemoryInfo(dynins, inst.dest_addresses[idx], Operand::WRITE, inst.executed, false, prfmdl);
   }

   prfmdl->queueInstruction(dynins);
   prfmdl->iterate();
}

void TraceThread::addDetailedMemoryInfo(DynamicInstruction *dynins, Sift::Instruction &inst, const dl::DecodedInst &decoded_inst, uint32_t mem_idx, Operand::Direction op_type, bool is_prefetch, PerformanceModel *prfmdl)
{
   UInt64 mem_address;
   // LDP/STP ARM instructions, second element to be ld/st, using the address of the first element
   if (decoded_inst.is_mem_pair() && ((int)mem_idx == inst.num_addresses))
   {
      assert((int)mem_idx < (inst.num_addresses + 1));
      mem_address = inst.addresses[mem_idx - 1] + Sim()->getDecoder()->size_mem_op(&decoded_inst, mem_idx);
   }
   else
   {
      assert(mem_idx < inst.num_addresses);
      mem_address = inst.addresses[mem_idx];
   }

   bool no_mapping = false;
   UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
   

   if (no_mapping)
   {
      dynins->addMemory(
          inst.executed,
          SubsecondTime::Zero(),
          0,
          Sim()->getDecoder()->size_mem_op(&decoded_inst, mem_idx),
          op_type,
          0,
          HitWhere::PREFETCH_NO_MAPPING);
   }
   else
   {
      dynins->addMemory(
          inst.executed,
          SubsecondTime::Zero(),
          pa,
          Sim()->getDecoder()->size_mem_op(&decoded_inst, mem_idx),
          op_type,
          0,
          HitWhere::UNKNOWN);
   }
}

void TraceThread::addChampSimMemoryInfo(DynamicInstruction *dynins, UInt64 mem_address, Operand::Direction op_type, bool executed, bool is_prefetch, PerformanceModel *prfmdl)
{
   bool no_mapping = false;
   UInt64 pa = va2pa(mem_address, is_prefetch ? &no_mapping : NULL);
   UInt32 mem_size = m_champsim_access_size ? m_champsim_access_size : Sift::CHAMPSIM_DEFAULT_MEM_SIZE;

   if (no_mapping)
   {
      dynins->addMemory(
         executed,
         SubsecondTime::Zero(),
         0,
         mem_size,
         op_type,
         0,
         HitWhere::PREFETCH_NO_MAPPING);
   }
   else
   {
      dynins->addMemory(
         executed,
         SubsecondTime::Zero(),
         pa,
         mem_size,
         op_type,
         0,
         HitWhere::UNKNOWN);
   }
}

void TraceThread::unblock()
{
   LOG_ASSERT_ERROR(m_blocked == true, "Must call only when m_blocked == true");

   SubsecondTime end_time = Sim()->getClockSkewMinimizationServer()->getGlobalTime(true /*upper_bound*/);
   m_thread->getSyscallMdl()->runExit(0);
   {
      ScopedLock sl(Sim()->getThreadManager()->getLock());
      // We were blocked on a system call, but started executing instructions again.
      // This means we woke up. Since there is no explicit wakeup nor an associated time,
      // use global time.
      Sim()->getThreadManager()->resumeThread_async(m_thread->getId(), INVALID_THREAD_ID, end_time, NULL);
   }

   if (m_thread->getCore())
   {
      m_thread->getCore()->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::SYSCALL));
   }
   else
   {
      // We were scheduled out during the system call
      m_thread->reschedule(end_time, NULL);
      m_thread->getCore()->getPerformanceModel()->queuePseudoInstruction(new SyncInstruction(end_time, SyncInstruction::UNSCHEDULED));
   }

   m_blocked = false;
}

void TraceThread::m_run_func_with_userpace_mimicos()
{
   // Set thread name for Sniper-in-Sniper simulations
   String threadName = String("trace-") + itostr(m_thread->getId());
   SimSetThreadName(threadName.c_str());

   // Pin this trace thread to a distinct host CPU to avoid contention
   pinTraceThreadToCPU(m_thread->getId());

   // Print which host CPU this thread is running on
   int host_cpu = sched_getcpu();
   std::cout << "[TraceThread] Thread " << m_thread->getId()
             << " (app " << m_app_id << ") started on host CPU " << host_cpu << std::endl;

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
   std::cout << "[TRACE:" << m_thread->getId() << "] -- " << threadName.c_str() << " --" << std::endl;
#endif
   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), m_time_start);

   // Open the trace (be sure to do this before potentially blocking on reschedule() as this causes deadlock)
   m_trace.initStream();
   m_trace_has_pa = m_trace.getTraceHasPhysicalAddresses();
   m_champsim_trace = m_trace.isChampSimTrace();

   if (m_thread->getCore() == NULL)
   {
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
      std::cout << "[TRACE:" << m_thread->getId() << "] -- Waiting for core to be assigned --" << std::endl;
#endif
      // We didn't get scheduled on startup, wait here
      SubsecondTime time = SubsecondTime::Zero();
      m_thread->reschedule(time, NULL);
   }

   Core *core = m_thread->getCore();
   PerformanceModel *prfmdl = core->getPerformanceModel();

   Sift::Instruction inst, next_inst;
   Sift::Instruction inst_kernel, next_inst_kernel;
   Sift::Instruction inst_app, next_inst_app;

   // Initialise instruction state
   to_be_replayed_inst.sinst = NULL;
   to_be_replayed_next_inst.sinst = NULL;
   inst_app.sinst = next_inst_app.sinst = NULL; // App reader state

   bool app_initialized = false;

   SubsecondTime kernel_start_time = SubsecondTime::Zero();
   SubsecondTime kernel_end_time = SubsecondTime::Zero();
   // By design, kernel SIFT reader runs first
   bool have_inst = m_current_sift_reader->Read(inst_kernel);


   kernel_start_time = prfmdl->getElapsedTime();

   if (have_inst)
   {
      inst = inst_kernel;

      int  executed_instructions = 0;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
      bool kernel_mode           = true;
      bool switched              = false;
#else
      bool kernel_mode           __attribute__((unused)) = true;
      bool switched              __attribute__((unused)) = false;
#endif

      while (have_inst)
      {
         bool have_next = false;
         switched = false;

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
         std::cout << "[TRACE:" << m_thread->getId() << "] -- Reading instruction: "
                   << std::hex << inst.sinst->addr << std::dec
                   << " from trace_id = " << getCurrentSiftReader()->getId() << std::endl;
#endif

         if (getCurrentSiftReader() != getAppSiftReader())
         {
            // Currently reading from kernel SIFT reader

            have_next = m_current_sift_reader->Read(next_inst_kernel);
            if (!have_next)
               break;   // EOF on kernel trace

            next_inst = next_inst_kernel;

            // NOTE: if we consumed a Context Switch command here, magic_server.cc
            // may just have switched currentSiftReader: Kernel -> App.
            if (getCurrentSiftReader() == getAppSiftReader())
            {

               switched = true;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
               std::cout << "[TRACE:" << m_thread->getId() << "] -- Switching SIFT reader to the App due to CS --" << std::endl;
#endif
               // Finished processing kernel instructions; record end time
               kernel_end_time = prfmdl->getElapsedTime();
               stats.kernel_time += (kernel_end_time - kernel_start_time);
               kernel_mode = false;

               if (to_be_replayed_inst.sinst)
               {
                  // Returning from a page fault: restore exact app state
                  assert(to_be_replayed_inst.sinst == inst_app.sinst &&
                         to_be_replayed_next_inst.sinst == next_inst_app.sinst);

                  inst      = to_be_replayed_inst;
                  next_inst = to_be_replayed_next_inst;
                  to_be_replayed_inst.sinst = to_be_replayed_next_inst.sinst = NULL;
                  have_next = true; // next_inst is valid by construction

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
                  std::cout << "[TRACE:" << m_thread->getId() << "] -- Replaying instruction: inst = "
                            << std::hex << inst.sinst->addr << std::dec << std::endl;
#endif
               }
               else
               {
                  // First time or normal resume on app reader
                  if (!app_initialized)
                  {
                     // First time: read both inst_app and next_inst_app
                     if (!m_current_sift_reader->Read(inst_app))
                        break;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
                     std::cout << "[TRACE:" << m_thread->getId() << "] -- First instruction from App SIFT reader: inst = "
                               << std::hex << inst_app.sinst->addr << std::dec << std::endl;
#endif
                     if (!readNextAppInstruction(next_inst_app))
                        break;
                     app_initialized = true;
                  }
                  else
                  {
                     // Continue: shift lookahead then read a new next_inst_app
                     inst_app = next_inst_app;
                     if (!readNextAppInstruction(next_inst_app))
                        break;
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
                     std::cout << "[TRACE:" << m_thread->getId() << "] -- Restored instruction from App SIFT reader: inst = "
                               << std::hex << inst_app.sinst->addr << std::dec << std::endl;
#endif
                  }

                  inst      = inst_app;
                  next_inst = next_inst_app;
                  have_next = true;
               }

#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
               std::cout << "[TRACE:" << m_thread->getId() << "] -- Instruction: " << std::hex << inst.sinst->addr << std::dec
                         << " -- Next instruction: " << std::hex << next_inst.sinst->addr << std::dec
                         << " -- Kernel mode: " << (kernel_mode ? "true" : "false") << std::endl;
#endif
            }
         }
         else
         {
            // Currently reading from app SIFT reader

            // We assume app_initialized is true when we get here
            inst_app = next_inst_app;
            if (!readNextAppInstruction(next_inst_app))
               break;   // EOF on app trace

            next_inst = next_inst_app;
            have_next = true;
         }

         if (!m_started)
         {
            // Received first instructions, let TraceManager know our SIFT connection is up and running
            // Only enable once we have received two instructions, otherwise, we could deadlock
            Sim()->getTraceManager()->signalStarted();
            m_started = true;
         }

         // We may have been blocked in a system call; starting to execute again means we continue
         if (m_blocked)
         {
            unblock();
         }

         core = m_thread->getCore();
         LOG_ASSERT_ERROR(core, "We cannot execute instructions while not on a core");
         prfmdl = core->getPerformanceModel();

         bool   do_icache_warmup = false;
         UInt64 icache_warmup_addr = 0, icache_warmup_size = 0;

         // Reconstruct and count basic blocks

         if (m_bbv_end || m_bbv_last != inst.sinst->addr)
         {
            // We're the start of a new basic block
            core->countInstructions(m_bbv_base, m_bbv_count);
            // In cache-only mode, we'll want to do I-cache warmup
            if (m_bbv_base)
            {
               do_icache_warmup = true;
               icache_warmup_addr = m_bbv_base;
               icache_warmup_size = m_bbv_last - m_bbv_base;
            }
            // Set up new basic block info
            m_bbv_base = inst.sinst->addr;
            m_bbv_count = 0;
         }
         m_bbv_count++;
         m_bbv_last = inst.sinst->addr + inst.sinst->size;
         // Force BBV end on non-taken branches
         m_bbv_end = inst.is_branch;

         // Kernel-mode (MimicOS) instructions are always fast-forwarded:
         // we only model the application's performance in detail.
         bool in_kernel_mode = (getCurrentSiftReader() != getAppSiftReader());

         if (in_kernel_mode)
         {
            // Fast-forward: just count the instruction
            core->countInstructions(0, 1);
         }
         else if (inst.is_champsim)
         {
            switch(Sim()->getInstrumentationMode())
            {
               case InstMode::FAST_FORWARD:
                  break;

               case InstMode::CACHE_ONLY:
                  handleChampSimWarmup(inst, next_inst, core);
                  break;

               case InstMode::DETAILED:
                  handleChampSimDetailed(inst, next_inst, prfmdl);
                  break;

               default:
                  LOG_PRINT_ERROR("Unknown instrumentation mode");
            }
         }
         else
         {
            switch(Sim()->getInstrumentationMode())
            {
               case InstMode::FAST_FORWARD:
                  break;

               case InstMode::CACHE_ONLY:
                  handleInstructionWarmup(inst, next_inst, core, do_icache_warmup, icache_warmup_addr, icache_warmup_size);
                  break;

               case InstMode::DETAILED:
                  if (!checkAndReplaySpinLoop(inst, next_inst, core, prfmdl))
                     handleInstructionDetailed(inst, next_inst, prfmdl);
                  break;

               default:
                  LOG_PRINT_ERROR("Unknown instrumentation mode");
            }
         }

         // We may have been rescheduled to a different core
         // by prfmdl->iterate (in handleInstructionDetailed),
         // or core->countInstructions (when using a fast-forward performance model)
         {
            SubsecondTime time = prfmdl->getElapsedTime();
            if (m_thread->reschedule(time, core))
            {
               core = m_thread->getCore();
               prfmdl = core->getPerformanceModel();
            }
         }

         if (m_stop)
         {
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Stopping trace thread --" << std::endl;
#endif
            break;
         }

         auto* mimic_os = Sim()->getMimicOS();
         core_id_t pf_core_id = core->getId();

         if (mimic_os->getIsPageFault(pf_core_id))
         {
            kernel_start_time = prfmdl->getElapsedTime();
            // Page fault must occur only in user space (app reader active)
            assert(getCurrentSiftReader() == getAppSiftReader());
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Switching SIFT reader back to the kernel due to page fault CS --" << std::endl;
#endif

            // Save app instruction pair to replay after kernel handles the fault
            to_be_replayed_inst      = inst_app;
            to_be_replayed_next_inst = next_inst_app;

            int num_requested_frames = mimic_os->getNumRequestedFrames(pf_core_id);
#if DEBUG_TRACE_THREAD >= DEBUG_BASIC
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Requesting "
                      << num_requested_frames << " frames for page fault handling" << std::endl;
#endif

            mimic_os->buildMessageWithArgs("page_fault",
                                           mimic_os->getVaTriggeredPageFault(pf_core_id),
                                           num_requested_frames);

            // Handle PF on user-space MimicOS (kernel) side
#if DEBUG_TRACE_THREAD >= DEBUG_DETAILED
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Switching SIFT reader to Kernel and sending response after CS --" << std::endl;
#endif
            setCurrentSiftReader(getKernelSiftReader());

            // Unlock Sift::Writer on MimicOS (kernel) side
            getCurrentSiftReader()->sendResponseAfterContextSwitch();

            // First instruction after switching back to kernel
            bool have_next_pf = m_current_sift_reader->Read(next_inst_kernel);
            if (!have_next_pf)
               break;   // EOF on kernel after PF handling

            next_inst = next_inst_kernel;
            have_next = have_next_pf;

            kernel_mode = true;
            mimic_os->resetPageFaultState(pf_core_id);
         }

         // Save current "next_inst" as the next "inst"
         inst      = next_inst;
         have_inst = have_next;

#ifdef HEARTBEAT
         if (executed_instructions % 20000 == 0)
         {
            // Print progress every 20k instructions
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Current SIFT reader: "
                      << (getCurrentSiftReader() == getKernelSiftReader() ? "Kernel" : "App") << " -- "
                      << "Executed instructions: " << executed_instructions
                      << ", Current time: " << prfmdl->getElapsedTime().getNS() << "ns"
                      << ", Current SIFT reader position: " << m_current_sift_reader->getPosition()
                      << std::endl;
            std::cout << "[TRACE:" << m_thread->getId() << "] -- Current instruction: " << std::hex << inst.sinst->addr << std::dec
                      << ", Next instruction: " << std::hex << next_inst.sinst->addr << std::dec
                      << ", Kernel mode: " << (kernel_mode ? "true" : "false")
                      << std::endl;
         }
#endif

         executed_instructions++;
      }
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");
   printf("[REPLAY SUMMARY] Thread %u: %lu spin iterations injected (%lu insns), %lu recorded spin insns skipped (%lu sync waits cut short)\n",
          m_thread->getId(), (unsigned long)m_replays, (unsigned long)m_injected_insns,
          (unsigned long)m_skipped_insns, (unsigned long)m_sync_skips);
   
   // Clean up ChampSim instruction cache to avoid leaks (destructor is never called)
   if (m_champsim_trace) {
      cleanupChampSimCache();
   }

   SubsecondTime time_end = prfmdl->getElapsedTime();

   Sim()->getThreadManager()->onThreadExit(m_thread->getId());
   Sim()->getTraceManager()->signalDone(this, time_end, m_stop /*aborted*/);
}

bool TraceThread::readNextAppInstruction(Sift::Instruction &inst_out)
{
   if (!m_injected_trace.empty())
   {
      inst_out = m_injected_trace.front();
      m_injected_trace.pop_front();
      return true;
   }
   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   return reader->Read(inst_out);
}

Lock TraceThread::s_spin_loops_lock;
std::map<std::pair<IntPtr, IntPtr>, TraceThread::SpinLoop> TraceThread::s_spin_loops;
std::map<IntPtr, IntPtr> TraceThread::s_last_loop;
std::map<std::pair<IntPtr, IntPtr>, TraceThread::PreCheckLink> TraceThread::s_prechecks;

const dl::DecodedInst &TraceThread::decoded(Sift::Instruction &inst)
{
   if (m_decoder_cache.count(inst.sinst->addr) == 0)
      m_decoder_cache[inst.sinst->addr] = staticDecode(inst);
   return *(m_decoder_cache[inst.sinst->addr]);
}

bool TraceThread::storesTo(Sift::Instruction &inst, IntPtr var_base)
{
   if (inst.num_addresses == 0 || !Sim()->getLockStateTracker())
      return false;
   const dl::DecodedInst &dec = decoded(inst);
   unsigned int n = Sim()->getDecoder()->num_memory_operands(&dec);
   for (unsigned int m = 0; m < n && m < inst.num_addresses; ++m)
      if (Sim()->getDecoder()->op_write_mem(&dec, m) &&
          Sim()->getLockStateTracker()->getVariableBase(va2pa(inst.addresses[m])) == var_base)
         return true;
   return false;
}

IntPtr TraceThread::otherSuccessor(Sift::Instruction &inst, IntPtr taken_pc)
{
   IntPtr fall = inst.sinst->addr + inst.sinst->size;
   uint64_t target = 0;
   if (!decoded(inst).branch_target(target))
      return 0;
   if (taken_pc == fall)
      return target;
   if (taken_pc == target)
      return fall;
   return 0;
}

bool TraceThread::getSpinLoop(IntPtr var_base, IntPtr load_pc, IntPtr &lo, IntPtr &hi)
{
   ScopedLock sl(s_spin_loops_lock);
   std::map<std::pair<IntPtr, IntPtr>, SpinLoop>::iterator it = s_spin_loops.find(std::make_pair(var_base, load_pc));
   if (it == s_spin_loops.end())
      return false;
   lo = it->second.lo;
   hi = it->second.hi;
   return true;
}

bool TraceThread::getSpinLoopInfo(IntPtr var_base, IntPtr load_pc, IntPtr &lo, IntPtr &hi, std::vector<IntPtr> &exits)
{
   ScopedLock sl(s_spin_loops_lock);
   std::map<std::pair<IntPtr, IntPtr>, SpinLoop>::iterator it = s_spin_loops.find(std::make_pair(var_base, load_pc));
   if (it == s_spin_loops.end())
      return false;
   lo = it->second.lo;
   hi = it->second.hi;
   exits = it->second.exits;
   return true;
}

void TraceThread::learnSpinLoopFromPath(IntPtr var_base, IntPtr load_pc)
{
   // Only from the recorded trace: an injected iteration (replay) is not necessarily a whole iteration of this loop
   if (m_path_overflow || !m_path_clean || m_path_replays != m_replays || m_path.size() < 2 || m_path[0].sinst->addr != load_pc)
      return;
   const std::pair<IntPtr, IntPtr> key(var_base, load_pc);
   {
      ScopedLock sl(s_spin_loops_lock);
      if (s_spin_loops.count(key) > 0)
         return;
   }
   // Only from a spin the detector reported: a path back to the load after the spin (an outer loop) is not an iteration
   if (!Sim()->getLockStateTracker()->isSpinningOn(m_thread->getId(), var_base))
      return;

   SpinLoop loop;
   loop.lo = loop.hi = load_pc;
   for (size_t i = 0; i < m_path.size(); ++i)
   {
      IntPtr a = m_path[i].sinst->addr;
      if (a < loop.lo) loop.lo = a;
      if (a > loop.hi) loop.hi = a;
   }
   // Exits: successors of the path's branches outside the range (the taken target from the decoder if not taken here)
   for (size_t i = 0; i < m_path.size(); ++i)
   {
      if (!m_path[i].is_branch)
         continue;
      IntPtr succ[2] = { m_path[i].sinst->addr + m_path[i].sinst->size, 0 };
      uint64_t target = 0;
      if (decoded(m_path[i]).branch_target(target))
         succ[1] = target;
      else if (i + 1 < m_path.size())
         succ[1] = m_path[i + 1].sinst->addr;
      for (int k = 0; k < 2; ++k)
         if (succ[k] != 0 && (succ[k] < loop.lo || succ[k] > loop.hi) &&
             std::find(loop.exits.begin(), loop.exits.end(), succ[k]) == loop.exits.end())
            loop.exits.push_back(succ[k]);
   }
   for (size_t i = 0; i < m_path.size(); ++i)
   {
      Sift::Instruction copy = m_path[i];
      // The reader that owns the static instruction goes away with its thread: keep a copy (never freed)
      Sift::StaticInstruction *sinst = new Sift::StaticInstruction(*copy.sinst);
      sinst->next = NULL;
      copy.sinst = sinst;
      loop.insts.push_back(copy);
   }

   ScopedLock sl(s_spin_loops_lock);
   if (s_spin_loops.count(key) > 0)
      return;
   s_spin_loops[key] = loop;
   s_last_loop[var_base] = load_pc;

   printf("[SPIN LOOP] Variable 0x%lx: spin loop at load PC 0x%lx: PC 0x%lx - 0x%lx (%zu insns), exits:",
          (unsigned long)var_base, (unsigned long)load_pc, (unsigned long)loop.lo, (unsigned long)loop.hi, loop.insts.size());
   for (size_t e = 0; e < loop.exits.size() && e < 4; ++e)
      printf(" 0x%lx", (unsigned long)loop.exits[e]);
   printf("%s (Thread %d)\n", loop.exits.size() > 4 ? " ..." : "", m_thread->getId());
   fflush(stdout);
}

void TraceThread::countVisits(Sift::Instruction &inst)
{
   if (inst.num_addresses > 0)
   {
      const dl::DecodedInst &dec = decoded(inst);
      unsigned int n = Sim()->getDecoder()->num_memory_operands(&dec);
      IntPtr pc = inst.sinst->addr;

      // Reads (not the read of an atomic read-modify-write instruction)
      if (!dec.is_atomic())
      {
         for (unsigned int m = 0; m < n && m < inst.num_addresses; ++m)
         {
            if (!Sim()->getDecoder()->op_read_mem(&dec, m))
               continue;
            IntPtr addr = va2pa(inst.addresses[m]);
            m_reads_since_branch.insert(addr);
            if (m_visit_addrs.find(addr) == m_visit_addrs.end())
            {
               m_visit_addrs[addr] = pc;
               PcVisits &pv = m_pc_visits[pc];
               if (pv.addr != addr)
               {
                  pv.addr = addr;
                  pv.visits = 0;
               }
               pv.visits++;
            }
         }
      }

      // Writes end every visit in progress; a visit to the written address read since the last branch is undone
      bool stored = false;
      for (unsigned int m = 0; m < n && m < inst.num_addresses; ++m)
      {
         if (!Sim()->getDecoder()->op_write_mem(&dec, m))
            continue;
         stored = true;
         IntPtr addr = va2pa(inst.addresses[m]);
         std::unordered_map<IntPtr, IntPtr>::const_iterator v = m_visit_addrs.find(addr);
         if (v != m_visit_addrs.end() && m_reads_since_branch.count(addr) > 0)
         {
            PcVisits &pv = m_pc_visits[v->second];
            if (pv.addr == addr && pv.visits > 0)
               pv.visits--;
         }
      }
      if (stored && !m_visit_addrs.empty())
      {
         if (m_visit_addrs.bucket_count() > 64)
            std::unordered_map<IntPtr, IntPtr>().swap(m_visit_addrs);   // after a long read-only stretch: free the buckets
         else
            m_visit_addrs.clear();
      }
   }
   if (inst.is_branch)
      m_reads_since_branch.clear();
}

UInt64 TraceThread::frontendEarlierWaits(IntPtr addr) const
{
   UInt64 waits = 0;
   for (std::unordered_map<IntPtr, PcVisits>::const_iterator p = m_pc_visits.begin(); p != m_pc_visits.end(); ++p)
      if (p->second.addr == addr)
         waits += p->second.visits;
   if (m_visit_addrs.count(addr) > 0 && waits > 0)
      waits--;
   return waits;
}

bool TraceThread::isPreCheckOf(const LoneCheck &lone, IntPtr var_base, IntPtr loop_pc)
{
   IntPtr lo, hi;
   std::vector<IntPtr> exits;
   if (!getSpinLoopInfo(var_base, loop_pc, lo, hi, exits))
      return false;
   // Entry side: from the branch, the trace went (past nops) into the loop
   if (lone.first_real != 0 && lone.first_real >= lo && lone.first_real <= hi)
      return true;
   // Exit side: the branch's other successor is where the loop exits
   return lone.other != 0 && std::find(exits.begin(), exits.end(), lone.other) != exits.end();
}

void TraceThread::linkPreCheck(const LoneCheck &lone, IntPtr var_base, IntPtr loop_pc)
{
   if (lone.pc == loop_pc)
      return;   // the loop's own check (a provisional wait chained over iterations of a loop not learned yet)
   ScopedLock sl(s_spin_loops_lock);
   const std::pair<IntPtr, IntPtr> key(var_base, lone.pc);
   if (s_prechecks.count(key) > 0)
      return;
   PreCheckLink link;
   link.loop_pc = loop_pc;
   link.into = lone.taken;
   s_prechecks[key] = link;
   printf("[SPIN LOOP] Variable 0x%lx: check at PC 0x%lx is the pre-check of the spin loop at load PC 0x%lx (goes into it at 0x%lx) (Thread %d)\n",
          (unsigned long)var_base, (unsigned long)lone.pc, (unsigned long)loop_pc, (unsigned long)lone.taken, m_thread->getId());
   fflush(stdout);
}

bool TraceThread::injectSpinLoop(Sift::Instruction &inst, Sift::Instruction &next_inst, IntPtr var_base, IntPtr load_pc)
{
   std::vector<Sift::Instruction> cycle;
   IntPtr lo;
   {
      ScopedLock sl(s_spin_loops_lock);
      std::map<std::pair<IntPtr, IntPtr>, SpinLoop>::iterator it = s_spin_loops.find(std::make_pair(var_base, load_pc));
      if (it == s_spin_loops.end())
         return false;
      cycle = it->second.insts;
      lo = it->second.lo;
   }

   // The stored loop carries the memory addresses of the thread that recorded it. Only the tracked variable is at the
   // same address for every thread; anything else (e.g. a thread-local operand, %fs:...) must be this thread's own.
   // Use this thread's latest instance of the instruction, else keep it only if all its accesses are to the variable,
   // else leave it out of the replay rather than access another thread's memory.
   std::vector<Sift::Instruction> own;
   for (size_t k = 0; k < cycle.size(); ++k)
   {
      Sift::Instruction b = cycle[k];
      if (b.num_addresses > 0)
      {
         std::unordered_map<IntPtr, Sift::Instruction>::iterator mine = m_sift_loop_insts.find(b.sinst->addr);
         if (mine != m_sift_loop_insts.end() && mine->second.num_addresses == b.num_addresses)
         {
            for (int a = 0; a < b.num_addresses; ++a)
               b.addresses[a] = mine->second.addresses[a];
         }
         else
         {
            bool only_var = true;
            for (int a = 0; a < b.num_addresses; ++a)
               if (Sim()->getLockStateTracker()->getVariableBase(va2pa(b.addresses[a])) != var_base)
                  only_var = false;
            if (!only_var)
               continue;
         }
      }
      own.push_back(b);
   }
   if (own.size() < 2)
      return false;

   IntPtr pc = inst.sinst->addr;
   Sift::Instruction deferred_next = next_inst;

   // This branch is on the loop: the replay goes once around the loop from this branch's other successor and ends on
   // this branch again, with its recorded outcome (-> deferred_next, checked again there)
   size_t at = own.size();
   for (size_t k = 0; k < own.size(); ++k)
      if (own[k].sinst->addr == pc)
      {
         at = k;
         break;
      }
   std::vector<Sift::Instruction> seq;
   if (at < own.size())
   {
      Sift::Instruction again = inst;   // this branch again at the end, with its recorded outcome
      for (size_t k = 1; k < own.size(); ++k)
         seq.push_back(own[(at + k) % own.size()]);
      inst.taken = (seq[0].sinst->addr != pc + inst.sinst->size);
      next_inst = seq[0];
      for (size_t k = 1; k < seq.size(); ++k)
         m_injected_trace.push_back(seq[k]);
      m_injected_trace.push_back(again);
      m_injected_trace.push_back(deferred_next);
   }
   else
   {
      // A branch outside the loop (e.g. a pre-check's): one iteration from the loop's head; its closing branch then
      // leaves the loop to deferred_next
      size_t head = 0;
      for (size_t k = 0; k < own.size(); ++k)
         if (own[k].sinst->addr == lo)
         {
            head = k;
            break;
         }
      Sift::Instruction again = inst;   // the held branch again at the end, with its recorded outcome
      for (size_t k = 0; k < own.size(); ++k)
         seq.push_back(own[(head + k) % own.size()]);
      inst.taken = (seq[0].sinst->addr != pc + inst.sinst->size);
      next_inst = seq[0];
      for (size_t k = 1; k < seq.size(); ++k)
         m_injected_trace.push_back(seq[k]);
      if (m_injected_trace.back().is_branch)
         m_injected_trace.back().taken = false;
      m_injected_trace.push_back(again);
      m_injected_trace.push_back(deferred_next);
   }

   m_synthetic_left = seq.size();
   m_replays++;
   m_injected_insns += seq.size();
   return true;
}

bool TraceThread::injectFallback(Sift::Instruction &inst, Sift::Instruction &next_inst, IntPtr var_base, IntPtr check_pc)
{
   IntPtr last_pc = 0;
   {
      ScopedLock sl(s_spin_loops_lock);
      std::map<IntPtr, IntPtr>::iterator l = s_last_loop.find(var_base);
      if (l != s_last_loop.end())
         last_pc = l->second;
   }
   if (last_pc != 0 && injectSpinLoop(inst, next_inst, var_base, last_pc))
      return true;

   // What this thread executed from the check up to this branch
   std::vector<IntPtr> body;
   for (size_t i = m_sift_loop_order.size(); i-- > 0; )
   {
      body.insert(body.begin(), m_sift_loop_order[i]);
      if (m_sift_loop_order[i] == check_pc)
         break;
   }
   if (body.size() < 2 || body[0] != check_pc)
      return false;

   Sift::Instruction deferred_next = next_inst;
   // Turn this branch into taken back to the check; the last one is this branch again, with its recorded outcome
   inst.taken = true;
   next_inst = m_sift_loop_insts[body[0]];
   for (size_t k = 1; k < body.size(); ++k)
      m_injected_trace.push_back(m_sift_loop_insts[body[k]]);
   m_injected_trace.push_back(deferred_next);

   m_synthetic_left = body.size() - 1;   // the last one is the held branch itself
   m_replays++;
   m_injected_insns += body.size();
   return true;
}

void TraceThread::cutShortSpin(Sift::Instruction &inst, Sift::Instruction &next_inst, IntPtr var_base, IntPtr lo, IntPtr hi, PerformanceModel *prfmdl)
{
   IntPtr resume_pc = next_inst.sinst->addr;
   Sift::Instruction resume = next_inst;
   std::vector<Sift::Instruction> tail;   // what follows 'resume' in the recorded trace, up to the first instruction out of [lo, hi]
   UInt64 skipped = 0, read = 0;
   double timeout = Sim()->getLockStateTracker()->getStuckTimeout();
   std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

   while (true)
   {
      Sift::Instruction r;
      if (!readNextAppInstruction(r))
         break;
      tail.push_back(r);
      if (r.sinst->addr < lo || r.sinst->addr > hi)
         break;
      if (r.sinst->addr == resume_pc)
      {
         // A later time at the same point: resume there instead
         skipped += tail.size();
         resume = r;
         tail.clear();
      }
      // The recorded spin only ends when the native thread gets past it: if that never happens, report it
      if (timeout > 0 && (++read & 0xfff) == 0)
      {
         double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
         if (secs >= timeout)
         {
            char what[256];
            snprintf(what, sizeof(what), "STUCK: the recorded spin never ends (skipping the rest of the spin after a pass: loop 0x%lx - 0x%lx, "
                     "branch 0x%lx, the recorded trace is at PC 0x%lx after %lu instructions)",
                     (unsigned long)lo, (unsigned long)hi, (unsigned long)inst.sinst->addr, (unsigned long)r.sinst->addr, (unsigned long)read);
            Sim()->getLockStateTracker()->reportStuck(var_base, m_thread->getId(), what, secs, prfmdl->getElapsedTime());
         }
      }
   }

   next_inst = resume;
   for (size_t k = tail.size(); k-- > 0; )
      m_injected_trace.push_front(tail[k]);

   if (skipped > 0)
   {
      m_sync_skips++;
      m_skipped_insns += skipped;
   }
}

bool TraceThread::checkAndReplaySpinLoop(Sift::Instruction &inst, Sift::Instruction &next_inst, Core* &core, PerformanceModel* &prfmdl)
{
   if (!inst.sinst || !next_inst.sinst)
      return false;

   // An instruction injected by a replay (not the held branch it comes back to)
   bool synthetic = (m_synthetic_left > 0);
   if (synthetic)
      m_synthetic_left--;

   // Wait counting from this thread's own reads (only the recorded trace, not replays)
   if (!synthetic && Sim()->getLockStateTracker())
      countVisits(inst);

   m_sift_loop_insts[inst.sinst->addr] = inst;
   m_sift_loop_order.push_back(inst.sinst->addr);
   if (m_sift_loop_order.size() > 64)
   {
      IntPtr old_pc = m_sift_loop_order.front();
      m_sift_loop_order.pop_front();
      bool still_recent = false;
      for (size_t i = 0; i < m_sift_loop_order.size(); ++i)
      {
         if (m_sift_loop_order[i] == old_pc)
         {
            still_recent = true;
            break;
         }
      }
      if (!still_recent)
         m_sift_loop_insts.erase(old_pc);
   }

   // Ask the LockStateTracker whether the current thread is spinning on a lock;
   // whether another thread still holds that lock is checked when the trace leaves the spin loop
   m_is_spinning = false;
   m_other_thread_holds_lock = false;

   if (Sim()->getLockStateTracker())
   {
      m_is_spinning = Sim()->getLockStateTracker()->isSpinning(m_thread->getId());
   }

   // Check of a tracked variable: spin-loop load of a lock, or any load of a sync variable.
   // A lock check is remembered until the branch that follows it.
   IntPtr sync_check_addr = 0;
   IntPtr check_now = 0;   // base of the variable this instruction checks (0 if it is not a check)
   if (Sim()->getLockStateTracker() && inst.num_addresses > 0 &&
       Sim()->getLockStateTracker()->isWaitLoad(inst.sinst->addr, va2pa(inst.addresses[0])))
   {
      // Only an instruction that just reads the variable is a check
      const dl::DecodedInst &dec_inst = decoded(inst);

      if (Sim()->getDecoder()->num_memory_operands(&dec_inst) > 0 &&
          Sim()->getDecoder()->op_read_mem(&dec_inst, 0) &&
          !Sim()->getDecoder()->op_write_mem(&dec_inst, 0))
      {
         m_check_pc = inst.sinst->addr;
         m_check_base = Sim()->getLockStateTracker()->getVariableBase(va2pa(inst.addresses[0]));
         check_now = m_check_base;

         // First check of this variable by this thread: hand over the waits it completed on it so far
         if (m_check_base != 0 && !synthetic && m_provided_vars.insert(m_check_base).second)
            Sim()->getLockStateTracker()->provideEarlierWaits(m_thread->getId(), m_check_base, frontendEarlierWaits(m_check_base));

         if (Sim()->getLockStateTracker()->isSyncVariable(va2pa(inst.addresses[0])))
         {
            sync_check_addr = m_check_base;
         }
         else
         {
            m_claim_load_pc = inst.sinst->addr;
            m_claim_addr = va2pa(inst.addresses[0]);

            // Wait episodes on a lock; the first check at a spin load is the start of the wait (lock pattern).
            // (Waits on sync variables are reported by the sync logic below.)
            if (m_check_base != 0)
            {
               if (m_wait_base != m_check_base)
               {
                  m_wait_base = m_check_base;
                  m_wait_reported = false;
               }
               m_wait_check_pc = inst.sinst->addr;
               if (!m_wait_reported && Sim()->getLockStateTracker()->isSpinLoad(inst.sinst->addr))
               {
                  Sim()->getLockStateTracker()->onWaitStart(m_thread->getId(), m_wait_base, inst.sinst->addr, prfmdl->getElapsedTime());
                  m_wait_reported = true;
               }
            }
         }
      }
   }

   // Path since the last check: back at the same check of the same variable closes one iteration of its spin loop
   if (check_now != 0)
   {
      if (m_path_pc == inst.sinst->addr && m_path_var == check_now)
         learnSpinLoopFromPath(check_now, m_path_pc);
      m_path.clear();
      m_path_overflow = false;
      m_path_var = check_now;
      m_path_pc = inst.sinst->addr;
      m_path_clean = m_injected_trace.empty();
      m_path_replays = m_replays;
      m_path.push_back(inst);
   }
   else if (m_path_pc != 0)
   {
      if (m_path.size() < MAX_PATH)
         m_path.push_back(inst);
      else
         m_path_overflow = true;
   }

   bool exit_checked = false;

   std::vector<IntPtr> loop_pcs;
   if (m_is_spinning &&
       Sim()->getSpinLoopDetector() &&
       Sim()->getSpinLoopDetector()->getRecordedLoopPCs(m_thread->getId(), loop_pcs) &&
       !loop_pcs.empty())
   {
      int branch_idx = -1;
      bool next_in_loop = false;
      for (size_t i = 0; i < loop_pcs.size(); ++i)
      {
         if (loop_pcs[i] == inst.sinst->addr) branch_idx = (int)i;
         if (loop_pcs[i] == next_inst.sinst->addr) next_in_loop = true;
      }
      bool current_in_loop = (branch_idx != -1);

      // Backward jump is repeating the loop
      if (next_inst.sinst->addr < inst.sinst->addr)
      {
         next_in_loop = true;
      }

      // Early exit: the lock was released but the trace keeps spinning, so skip the rest of the recorded spin.
      // The thread still takes the lock at its own recorded exit, so the handoff order follows the trace.
      if (current_in_loop && next_in_loop && Sim()->getLockStateTracker()->isLockFree(m_thread->getId()))
      {
         m_skipped_insns++;
         return true;
      }

      // The recorded loop body can be from an older spin: it is only replayed if this thread still has all its instructions
      bool have_body = true;
      for (size_t i = 0; i < loop_pcs.size(); ++i)
      {
         if (m_sift_loop_insts.count(loop_pcs[i]) == 0)
            have_body = false;
      }

      IntPtr stored_lo, stored_hi;
      bool have_stored = m_check_base != 0 && getSpinLoop(m_check_base, m_check_pc, stored_lo, stored_hi);

      if (inst.is_branch && current_in_loop && !next_in_loop && (have_body || have_stored))
      {
         // The trace leaves the spin loop here: take the lock, or keep spinning while another thread holds it
         exit_checked = true;
         m_other_thread_holds_lock =!Sim()->getLockStateTracker()->tryClaim(m_thread->getId(), inst.sinst->addr,
                                                                             prfmdl->getElapsedTime(), core->getInstructionCount());

         if (m_other_thread_holds_lock && !injectSpinLoop(inst, next_inst, m_check_base, m_check_pc))
         {
            Sift::Instruction deferred_next = next_inst;

            // Turn this branch into taken back into the loop
            inst.taken = true;

            // Tian's iteration starts at the anchor load, not at the loop head,
            // so start right after the exit branch and end ON the exit branch.
            size_t N = loop_pcs.size();
            next_inst = m_sift_loop_insts[loop_pcs[(branch_idx + 1) % N]];

            for (size_t k = 2; k <= N; ++k)
            {
               Sift::Instruction syn_inst = m_sift_loop_insts[loop_pcs[(branch_idx + k) % N]];
               if (k == N)
                  syn_inst.taken = false;   // the exit branch again -> falls through to deferred_next -> re-checked
               m_injected_trace.push_back(syn_inst);
            }

            // Put back the original exit instruction at the end of the injected iteration
            m_injected_trace.push_back(deferred_next);

            m_replays++;
            m_injected_insns += N;
         }
      }
   }

   // A thread that loads a lock variable at a spin-loop load and leaves the loop without a detected spin
   // takes the lock here, or becomes a contender and redoes the loop until it can become the holder
   if (Sim()->getLockStateTracker())
   {
      if (m_claim_load_pc != 0 && inst.is_branch)
      {
         IntPtr load_pc = m_claim_load_pc;
         m_claim_load_pc = 0;

         // A backward jump repeats the loop; otherwise the trace leaves it here
         if (!exit_checked &&
             next_inst.sinst->addr > inst.sinst->addr &&
             !Sim()->getLockStateTracker()->tryClaimLoad(m_thread->getId(), m_claim_addr, inst.sinst->addr,
                                                         prfmdl->getElapsedTime(), core->getInstructionCount()))
         {
            // Replay the stored spin loop of the lock if there is one, otherwise what this thread saw: from the spin-loop load up to this branch
            IntPtr lock_base = Sim()->getLockStateTracker()->getVariableBase(m_claim_addr);
            std::vector<IntPtr> body;
            if (lock_base != 0 && injectSpinLoop(inst, next_inst, lock_base, load_pc))
            {
               m_other_thread_holds_lock = true;
            }
            else
            {
               for (size_t i = m_sift_loop_order.size(); i-- > 0; )
               {
                  body.insert(body.begin(), m_sift_loop_order[i]);
                  if (m_sift_loop_order[i] == load_pc)
                     break;
               }
            }

            if (!m_other_thread_holds_lock && body.size() > 1 && body[0] == load_pc)
            {
               m_other_thread_holds_lock = true;

               Sift::Instruction deferred_next = next_inst;

               // Turn this branch into taken back to the spin-loop load
               inst.taken = true;
               next_inst = m_sift_loop_insts[body[0]];

               // The last one is this branch again, with its recorded outcome -> continues to deferred_next -> re-checked
               for (size_t k = 1; k < body.size(); ++k)
               {
                  m_injected_trace.push_back(m_sift_loop_insts[body[k]]);
               }

               // Put back the original exit instruction at the end of the injected iteration
               m_injected_trace.push_back(deferred_next);

               m_replays++;
               m_injected_insns += body.size();
            }
         }
      }
   }

   // Sync variable waits (each wait uses one write by another thread; a wait is never passed without one).
   // - At a check: decide whether it is the same wait or a new one (pre-check of a loop -> same wait).
   // - At a branch: in the bounding loop's range -> still waiting (after a pass the rest of the recorded spin is
   //   skipped); leaving with no write yet -> held: the loop is replayed until the write commits.
   // - A read of the variable followed by a store to it with no branch in between is a read-modify-write, not a wait.
   LockStateTracker *tracker = Sim()->getLockStateTracker();
   if (tracker && !m_other_thread_holds_lock && synthetic)
   {
      // Inside a replay: only check again whether the write is there for the held wait
      if (sync_check_addr != 0 && sync_check_addr == m_sync_addr && !m_sync_passed && !m_sync_provisional)
         m_sync_passed = tracker->tryPassSync(m_thread->getId(), m_sync_addr, inst.sinst->addr,
                                              prfmdl->getElapsedTime(), core->getInstructionCount(), false, true);
   }
   else if (tracker && !m_other_thread_holds_lock)
   {
      thread_id_t tid = m_thread->getId();

      // The lone check memory: first non-nop instruction after its branch, and how long ago it was
      if (m_lone.valid)
      {
         if (++m_lone.insns_since > LONE_WINDOW)
            m_lone.valid = false;
         else if (m_lone.first_real == 0 && !decoded(inst).is_nop())
            m_lone.first_real = inst.sinst->addr;
      }

      // Stores by this thread to the variable of the open wait or of the lone check
      if (inst.num_addresses > 0 && (m_sync_addr != 0 || m_lone.valid))
      {
         if (m_sync_addr != 0 && storesTo(inst, m_sync_addr))
         {
            if (!m_sync_branch_since_check)
            {
               // Read, then store with no branch in between: a read-modify-write (e.g. a ticketlock release), not a wait
               tracker->undoWait(tid, m_sync_addr, m_sync_check_pc, inst.sinst->addr, prfmdl->getElapsedTime());
               m_sync_addr = 0;
               m_sync_provisional = false;
            }
         }
         if (m_lone.valid && storesTo(inst, m_lone.var))
            m_lone.valid = false;
      }

      if (sync_check_addr != 0)
      {
         IntPtr var = sync_check_addr;
         IntPtr pc = inst.sinst->addr;
         IntPtr lo = 0, hi = 0;
         bool has_loop = getSpinLoop(var, pc, lo, hi);
         bool new_wait = false;

         if (m_sync_addr == var)
         {
            // Same wait: still in its loop, or on the way into it from a linked pre-check
            if (m_sync_provisional && has_loop)
            {
               // Settle a provisional wait now that this check's loop is known
               m_sync_provisional = false;
               if (isPreCheckOf(m_prov_lone, var, pc))
                  linkPreCheck(m_prov_lone, var, pc);
               else
               {
                  new_wait = true;
                  m_sync_passed = false;
                  m_sync_wait_reported = false;
               }
            }
         }
         else if (m_lone.valid && m_lone.var == var)
         {
            // Straight from a passed lone check of the same variable: its pre-check (same wait) or a new wait.
            // If that lone check only ended a provisional wait (iterations of a loop not learned yet), the check to
            // compare is still the provisional wait's original one, not this loop's own check.
            const LoneCheck &from = m_lone.chained ? m_prov_lone : m_lone;
            if (has_loop)
            {
               if (isPreCheckOf(from, var, pc))
                  linkPreCheck(from, var, pc);
               else
                  new_wait = true;
            }
            else
            {
               // This check's loop is not known yet: same wait for now, settled once it is learned
               if (!m_lone.chained)
                  m_prov_lone = m_lone;
               m_sync_provisional = true;
            }
            m_sync_addr = var;
            m_sync_passed = !new_wait;
            m_sync_loop_pc = 0;
            if (new_wait)
               m_sync_wait_reported = false;
         }
         else
         {
            new_wait = true;
            m_sync_addr = var;
            m_sync_passed = false;
            m_sync_provisional = false;
            m_sync_loop_pc = 0;
            m_sync_wait_reported = false;
         }
         m_lone.valid = false;

         m_sync_check_pc = pc;
         m_sync_branch_since_check = false;
         if (has_loop && (m_sync_loop_pc == 0 || new_wait))
            m_sync_loop_pc = pc;

         if (!m_sync_passed && !m_sync_provisional)
            m_sync_passed = tracker->tryPassSync(tid, var, pc, prfmdl->getElapsedTime(), core->getInstructionCount(), new_wait);

         // Lock pattern: the start of a wait is its first check at a spin load
         if (!m_sync_wait_reported && tracker->isSpinLoad(pc))
         {
            tracker->onWaitStart(tid, var, pc, prfmdl->getElapsedTime());
            m_sync_wait_reported = true;
         }
      }

      if (m_sync_addr != 0 && !tracker->isSyncVariable(m_sync_addr))
      {
         m_sync_addr = 0;   // the variable is a lock now
         m_sync_provisional = false;
      }
      if (m_lone.valid && !tracker->isSyncVariable(m_lone.var))
         m_lone.valid = false;

      if (m_sync_addr != 0 && inst.is_branch && m_sync_provisional)
      {
         // First branch after a provisional check: was the earlier lone check its pre-check? Compare the branches:
         // entry side: from the lone check's branch the trace went (past nops) to where this branch jumps back to;
         // exit side: the lone check's branch's other successor is a successor of this branch (where the loop exits)
         IntPtr pc = inst.sinst->addr;
         IntPtr fall = pc + inst.sinst->size;
         uint64_t target = 0;
         bool has_target = decoded(inst).branch_target(target);
         const LoneCheck &from = m_prov_lone;
         bool entry = has_target && (IntPtr)target <= m_sync_check_pc && from.first_real == (IntPtr)target;
         bool exit = from.other != 0 && (from.other == fall || (has_target && from.other == (IntPtr)target));
         m_sync_provisional = false;
         if (entry || exit)
            linkPreCheck(from, m_sync_addr, m_sync_check_pc);
         else
         {
            // Not a pre-check's loop: this check is a wait of its own, it takes its write now (or is held below)
            m_sync_passed = tracker->tryPassSync(m_thread->getId(), m_sync_addr, m_sync_check_pc,
                                                 prfmdl->getElapsedTime(), core->getInstructionCount(), true);
            m_sync_wait_reported = false;
            if (tracker->isSpinLoad(m_sync_check_pc))
            {
               tracker->onWaitStart(m_thread->getId(), m_sync_addr, m_sync_check_pc, prfmdl->getElapsedTime());
               m_sync_wait_reported = true;
            }
         }
      }

      if (m_sync_addr != 0 && inst.is_branch)
      {
         IntPtr var = m_sync_addr;
         IntPtr pc = inst.sinst->addr;
         IntPtr lo = 0, hi = 0;
         bool in_loop = (m_sync_loop_pc != 0) && getSpinLoop(var, m_sync_loop_pc, lo, hi);

         if (in_loop)
         {
            // In the range of the bounding loop: still waiting. After a pass, skip the rest of the recorded spin.
            if (m_sync_passed && next_inst.sinst->addr >= lo && next_inst.sinst->addr <= hi)
               cutShortSpin(inst, next_inst, var, lo, hi, prfmdl);

            IntPtr next_pc = next_inst.sinst->addr;
            if (next_pc < lo || next_pc > hi)
            {
               if (m_sync_passed)
                  m_sync_addr = 0;   // the wait is over
               else if (injectSpinLoop(inst, next_inst, var, m_sync_loop_pc) ||
                        injectFallback(inst, next_inst, var, m_sync_check_pc))
                  m_other_thread_holds_lock = true;   // held: one more iteration, then back to this branch
               else
                  tracker->reportStuck(var, tid, "STUCK: cannot hold the thread (no loop to replay)", 0, prfmdl->getElapsedTime());
            }
         }
         else
         {
            // No loop bounds this wait: a lone check (a pre-check, a one-off read) or a loop not learned yet
            IntPtr next_pc = next_inst.sinst->addr;
            PreCheckLink link;
            bool linked = false;
            {
               ScopedLock sl(s_spin_loops_lock);
               std::map<std::pair<IntPtr, IntPtr>, PreCheckLink>::iterator l = s_prechecks.find(std::make_pair(var, m_sync_check_pc));
               if (l != s_prechecks.end())
               {
                  link = l->second;
                  linked = true;
               }
            }

            if (next_pc <= m_sync_check_pc && tracker->isSpinLoad(m_sync_check_pc))
            {
               // A confirmed spin load whose loop is not learned yet, branching back to (or before) it: the native
               // trace is still spinning, the same wait goes on (its real iterations are what the loop is learned from)
            }
            else if (linked && next_pc == link.into)
            {
               // A pre-check going into its loop: the same wait goes on in the loop
               m_sync_loop_pc = link.loop_pc;
            }
            else if (m_sync_passed)
            {
               // The wait ends at this branch; the next check of the variable may still be its loop (pre-check)
               m_lone.var = var;
               m_lone.pc = m_sync_check_pc;
               m_lone.branch_pc = pc;
               m_lone.taken = next_pc;
               m_lone.other = otherSuccessor(inst, next_pc);
               m_lone.first_real = 0;
               m_lone.insns_since = 0;
               m_lone.valid = !linked;   // a linked pre-check going elsewhere has left the wait
               m_lone.chained = m_sync_provisional;
               m_sync_addr = 0;
               m_sync_provisional = false;
            }
            else if ((linked && injectSpinLoop(inst, next_inst, var, link.loop_pc)) ||
                     injectFallback(inst, next_inst, var, m_sync_check_pc))
            {
               m_other_thread_holds_lock = true;   // held at this check until the write commits
            }
            else
               tracker->reportStuck(var, tid, "STUCK: cannot hold the thread (no loop to replay)", 0, prfmdl->getElapsedTime());
         }
      }

      if (inst.is_branch)
         m_sync_branch_since_check = true;
   }
   // The wait on a lock ends when the trace leaves its spin loop (and is not kept in it by a replay)
   if (m_wait_base != 0 && inst.is_branch && !m_other_thread_holds_lock)
   {
      IntPtr lo = 0, hi = 0;
      getSpinLoop(m_wait_base, m_wait_check_pc, lo, hi);
      IntPtr pc = inst.sinst->addr;
      IntPtr next_pc = next_inst.sinst->addr;
      bool stays = (lo != 0)
         ? ((next_pc >= lo && next_pc <= hi) || (pc < next_pc && next_pc <= lo))
         : (next_pc <= m_wait_check_pc);
      if (!stays)
         m_wait_base = 0;
   }

   return false;
}

void TraceThread::m_run_func_default()
{
   // Set thread name for Sniper-in-Sniper simulations
   String threadName = String("trace-") + itostr(m_thread->getId());
   SimSetThreadName(threadName.c_str());

   // Pin this trace thread to a distinct host CPU to avoid contention
   pinTraceThreadToCPU(m_thread->getId());

   // Print which host CPU this thread is running on
   int host_cpu = sched_getcpu();
   std::cout << "[TraceThread] Thread " << m_thread->getId()
             << " (app " << m_app_id << ") started on host CPU " << host_cpu << std::endl;

   Sim()->getThreadManager()->onThreadStart(m_thread->getId(), m_time_start);

   // Open the trace (be sure to do this before potentially blocking on reschedule() as this causes deadlock)
   m_trace.initStream();
   m_trace_has_pa = m_trace.getTraceHasPhysicalAddresses();
   m_champsim_trace = m_trace.isChampSimTrace();

   if (m_thread->getCore() == NULL)
   {
      // We didn't get scheduled on startup, wait here
      SubsecondTime time = SubsecondTime::Zero();
      m_thread->reschedule(time, NULL);
   }

   Core *core = m_thread->getCore();
   PerformanceModel *prfmdl = core->getPerformanceModel();

   Sift::Instruction inst, next_inst;

   bool have_first = m_trace.Read(inst);

   while(have_first && readNextAppInstruction(next_inst))
   {
   
      if (!m_started)
      {
         // Received first instructions, let TraceManager know our SIFT connection is up and running
         // Only enable once we have received two instructions, otherwise, we could deadlock
         Sim()->getTraceManager()->signalStarted();
         m_started = true;
      }
      if (m_blocked)
      {
         unblock();
      }

         core = m_thread->getCore();
         LOG_ASSERT_ERROR(core, "We cannot execute instructions while not on a core");
         prfmdl = core->getPerformanceModel();

         bool   do_icache_warmup = false;
         UInt64 icache_warmup_addr = 0, icache_warmup_size = 0;

         // Reconstruct and count basic blocks

      if (m_bbv_end || m_bbv_last != inst.sinst->addr)
      {
         // We're the start of a new basic block
         core->countInstructions(m_bbv_base, m_bbv_count);
         // In cache-only mode, we'll want to do I-cache warmup
         if (m_bbv_base)
         {
            do_icache_warmup = true;
            icache_warmup_addr = m_bbv_base;
            icache_warmup_size = m_bbv_last - m_bbv_base;
         }
         // Set up new basic block info
         m_bbv_base = inst.sinst->addr;
         m_bbv_count = 0;
      }
      m_bbv_count++;
      m_bbv_last = inst.sinst->addr + inst.sinst->size;
      // Force BBV end on non-taken branches
      m_bbv_end = inst.is_branch;


      if (inst.is_champsim)
      {
         switch(Sim()->getInstrumentationMode())
         {
            case InstMode::FAST_FORWARD:
               break;

            case InstMode::CACHE_ONLY:
               handleChampSimWarmup(inst, next_inst, core);
               break;

            case InstMode::DETAILED:
               handleChampSimDetailed(inst, next_inst, prfmdl);
               break;

            default:
               LOG_PRINT_ERROR("Unknown instrumentation mode");
         }
      }
      else
      {
         switch(Sim()->getInstrumentationMode())
         {
            case InstMode::FAST_FORWARD:
               break;

            case InstMode::CACHE_ONLY:
               handleInstructionWarmup(inst, next_inst, core, do_icache_warmup, icache_warmup_addr, icache_warmup_size);
               break;

            case InstMode::DETAILED:
               if (!checkAndReplaySpinLoop(inst, next_inst, core, prfmdl))
                  handleInstructionDetailed(inst, next_inst, prfmdl);
               break;

            default:
               LOG_PRINT_ERROR("Unknown instrumentation mode");
         }
      }


      // We may have been rescheduled to a different core
      // by prfmdl->iterate (in handleInstructionDetailed),
      // or core->countInstructions (when using a fast-forward performance model)
      SubsecondTime time = prfmdl->getElapsedTime();
      if (m_thread->reschedule(time, core))
      {
         core = m_thread->getCore();
         prfmdl = core->getPerformanceModel();
      }


      if (m_stop)
         break;

      inst = next_inst;
   }

   printf("[TRACE:%u] -- %s --\n", m_thread->getId(), m_stop ? "STOP" : "DONE");
   printf("[REPLAY SUMMARY] Thread %u: %lu spin iterations injected (%lu insns), %lu recorded spin insns skipped (%lu sync waits cut short)\n",
          m_thread->getId(), (unsigned long)m_replays, (unsigned long)m_injected_insns,
          (unsigned long)m_skipped_insns, (unsigned long)m_sync_skips);
   
   // Clean up ChampSim instruction cache to avoid leaks (destructor is never called)
   if (m_champsim_trace) {
      cleanupChampSimCache();
   }

   SubsecondTime time_end = prfmdl->getElapsedTime();

   writeDIPs();

   Sim()->getThreadManager()->onThreadExit(m_thread->getId());
   Sim()->getTraceManager()->signalDone(this, time_end, m_stop /*aborted*/);
}

void TraceThread::spawn()
{
   m__thread = _Thread::create(this);
   m__thread->run();
}

UInt64 TraceThread::getProgressExpect()
{
   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   return reader->getLength();
}

UInt64 TraceThread::getProgressValue()
{
   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   return reader->getPosition();
}

void TraceThread::frontEndStop(){
   // Guard against being called before thread is fully initialized or after it's stopped
   if (m_stopped)
   {
      return;
   }

   if (m_current_sift_reader != NULL)
   {
      m_current_sift_reader->frontEndStop();
   }
   else
   {
      m_trace.frontEndStop();
   }
}

void TraceThread::handleAccessMemory(Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char *data_buffer, UInt32 data_size)
{
   Sift::MemoryLockType sift_lock_signal;
   Sift::MemoryOpType sift_mem_op;

   switch (lock_signal)
   {
   case (Core::NONE):
      sift_lock_signal = Sift::MemNoLock;
      break;
   case (Core::LOCK):
      sift_lock_signal = Sift::MemLock;
      break;
   case (Core::UNLOCK):
      sift_lock_signal = Sift::MemUnlock;
      break;
   default:
      sift_lock_signal = Sift::MemInvalidLock;
      break;
   }

   switch (mem_op_type)
   {
   case (Core::READ):
   case (Core::READ_EX):
      sift_mem_op = Sift::MemRead;
      break;
   case (Core::WRITE):
      sift_mem_op = Sift::MemWrite;
      break;
   default:
      sift_mem_op = Sift::MemInvalidOp;
      break;
   }
   
   Sift::Reader *reader = m_current_sift_reader ? m_current_sift_reader : &m_trace;
   reader->AccessMemory(sift_lock_signal, sift_mem_op, d_addr, (uint8_t*)data_buffer, data_size);
}

void TraceThread::writeDIPs()
{
   for (auto &p : m_icache)
   {
      if (p.second)
      {
         Sim()->getStatsManager()->recordPicsD(p.second->getVaddress(), p.second->getName(), p.second->getPICS_d());
         Sim()->getStatsManager()->recordPicsC(p.second->getVaddress(), p.second->getName(), p.second->getPICS_c());
      }
   }
}
