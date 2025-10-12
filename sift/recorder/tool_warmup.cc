//#include "tool_warmup.h"
#include "threads.h"
#include "globals.h"
#include <algorithm>

#include <iostream>
#include <iomanip>

#define ADD_FORCE_READ(addr)          ((addr) | (1ULL << 0))
#define ADD_FORCE_WRITE(addr)         ((addr) | (1ULL << 1))
#define ADD_FORCE_NOP(addr)           ((addr) | (1ULL << 2))
#define ADD_FORCE_ICACHE(addr)        ((addr) | (1ULL << 3))
#define ADD_FORCE_BRANCH(addr, taken) ((addr) | (1ULL << 4) | (static_cast<unsigned long long>(taken & 0x1) << 5))

ToolWarmup::ToolWarmup(Sift::Writer *_output, int id, bool global_sync)
   : m_output(_output)
   , m_warmup(NULL)
   , m_mem_count(0)
   , m_counter(0)
   , m_id(id)
   , m_global_sync(global_sync)
   , m_starttime(std::time(0))
{
      std::string arch_name = KnobArch.Value();
      std::cout << "WarmUp using arch " << arch_name << std::endl;
      if( arch_name == "gainestown" ) {
        CACHE_SHARED_BY_CORES=4;
        CACHE_SIZE_BYTES = 8 * 1024 * 1024 ;
        CACHE_ASSOCIATIVITY = 16;

      } else if (arch_name == "skylake") {
        CACHE_SHARED_BY_CORES=16;
        CACHE_SIZE_BYTES = 22 * 1024 * 1024 ;
        CACHE_ASSOCIATIVITY = 11;
      } else if (arch_name == "sunnycove") {
        CACHE_SHARED_BY_CORES=8;
        CACHE_SIZE_BYTES = 16 * 1024 * 1024 ;
        CACHE_ASSOCIATIVITY = 16;
      } else {
        printf( "%s not supported\n", arch_name.c_str());
        sift_assert(false);
      }
   treelist = TreeList<uint64_t,std::pair<uint64_t,uint64_t>>( NUM_LINES_IN_CACHE );
   if (m_output != NULL)
   {
      initSiftWarmup();
   }
   PIN_InitLock(&m_addrlock);
}

ToolWarmup::~ToolWarmup()
{
   if (m_id == 0)
   {
      m_time_record.push_back(std::make_pair(std::time(0), "end"));
      for (auto x = m_time_record.begin() ; x != m_time_record.end() ; ++x)
      {
         std::cout << "[TOOL_WARMUP] T: " << ((*x).first - m_starttime) << " M: " << (*x).second << std::endl;
      }
   }
}

void ToolWarmup::setSiftOutput(Sift::Writer *_output)
{
   assert(m_output == NULL);
   m_output = _output;
}

void ToolWarmup::initSiftWarmup()
{
   m_warmup = new Sift::Warmup("", 0, m_output);
}

void ToolWarmup::memoryCallback(uint64_t address, bool is_read_not_write)
{
   if (s_debug >= 1)
   {
      m_mem_count++;
      if (m_mem_count % 100000 == 0)
      {
         std::cout << "[" << m_id << ":W]"; /*warmup heartbeat*/
      }
   }

   // Keep track of the times for our memory addresses
   address = address & CACHE_LINE_MASK;
   uint64_t time;
   if (m_global_sync)
     time = atomic_inc_return(&s_counter);
   else
     time = ++m_counter;

   if (m_global_sync)
      PIN_GetLock(&m_addrlock, m_id);
   if (is_read_not_write)
//      treelist.update(address,time,0);
      (m_addr2time[address]).first = time;
   else
  //    treelist.update(address,0,time);
      (m_addr2time[address]).second = time;

   if (m_global_sync)
      PIN_ReleaseLock(&m_addrlock);
}

void ToolWarmup::icacheCallback(uint64_t address)
{
   // Keep track of the times for our memory addresses
   address = address & CACHE_LINE_MASK;
   uint64_t time;
   if (m_global_sync)
     time = atomic_inc_return(&s_counter);
   else
     time = ++m_counter;

   if (m_global_sync)
      PIN_GetLock(&m_addrlock, m_id);

   m_insn2time[address] = time;

   if (m_global_sync)
      PIN_ReleaseLock(&m_addrlock);
}

void ToolWarmup::NewThread(bool send_immediately)
{
   uint64_t time = ++m_counter;
   if (!send_immediately)
      m_newthreadtime.push_back(time);
   else
      m_warmup->NewThread();
}

void ToolWarmup::clearTimeStamp() { 
   if (m_addr2time.size()>100000000) {
       m_addr2time.clear();
   }
}
void ToolWarmup::writeWarmupInstructions()
{
   if (m_output == NULL)
   {
      std::cerr << "[TOOL_WARMUP:" << m_id << "] ERROR: Cannot writeWarmupInstructions() without a valid output target." << std::endl;
   }

   m_time_record.push_back(std::make_pair(std::time(0), "begin writeWarmupInstructions"));

   // Populate our vector with accesses
   std::vector<Time3Addr> maxtimevec;
//   if (m_addr2time.size()==0) {
//        return;
//   }
   for (auto i = m_addr2time.begin() ; i != m_addr2time.end() ; ++i)
//   for( auto x : treelist.elemlist )
   {
      auto &x = *i;
      auto addr = x.first;
      auto time_read = (x.second).first;
      auto time_written = (x.second).second;
      maxtimevec.push_back(Time3Addr(time_read,time_written,0,addr));
   }
//   treelist.clear();
//   m_addr2time.clear();
//   clearTimeStamp();
   // Populate our vector with icache accesses
   // This is a first-order approximation, as it might be better to warmup the cache hierarchy on a per-level basis
   int count = 0;
   for (auto i = m_insn2time.begin() ; i != m_insn2time.end() ; ++i)
   {
      auto addr = i->first;
      auto time_icache = i->second;
      maxtimevec.push_back(Time3Addr(0,0,time_icache,addr));
      if (s_debug >= 2)
         ++count;
   }
   if (s_debug >= 2)
      std::cerr << "[TOOL_WARMUP:" << m_id << "] Added " << count << " instruction accesses" << std::endl;

   // Sort the access times
   std::sort(maxtimevec.begin(), maxtimevec.end());

   // Get a number of accesses, equal to the number of lines in the cache
   int num_lines = std::min<uint64_t>(NUM_LINES_IN_CACHE, maxtimevec.size());
   int start = maxtimevec.size() - num_lines;

   // Extract each item as individual accesses
   int count2 = 0;
   std::vector<TimeAddr> time2addr;
   std::vector<Time3Addr>::iterator x = maxtimevec.begin();
   for (std::advance(x, start) ; x != maxtimevec.end() ; ++x)
   {
      uint64_t addr = x->m_addr;
      uint64_t time_read = x->m_time[0];
      uint64_t time_written = x->m_time[1];
      uint64_t time_icache = x->m_time[2];
      if (s_debug >= 2)
         std::cout << "[TOOL_WARMUP] @ " << time_read << "," << time_written << " 0x" << std::hex << addr << std::dec << std::endl;

      // Use the lower bits of the address as a read/write indicator
      if (time_read)
      {
         auto force_read_addr = ADD_FORCE_READ(addr);
         time2addr.push_back(TimeAddr(time_read, force_read_addr));
      }
      if (time_written)
      {
         auto force_write_addr = ADD_FORCE_WRITE(addr);
         time2addr.push_back(TimeAddr(time_written, force_write_addr));
      }
      if (time_icache)
      {
         auto force_icache_addr = ADD_FORCE_ICACHE(addr);
         time2addr.push_back(TimeAddr(time_icache, force_icache_addr));
         if (s_debug >= 2)
           count2++;
      }
   }
   if (s_debug >= 2)
      std::cerr << "[TOOL_WARMUP:" << m_id << "] Forced " << count2 << " instruction accesses" << std::endl;

   // Sort our vector
   std::sort(time2addr.begin(), time2addr.end());

   // Equalize, as the maximum number of values can be 2 * NUM_LINES_IN_CACHE (for both reads and writes)
   num_lines = std::min<uint64_t>(2*NUM_LINES_IN_CACHE, time2addr.size());
   start = time2addr.size() - num_lines;
   int dummy_addresses = (2*NUM_LINES_IN_CACHE) - num_lines;

   if (s_debug >= 1)
      std::cout << "[TOOL_WARMUP:" << m_id << "] num_lines=" << num_lines << " start=" << start << " dummy=" << dummy_addresses << std::endl;

   // Open all of our newthread pipes
   for (unsigned int i = 0 ; i < m_newthreadtime.size() ; i++)
   {
      m_warmup->NewThread();
   }
   m_newthreadtime.clear();


   m_time_record.push_back(std::make_pair(std::time(0), "begin address replay"));

   // Write out dummy data to keep all threads aligned during playback
   uint64_t dummy_addr = ADD_FORCE_NOP(CACHE_LINE_SIZE_BYTES * m_id);
   for (int i = 0 ; i < dummy_addresses ; i++)
   {
      // TODO Possibly use another bit to mark these as a nop
      if( !permit_to_write_warmup_ins ) {
          break;
      }
          m_warmup->MemoryAccess(dummy_addr);
   }

   // Write out the addresses from the vector
   std::vector<TimeAddr>::iterator y = time2addr.begin();
   for (std::advance(y, start) ; y != time2addr.end() ; ++y)
   {
      uint64_t time = y->m_time;
      uint64_t addr = y->m_addr;
      if (s_debug >= 2) {
         std::cout << "[TOOL_WARMUP] @ " << time << " 0x" << std::hex << addr << std::dec << std::endl;
      }
      if(!permit_to_write_warmup_ins) {
        break;
      }
          m_warmup->MemoryAccess(addr);
   }

   m_time_record.push_back(std::make_pair(std::time(0), "end address replay"));


   // delete m_warmup;
   // m_warmup = NULL;

   m_time_record.push_back(std::make_pair(std::time(0), "end writeWarmupInstructions"));
}


ToolWarmup::atomic_t __attribute__((aligned(0x40))) ToolWarmup::s_counter = {0,{0}};

// FIXME Not yet compatible with pinplay
void PinToolWarmup::getCode(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   // Check to see if the code address can be handled by the warmup tool. If not, the address range must come from the native application
   if (!ToolWarmup::copyData(dst, src, size))
   {
     PIN_SafeCopy(dst, src, size);
   }
}

VOID PinToolWarmup::handleMemoryWarmup(THREADID threadid, ADDRINT address, BOOL is_read_not_write, ADDRINT ptr_PinToolWarmup)
{
    PinToolWarmup *ptw = reinterpret_cast<PinToolWarmup*>(ptr_PinToolWarmup);

    if( ptw->if_mtr_sampling) {
    if (threadid >= ptw->m_num_threads) {
        std::cerr << "[PINTOOLWARMUP:" << threadid << "] Error, invalid threadid " << threadid << "\n";
        return;
    }
    ptw->m_tool_warmup[threadid]->memoryCallback(address, is_read_not_write);
    }
}

UINT32 PinToolWarmup::addMemoryModeling(INS ins, PinToolWarmup *ptw)
{
//   if (current_mode == Sift::ModeDetailed) {
//       return 0;
//   }

   UINT32 num_addresses = 0;
   AFUNPTR mem_fun;
   mem_fun = AFUNPTR(PinToolWarmup::handleMemoryWarmup);

   if (INS_IsMemoryRead (ins) || INS_IsMemoryWrite (ins))
   {
      for (unsigned int i = 0; i < INS_MemoryOperandCount(ins); i++)
      {
         if (INS_MemoryOperandIsRead(ins, i))
         {
            INS_InsertCall(ins, IPOINT_BEFORE,
                  mem_fun,
                  IARG_THREAD_ID,
                  IARG_MEMORYOP_EA, i,
                  IARG_BOOL, true /*is_read_not_write*/,
                  IARG_ADDRINT, ptw,
                  IARG_END);
            num_addresses++;
         }
         if (INS_MemoryOperandIsWritten(ins, i))
         {
            INS_InsertCall(ins, IPOINT_BEFORE,
                  mem_fun,
                  IARG_THREAD_ID,
                  IARG_MEMORYOP_EA, i,
                  IARG_BOOL, false /*is_read_not_write*/,
                  IARG_ADDRINT, ptw,
                  IARG_END);
            num_addresses++;
         }
         if (num_addresses >= Sift::MAX_DYNAMIC_ADDRESSES)
         {
            break;
         }
      }
   }
   assert(num_addresses <= Sift::MAX_DYNAMIC_ADDRESSES);

   return num_addresses;

}

VOID PinToolWarmup::traceCallback(TRACE trace, void *v)
{
   
   BBL bbl_head = TRACE_BblHead(trace);
   
   for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
   {
      for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
      {
         addMemoryModeling(ins, static_cast<PinToolWarmup*>(v));

         if (ins == BBL_InsTail(bbl))
            break;
      }
   }
}

VOID PinToolWarmup::threadStart(THREADID threadIndex, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    PinToolWarmup *ptw = reinterpret_cast<PinToolWarmup*>(v);
    PIN_GetLock(&ptw->m_lock, threadIndex);

    if (threadIndex >= ptw->m_num_threads)
    {
        std::cerr << "[PINTOOLWARMUP:" << threadIndex << "] Error, thread mapping not consistent " << threadIndex << " != " << ptw->m_num_threads << "\n";
        return;
    }

    if ((threadIndex) >= ptw->m_max_num_threads)
    {
        std::cerr << "[PINTOOLWARMUP:" << threadIndex << "] max thread count: "  << ptw->m_max_num_threads << "\n";
        return;
    }

     // ptw->m_sift_writer[ptw->m_num_threads] = new Sift::Writer((std::string("test_sift.out.")+decstr(ptw->m_num_threads)).c_str() /*filename*/, PinToolWarmup::getCode, false, "" /*response_filename*/, ptw->m_num_threads /*threadid*/, false /*arch32*/, false, false /*KnobSendPhysicalAddresses.Value()*/ /*!warmup*/ /*is_fifo*/);
//     ptw->m_sift_writer[ptw->m_num_threads] = thread_data[ptw->m_num_threads].output;
//    ptw->m_tool_warmup[ptw->m_num_threads] = new ToolWarmup(ptw->m_sift_writer[ptw->m_num_threads], ptw->m_num_threads, false /*global_sync*/);
     ptw->m_sift_writer[ threadIndex ] = thread_data[ threadIndex].output;
     ptw->m_tool_warmup[ threadIndex ] = new ToolWarmup(ptw->m_sift_writer[ threadIndex], threadIndex, false /*global_sync*/);
   


    PIN_ReleaseLock(&ptw->m_lock);
}

void PinToolWarmup::fini(INT32 code, VOID *v)
{

}

//   Sift::Writer *m_sift_writer = NULL;
//   ToolWarmup *m_tool_warmup = NULL;
bool PinToolWarmup::activate(int max_num_threads)
{
    if (m_sift_writer || m_tool_warmup)
    {
        return false;
    }
    m_sift_writer = new Sift::Writer*[max_num_threads];
    m_tool_warmup = new ToolWarmup*[max_num_threads];
    for (int i = 0 ; i < max_num_threads ; i++)
    {
        m_sift_writer[i] = NULL;
        m_tool_warmup[i] = NULL;
    }
    m_num_threads = max_num_threads;
    m_max_num_threads = max_num_threads;

    PIN_InitLock(&m_lock);

    TRACE_AddInstrumentFunction(PinToolWarmup::traceCallback, this);
    PIN_AddThreadStartFunction(PinToolWarmup::threadStart, this);
    PIN_AddFiniFunction(PinToolWarmup::fini, this);

    return true;
}
bool PinToolWarmup::stopWriteWarmupInstructions()
{
    
    for(uint32_t i = 0 ; i < m_num_threads ; i++ ) {
        if(m_tool_warmup[i]!=NULL) {
            m_tool_warmup[i]->stopWriteWarmupInstructions();
        }
    }
    return true;
}
bool PinToolWarmup::startWriteWarmupInstructions()
{
    
    for(uint32_t i = 0 ; i < m_num_threads ; i++ ) {
        if(m_tool_warmup[i]!=NULL) {
            m_tool_warmup[i]->startWriteWarmupInstructions();
        }
    }
    return true;
}

bool PinToolWarmup::writeWarmupInstructions(unsigned int threadid)
{
    if (threadid >= m_num_threads)
    {
        std::cerr << "[PINTOOLWARMUP] " << __FUNCTION__ << " Invalid threadid " << threadid << " "<< m_num_threads<< "\n";
        return false;
    }
    m_tool_warmup[threadid]->writeWarmupInstructions();
    return true;
}
void PinToolWarmup::clearTimeStamp()
{
    for (uint64_t threadid =0;threadid<m_num_threads;threadid++) {
//        if (threadid >= m_num_threads)
//        {
//            std::cerr << "[PINTOOLWARMUP] " << __FUNCTION__ << " Invalid threadid " << threadid << " "<< m_num_threads<< "\n";
//        //    return false;
//        }
        if(m_tool_warmup[threadid]!=NULL) {
            m_tool_warmup[threadid]->clearTimeStamp();
        }
    }
}

ToolWarmupGlobal::ToolWarmupGlobal()
      : m_sent_newthread(false)
      , m_starttime(std::time(0))
   {
      std::string arch_name = KnobArch.Value();
      if( arch_name == "gainestown" ) {
        CACHE_SHARED_BY_CORES=4;
        CACHE_SIZE_BYTES = 8 * 1024 * 1024 ;
        CACHE_ASSOCIATIVITY = 16;

      } else if (arch_name == "skylake") {
        CACHE_SHARED_BY_CORES=16;
        CACHE_SIZE_BYTES = 22 * 1024 * 1024 ;
        CACHE_ASSOCIATIVITY = 11;
      } else if (arch_name == "sunnycove") {
        CACHE_SHARED_BY_CORES=8;
        CACHE_SIZE_BYTES = 16 * 1024 * 1024 ;
        CACHE_ASSOCIATIVITY = 16;
      } else {
        printf( "%s not supported\n", arch_name.c_str());
        sift_assert(false);
      }
   }


bool ToolWarmupGlobal::writeWarmupInstructionsOldOrig()
{
   if (!m_sent_newthread)
   {
      if (m_warmup_list.front() == NULL || !m_warmup_list.front()->_valid())
      {
         std::cerr << "[TOOL_WARMUP_GLOBAL] The head of the warmup list is not valid!" << std::endl;
         return false;
      }

      // Open all of our newthread pipes
      for (unsigned int i = 1 ; i < m_warmup_list.size() ; i++)
      {
         if (s_debug)
            std::cerr << "[TOOL_WARMUP_GLOBAL] starting new thread" << std::endl;
         m_warmup_list.front()->NewThread(true /*send_immediately*/);
      }
      m_sent_newthread = true;
   }

   // Check to make sure that all of the warmup threads are ready before trying to write the instructions
   for (auto t = m_warmup_list.begin() ; t != m_warmup_list.end() ; ++t)
   {
      if (! (*t)->_valid())
        return false;
   }

   // Iterate over each thread, adding a cache-size worth of addresses to the global ordering
   int tid = 0;
   std::vector<TimeAddrThread> time2addrthread;
   for (auto t = m_warmup_list.begin() ; t != m_warmup_list.end() ; ++t, ++tid)
   {
      if (s_debug)
         std::cerr << "[TOOL_WARMUP_GLOBAL] Getting lock for thread " << tid << std::endl;

      (*t)->_getLock();

      const AddrTime2Map *addr2time = (*t)->getAddr2Time();

      // Populate our vector with accesses
      std::vector<Time2Addr> maxtimevec;
      for (auto i = addr2time->begin() ; i != addr2time->end() ; ++i)
      {
         auto &x = *i;
         auto addr = x.first;
         auto time_read = (x.second).first;
         auto time_written = (x.second).second;
         maxtimevec.push_back(Time2Addr(time_read,time_written,addr));
      }

      (*t)->_releaseLock();

      // Sort the access times
      std::sort(maxtimevec.begin(), maxtimevec.end());

      // Get a number of accesses, equal to the number of lines in the cache
      int num_lines = std::min<uint64_t>(NUM_LINES_IN_CACHE, maxtimevec.size());
      int start = maxtimevec.size() - num_lines;

      // Save both the reads and writes as individual accesses
      std::vector<Time2Addr>::iterator x = maxtimevec.begin();
      for (std::advance(x, start) ; x != maxtimevec.end() ; ++x)
      {
         uint64_t addr = x->m_addr;
         uint64_t time_read = x->m_time[0];
         uint64_t time_written = x->m_time[1];
         uint64_t time_icache = 0; // ERROR: x->m_time[2];
         if (s_debug >= 2)
            std::cout << "[TOOL_WARMUP] @ " << time_read << "," << time_written << " 0x" << std::hex << addr << std::dec << std::endl;

         // Use the lower bits of the address as a read/write indicator
         if (time_read)
         {
            auto force_read_addr = ADD_FORCE_READ(addr);
            time2addrthread.push_back(TimeAddrThread(time_read, force_read_addr, tid));
         }
         if (time_written)
         {
            auto force_write_addr = ADD_FORCE_WRITE(addr);
            time2addrthread.push_back(TimeAddrThread(time_written, force_write_addr, tid));
         }
         if (time_icache)
         {
            auto force_icache_addr = ADD_FORCE_ICACHE(addr);
            time2addrthread.push_back(TimeAddrThread(time_icache, force_icache_addr, tid));
         }
      }
   }

   // Sort our global address vector
   std::sort(time2addrthread.begin(), time2addrthread.end());

   // Send the memory accesses to the appropriate thread
   // Other threads get NOPs to keep them in sync
   uint64_t addr_nop = ADD_FORCE_NOP(0);
   for (auto i = time2addrthread.begin() ; i != time2addrthread.end() ; ++i)
   {
      uint64_t addr = i->m_addr;
      uint64_t thread = i->m_thread;
      if (s_debug >= 2)
         std::cout << "[TOOL_WARMUP:" << thread << "] @ " << i->m_time << " 0x" << std::hex << addr << std::dec << std::endl;
      for (unsigned int m = 0 ; m < m_warmup_list.size() ; m++)
      {
         if (m == thread)
            m_warmup_list[m]->_memoryAccess(addr);
         else
            m_warmup_list[m]->_memoryAccess(addr_nop);
      }
   }
   return true;
}

bool ToolWarmupGlobal::writeWarmupInstructions()
{
   if (!m_sent_newthread)
   {
      if (m_warmup_list.front() == NULL || !m_warmup_list.front()->_valid())
      {
         std::cerr << "[TOOL_WARMUP_GLOBAL] The head of the warmup list is not valid!" << std::endl;
         return false;
      }

      // Open all of our newthread pipes
      for (unsigned int i = 1 ; i < m_warmup_list.size() ; i++)
      {
         if (s_debug)
            std::cerr << "[TOOL_WARMUP_GLOBAL] starting new thread" << std::endl;
         m_warmup_list.front()->NewThread(true /*send_immediately*/);
      }
      m_sent_newthread = true;
   }

   // Check to make sure that all of the warmup threads are ready before trying to write the instructions
   for (auto t = m_warmup_list.begin() ; t != m_warmup_list.end() ; ++t)
   {
      if (! (*t)->_valid())
        return false;
   }

   m_time_record.push_back(std::make_pair(std::time(0), "begin collecting thread data"));

   size_t num_threads = m_warmup_list.size();
   size_t num_caches = ((num_threads+(CACHE_SHARED_BY_CORES-1)) / CACHE_SHARED_BY_CORES); // Round, as we need another cache for uneven sharers

   size_t num_total_sets = NUM_SETS_IN_CACHE; // max of the number of sets in the caches
   size_t num_total_assoc = num_caches * CACHE_ASSOCIATIVITY;

   if (s_debug >= 1)
   {
      std::cerr << "[TOOL_WARMUP_GLOBAL] nthreads: " << num_threads << " num_caches: " << num_caches << " num_total_sets: " << num_total_sets << " num_total_assoc: " << num_total_assoc << std::endl;
   }

   // We need to take into account LRU/set data for each cache
   std::vector< std::vector<MTR> > set_time3addrthread;
   set_time3addrthread.resize(num_total_sets);
   // Map an address from a set to a specific index in the above MTR vector
   std::vector< std::unordered_map<uint64_t,uint64_t> > set_addrtoindex;
   set_addrtoindex.resize(num_total_sets);

   // Iterate over each thread, adding their unique accesses to the per-LRU accesses
   // We could optimize here and only keep the last num_total_assoc entries (or only insert if we need to)
   //  as the worst case is that a single cache inserts all of the entries needed
   int thread = 0;
   for (auto t = m_warmup_list.begin() ; t != m_warmup_list.end() ; ++t, ++thread)
   {
      (*t)->_getLock();

      const AddrTime2Map *addr2time = (*t)->getAddr2Time();

      // Populate our set vector with data accesses
      for (auto i = addr2time->begin() ; i != addr2time->end() ; ++i)
      {
         auto &x = *i;
         auto addr = x.first;
         auto time_read = (x.second).first;
         auto time_written = (x.second).second;
         auto set_index = SET_INDEX(addr);
         if (s_debug)
            assert(set_index < num_total_sets);
         if (time_read)
           addLruThreadRead(set_time3addrthread[set_index], set_addrtoindex[set_index], addr, time_read, thread, /*is_icache*/ false);
         if (time_written)
           addLruThreadWrite(set_time3addrthread[set_index], set_addrtoindex[set_index], addr, time_written, thread);
      }

      const AddrTimeMap *insn2time = (*t)->getInsn2Time();

      // Populate our set vector with instruction accesses
      for (auto i = insn2time->begin() ; i != insn2time->end() ; ++i)
      {
         auto &x = *i;
         auto addr = x.first;
         auto time_icache = x.second;
         auto set_index = SET_INDEX(addr);
         if (s_debug)
            assert(set_index < num_total_sets);
         addLruThreadRead(set_time3addrthread[set_index], set_addrtoindex[set_index], addr, time_icache, thread, /*is_icache*/ true);
      }

      (*t)->_releaseLock();

      if (s_debug >= 1)
      {
         std::cout << "[TOOL_WARMUP_GLOBAL] Set info:" << std::endl;
         for (size_t i = 0 ; i < set_time3addrthread.size() ; i+=32)
         {
            for (int j = 0 ; j < 32 ; j++)
            {
               std::cout << std::setw(4) << set_time3addrthread[i+j].size();
            }
            std::cout << std::endl;
         }
      }
   }

   m_time_record.push_back(std::make_pair(std::time(0), "end collecting thread data"));

   // Iterate over each set, adding num_caches * LRU-size (assoc) for each set
   std::vector<TimeAddrThread> time2addrthread;
   for (auto setvi = set_time3addrthread.begin() ; setvi != set_time3addrthread.end() ; ++setvi)
   {
      auto &setv = *setvi;
      // Sort the access times
      std::sort(setv.begin(), setv.end());

      // Get a number of accesses, equal to the number of lines in the cache
      int num_lines = std::min<uint64_t>(num_total_assoc, setv.size());
      int start = setv.size() - num_lines;

      // Save both the reads and writes as individual accesses
      // Replay all reads and writes for more generic cache support
      std::vector<MTR>::iterator x = setv.begin();
      for (std::advance(x, start) ; x != setv.end() ; ++x)
      {
         // Use the lower bits of the address as a read/write indicator
         auto addr = x->address;
         for (size_t t = 0 ; t < MTR_MAX_THREADS ; ++t)
         {
            auto time_read = x->read_times[t];
            if (time_read)
            {
               if (x->is_icache[t])
               {
                  auto force_icache_addr = ADD_FORCE_ICACHE(addr);
                  time2addrthread.push_back(TimeAddrThread(time_read, force_icache_addr, t));
               }
               else
               {
                  auto force_read_addr = ADD_FORCE_READ(addr);
                  time2addrthread.push_back(TimeAddrThread(time_read, force_read_addr, t));
               }
            }
         }
         auto time_written = x->write_time;
         if (time_written)
         {
            auto force_write_addr = ADD_FORCE_WRITE(addr);
            time2addrthread.push_back(TimeAddrThread(time_written, force_write_addr, x->write_thread));
         }
         //if (s_debug >= 2)
         //   std::cout << "[TOOL_WARMUP_GLOBAL] @ " << time_read << "," << time_written << " 0x" << std::hex << addr << std::dec << std::endl;
      }
   }

   // Sort our global address vector
   std::sort(time2addrthread.begin(), time2addrthread.end());

   m_time_record.push_back(std::make_pair(std::time(0), "begin memoryAccess"));

   // Send the memory accesses to the appropriate thread
   // Other threads get NOPs to keep them in sync
   uint64_t addr_nop = ADD_FORCE_NOP(0);
   for (auto i = time2addrthread.begin() ; i != time2addrthread.end() ; ++i)
   {
      uint64_t addr = i->m_addr;
      uint64_t thread = i->m_thread;
      if (s_debug >= 2)
         std::cout << "[TOOL_WARMUP_GLOBAL:" << thread << "] @ " << i->m_time << " 0x" << std::hex << addr << std::dec << std::endl;
      for (unsigned int m = 0 ; m < m_warmup_list.size() ; m++)
      {
         if (m == thread)
            m_warmup_list[m]->_memoryAccess(addr);
         else
            m_warmup_list[m]->_memoryAccess(addr_nop);
      }
   }

   m_time_record.push_back(std::make_pair(std::time(0), "exit writeWarmupInstructions"));

   return true;
}
