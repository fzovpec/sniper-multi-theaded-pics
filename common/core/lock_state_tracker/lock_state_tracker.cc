#include "lock_state_tracker.h"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"
#include "simulator.h"
#include "stats.h"
#include "thread_manager.h"
#include "config.hpp"
#include <inttypes.h>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <algorithm>

LockStateTracker* LockStateTracker::create()
{
   if (!Sim()->getCfg()->getBool("core/lock_state_tracking"))
      return NULL;

   SInt64 rounds = Sim()->getCfg()->getInt("core/lock_state_tracker/lock_detection_rounds");
   bool log_events = Sim()->getCfg()->getBool("core/lock_state_tracker/log_events");
   double stuck_timeout = Sim()->getCfg()->getFloat("core/lock_state_tracker/stuck_timeout");
   double lock_demote_delay = Sim()->getCfg()->getFloat("core/lock_state_tracker/lock_demote_delay");
   return new LockStateTracker(rounds > 1 ? rounds : 1, log_events, stuck_timeout, lock_demote_delay);
}

LockStateTracker::LockStateTracker(UInt64 lock_detection_rounds, bool log_events, double stuck_timeout, double lock_demote_delay)
   : m_lock_detection_rounds(lock_detection_rounds),
     m_log_events(log_events),
     m_stuck_timeout(stuck_timeout),
     m_lock_demote_delay(lock_demote_delay),
     m_total_acquires(0),
     m_total_unlocks(0),
     m_total_held_time(SubsecondTime::Zero()),
     m_total_held_insns(0)
{
   printf("[LOCK_STATE_TRACKER] Initialized Lock State Tracker (lock after %" PRIu64 " acquire/release rounds)%s\n", m_lock_detection_rounds,
          m_log_events ? "" : ", per-event lines off");
}

LockStateTracker::~LockStateTracker()
{
   printf("\n============= [Lock State Tracker Final Summary] =============\n");
   printf("  Unique Lock Addresses Tracked:     %zu\n", m_locks.size() + m_sync_vars.size());
   for (LockMap::iterator it = m_locks.begin(); it != m_locks.end(); ++it)
   {
      printf("    - Lock Address: 0x%lx (%u bytes)\n", (unsigned long)it->first, it->second.size);
   }
   for (SyncVariableMap::iterator it = m_sync_vars.begin(); it != m_sync_vars.end(); ++it)
   {
      printf("    - Sync Variable Address: 0x%lx (%u bytes)\n", (unsigned long)it->first, it->second.size);
   }
   printf("  Total Lock Acquires Detected:      %" PRIu64 "\n", m_total_acquires);
   printf("  Total Lock Unlocks Detected:       %" PRIu64 "\n", m_total_unlocks);
   if (m_total_unlocks > 0)
   {
      printf("  Total Critical Section Duration:   %" PRIu64 " ns\n", m_total_held_time.getNS());
      printf("  Average Critical Section Duration: %" PRIu64 " ns (%" PRIu64 " instructions)\n",
             m_total_held_time.getNS() / m_total_unlocks,
             m_total_held_insns / m_total_unlocks);
   }
   printf("==============================================================\n\n");
   fflush(stdout);
}

IntPtr LockStateTracker::getPC(const DynamicMicroOp *uop)
{
   const Instruction *ins = uop->getMicroOp()->getInstruction();
   if (!ins)
      return uop->getMicroOp()->getInstructionPointer().phys;
   return ins->getVaddress() != 0 ? ins->getVaddress() : ins->getAddress();
}

// Find the entry of map overlapping [addr, addr + size)
template <typename Map>
static typename Map::iterator findOverlap(Map &map, IntPtr addr, UInt32 size)
{
   if (size == 0)
      size = 1;

   typename Map::iterator it = map.upper_bound(addr + size - 1);
   if (it == map.begin())
      return map.end();
   --it;
   if (it->first + it->second.size > addr)
      return it;
   return map.end();
}

// Find the known lock overlapping [addr, addr + size). Must be called with m_lock held.
LockStateTracker::LockMap::iterator LockStateTracker::findLock(IntPtr addr, UInt32 size)
{
   return findOverlap(m_locks, addr, size);
}

// Find the known sync variable overlapping [addr, addr + size). Must be called with m_lock held.
LockStateTracker::SyncVariableMap::iterator LockStateTracker::findSyncVariable(IntPtr addr, UInt32 size)
{
   return findOverlap(m_sync_vars, addr, size);
}

void LockStateTracker::setHolder(LockState &lock, thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount)
{
   lock.holder = tid;
   lock.released = false;
   lock.from_spin = false;
   lock.acq_pc = pc;
   lock.acq_time = time;
   lock.acq_icount = icount;
   lock.contenders.erase(tid);
   lock.sync.pass(tid, pc, time, icount);   // creates tid's waiter state from its earlier waits if needed   // keep the sync record level, in case the lock is demoted
   m_lock_stalled.erase(tid);
   m_total_acquires++;
}

void LockStateTracker::logEvent(IntPtr base, bool per_event, const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);

   std::deque<std::string> &recent = m_recent[base];
   recent.push_back(buf);
   if (recent.size() > RECENT_EVENTS)
      recent.pop_front();

   if (!per_event || m_log_events)
   {
      fputs(buf, stdout);
      fflush(stdout);
   }
}

double LockStateTracker::heldFor(std::unordered_map<thread_id_t, std::chrono::steady_clock::time_point> &since, thread_id_t tid)
{
   std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
   std::unordered_map<thread_id_t, std::chrono::steady_clock::time_point>::iterator f = since.find(tid);
   if (f == since.end())
   {
      since[tid] = now;
      return 0;
   }
   return std::chrono::duration<double>(now - f->second).count();
}

void LockStateTracker::dumpState(IntPtr base, const char *title, thread_id_t tid, double seconds, SubsecondTime time)
{
   printf("\n######################## [Lock State Tracker] %s ########################\n", title);
   printf("  Variable 0x%lx, Thread %d, for %.1f s wall clock, at T=%" PRIu64 " ns\n", (unsigned long)base, tid, seconds, time.getNS());

   const SyncVariable *var = NULL;
   LockMap::iterator it = m_locks.find(base);
   SyncVariableMap::iterator sv = m_sync_vars.find(base);
   if (it != m_locks.end())
   {
      var = &it->second.sync;
      printf("  It is a LOCK: holder %d, released %d, contenders:", it->second.holder, it->second.released);
      for (std::unordered_set<thread_id_t>::const_iterator c = it->second.contenders.begin(); c != it->second.contenders.end(); ++c)
         printf(" %d", *c);
      printf("\n");
   }
   else if (sv != m_sync_vars.end())
   {
      var = &sv->second;
      printf("  It is a SYNC VARIABLE%s\n", var->not_a_lock ? " (NOT a lock)" : "");
   }

   if (var)
   {
      printf("  Writes: %" PRIu64 " in total;", var->stores_total);
      for (std::unordered_map<thread_id_t, UInt64>::const_iterator w = var->stores_by.begin(); w != var->stores_by.end(); ++w)
         printf(" Thread %d: %" PRIu64 ";", w->first, w->second);
      printf("\n");
      for (std::unordered_map<thread_id_t, SyncVariable::WaiterState>::const_iterator w = var->waiters.begin(); w != var->waiters.end(); ++w)
         printf("  Waiter Thread %d: waits used %" PRIu64 ", writes by others %" PRIu64 ", current wait passed %d, last pass PC 0x%lx at T=%" PRIu64 " ns\n",
                w->first, w->second.consumed, var->otherWrites(w->first), w->second.passed, (unsigned long)w->second.pass_pc, w->second.pass_time.getNS());
   }

   ThreadManager *tm = Sim()->getThreadManager();
   for (thread_id_t t = 0; t < (thread_id_t)tm->getNumThreads(); ++t)
   {
      printf("  Thread %d: %s", t, tm->isThreadRunning(t) ? "running" : (tm->isThreadInitializing(t) ? "initializing" : "not running (blocked or finished)"));
      if (m_stalled.count(t))
         printf(", waiting for a write on a sync variable");
      std::unordered_map<thread_id_t, IntPtr>::const_iterator l = m_lock_stalled.find(t);
      if (l != m_lock_stalled.end())
         printf(", refused by lock 0x%lx", (unsigned long)l->second);
      std::unordered_map<thread_id_t, IntPtr>::const_iterator sp = m_spinning.find(t);
      if (sp != m_spinning.end())
         printf(", spinning on 0x%lx", (unsigned long)sp->second);
      std::unordered_map<thread_id_t, IntPtr>::const_iterator pc = m_last_check_pc.find(t);
      if (pc != m_last_check_pc.end())
         printf(", last check PC 0x%lx", (unsigned long)pc->second);
      printf("\n");
   }

   printf("  Last events of 0x%lx:\n", (unsigned long)base);
   std::map<IntPtr, std::deque<std::string> >::const_iterator r = m_recent.find(base);
   if (r != m_recent.end())
      for (std::deque<std::string>::const_iterator e = r->second.begin(); e != r->second.end(); ++e)
         printf("    %s", e->c_str());
   printf("###########################################################################################\n\n");
   fflush(stdout);
}

void LockStateTracker::reportStuck(IntPtr var_base, thread_id_t tid, const char *what, double seconds, SubsecondTime time)
{
   ScopedLock sl(m_lock);
   dumpState(var_base, what, tid, seconds, time);
   LOG_PRINT_ERROR("Lock State Tracker: Thread %d is stuck on 0x%lx: %s (see the dump above)", tid, (unsigned long)var_base, what);
}

void LockStateTracker::provideEarlierWaits(thread_id_t tid, IntPtr var_base, UInt64 waits)
{
   ScopedLock sl(m_lock);

   SyncVariable *var = NULL;
   SyncVariableMap::iterator sv = m_sync_vars.find(var_base);
   if (sv != m_sync_vars.end())
      var = &sv->second;
   else
   {
      LockMap::iterator it = m_locks.find(var_base);
      if (it != m_locks.end())
         var = &it->second.sync;
   }
   if (!var || var->waiters.count(tid) > 0 || var->earlier_waits.count(tid) > 0)
      return;

   var->earlier_waits[tid] = waits;
   logEvent(var_base, false, "[Lock State Tracker] Thread %d starts on 0x%lx: %" PRIu64 " earlier waits (counted by its frontend), %" PRIu64 " writes by others so far\n",
            tid, (unsigned long)var_base, waits, var->otherWrites(tid));
}

bool LockStateTracker::isSpinningOn(thread_id_t tid, IntPtr var_base)
{
   ScopedLock sl(m_lock);
   std::unordered_map<thread_id_t, IntPtr>::iterator s = m_spinning.find(tid);
   return s != m_spinning.end() && s->second == var_base;
}

void LockStateTracker::undoWait(thread_id_t tid, IntPtr mem_addr, IntPtr read_pc, IntPtr store_pc, SubsecondTime time)
{
   ScopedLock sl(m_lock);

   SyncVariableMap::iterator sv = findSyncVariable(mem_addr, 1);
   if (sv == m_sync_vars.end())
      return;
   std::unordered_map<thread_id_t, SyncVariable::WaiterState>::iterator w = sv->second.waiters.find(tid);
   if (w == sv->second.waiters.end())
      return;

   if (w->second.used_write && w->second.consumed > 0)
      w->second.consumed--;
   w->second.used_write = false;
   w->second.passed = false;
   m_stalled.erase(tid);
   m_stuck_since.erase(tid);

   logEvent(sv->first, true, "[Lock State Tracker] Thread %d: read of Sync Variable 0x%lx at PC 0x%lx is a read-modify-write (store at PC 0x%lx), not a wait (T=%" PRIu64 " ns)\n",
            tid, (unsigned long)sv->first, (unsigned long)read_pc, (unsigned long)store_pc, time.getNS());
}

bool LockStateTracker::holderIsDeadlocked(const LockState &lock, thread_id_t tid)
{
   if (lock.holder == INVALID_THREAD_ID || lock.holder == tid)
      return false;
   // A holder that is blocked or sleeping (not waiting on a tracked variable) can still release the lock
   if (m_stalled.count(lock.holder) == 0 && m_lock_stalled.count(lock.holder) == 0)
      return false;
   return !canBeSignalled(tid);
}

bool LockStateTracker::canBeSignalled(thread_id_t tid)
{
   ThreadManager *tm = Sim()->getThreadManager();
   for (thread_id_t t = 0; t < (thread_id_t)tm->getNumThreads(); ++t)
   {
      if (t != tid && m_stalled.count(t) == 0 && m_lock_stalled.count(t) == 0 && (tm->isThreadRunning(t) || tm->isThreadInitializing(t)))
         return true;
   }
   return false;
}

const char *LockStateTracker::observe(SyncVariable &var, thread_id_t tid, PatternEvent ev)
{
   // The first event of a thread can be the release of an acquire made before the variable was tracked
   if (ev == PLAIN_STORE && var.holding.find(tid) == var.holding.end())
   {
      var.holding[tid] = false;
      var.pending_wait[tid] = false;
      return NULL;
   }

   bool &holding = var.holding[tid];
   bool &pending = var.pending_wait[tid];

   switch (ev)
   {
      case WAIT_START:
         if (!holding)
            holding = true;
         else if (!pending)
            pending = true;   // the frontend runs ahead of commit: the previous release may not have committed yet
         else
            return "waits again without having written it in between";
         break;

      case ATOMIC_STORE:
         if (!holding)
            holding = true;   // acquire without a wait (uncontended test-and-set); when holding, the acquire's own store
         break;

      case PLAIN_STORE:
         if (!holding)
            return "writes it without having waited on it or acquired it";
         holding = pending;   // released; a wait that started before this release committed is the next acquire
         pending = false;
         if (!var.not_a_lock)
            var.lock_rounds++;
         break;
   }
   return NULL;
}

void LockStateTracker::notALock(IntPtr base, SyncVariable &var, thread_id_t tid, const char *reason, SubsecondTime time)
{
   if (!var.not_a_lock)
   {
      var.not_a_lock = true;
      logEvent(base, false, "[Lock State Tracker] 0x%lx is NOT a lock: Thread %d %s (T=%" PRIu64 " ns)\n",
             (unsigned long)base, tid, reason, time.getNS());
   }

   LockMap::iterator it = m_locks.find(base);
   if (it != m_locks.end())
      demote(it, time);
}

LockStateTracker::SyncVariableMap::iterator LockStateTracker::demote(LockMap::iterator it, SubsecondTime time)
{
   IntPtr base = it->first;
   LockState &lock = it->second;

   // Close the current holder's critical section in the timeline
   if (lock.holder != INVALID_THREAD_ID && Sim()->getStatsManager())
      Sim()->getStatsManager()->recordSyncEvent(lock.holder, "unlock", time.getNS(), base);

   SyncVariable var = lock.sync;
   var.not_a_lock = true;

   // Threads kept spinning one by one on the lock are now waiters of a sync variable: one write lets all of them through
   for (std::unordered_map<thread_id_t, IntPtr>::iterator s = m_lock_stalled.begin(); s != m_lock_stalled.end(); )
   {
      if (s->second == base)
         s = m_lock_stalled.erase(s);
      else
         ++s;
   }

   logEvent(base, false, "[Lock State Tracker] Lock 0x%lx is a sync variable again (T=%" PRIu64 " ns)\n", (unsigned long)base, time.getNS());

   m_locks.erase(it);
   return m_sync_vars.insert(std::make_pair(base, var)).first;
}

void LockStateTracker::promote(SyncVariableMap::iterator sv, thread_id_t tid, SubsecondTime time)
{
   SyncVariable &var = sv->second;

   logEvent(sv->first, false, "[Lock State Tracker] 0x%lx is a LOCK: %" PRIu64 " acquire/release rounds without a violation, last released by Thread %d at T=%" PRIu64 " ns\n",
          (unsigned long)sv->first, var.lock_rounds, tid, time.getNS());

   // A thread inside its critical section right now: acquired by its pattern and past its spin (its current wait passed)
   std::vector<thread_id_t> inside;
   for (std::unordered_map<thread_id_t, bool>::const_iterator h = var.holding.begin(); h != var.holding.end(); ++h)
   {
      if (!h->second || m_stalled.count(h->first) > 0)
         continue;
      std::unordered_map<thread_id_t, SyncVariable::WaiterState>::const_iterator w = var.waiters.find(h->first);
      if (w != var.waiters.end() && w->second.passed)
         inside.push_back(h->first);
   }

   // Promoted on a release: the lock starts free, or held by the one thread inside its critical section;
   // the threads that spun on it are its contenders
   IntPtr base = sv->first;
   LockState &lock = m_locks.insert(std::make_pair(base, LockState())).first->second;
   lock.size = var.size;
   lock.released = true;
   lock.contenders = var.spinners;
   lock.sync = var;

   if (inside.size() == 1)
   {
      thread_id_t t = inside[0];
      const SyncVariable::WaiterState &w = var.waiters[t];
      lock.holder = t;
      lock.released = false;
      lock.from_spin = true;
      lock.acq_pc = w.pass_pc;
      lock.acq_time = w.pass_time;
      lock.acq_icount = w.pass_icount;
      lock.contenders.erase(t);
      m_total_acquires++;
      logEvent(base, false, "[Lock State Tracker] Thread %d holds Lock 0x%lx at promotion (acquired at T=%" PRIu64 " ns, PC 0x%lx)\n",
               t, (unsigned long)base, w.pass_time.getNS(), (unsigned long)w.pass_pc);
   }
   else if (inside.size() > 1)
      dumpState(base, "PROMOTION: more than one thread inside the critical section, the lock starts free", tid, 0, time);

   for (std::unordered_map<thread_id_t, SyncVariable::WaiterState>::iterator o = var.waiters.begin(); o != var.waiters.end(); ++o)
      m_stalled.erase(o->first);
   m_sync_vars.erase(sv);
}

bool LockStateTracker::claimSync(SyncVariable &var, IntPtr var_addr, thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount, bool new_wait, bool in_replay)
{
   SyncVariable::WaiterState &w = var.waiter(tid);
   if (new_wait)
   {
      w.passed = false;
      w.used_write = false;
   }
   m_last_check_pc[tid] = pc;
   if (w.passed)
      return true;

   if (var.canPass(tid))
   {
      m_stalled.erase(tid);
      m_stuck_since.erase(tid);
      var.pass(tid, pc, time, icount);
      logEvent(var_addr, true, "[Lock State Tracker] Thread %d PASSED Sync Variable 0x%lx at T=%" PRIu64 " ns (wait %" PRIu64 ", writes seen %" PRIu64 ") | PC 0x%lx%s\n",
               tid, (unsigned long)var_addr, time.getNS(), var.waiters[tid].consumed, var.otherWrites(tid), (unsigned long)pc,
               in_replay ? " (in a replay)" : "");
      return true;
   }

   if (m_stalled.insert(tid).second)
      logEvent(var_addr, true, "[Lock State Tracker] Thread %d WAITING on Sync Variable 0x%lx at T=%" PRIu64 " ns (wait %" PRIu64 ") | PC 0x%lx\n",
               tid, (unsigned long)var_addr, time.getNS(), var.waiters[tid].consumed + 1, (unsigned long)pc);

   // No pass without a write: a wait that no thread can end anymore means the tracker lost count. Every other thread
   // waiting (or blocked, or finished) can last a moment (a write still in flight); for stuck_timeout it is fatal.
   if (m_stuck_timeout > 0)
   {
      if (canBeSignalled(tid))
         m_stuck_since.erase(tid);
      else
      {
         double held = heldFor(m_stuck_since, tid);
         if (held >= m_stuck_timeout)
         {
            dumpState(var_addr, "STUCK: waits for a write that no thread can make", tid, held, time);
            LOG_PRINT_ERROR("Lock State Tracker: Thread %d waits on Sync Variable 0x%lx for a write that no thread can make (see the dump above)",
                            tid, (unsigned long)var_addr);
         }
      }
   }
   return false;
}

void LockStateTracker::onSpinStart(thread_id_t tid, IntPtr lock_addr, UInt32 size, IntPtr load_pc, const std::unordered_map<thread_id_t, UInt64> &store_counts)
{
   ScopedLock sl(m_lock);

   m_spin_load_pcs.insert(load_pc);

   LockMap::iterator it = findLock(lock_addr, size);
   if (it == m_locks.end())
   {
      SyncVariableMap::iterator sv = findSyncVariable(lock_addr, size);
      if (sv == m_sync_vars.end())
      {
         sv = m_sync_vars.insert(std::make_pair(lock_addr, SyncVariable(size ? size : 1))).first;

         // Writes made before the variable was found count as well
         for (std::unordered_map<thread_id_t, UInt64>::const_iterator s = store_counts.begin(); s != store_counts.end(); ++s)
         {
            sv->second.stores_by[s->first] = s->second;
            sv->second.stores_total += s->second;
         }

         logEvent(lock_addr, false, "[Lock State Tracker] Tracking sync variable 0x%lx (%u bytes), %" PRIu64 " writes so far | spinning load PC 0x%lx (Thread %d)\n",
                (unsigned long)lock_addr, sv->second.size, sv->second.stores_total, (unsigned long)load_pc, tid);
      }

      // The waiter state of each thread is created at its first check, from the earlier waits its own frontend counted
      sv->second.spinners.insert(tid);
      m_spinning[tid] = sv->first;
      return;
   }

   LockState &lock = it->second;

   lock.sync.spinners.insert(tid);

   // A holder that left its spin loop really holds the lock: this is a late report of spin loads committing after the claim
   if (lock.holder == tid && lock.from_spin)
      return;

   // A holder does not spin on its own lock: its atomic store was an attempt, not an acquire
   if (lock.holder == tid)
   {
      lock.holder = INVALID_THREAD_ID;
      m_total_acquires--;
      logEvent(it->first, false, "[Lock State Tracker] Thread %d is spinning on Lock 0x%lx, no longer its holder\n", tid, (unsigned long)it->first);
   }

   lock.contenders.insert(tid);
   m_spinning[tid] = it->first;
}

void LockStateTracker::onSpinEnd(thread_id_t tid)
{
   ScopedLock sl(m_lock);
   m_spinning.erase(tid);
}

bool LockStateTracker::isSpinning(thread_id_t tid)
{
   ScopedLock sl(m_lock);
   std::unordered_map<thread_id_t, IntPtr>::iterator spin_it = m_spinning.find(tid);
   return spin_it != m_spinning.end() && m_locks.count(spin_it->second) > 0;
}

bool LockStateTracker::isLockFree(thread_id_t tid)
{
   ScopedLock sl(m_lock);

   std::unordered_map<thread_id_t, IntPtr>::iterator spin_it = m_spinning.find(tid);
   if (spin_it == m_spinning.end())
      return false;

   // Sync variable: the wait is over once a write by another thread is there for it
   SyncVariableMap::iterator sv = m_sync_vars.find(spin_it->second);
   if (sv != m_sync_vars.end())
      return sv->second.canPass(tid);

   LockMap::iterator it = m_locks.find(spin_it->second);
   if (it == m_locks.end())
      return false;

   const LockState &lock = it->second;

   if (lock.holder != INVALID_THREAD_ID || !lock.released)
      return false;

   // A contender that is not seen spinning may take the lock without the tracker noticing
   for (std::unordered_set<thread_id_t>::const_iterator c = lock.contenders.begin(); c != lock.contenders.end(); ++c)
   {
      std::unordered_map<thread_id_t, IntPtr>::const_iterator s = m_spinning.find(*c);
      if (s == m_spinning.end() || s->second != it->first)
         return false;
   }
   return true;
}

bool LockStateTracker::tryClaim(thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount)
{
   ScopedLock sl(m_lock);

   std::unordered_map<thread_id_t, IntPtr>::iterator spin_it = m_spinning.find(tid);
   if (spin_it == m_spinning.end())
      return true;

   SyncVariableMap::iterator sv = m_sync_vars.find(spin_it->second);
   if (sv != m_sync_vars.end())
      return claimSync(sv->second, sv->first, tid, pc, time, icount, false);   // the end of the wait the thread is spinning in

   LockMap::iterator it = m_locks.find(spin_it->second);
   if (it == m_locks.end())
      return true;

   LockState &lock = it->second;

   if (lock.holder == tid)
      return true;
   if (lock.holder != INVALID_THREAD_ID)
   {
      // A real lock's holder is running in its critical section (or blocked in it, and will wake up): if the holder is
      // itself waiting and so is every other thread, for lock_demote_delay, this is not a lock
      m_last_check_pc[tid] = pc;
      if (holderIsDeadlocked(lock, tid))
      {
         double held = heldFor(m_refused_since, tid);
         if (held >= m_lock_demote_delay)
         {
            IntPtr base = it->first;
            m_refused_since.erase(tid);
            dumpState(base, "LOCK DEMOTED: refused while its holder and every other thread wait", tid, held, time);
            notALock(base, lock.sync, tid, "is refused while the holder and every other thread wait", time);
            sv = m_sync_vars.find(base);
            return claimSync(sv->second, sv->first, tid, pc, time, icount, false);
         }
      }
      else
         m_refused_since.erase(tid);
      m_lock_stalled[tid] = it->first;
      return false;
   }

   setHolder(lock, tid, pc, time, icount);
   lock.from_spin = true;

   logEvent(it->first, true, "[Lock State Tracker] Thread %d ACQUIRED Lock 0x%lx at T=%" PRIu64 " ns | Acq PC 0x%lx (from spin)\n",
             tid, (unsigned long)it->first, time.getNS(), (unsigned long)pc);
   return true;
}

bool LockStateTracker::isWaitLoad(IntPtr pc, IntPtr mem_addr)
{
   ScopedLock sl(m_lock);
   if (m_spin_load_pcs.count(pc) > 0)
      return true;

   return findSyncVariable(mem_addr, 1) != m_sync_vars.end();
}

bool LockStateTracker::isSyncVariable(IntPtr mem_addr)
{
   ScopedLock sl(m_lock);
   return findSyncVariable(mem_addr, 1) != m_sync_vars.end();
}

bool LockStateTracker::isSpinLoad(IntPtr pc)
{
   ScopedLock sl(m_lock);
   return m_spin_load_pcs.count(pc) > 0;
}

void LockStateTracker::onWaitStart(thread_id_t tid, IntPtr var_base, IntPtr pc, SubsecondTime time)
{
   ScopedLock sl(m_lock);

   if (m_spin_load_pcs.count(pc) == 0)
      return;

   SyncVariableMap::iterator sv = m_sync_vars.find(var_base);
   if (sv != m_sync_vars.end())
   {
      const char *violation = observe(sv->second, tid, WAIT_START);
      if (violation)
         notALock(sv->first, sv->second, tid, violation, time);
      return;
   }

   LockMap::iterator it = m_locks.find(var_base);
   if (it != m_locks.end())
   {
      const char *violation = observe(it->second.sync, tid, WAIT_START);
      if (violation)
         notALock(it->first, it->second.sync, tid, violation, time);
   }
}

bool LockStateTracker::tryPassSync(thread_id_t tid, IntPtr mem_addr, IntPtr pc, SubsecondTime time, UInt64 icount, bool new_wait, bool in_replay)
{
   ScopedLock sl(m_lock);

   SyncVariableMap::iterator sv = findSyncVariable(mem_addr, 1);
   if (sv == m_sync_vars.end())
      return true;

   return claimSync(sv->second, sv->first, tid, pc, time, icount, new_wait, in_replay);
}

IntPtr LockStateTracker::getVariableBase(IntPtr mem_addr)
{
   ScopedLock sl(m_lock);

   SyncVariableMap::iterator sv = findSyncVariable(mem_addr, 1);
   if (sv != m_sync_vars.end())
      return sv->first;

   LockMap::iterator it = findLock(mem_addr, 1);
   if (it != m_locks.end())
      return it->first;

   return 0;
}

bool LockStateTracker::tryClaimLoad(thread_id_t tid, IntPtr mem_addr, IntPtr pc, SubsecondTime time, UInt64 icount)
{
   ScopedLock sl(m_lock);

   SyncVariableMap::iterator sv = findSyncVariable(mem_addr, 1);
   if (sv != m_sync_vars.end())
      return claimSync(sv->second, sv->first, tid, pc, time, icount, false);

   LockMap::iterator it = findLock(mem_addr, 1);
   if (it == m_locks.end())
      return true;

   LockState &lock = it->second;

   if (lock.holder == tid)
      return true;

   if (lock.holder != INVALID_THREAD_ID)
   {
      // A real lock's holder is running in its critical section (or blocked in it, and will wake up): if the holder is
      // itself waiting and so is every other thread, for lock_demote_delay, this is not a lock
      m_last_check_pc[tid] = pc;
      if (holderIsDeadlocked(lock, tid))
      {
         double held = heldFor(m_refused_since, tid);
         if (held >= m_lock_demote_delay)
         {
            IntPtr base = it->first;
            m_refused_since.erase(tid);
            dumpState(base, "LOCK DEMOTED: refused while its holder and every other thread wait", tid, held, time);
            notALock(base, lock.sync, tid, "is refused while the holder and every other thread wait", time);
            sv = m_sync_vars.find(base);
            return claimSync(sv->second, sv->first, tid, pc, time, icount, false);
         }
      }
      else
         m_refused_since.erase(tid);
      m_lock_stalled[tid] = it->first;
      if (lock.contenders.insert(tid).second)
      {
         logEvent(it->first, true, "[Lock State Tracker] Thread %d JOINED contenders of Lock 0x%lx (held by Thread %d) at T=%" PRIu64 " ns\n",
                   tid, (unsigned long)it->first, lock.holder, time.getNS());
      }
      return false;
   }

   setHolder(lock, tid, pc, time, icount);
   lock.from_spin = true;

   logEvent(it->first, true, "[Lock State Tracker] Thread %d ACQUIRED Lock 0x%lx at T=%" PRIu64 " ns | Acq PC 0x%lx (no detected spin)\n",
             tid, (unsigned long)it->first, time.getNS(), (unsigned long)pc);

   if (Sim()->getStatsManager())
   {
      Sim()->getStatsManager()->recordSyncEvent(tid, "lock", time.getNS(), it->first);
   }
   return true;
}

void LockStateTracker::onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time)
{
   if (!uop || !uop->getMicroOp())
      return;

   // Acquires and unlocks are always STORE operations
   if (!uop->getMicroOp()->isStore())
      return;

   IntPtr mem_addr = uop->getAddress().address;
   if (mem_addr == 0)
      return;

   const Instruction *ins = uop->getMicroOp()->getInstruction();
   IntPtr pc = getPC(uop);
   bool is_atomic = ins ? ins->isAtomic() : false;

   ScopedLock sl(m_lock);

   UInt32 access_size = uop->getMicroOp()->getMemoryAccessSize();
   LockMap::iterator it;

   PatternEvent ev = is_atomic ? ATOMIC_STORE : PLAIN_STORE;

   // 0. Sync variable: every write is a signal for the threads waiting on it, and one more step of the writer's
   //    lock pattern. After enough acquire -> release rounds without a violation it becomes a lock.
   SyncVariableMap::iterator sv = findSyncVariable(mem_addr, access_size);
   if (sv != m_sync_vars.end())
   {
      SyncVariable &var = sv->second;
      var.stores_total++;
      var.stores_by[tid]++;

      logEvent(sv->first, true, "[Lock State Tracker] Core %d (Thread %d) WROTE Sync Variable 0x%lx at T=%" PRIu64 " ns (write %" PRIu64 ") | PC 0x%lx\n",
                core->getId(), tid, (unsigned long)sv->first, time.getNS(), var.stores_total, (unsigned long)pc);

      const char *violation = observe(var, tid, ev);
      if (violation)
         notALock(sv->first, var, tid, violation, time);
      else if (ev == PLAIN_STORE && !var.not_a_lock && var.lock_rounds >= m_lock_detection_rounds)
         promote(sv, tid, time);
      return;
   }

   it = findLock(mem_addr, access_size);
   if (it == m_locks.end())
      return;

   // Lock: keep its sync record up to date, and drop back to a sync variable if the writer breaks the lock pattern
   it->second.sync.stores_total++;
   it->second.sync.stores_by[tid]++;
   {
      const char *violation = observe(it->second.sync, tid, ev);
      if (violation)
      {
         notALock(it->first, it->second.sync, tid, violation, time);
         return;
      }
   }

   IntPtr lock_addr = it->first;
   LockState &lock = it->second;
   UInt64 cur_icount = core->getInstructionCount();

   // 1. Store by the holder: this is the unlock, unless it is the holder's own acquire store committing
   if (lock.holder == tid)
   {
      if (is_atomic && m_acquire_pcs.count(pc) > 0)
         return;

      SubsecondTime held_time = (time > lock.acq_time) ? (time - lock.acq_time) : SubsecondTime::Zero();
      UInt64 held_insns = (cur_icount > lock.acq_icount) ? (cur_icount - lock.acq_icount) : 0;

      m_total_unlocks++;
      m_total_held_time += held_time;
      m_total_held_insns += held_insns;

      logEvent(lock_addr, true, "[Lock State Tracker] Core %d (Thread %d) UNLOCKED Lock 0x%lx at T=%" PRIu64 " ns (Acquired at T=%" PRIu64 " ns) | Held: %" PRIu64 " ns (%" PRIu64 " insns) | Acq PC 0x%lx -> Unlock PC 0x%lx\n",
                core->getId(), tid, (unsigned long)lock_addr, time.getNS(), lock.acq_time.getNS(), held_time.getNS(), held_insns,
                (unsigned long)lock.acq_pc, (unsigned long)pc);

      // Record unlock event in SQLite stats for timeline plotting
      if (Sim()->getStatsManager())
      {
         Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", time.getNS(), lock_addr);
      }

      // No successor is chosen here: the next holder is the thread whose trace leaves its spin loop first
      lock.holder = INVALID_THREAD_ID;
      lock.released = true;
      return;
   }

   // 2. Atomic RMW store by a non-holder: acquire if the lock is free, otherwise join the contenders
   if (is_atomic)
   {
      m_acquire_pcs.insert(pc);

      if (lock.holder == INVALID_THREAD_ID)
      {
         setHolder(lock, tid, pc, time, cur_icount);

         logEvent(lock_addr, true, "[Lock State Tracker] Core %d (Thread %d) ACQUIRED Lock 0x%lx (atomic) at T=%" PRIu64 " ns | Acq PC 0x%lx\n",
                   core->getId(), tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)pc);

         // Record uncontended lock acquisition in SQLite stats
         if (Sim()->getStatsManager())
         {
            Sim()->getStatsManager()->recordSyncEvent(tid, "lock", time.getNS(), lock_addr);
         }
      }
      else if (lock.contenders.insert(tid).second)
      {
         logEvent(lock_addr, true, "[Lock State Tracker] Core %d (Thread %d) JOINED contenders of Lock 0x%lx (held by Thread %d) at T=%" PRIu64 " ns\n",
                   core->getId(), tid, (unsigned long)lock_addr, lock.holder, time.getNS());
      }
      return;
   }

   // 3. Non-atomic store by a contender: it acquired the lock without a detected spin and now releases it.
   //    Stores by any other thread are not unlocks and are ignored.
   if (lock.contenders.erase(tid) > 0)
   {
      m_total_acquires++;
      m_total_unlocks++;

      logEvent(lock_addr, true, "[Lock State Tracker] Core %d (Thread %d) UNLOCKED Lock 0x%lx at T=%" PRIu64 " ns (acquired without detected spin) | Unlock PC 0x%lx\n",
                core->getId(), tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)pc);

      if (Sim()->getStatsManager())
      {
         Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", time.getNS(), lock_addr);
      }
   }
}
