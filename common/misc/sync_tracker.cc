#include "sync_tracker.h"
#include "instruction.h"
#include "pics.h"
#include "simulator.h"
#include "stats.h"

#include <deque>
#include <unordered_map>
#include <algorithm>
#include <pthread.h>
#include <cstdio>
#include <inttypes.h>

static pthread_mutex_t s_sync_tracker_lock = PTHREAD_MUTEX_INITIALIZER;

struct WaitState {
   bool is_waiting;
   SubsecondTime wait_start;
   IntPtr lock_address;
};

struct CommittedInst {
   Instruction *ins;
   SubsecondTime time;
};

static std::unordered_map<thread_id_t, SubsecondTime> s_thread_start_time;
static std::unordered_map<thread_id_t, WaitState> s_waiting_threads;
static std::unordered_map<thread_id_t, SubsecondTime> s_lock_acq_time;
static std::unordered_map<thread_id_t, IntPtr> s_lock_addr;
static std::unordered_map<thread_id_t, bool> s_has_pending_lock;
static std::unordered_map<thread_id_t, bool> s_is_slow_path;
static std::unordered_map<thread_id_t, std::deque<CommittedInst>> s_thread_instructions;
static SubsecondTime s_last_global_unlock = SubsecondTime::Zero();
static bool s_is_lock_held = false;
static thread_id_t s_lock_holder = -1;

void SyncTracker::onThreadStart(thread_id_t tid, SubsecondTime time)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_thread_start_time[tid] = time;
   s_waiting_threads[tid] = { false, SubsecondTime::Zero(), 0 };
   s_has_pending_lock[tid] = false;
   s_is_slow_path[tid] = false;
   s_lock_addr[tid] = 0;
   s_thread_instructions[tid].clear();
   if (s_lock_holder == tid)
   {
      s_is_lock_held = false;
      s_lock_holder = -1;
   }
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadExit(thread_id_t tid)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_waiting_threads[tid].is_waiting = false;
   s_has_pending_lock[tid] = false;
   s_is_slow_path[tid] = false;
   s_lock_addr[tid] = 0;
   s_thread_instructions[tid].clear();
   if (s_lock_holder == tid)
   {
      s_is_lock_held = false;
      s_lock_holder = -1;
   }
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadWaitStart(thread_id_t tid, SubsecondTime time, IntPtr lock_address)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_waiting_threads[tid].is_waiting = true;
   s_waiting_threads[tid].wait_start = time;
   s_waiting_threads[tid].lock_address = lock_address;
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadWaitEnd(thread_id_t tid, SubsecondTime time)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_waiting_threads[tid].is_waiting = false;
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onLockAcquired(thread_id_t tid, SubsecondTime time, IntPtr lock_address)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_is_lock_held = true;
   s_lock_holder = tid;
   s_lock_acq_time[tid] = time;
   s_lock_addr[tid] = lock_address;
   s_has_pending_lock[tid] = true;
   s_is_slow_path[tid] = true;
   s_thread_instructions[tid].clear();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onInstructionCommit(thread_id_t tid, Instruction *ins, SubsecondTime time, IntPtr mem_addr)
{
   if (!ins)
      return;

   pthread_mutex_lock(&s_sync_tracker_lock);

   const char *name = ins->getName();
   bool is_cmpxchg = false;
   bool is_xchg = false;

   const char *disas = ins->getDisassembly().empty() ? NULL : ins->getDisassembly().c_str();

   if (ins->isAtomic() || (disas && (strstr(disas, "cmpxchg") != NULL || strstr(disas, "CMPXCHG") != NULL || strstr(disas, "xchg") != NULL || strstr(disas, "XCHG") != NULL)))
   {
      if (name)
      {
         if (strstr(name, "CMPXCHG") != NULL || strstr(name, "cmpxchg") != NULL)
            is_cmpxchg = true;
         else if (strstr(name, "XCHG") != NULL || strstr(name, "xchg") != NULL)
            is_xchg = true;
      }
      if (!is_cmpxchg && !is_xchg && disas)
      {
         if (strstr(disas, "cmpxchg") != NULL || strstr(disas, "CMPXCHG") != NULL)
            is_cmpxchg = true;
         else if (strstr(disas, "xchg") != NULL || strstr(disas, "XCHG") != NULL)
            is_xchg = true;
      }
      printf("This is an atomic instruction [tid=%d] %d, %d (name: %s, disas: %s, addr: %p)\n",
             tid, is_cmpxchg, is_xchg, name ? name : "NULL", disas ? disas : "NULL", (void*)mem_addr);
      fflush(stdout);
   }

   if (is_cmpxchg)
   {
      // Fast path lock acquisition: thread acquires lock if it doesn't already hold one,
      // is not currently waiting in futex, and the lock is not held by another thread.
      bool is_waiting = s_waiting_threads.count(tid) && s_waiting_threads[tid].is_waiting;
      bool lock_held_by_other = false;
      if (mem_addr != 0)
      {
         for (auto &kv : s_has_pending_lock)
         {
            if (kv.first != tid && kv.second && s_lock_addr[kv.first] == mem_addr)
            {
               lock_held_by_other = true;
               break;
            }
         }
      }
      else if (s_is_lock_held && s_lock_holder != tid)
      {
         lock_held_by_other = true;
      }

      if (!s_has_pending_lock[tid] && !is_waiting && !lock_held_by_other)
      {
         s_is_lock_held = true;
         s_lock_holder = tid;
         s_lock_acq_time[tid] = time;
         s_lock_addr[tid] = mem_addr;
         s_has_pending_lock[tid] = true;
         s_is_slow_path[tid] = false;
         s_thread_instructions[tid].clear();
         printf("[FAST-PATH LOCK] Thread %d acquired lock (addr %p, t = %" PRIu64 " ns)\n",
                tid, (void*)mem_addr, time.getNS());
         fflush(stdout);
         Sim()->getStatsManager()->recordSyncEvent(tid, "end_lock_acquisition", time.getNS(), mem_addr);
      }
   }
   else if (is_xchg)
   {
      // Fast path unlock: only release if this thread holds the lock via fast-path (slow path locks can only be released on slow path / FUTEX_WAKE)
      // and no threads are sleeping in futex on this mutex.
      IntPtr lock_address = mem_addr != 0 ? mem_addr : s_lock_addr[tid];
      bool has_waiters = false;
      for (auto &kv : s_waiting_threads)
      {
         if (kv.first != tid && kv.second.is_waiting && 
             lock_address != 0 && kv.second.lock_address != 0 && 
             kv.second.lock_address == lock_address)
         {
            has_waiters = true;
            break;
         }
      }

      bool is_waiting = s_waiting_threads.count(tid) && s_waiting_threads[tid].is_waiting;
      bool is_lock_wait_xchg = (disas && (strstr(disas, "eax") != NULL || strstr(disas, "EAX") != NULL));
      if (!is_lock_wait_xchg && !is_waiting && !has_waiters && s_has_pending_lock[tid] && !s_is_slow_path[tid])
      {
         printf("[FAST-PATH UNLOCK] Thread %d released lock (addr %p, t = %" PRIu64 " ns)\n",
                tid, (void*)lock_address, time.getNS());
         fflush(stdout);
         pthread_mutex_unlock(&s_sync_tracker_lock);
         onUnlock(tid, time, lock_address);
         return;
      }
   }

   std::deque<CommittedInst> &inst_buf = s_thread_instructions[tid];
   inst_buf.push_back({ ins, time });
   if (inst_buf.size() > 500000)
   {
      inst_buf.pop_front();
   }

   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onUnlock(thread_id_t tid, SubsecondTime time, IntPtr lock_address)
{
   pthread_mutex_lock(&s_sync_tracker_lock);

   if (!s_has_pending_lock[tid])
   {
      pthread_mutex_unlock(&s_sync_tracker_lock);
      return;
   }

   if (s_lock_holder == tid)
   {
      s_is_lock_held = false;
      s_lock_holder = -1;
   }

   SubsecondTime start_crit = s_lock_acq_time[tid];
   IntPtr final_addr = lock_address != 0 ? lock_address : s_lock_addr[tid];
   s_has_pending_lock[tid] = false;
   s_is_slow_path[tid] = false;
   s_lock_addr[tid] = 0;
   s_last_global_unlock = time;

   Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", time.getNS(), final_addr);

   if (time <= start_crit)
   {
      s_thread_instructions[tid].clear();
      pthread_mutex_unlock(&s_sync_tracker_lock);
      return;
   }

   // 1. Calculate total wait time across all other threads contending during [start_crit, time]
   SubsecondTime total_wait = SubsecondTime::Zero();
   for (auto &kv : s_waiting_threads)
   {
      thread_id_t other_tid = kv.first;
      if (other_tid == tid)
         continue;

      WaitState &wstate = kv.second;
      if (wstate.is_waiting)
      {
         SubsecondTime overlap_start = std::max(wstate.wait_start, start_crit);
         if (time > overlap_start)
         {
            total_wait += (time - overlap_start);
         }
      }
   }

   // 2. Identify instructions committed by thread 'tid' within [start_crit, time]
   std::deque<CommittedInst> &inst_buf = s_thread_instructions[tid];
   size_t cs_inst_count = 0;
   for (size_t i = 0; i < inst_buf.size(); ++i)
   {
      if (inst_buf[i].time >= start_crit && inst_buf[i].time <= time)
      {
         cs_inst_count++;
      }
   }

   // 3. If there was multi-threaded wait and instructions executed, attribute with equal weight
   if (total_wait > SubsecondTime::Zero() && cs_inst_count > 0)
   {
      SubsecondTime wait_per_inst = total_wait / cs_inst_count;
      printf("[PICS MT-WAIT] Thread %d CS [%" PRIu64 " - %" PRIu64 " ns]: %zu instrs, W_total = %" PRIu64 " ns (added %" PRIu64 " fs/instr)\n",
             tid, start_crit.getNS(), time.getNS(), cs_inst_count, total_wait.getNS(), wait_per_inst.getFS());
      fflush(stdout);

      for (size_t i = 0; i < inst_buf.size(); ++i)
      {
         if (inst_buf[i].time >= start_crit && inst_buf[i].time <= time)
         {
            Instruction *ins = inst_buf[i].ins;
            if (ins)
            {
               // ins->getPICS_d()->add_mt_wait(wait_per_inst);
               // ins->getPICS_c()->add_mt_wait(wait_per_inst);
            }
         }
      }
   }

   // 4. Clear buffered instructions for this thread up to this unlock
   inst_buf.clear();

   pthread_mutex_unlock(&s_sync_tracker_lock);
}

