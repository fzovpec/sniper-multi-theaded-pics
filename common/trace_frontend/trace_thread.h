#ifndef __TRACE_THREAD_H
#define __TRACE_THREAD_H

#include "fixed_types.h"
#include "_thread.h"
#include "thread.h"
#include "core.h"
#include "sift_reader.h"
#include "operand.h"
#include "sniper_semaphore.h"
#include "lock.h"

#include <decoder.h>

#include <unordered_map>
#include <unordered_set>
#include <map>
#include <deque>

#define NUM_PAPI_COUNTERS 6

#define PAPI_TOT_INS 0
#define PAPI_TOT_CYC 1
#define PAPI_L1_DCM 2
#define PAPI_L2_DCM 3
#define PAPI_L3_TCM 4
#define PAPI_BR_MSP 5

class Instruction;
class DynamicInstruction;

class TraceThread : public Runnable
{
   private:
      // In multi-process mode, we want each process to have its own private memory space
      // Therefore, perform a virtual to physical address mapping by including the core_id
      // Virtual addresses are converted to physical addresses by pasting the core_id at
      // bit positions pa_core_shift..+pa_core_size
      // The highest virtual address used is normally 00007fffffffffff, with pa_core_shift==48
      // the core_id is 2 above the highest used bit.
      static const UInt64 pa_core_shift = 48;
      static const UInt64 pa_core_size = 16;
      static const UInt64 pa_va_mask = ~(((UInt64(1) << pa_core_size) - 1) << pa_core_shift);
      // Optionally we can also do address randomization on a per-page basis.
      // This can avoid artificial set contention when replaying multiple copies of the same trace.
      static const UInt64 va_page_shift = 12;
      static const UInt64 va_page_mask = (UInt64(1) << va_page_shift) - 1;

      static UInt64 _va2pa(UInt64 self, UInt64 va) { return ((TraceThread*)self)->va2pa(va); }
      UInt64 va2pa(UInt64 va, bool *noMapping = NULL);
      UInt64 remapAddress(UInt64 va_page);

      _Thread *m__thread;
      Thread *m_thread;
      SubsecondTime m_time_start;

      Sift::Reader m_trace;
      Sift::Reader *m_kernel_trace;          // SIFT reader for the kernel trace, if any
      Sift::Reader *m_app_trace;             // SIFT reader for the application trace, if any
      Sift::Reader *m_current_sift_reader;   // Currently active SIFT reader

      bool is_in_kernel_mode;

      bool m_trace_has_pa;
      bool m_champsim_trace;
      UInt32 m_champsim_access_size;
      bool m_address_randomization;
      bool m_appid_from_coreid;
      uint8_t m_address_randomization_table[256];
      bool m_stop;
      std::unordered_map<IntPtr, Instruction *> m_icache;
      std::unordered_map<IntPtr, const dl::DecodedInst *> m_decoder_cache;
      
      // ChampSim instruction cache: key is (PC, is_branch, num_src_regs, num_dest_regs, num_loads, num_stores)
      // This allows reusing instructions when the same PC has the same operand signature
      struct ChampSimCacheKey {
         IntPtr pc;
         uint8_t is_branch;
         uint8_t num_src_regs;
         uint8_t num_dest_regs;
         uint8_t num_loads;
         uint8_t num_stores;
         
         bool operator==(const ChampSimCacheKey& other) const {
            return pc == other.pc && is_branch == other.is_branch &&
                   num_src_regs == other.num_src_regs && num_dest_regs == other.num_dest_regs &&
                   num_loads == other.num_loads && num_stores == other.num_stores;
         }
      };
      struct ChampSimCacheKeyHash {
         size_t operator()(const ChampSimCacheKey& k) const {
            // Combine all fields into a hash
            return std::hash<IntPtr>()(k.pc) ^ 
                   (std::hash<uint8_t>()(k.is_branch) << 1) ^
                   (std::hash<uint8_t>()(k.num_src_regs) << 2) ^
                   (std::hash<uint8_t>()(k.num_dest_regs) << 3) ^
                   (std::hash<uint8_t>()(k.num_loads) << 4) ^
                   (std::hash<uint8_t>()(k.num_stores) << 5);
         }
      };
      std::unordered_map<ChampSimCacheKey, Instruction*, ChampSimCacheKeyHash> m_champsim_icache;
      
      UInt64 m_bbv_base;
      UInt64 m_bbv_count;
      UInt64 m_bbv_last;
      bool m_bbv_end;
      static int m_isa;
      uint8_t m_output_leftover[160];
      uint16_t m_output_leftover_size;
      String m_tracefile;
      String m_responsefile;

      String m_tracefile_kernel;
      String m_responsefile_kernel;
      
      app_id_t m_app_id;
      bool m_blocked;
      bool m_cleanup;
      bool m_started;

   public:
      bool m_stopped;
   private:

      // ChampSim instruction cache statistics
      UInt64 m_champsim_icache_hits;
      UInt64 m_champsim_icache_misses;

      int fd_read;
      int fd_write;

      // INVARIANT: to_be_replayed_inst.sinst != NULL iff thread->getCore()->getMemoryManager()->is_page_fault = True
      Sift::Instruction  to_be_replayed_inst;      // This is the instruction that is currently being replayed, if any
      Sift::Instruction  to_be_replayed_next_inst; 

      struct statistics
      {
         SubsecondTime kernel_time;
      } stats;

      // Spin-loop detection state
      bool m_is_spinning;
      bool m_other_thread_holds_lock;
      // Last spin-loop load of a lock variable that was not followed by a branch yet (0 if none)
      IntPtr m_claim_load_pc;
      IntPtr m_claim_addr;
      // Last check of a tracked variable
      IntPtr m_check_pc;
      IntPtr m_check_base;
      // Current wait on any tracked variable (lock or sync): from its first check until the trace leaves the spin loop (0 if none).
      // Its start, at the first check at a spin load, is reported to the Lock State Tracker (lock pattern of the thread).
      IntPtr m_wait_base;
      IntPtr m_wait_check_pc;
      bool m_wait_reported;
      // Current wait on a sync variable (base address, 0 if none). One wait uses one write by another thread.
      // It lasts while the trace stays in the range of the spin loop that bounds it (m_sync_loop_pc), or, for a check with
      // no known loop, until the branch after the check. Whether a check opens a new wait is decided at the check.
      IntPtr m_sync_addr;
      IntPtr m_sync_check_pc;       // PC of the last check of the variable in this wait
      IntPtr m_sync_loop_pc;        // load PC of the stored spin loop that bounds this wait (0: none known)
      bool m_sync_passed;           // a write by another thread was there for this wait
      bool m_sync_provisional;      // reached a check with no stored loop straight from a passed lone check: same wait for
                                    // now, settled when that check's loop is learned (m_prov_lone)
      bool m_sync_branch_since_check;   // a branch was executed since the last check (read-modify-write rule)
      bool m_sync_wait_reported;    // the start of this wait was reported to the tracker (lock pattern)
      // Last lone check of a sync variable (no known loop at its PC) whose wait passed and ended at its branch: if the next
      // check of the variable comes soon, with no store to it in between, it may be the same wait (pre-check of a loop)
      struct LoneCheck
      {
         IntPtr var;
         IntPtr pc;           // the check
         IntPtr branch_pc;    // the branch after it
         IntPtr taken;        // where the trace went from that branch
         IntPtr other;        // the branch's other successor (0 if unknown)
         IntPtr first_real;   // first instruction after the branch that is not a nop (0 until seen)
         UInt64 insns_since;
         bool valid;
         bool chained;        // it ended a provisional wait: that wait's original lone check (m_prov_lone) still applies
         LoneCheck() : var(0), pc(0), branch_pc(0), taken(0), other(0), first_real(0), insns_since(0), valid(false), chained(false) {}
      };
      static const UInt64 LONE_WINDOW = 512;   // instructions after a lone check within which a check can be its loop
      LoneCheck m_lone;
      LoneCheck m_prov_lone;   // the lone check a provisional wait came from
      // Path of this thread since its last check of a tracked variable (to learn the spin loop at that check)
      static const size_t MAX_PATH = 8192;
      IntPtr m_path_var;
      IntPtr m_path_pc;
      std::vector<Sift::Instruction> m_path;
      bool m_path_overflow;
      // Earlier waits of this thread, counted here in the frontend (where waits are decided; it never runs a wrong path)
      // from its reads of any address: a visit is a run of reads of an address with no store by the thread in between;
      // a read followed by a store to the same address with no branch in between is a read-modify-write, not a visit.
      // Per load PC (memory bounded by the code size): the address it last read and the visits to it.
      struct PcVisits
      {
         IntPtr addr;
         UInt64 visits;
         PcVisits() : addr(0), visits(0) {}
      };
      std::unordered_map<IntPtr, PcVisits> m_pc_visits;
      std::unordered_map<IntPtr, IntPtr> m_visit_addrs;     // addresses read since the last store (visits in progress) -> load PC
      std::unordered_set<IntPtr> m_reads_since_branch;      // addresses read since the last branch
      std::unordered_set<IntPtr> m_provided_vars;           // tracked variables whose earlier waits were handed to the tracker
      void countVisits(Sift::Instruction &inst);
      // Visits to addr completed so far (a visit still in progress is the current wait, not an earlier one)
      UInt64 frontendEarlierWaits(IntPtr addr) const;
      // Instructions still to come that a replay injected (not the held branch itself): they only re-check the
      // variable, the wait's boundaries are decided at the held branch when the replay comes back to it
      UInt64 m_synthetic_left;
      bool m_path_clean;        // no injected instruction pending when the path started
      UInt64 m_path_replays;    // m_replays when the path started: a replay during the path makes it unusable
      // Totals printed when the thread ends
      UInt64 m_replays;          // spin loop iterations injected
      UInt64 m_injected_insns;   // instructions injected by those iterations
      UInt64 m_skipped_insns;    // recorded spin instructions not simulated
      UInt64 m_sync_skips;       // recorded spins cut short after a sync wait passed
      std::deque<Sift::Instruction> m_injected_trace;
      std::unordered_map<IntPtr, Sift::Instruction> m_sift_loop_insts;
      std::deque<IntPtr> m_sift_loop_order;

      // Spin loop of a lock or sync variable at one load PC (a site), shared by all threads. A variable can be waited on
      // in several loops (e.g. one per inlined barrier), so the key is (variable, load PC). Learned from the path a
      // thread took from the load back to the same load during a spin reported by the spin loop detector:
      // [lo, hi] = PC range of that path, exits = successors of its branches outside the range,
      // insts = that iteration starting at the load (static instructions are copies owned by the store).
      struct SpinLoop
      {
         IntPtr lo;
         IntPtr hi;
         std::vector<IntPtr> exits;
         std::vector<Sift::Instruction> insts;
      };
      static Lock s_spin_loops_lock;
      static std::map<std::pair<IntPtr, IntPtr>, SpinLoop> s_spin_loops;   // (variable base address, load PC) -> spin loop
      static std::map<IntPtr, IntPtr> s_last_loop;                         // variable -> load PC of its last stored loop
      // Pre-check of a loop: (variable, PC of the lone check) -> load PC of the loop and the successor of the pre-check's
      // branch that goes into it
      struct PreCheckLink
      {
         IntPtr loop_pc;
         IntPtr into;
      };
      static std::map<std::pair<IntPtr, IntPtr>, PreCheckLink> s_prechecks;

      // PC range of the stored spin loop of the variable at load PC load_pc, false if none is stored
      bool getSpinLoop(IntPtr var_base, IntPtr load_pc, IntPtr &lo, IntPtr &hi);
      bool getSpinLoopInfo(IntPtr var_base, IntPtr load_pc, IntPtr &lo, IntPtr &hi, std::vector<IntPtr> &exits);
      // Store m_path (the path from load_pc back to it) as the spin loop of the variable at load_pc, if none is stored yet
      void learnSpinLoopFromPath(IntPtr var_base, IntPtr load_pc);
      // Make the branch inst go into the stored spin loop of the variable at load_pc and inject one iteration of it,
      // then come back to the trace at next_inst. Returns false if no loop is stored for them.
      bool injectSpinLoop(Sift::Instruction &inst, Sift::Instruction &next_inst, IntPtr var_base, IntPtr load_pc);
      // Replay for a thread held at branch inst with no loop of its own: the variable's last stored loop, else what this
      // thread executed from the check up to this branch. False if neither is possible.
      bool injectFallback(Sift::Instruction &inst, Sift::Instruction &next_inst, IntPtr var_base, IntPtr check_pc);
      // After a pass: skip the rest of the recorded spin in [lo, hi]; the trace resumes at the last time it was at
      // next_inst's PC before leaving the range
      void cutShortSpin(Sift::Instruction &inst, Sift::Instruction &next_inst, IntPtr var_base, IntPtr lo, IntPtr hi, PerformanceModel *prfmdl);
      // Is lone check 'lone' the pre-check of the loop at (var_base, loop_pc)? (its branch went into the loop: the first
      // non-nop instruction after it is in the loop's range, or its other successor is an exit of the loop)
      bool isPreCheckOf(const LoneCheck &lone, IntPtr var_base, IntPtr loop_pc);
      void linkPreCheck(const LoneCheck &lone, IntPtr var_base, IntPtr loop_pc);
      // Decoding helpers
      const dl::DecodedInst &decoded(Sift::Instruction &inst);
      bool storesTo(Sift::Instruction &inst, IntPtr var_base);
      IntPtr otherSuccessor(Sift::Instruction &inst, IntPtr taken_pc);

      bool readNextAppInstruction(Sift::Instruction &inst_out);
      // Returns true if inst is a spin loop instruction that must be skipped (not simulated)
      bool checkAndReplaySpinLoop(Sift::Instruction &inst, Sift::Instruction &next_inst, Core* &core, PerformanceModel* &prfmdl);

      // Make run() a function pointer which is initialized in the constructor
      typedef void (TraceThread::*RunFunc)();
      RunFunc m_current_run_func;
      
      void run(){
         (this->*m_current_run_func)();
      }

      void m_run_func_default();
      void m_run_func_with_userpace_mimicos();

      Sift::Mode handleInstructionCountFunc(uint32_t icount);
      void handleCacheOnlyFunc(uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address);
      void handleOutputFunc(uint8_t fd, const uint8_t *data, uint32_t size);
      uint64_t handleSyscallFunc(uint16_t syscall_number, const uint8_t *data, uint32_t size);
      int32_t handleNewThreadFunc();
      int32_t handleForkFunc();
      int32_t handleJoinFunc(int32_t thread);
      uint64_t handleMagicFunc(uint64_t a, uint64_t b, uint64_t c);
      bool handleEmuFunc(Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res);
      void handleRoutineChangeFunc(Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip);
      void handleRoutineAnnounceFunc(uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename);



      Instruction* decode(Sift::Instruction &inst);
      Instruction* decodeChampsim(Sift::Instruction &inst);
      void handleInstructionWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core, bool do_icache_warmup, UInt64 icache_warmup_addr, UInt64 icache_warmup_size);
      void handleChampSimWarmup(Sift::Instruction &inst, Sift::Instruction &next_inst, Core *core);
      void handleInstructionDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl);
      void handleChampSimDetailed(Sift::Instruction &inst, Sift::Instruction &next_inst, PerformanceModel *prfmdl);
      void addDetailedMemoryInfo(DynamicInstruction *dynins, Sift::Instruction &inst, const dl::DecodedInst &decoded_inst, uint32_t mem_idx, Operand::Direction op_type, bool is_pretetch, PerformanceModel *prfmdl);
      void addChampSimMemoryInfo(DynamicInstruction *dynins, UInt64 mem_address, Operand::Direction op_type, bool executed, bool is_prefetch, PerformanceModel *prfmdl);
      void unblock();

      SubsecondTime getCurrentTime() const;
      

      dl::DecoderFactory *m_factory;  // we need a factory here to be able to create instructions of any kind
      const dl::DecodedInst* staticDecode(Sift::Instruction &inst);

      long long *m_papi_counters;
      bool m_virtuos_app;
      
      Lock m_lock;

   public:

         static Sift::Mode __handleInstructionCountFunc(void* arg, uint32_t icount)
      { return ((TraceThread*)arg)->handleInstructionCountFunc(icount); }
      static void __handleCacheOnlyFunc(void* arg, uint8_t icount, Sift::CacheOnlyType type, uint64_t eip, uint64_t address)
      { ((TraceThread*)arg)->handleCacheOnlyFunc(icount, type, eip, address); }
      static void __handleOutputFunc(void* arg, uint8_t fd, const uint8_t *data, uint32_t size)
      { ((TraceThread*)arg)->handleOutputFunc(fd, data, size); }
      static uint64_t __handleSyscallFunc(void* arg, uint16_t syscall_number, const uint8_t *data, uint32_t size)
      { return ((TraceThread*)arg)->handleSyscallFunc(syscall_number, data, size); }
      static int32_t __handleNewThreadFunc(void* arg)
      { return ((TraceThread*)arg)->handleNewThreadFunc(); }
      static int32_t __handleJoinFunc(void* arg, int32_t join_thread_id)
      { return ((TraceThread*)arg)->handleJoinFunc(join_thread_id); }
      static uint64_t __handleMagicFunc(void* arg, uint64_t a, uint64_t b, uint64_t c)
      { return ((TraceThread*)arg)->handleMagicFunc(a, b, c); }
      static bool __handleEmuFunc(void* arg, Sift::EmuType type, Sift::EmuRequest &req, Sift::EmuReply &res)
      { return ((TraceThread*)arg)->handleEmuFunc(type, req, res); }
      static void __handleRoutineChangeFunc(void* arg, Sift::RoutineOpType event, uint64_t eip, uint64_t esp, uint64_t callEip)
      { ((TraceThread*)arg)->handleRoutineChangeFunc(event, eip, esp, callEip); }
      static void __handleRoutineAnnounceFunc(void* arg, uint64_t eip, const char *name, const char *imgname, uint64_t offset, uint32_t line, uint32_t column, const char *filename)
      { ((TraceThread*)arg)->handleRoutineAnnounceFunc(eip, name, imgname, offset, line, column, filename); }
      static int32_t __handleForkFunc(void* arg)
      { return ((TraceThread*)arg)->handleForkFunc();}

      TraceThread(Thread *thread, SubsecondTime time_start, String tracefile, String responsefile, app_id_t app_id, bool cleanup);
      ~TraceThread();

      void spawn();
      void stop() { m_stop = true; }
      void cleanupChampSimCache();  // Clean up ChampSim instruction cache to avoid leaks
      UInt64 getProgressExpect();
      UInt64 getProgressValue();
      void frontEndStop(); //Ask all trace_threads to send signal to front-end to shutdown
      void writeDIPs();

      Thread* getThread() const { return m_thread; }
      bool getVirtuosApp() { return m_virtuos_app; }
     
      void handleAccessMemory(Core::lock_signal_t lock_signal, Core::mem_op_t mem_op_type, IntPtr d_addr, char* data_buffer, UInt32 data_size);
      
      Sift::Reader* getSiftReader() { return &m_trace; }
      
      Sift::Reader* getKernelSiftReader() { return m_kernel_trace; }
      void setKernelSIFTRreader(Sift::Reader *reader) { m_kernel_trace = reader; }
         
      Sift::Reader* getAppSiftReader() { return m_app_trace; }
      void setAppSiftReader(Sift::Reader *reader) { m_app_trace = reader; }


      Sift::Reader* getCurrentSiftReader() { return m_current_sift_reader; }
      void setCurrentSiftReader(Sift::Reader *reader) { m_current_sift_reader = reader; }
};

#endif // __TRACE_THREAD_H
