#include "sync_tracker.h"
#include "instruction.h"
#include "pics.h"
#include "simulator.h"
#include "stats.h"

#include <deque>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <pthread.h>
#include <cstdio>
#include <inttypes.h>

static pthread_mutex_t s_sync_tracker_lock = PTHREAD_MUTEX_INITIALIZER;

struct WaitState {
   bool is_waiting;
   SubsecondTime wait_start;
   IntPtr lock_address;
};

struct WaitInterval {
   thread_id_t tid;
   IntPtr lock_address;
   SubsecondTime start;
   SubsecondTime end;
};

struct CommittedInst {
   Instruction *ins;
   SubsecondTime time;
};

static constexpr int MAX_TRACKED_THREADS = 64;

static std::unordered_map<thread_id_t, SubsecondTime> s_thread_start_time;
static std::unordered_map<thread_id_t, bool> s_active_threads;
static std::unordered_map<thread_id_t, bool> s_blocked_threads;
static std::unordered_map<thread_id_t, WaitState> s_waiting_threads;
static std::deque<WaitInterval> s_completed_waits;
static std::unordered_map<thread_id_t, SubsecondTime> s_lock_acq_time;
static std::unordered_map<thread_id_t, IntPtr> s_lock_addr;
static std::unordered_map<thread_id_t, bool> s_has_pending_lock;
static std::unordered_map<thread_id_t, bool> s_is_slow_path;
static std::unordered_map<thread_id_t, bool> s_deferred_acq;
static std::unordered_map<IntPtr, SubsecondTime> s_last_unlock_by_addr;
static std::unordered_map<thread_id_t, std::deque<CommittedInst>> s_thread_instructions;
static std::unordered_map<Instruction*, SubsecondTime> s_cs_start_d[MAX_TRACKED_THREADS];
static std::unordered_map<Instruction*, SubsecondTime> s_cs_start_c[MAX_TRACKED_THREADS];
static SubsecondTime s_last_global_unlock = SubsecondTime::Zero();
static bool s_is_lock_held = false;
static thread_id_t s_lock_holder = -1;

static std::atomic<uint32_t> s_active_thread_count{1};
static std::atomic<bool> s_in_cs_atomic[MAX_TRACKED_THREADS];

static void updateAtomicStates_locked()
{
   uint32_t active_count = 0;
   for (const auto &kv : s_active_threads)
   {
      thread_id_t t = kv.first;
      if (kv.second && !s_blocked_threads[t])
      {
         active_count++;
      }
      if (t >= 0 && t < MAX_TRACKED_THREADS)
      {
         bool in_cs = s_has_pending_lock[t] && !s_deferred_acq[t] && !s_waiting_threads[t].is_waiting;
         s_in_cs_atomic[t].store(in_cs, std::memory_order_relaxed);
      }
   }
   s_active_thread_count.store(active_count > 0 ? active_count : 1, std::memory_order_relaxed);
}

uint32_t SyncTracker::getActiveThreadCount()
{
   return s_active_thread_count.load(std::memory_order_relaxed);
}

bool SyncTracker::isInCriticalSection(thread_id_t tid)
{
   if (tid >= 0 && tid < MAX_TRACKED_THREADS)
      return s_in_cs_atomic[tid].load(std::memory_order_relaxed);
   return false;
}

SubsecondTime SyncTracker::scaleTime(thread_id_t tid, SubsecondTime t)
{
   if (isInCriticalSection(tid))
      return t;
   uint32_t active = getActiveThreadCount();
   return (active > 1) ? (t / active) : t;
}

void SyncTracker::onThreadStart(thread_id_t tid, SubsecondTime time)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_thread_start_time[tid] = time;
   s_active_threads[tid] = true;
   s_blocked_threads[tid] = false;
   s_waiting_threads[tid] = { false, SubsecondTime::Zero(), 0 };
   s_has_pending_lock[tid] = false;
   s_is_slow_path[tid] = false;
   s_deferred_acq[tid] = false;
   s_lock_addr[tid] = 0;
   s_thread_instructions[tid].clear();
   if (tid >= 0 && tid < MAX_TRACKED_THREADS)
   {
      s_cs_start_d[tid].clear();
      s_cs_start_c[tid].clear();
   }
   if (s_lock_holder == tid)
   {
      s_is_lock_held = false;
      s_lock_holder = -1;
   }
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadExit(thread_id_t tid)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_active_threads[tid] = false;
   s_blocked_threads[tid] = false;
   s_waiting_threads[tid].is_waiting = false;
   s_has_pending_lock[tid] = false;
   s_is_slow_path[tid] = false;
   s_deferred_acq[tid] = false;
   s_lock_addr[tid] = 0;
   s_thread_instructions[tid].clear();
   if (s_lock_holder == tid)
   {
      s_is_lock_held = false;
      s_lock_holder = -1;
   }
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadBlock(thread_id_t tid)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_blocked_threads[tid] = true;
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadUnblock(thread_id_t tid)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_blocked_threads[tid] = false;
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadWaitStart(thread_id_t tid, SubsecondTime time, IntPtr lock_address)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_blocked_threads[tid] = true;
   s_waiting_threads[tid].is_waiting = true;
   s_waiting_threads[tid].wait_start = time;
   s_waiting_threads[tid].lock_address = lock_address;
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onThreadWaitEnd(thread_id_t tid, SubsecondTime time)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   s_blocked_threads[tid] = false;
   if (s_waiting_threads[tid].is_waiting)
   {
      if (time > s_waiting_threads[tid].wait_start)
      {
         s_completed_waits.push_back({ tid, s_waiting_threads[tid].lock_address, s_waiting_threads[tid].wait_start, time });
      }
      s_waiting_threads[tid].is_waiting = false;
   }
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

void SyncTracker::onLockAcquired(thread_id_t tid, SubsecondTime time, IntPtr lock_address)
{
   bool lock_held_by_other = isLockHeldByOther(tid, lock_address);

   pthread_mutex_lock(&s_sync_tracker_lock);
   s_lock_addr[tid] = lock_address;
   s_has_pending_lock[tid] = true;
   s_is_slow_path[tid] = true;
   s_thread_instructions[tid].clear();

   if (lock_held_by_other)
   {
      s_lock_acq_time[tid] = time;
      s_deferred_acq[tid] = true;
   }
   else
   {
      SubsecondTime acq_time = time;
      if (lock_address != 0 && s_last_unlock_by_addr.count(lock_address) && s_last_unlock_by_addr[lock_address] > acq_time)
         acq_time = s_last_unlock_by_addr[lock_address];

      s_is_lock_held = true;
      s_lock_holder = tid;
      s_lock_acq_time[tid] = acq_time;
      s_deferred_acq[tid] = false;
      Sim()->getStatsManager()->recordSyncEvent(tid, "end_lock_acquisition", acq_time.getNS(), lock_address);
   }
   updateAtomicStates_locked();
   pthread_mutex_unlock(&s_sync_tracker_lock);
}

bool SyncTracker::isLockHeldByOther(thread_id_t tid, IntPtr lock_address)
{
   pthread_mutex_lock(&s_sync_tracker_lock);
   bool held = false;
   if (lock_address != 0)
   {
      for (auto &kv : s_has_pending_lock)
      {
         if (kv.first != tid && kv.second && s_lock_addr[kv.first] == lock_address)
         {
            held = true;
            break;
         }
      }
   }
   else if (s_is_lock_held && s_lock_holder != tid)
   {
      held = true;
   }
   pthread_mutex_unlock(&s_sync_tracker_lock);
   return held;
}

void SyncTracker::onInstructionCommit(thread_id_t tid, Instruction *ins, SubsecondTime time, IntPtr mem_addr)
{
   if (!ins)
      return;

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
      fflush(stdout);
   }

   bool lock_held_by_other = is_cmpxchg ? isLockHeldByOther(tid, mem_addr) : false;

   pthread_mutex_lock(&s_sync_tracker_lock);

   if (is_cmpxchg)
   {
      // Fast path lock acquisition: thread acquires lock if it doesn't already hold one
      // and is not currently waiting in futex.
      bool is_waiting = s_waiting_threads.count(tid) && s_waiting_threads[tid].is_waiting;

      if (!s_has_pending_lock[tid] && !is_waiting)
      {
         s_lock_addr[tid] = mem_addr;
         s_has_pending_lock[tid] = true;
         s_is_slow_path[tid] = false;
         s_thread_instructions[tid].clear();

         if (lock_held_by_other)
         {
            // Another thread still holds this lock in simulated time (e.g. a few ns of clock skew
            // before its unlock commits); defer recording end_lock_acquisition until onUnlock.
            s_lock_acq_time[tid] = time;
            s_deferred_acq[tid] = true;
         }
         else
         {
            SubsecondTime acq_time = time;
            if (mem_addr != 0 && s_last_unlock_by_addr.count(mem_addr) && s_last_unlock_by_addr[mem_addr] > acq_time)
               acq_time = s_last_unlock_by_addr[mem_addr];

            s_is_lock_held = true;
            s_lock_holder = tid;
            s_lock_acq_time[tid] = acq_time;
            s_deferred_acq[tid] = false;
            printf("[FAST-PATH LOCK] Thread %d acquired lock (addr %p, t = %" PRIu64 " ns)\n",
                   tid, (void*)mem_addr, acq_time.getNS());
            fflush(stdout);
            Sim()->getStatsManager()->recordSyncEvent(tid, "end_lock_acquisition", acq_time.getNS(), mem_addr);
         }
         updateAtomicStates_locked();
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

   bool in_cs = s_has_pending_lock[tid] && !s_deferred_acq[tid] && !(s_waiting_threads.count(tid) && s_waiting_threads[tid].is_waiting);
   if (!in_cs && tid >= 0 && tid < MAX_TRACKED_THREADS)
   {
      s_cs_start_d[tid][ins] = ins->getPICS_d()->get_total_time();
      s_cs_start_c[tid][ins] = ins->getPICS_c()->get_total_time();
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

   if (s_deferred_acq[tid])
   {
      s_deferred_acq[tid] = false;
      if (final_addr != 0 && s_last_unlock_by_addr.count(final_addr) && s_last_unlock_by_addr[final_addr] > start_crit)
         start_crit = s_last_unlock_by_addr[final_addr];
      s_lock_acq_time[tid] = start_crit;
      printf("[FAST-PATH LOCK] Thread %d acquired lock (addr %p, t = %" PRIu64 " ns)\n",
             tid, (void*)final_addr, start_crit.getNS());
      fflush(stdout);
      Sim()->getStatsManager()->recordSyncEvent(tid, "end_lock_acquisition", start_crit.getNS(), final_addr);
   }

   SubsecondTime unlock_time = std::max(time, start_crit);
   s_has_pending_lock[tid] = false;
   s_is_slow_path[tid] = false;
   s_lock_addr[tid] = 0;
   s_last_global_unlock = unlock_time;
   if (final_addr != 0 && unlock_time > s_last_unlock_by_addr[final_addr])
      s_last_unlock_by_addr[final_addr] = unlock_time;

   Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", unlock_time.getNS(), final_addr);

   // If another thread already committed its lock acquisition on final_addr slightly ahead of our unlock,
   // activate its critical section starting at unlock_time.
   for (auto &kv : s_deferred_acq)
   {
      thread_id_t other_tid = kv.first;
      if (other_tid != tid && kv.second && s_has_pending_lock[other_tid] && s_lock_addr[other_tid] == final_addr)
      {
         kv.second = false;
         SubsecondTime acq_time = std::max(s_lock_acq_time[other_tid], unlock_time);
         s_lock_acq_time[other_tid] = acq_time;
         s_is_lock_held = true;
         s_lock_holder = other_tid;
         if (!s_is_slow_path[other_tid])
         {
            printf("[FAST-PATH LOCK] Thread %d acquired lock (addr %p, t = %" PRIu64 " ns)\n",
                   other_tid, (void*)final_addr, acq_time.getNS());
            fflush(stdout);
         }
         Sim()->getStatsManager()->recordSyncEvent(other_tid, "end_lock_acquisition", acq_time.getNS(), final_addr);
         break;
      }
   }

   updateAtomicStates_locked();

   if (unlock_time <= start_crit)
   {
      s_thread_instructions[tid].clear();
      pthread_mutex_unlock(&s_sync_tracker_lock);
      return;
   }

   // 1. Calculate total wait time across all other threads contending on final_addr during [start_crit, unlock_time]
   SubsecondTime total_wait = SubsecondTime::Zero();
   for (auto &kv : s_waiting_threads)
   {
      thread_id_t other_tid = kv.first;
      if (other_tid == tid)
         continue;

      WaitState &wstate = kv.second;
      if (wstate.is_waiting && (final_addr == 0 || wstate.lock_address == final_addr))
      {
         SubsecondTime overlap_start = std::max(wstate.wait_start, start_crit);
         if (unlock_time > overlap_start)
         {
            total_wait += (unlock_time - overlap_start);
         }
      }
   }

   for (const auto &iv : s_completed_waits)
   {
      if (iv.tid == tid)
         continue;
      if (final_addr != 0 && iv.lock_address != final_addr)
         continue;

      SubsecondTime overlap_start = std::max(iv.start, start_crit);
      SubsecondTime overlap_end = std::min(iv.end, unlock_time);
      if (overlap_end > overlap_start)
      {
         total_wait += (overlap_end - overlap_start);
      }
   }

   // 2. Collect unique instructions committed by thread 'tid' during this critical section
   std::deque<CommittedInst> &inst_buf = s_thread_instructions[tid];
   size_t cs_inst_count = inst_buf.size();

   std::vector<Instruction*> unique_insts;
   std::unordered_set<Instruction*> seen_insts;
   for (size_t i = 0; i < inst_buf.size(); ++i)
   {
      Instruction *ins = inst_buf[i].ins;
      if (ins && seen_insts.insert(ins).second)
      {
         unique_insts.push_back(ins);
      }
   }

   // 3. Compute each unique instruction's time spent in this critical section (dispatch & commit views)
   std::vector<SubsecondTime> d_deltas(unique_insts.size(), SubsecondTime::Zero());
   std::vector<SubsecondTime> c_deltas(unique_insts.size(), SubsecondTime::Zero());
   SubsecondTime total_cs_d = SubsecondTime::Zero();
   SubsecondTime total_cs_c = SubsecondTime::Zero();

   for (size_t i = 0; i < unique_insts.size(); ++i)
   {
      Instruction *ins = unique_insts[i];
      SubsecondTime d_tot = ins->getPICS_d()->get_total_time();
      SubsecondTime c_tot = ins->getPICS_c()->get_total_time();
      SubsecondTime d_base = (tid >= 0 && tid < MAX_TRACKED_THREADS) ? s_cs_start_d[tid][ins] : SubsecondTime::Zero();
      SubsecondTime c_base = (tid >= 0 && tid < MAX_TRACKED_THREADS) ? s_cs_start_c[tid][ins] : SubsecondTime::Zero();

      d_deltas[i] = (d_tot > d_base) ? (d_tot - d_base) : SubsecondTime::Zero();
      c_deltas[i] = (c_tot > c_base) ? (c_tot - c_base) : SubsecondTime::Zero();
      total_cs_d += d_deltas[i];
      total_cs_c += c_deltas[i];

      if (tid >= 0 && tid < MAX_TRACKED_THREADS)
      {
         s_cs_start_d[tid][ins] = d_tot;
         s_cs_start_c[tid][ins] = c_tot;
      }
   }

   // 4. Attribute total_wait time-proportionally to each instruction's critical section time
   if (total_wait > SubsecondTime::Zero() && !unique_insts.empty())
   {
      printf("[PICS MT-WAIT] Thread %d CS [%" PRIu64 " - %" PRIu64 " ns]: %zu dyn instrs (%zu static), W_total = %" PRIu64 " ns (CS_d = %" PRIu64 " ns, CS_c = %" PRIu64 " ns)\n",
             tid, start_crit.getNS(), unlock_time.getNS(), cs_inst_count, unique_insts.size(),
             total_wait.getNS(), total_cs_d.getNS(), total_cs_c.getNS());
      fflush(stdout);

      for (size_t i = 0; i < unique_insts.size(); ++i)
      {
         Instruction *ins = unique_insts[i];
         double frac_d = (total_cs_d > SubsecondTime::Zero())
                         ? (static_cast<double>(d_deltas[i].getFS()) / static_cast<double>(total_cs_d.getFS()))
                         : (1.0 / static_cast<double>(unique_insts.size()));
         double frac_c = (total_cs_c > SubsecondTime::Zero())
                         ? (static_cast<double>(c_deltas[i].getFS()) / static_cast<double>(total_cs_c.getFS()))
                         : (1.0 / static_cast<double>(unique_insts.size()));

         SubsecondTime wait_d = SubsecondTime::FS(static_cast<uint64_t>(std::llround(static_cast<double>(total_wait.getFS()) * frac_d)));
         SubsecondTime wait_c = SubsecondTime::FS(static_cast<uint64_t>(std::llround(static_cast<double>(total_wait.getFS()) * frac_c)));

         ins->getPICS_d()->add_mt_wait(wait_d);
         ins->getPICS_c()->add_mt_wait(wait_c);
      }
   }

   // 5. Clear buffered instructions for this thread up to this unlock
   inst_buf.clear();

   pthread_mutex_unlock(&s_sync_tracker_lock);
}

