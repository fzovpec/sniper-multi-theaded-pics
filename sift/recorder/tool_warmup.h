#pragma once
//#ifndef __TOOL_WARMUP_H__
//#define __TOOL_WAMRUP_H__

#include "sift_writer.h"
#include "sift_warmup.h"
#include "sim_api.h"
//#include "globals.h"

#include "pin.H"

#include "sift_assert.h"
#include <vector>
#include <deque>
#include <unordered_map>
#include <iostream>
#include <ctime>
#include <utility>
#include <queue>
#define CACHE_LINE_SIZE_BYTES 64
#define CACHE_LINE_SIZE_BYTES_LOG2 6
#define CACHE_LINE_MASK (~(CACHE_LINE_SIZE_BYTES-1ULL)) // 0xffff..00
//#define CACHE_SIZE_BYTES (22 * 1024 * 1024) // 8MiB
#define NUM_LINES_IN_CACHE (CACHE_SIZE_BYTES/CACHE_LINE_SIZE_BYTES)
//#define CACHE_SHARED_BY_CORES 16
//#define CACHE_ASSOCIATIVITY 11
#define NUM_SETS_IN_CACHE (NUM_LINES_IN_CACHE/CACHE_ASSOCIATIVITY)
#define SET_INDEX(addr) ( ((addr) >> CACHE_LINE_SIZE_BYTES_LOG2) & (NUM_SETS_IN_CACHE-1) )
#include "treelist.h"
struct TimeAddr {
   uint64_t m_time;
   uint64_t m_addr;

   TimeAddr(uint64_t _time, uint64_t _addr)
      : m_time(_time)
      , m_addr(_addr)
   {}

   bool operator < (const TimeAddr& at) const
   {
      return (m_time < at.m_time);
   }
};

struct TimeAddrThread {
   uint64_t m_time;
   uint64_t m_addr;
   uint64_t m_thread;

   TimeAddrThread(uint64_t _time, uint64_t _addr, uint64_t _thread)
      : m_time(_time)
      , m_addr(_addr)
      , m_thread(_thread)
   {}

   bool operator < (const TimeAddrThread& at) const
   {
      return (m_time < at.m_time);
   }
};

struct Time2Addr {
   uint64_t m_time[2];
   uint64_t m_addr;

   Time2Addr(uint64_t _time0, uint64_t _time1, uint64_t _addr)
      : m_time{_time0,_time1}
      , m_addr(_addr)
   {}

   bool operator < (const Time2Addr& at) const
   {
      return (std::max(m_time[0],m_time[1]) < std::max(at.m_time[0],at.m_time[1]));
   }
};

struct Time3Addr {
   uint64_t m_time[3];
   uint64_t m_addr;

   Time3Addr(uint64_t _time0, uint64_t _time1, uint64_t _time2, uint64_t _addr)
      : m_time{_time0,_time1,_time2}
      , m_addr(_addr)
   {}

   bool operator < (const Time3Addr& at) const
   {
      return (std::max(std::max(m_time[0],m_time[1]),m_time[2]) < std::max(std::max(at.m_time[0],at.m_time[1]),at.m_time[2]));
   }
};

struct Time3AddrThread {
   uint64_t m_time[3];
   uint64_t m_addr;
   uint64_t m_thread;

   Time3AddrThread(uint64_t _time0, uint64_t _time1, uint64_t _time2, uint64_t _addr, uint64_t _thread)
      : m_time{_time0,_time1,_time2}
      , m_addr(_addr)
      , m_thread(_thread)
   {}

   bool operator < (const Time3AddrThread& at) const
   {
      return (std::max(std::max(m_time[0],m_time[1]),m_time[2]) < std::max(std::max(at.m_time[0],at.m_time[1]),at.m_time[2]));
   }
};

#define MTR_MAX_THREADS 32
struct MTR {
   MTR()
      : read_times{0}
      , address(0)
      , write_thread(0)
      , write_time(0)
      , max_time(0)
      , is_icache{0}
   {
   }
   uint64_t read_times[MTR_MAX_THREADS];
   uint64_t address;
   uint64_t write_thread;
   uint64_t write_time;
   uint64_t max_time;
   uint8_t is_icache[MTR_MAX_THREADS];
   bool operator < (const MTR& mtr) const
   {
      return max_time < mtr.max_time;
   }
};

inline void addLruThreadRead(std::vector<MTR> &set_mtr,
                             std::unordered_map<uint64_t,uint64_t> &set_addrtoindex,
                             uint64_t address, uint64_t time, uint64_t thread, bool is_icache = false)
{
   if (set_addrtoindex.count(address) == 0)
   {
      set_mtr.resize(set_mtr.size()+1);
      set_mtr.back().address = address;
      set_addrtoindex[address] = set_mtr.size()-1;
   }
   auto &mtr = set_mtr[ set_addrtoindex[address] ];
   // Assumes that reads coming from a specific CPU are unique and non-repeating
   mtr.read_times[thread] = time;
   mtr.is_icache[thread] = is_icache;
   mtr.max_time = std::max(mtr.max_time,time);
}

inline void addLruThreadWrite(std::vector<MTR> &set_mtr,
                              std::unordered_map<uint64_t,uint64_t> &set_addrtoindex,
                              uint64_t address, uint64_t time, uint64_t thread)
{
   if (set_addrtoindex.count(address) == 0)
   {
      set_mtr.resize(set_mtr.size()+1);
      set_mtr.back().address = address;
      set_addrtoindex[address] = set_mtr.size()-1;
   }
   auto &mtr = set_mtr[ set_addrtoindex[address] ];
   if (mtr.write_time < time)
   {
      mtr.write_time = time;
      mtr.write_thread = thread;
      mtr.max_time = std::max(mtr.max_time,time);
   }
}

//typedef std::unordered_map<uint64_t,std::tuple<uint64_t,uint64_t>> AddrTime2Map;
typedef std::unordered_map<uint64_t,std::pair<uint64_t,uint64_t>> AddrTime2Map;
typedef std::unordered_map<uint64_t,uint64_t> AddrTimeMap;

class ToolWarmup {
public:
  int CACHE_SIZE_BYTES;// (22 * 1024 * 1024) // 8MiB
  int CACHE_SHARED_BY_CORES;// 16
  int CACHE_ASSOCIATIVITY;// 11

   ToolWarmup(Sift::Writer *_output, int id, bool global_sync = false);
   ~ToolWarmup();
   void clearTimeStamp();
   volatile bool permit_to_write_warmup_ins=true;
   void memoryCallback(uint64_t address, bool is_read_not_write = true);
   void icacheCallback(uint64_t address);
   void NewThread(bool sent_immediately = false);
   void writeWarmupInstructions() ;
   void startWriteWarmupInstructions() {
     permit_to_write_warmup_ins = true;
   }
   void stopWriteWarmupInstructions() {
     permit_to_write_warmup_ins = false;
   }
   static bool copyData(uint8_t *dst, const uint8_t *src, uint32_t size)
   {
      return Sift::Warmup::getCode(dst, src, size);
   }
   void setSiftOutput(Sift::Writer *_output);
   void initSiftWarmup();
   const AddrTime2Map* getAddr2Time() const {return &m_addr2time;}
   const AddrTimeMap* getInsn2Time() const {return &m_insn2time;}
   void _memoryAccess(uint64_t addr) { m_warmup->MemoryAccess(addr); } // For direct memory access. Only for use by ToolWarmupGlobal
   void _getLock() { PIN_GetLock(&m_addrlock, m_id); }
   void _releaseLock() { PIN_ReleaseLock(&m_addrlock); }
   bool _valid() { return (m_output != NULL) && (m_warmup != NULL); }

private:

   typedef struct
   {
      volatile uint64_t counter;
      int _dummy[15];
   } atomic_t;
   static atomic_t s_counter;
   static inline uint64_t atomic_add_return(uint64_t i, atomic_t *v)
   {
      return __sync_fetch_and_add(&(v->counter), i);
   }
   #define atomic_inc_return(v)  (atomic_add_return(1, v))

   static const int s_debug = 0;

   Sift::Writer *m_output;
   Sift::Warmup *m_warmup;

   int64_t m_mem_count;
   uint64_t m_counter;
   int m_id;
   bool m_global_sync;
   time_t m_starttime;

   AddrTime2Map m_addr2time; // Time for read, and for write. Zero means not read/written
   //std::queue<std::pair<uint64_t,uint64_t>> m_addr2time_queue;//first is addr; second is timestamp
   TreeList<uint64_t,std::pair<uint64_t,uint64_t>> treelist;

   AddrTimeMap m_insn2time;  // Time for icache reads
   std::deque<uint64_t> m_newthreadtime;

   std::vector<std::pair<time_t, std::string> > m_time_record;

   PIN_LOCK m_addrlock;
};

class PinToolWarmup
{
private:
   bool if_mtr_sampling = true;
   Sift::Writer **m_sift_writer = NULL;
   ToolWarmup **m_tool_warmup = NULL;
   UINT64 m_num_threads = 0;
   UINT64 m_max_num_threads = 0;
   PIN_LOCK m_lock;
public:
   PinToolWarmup() {}
   ~PinToolWarmup() {}
   static void getCode(uint8_t *dst, const uint8_t *src, uint32_t size);
   static UINT32 addMemoryModeling(INS ins, PinToolWarmup *ptw);
   static VOID traceCallback(TRACE trace, void *v);
   static VOID threadStart(THREADID threadIndex, CONTEXT *ctxt, INT32 flags, VOID *v);
   static VOID fini(INT32 code, VOID *v);
   static VOID handleMemoryWarmup(THREADID threadid, ADDRINT address, BOOL is_read_not_write, ADDRINT ptr_PinToolWarmup);
   bool activate(int max_num_threads = 128);
   bool writeWarmupInstructions(unsigned int threadid);
   void clearTimeStamp();
   bool stopWriteWarmupInstructions();
   bool startWriteWarmupInstructions();
   void mtr_sampling() 
   {
      if_mtr_sampling = true;
   }
   void disable_mtr_sampling()
   {
      if_mtr_sampling = false;
   }
   UINT64 getNumThreads(){ return m_num_threads; }
};


class ToolWarmupGlobal
{
public:
  int CACHE_SIZE_BYTES;// (22 * 1024 * 1024) // 8MiB
  int CACHE_SHARED_BY_CORES;// 16
  int CACHE_ASSOCIATIVITY;// 11

   ToolWarmupGlobal();
   ~ToolWarmupGlobal()
   {
      m_time_record.push_back(std::make_pair(std::time(0), "end"));
      for (auto x = m_time_record.begin() ; x != m_time_record.end() ; ++x)
      {
         std::cout << "[TOOL_WARMUP_GLOBAL] T: " << ((*x).first - m_starttime) << " M: " << (*x).second << std::endl;
      }
   }
   void registerWarmup(ToolWarmup* tw) { m_warmup_list.push_back(tw); }
   bool writeWarmupInstructionsOldOrig();
   bool writeWarmupInstructions();
private:
   static const int s_debug = 0;
   bool m_sent_newthread;
   time_t m_starttime;

   std::vector<ToolWarmup*> m_warmup_list;
   //std::vector<std::tuple<time_t, std::string> > m_time_record;
   std::vector<std::pair<time_t, std::string> > m_time_record;
};

//#endif /*__TOOL_WAMRUP_H*/
