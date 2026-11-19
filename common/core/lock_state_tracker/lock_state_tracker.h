#ifndef __LOCK_STATE_TRACKER_H
#define __LOCK_STATE_TRACKER_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "lock.h"
#include "sync_variable.h"
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <string>
#include <chrono>

class Core;
class DynamicMicroOp;

// Tracks the state of user-level locks found by the spin loop detector.
// Each lock has at most one holder and a set of contenders (threads that tried to acquire it while it was held).
// A spinning thread becomes holder when its own trace leaves the spin loop (tryClaim), so the handoff order follows the trace.
class LockStateTracker
{
   private:
      struct LockState
      {
         UInt32 size;
         thread_id_t holder;
         bool released;   // the last holder's unlock was seen and nobody acquired the lock since
         bool from_spin;  // the holder took the lock by leaving its spin loop (tryClaim), not by an atomic store
         IntPtr acq_pc;
         SubsecondTime acq_time;
         UInt64 acq_icount;
         std::unordered_set<thread_id_t> contenders;
         // The variable's sync record, kept up to date while it is a lock so it can go back to being a sync variable
         SyncVariable sync;

         LockState() : size(0), holder(INVALID_THREAD_ID), released(false), from_spin(false), acq_pc(0), acq_time(SubsecondTime::Zero()), acq_icount(0) {}
      };

      typedef std::map<IntPtr, LockState> LockMap;
      typedef std::map<IntPtr, SyncVariable> SyncVariableMap;

      Lock m_lock;

      // A variable found by the spin loop detector starts as a sync variable
      // and becomes a lock when a thread that waited on it writes it.
      // An address is in at most one of the two maps.
      // Known locks: base address -> state, covering [base, base + size)
      LockMap m_locks;
      // Known sync variables: base address -> state, covering [base, base + size)
      SyncVariableMap m_sync_vars;
      // Threads that are currently spinning: tid -> base address of the lock or sync variable
      std::unordered_map<thread_id_t, IntPtr> m_spinning;
      // PCs of atomic stores executed by threads that did not hold the lock (acquire attempts)
      std::unordered_set<IntPtr> m_acquire_pcs;
      // PCs of the spinning loads reported by the spin loop detector
      std::unordered_set<IntPtr> m_spin_load_pcs;
      // Threads that are waiting on a sync variable for a write that has not been seen yet
      std::unordered_set<thread_id_t> m_stalled;
      // Threads kept spinning on a lock held by another thread: tid -> lock base address
      std::unordered_map<thread_id_t, IntPtr> m_lock_stalled;
      // Acquire -> release rounds without a violation before a variable is promoted to a lock
      UInt64 m_lock_detection_rounds;
      // Print one line per wait / pass / write / acquire / unlock (off for long runs)
      bool m_log_events;
      // Wall-clock seconds a wait may last while no thread can end it before the simulation is aborted (0: never)
      double m_stuck_timeout;
      // Wall-clock seconds a thread must stay refused by a lock whose holder and all other threads wait before the lock is demoted
      double m_lock_demote_delay;
      // Since when (wall clock) a thread waits on a sync variable that no thread can write / is refused by a deadlocked lock
      std::unordered_map<thread_id_t, std::chrono::steady_clock::time_point> m_stuck_since;
      std::unordered_map<thread_id_t, std::chrono::steady_clock::time_point> m_refused_since;
      // Last check PC per thread (for the dumps)
      std::unordered_map<thread_id_t, IntPtr> m_last_check_pc;
      // Last events per variable (kept even when per-event lines are off, printed in the dumps)
      static const size_t RECENT_EVENTS = 500;
      std::map<IntPtr, std::deque<std::string> > m_recent;

      enum PatternEvent { WAIT_START, ATOMIC_STORE, PLAIN_STORE };

      // Aggregated statistics
      UInt64 m_total_acquires;
      UInt64 m_total_unlocks;
      SubsecondTime m_total_held_time;
      UInt64 m_total_held_insns;

      LockMap::iterator findLock(IntPtr addr, UInt32 size);
      SyncVariableMap::iterator findSyncVariable(IntPtr addr, UInt32 size);
      void setHolder(LockState &lock, thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount);

      // Sync variable: true if a write by another thread is available for this wait of tid (each wait uses up one write)
      // new_wait: the check is the first one of a new wait of tid. Never passes without a write: a wait that no thread
      // can end for stuck_timeout aborts the simulation with a dump.
      bool claimSync(SyncVariable &var, IntPtr var_addr, thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount, bool new_wait, bool in_replay = false);
      // True if a thread other than tid is running and not itself waiting on a sync variable or a lock
      bool canBeSignalled(thread_id_t tid);
      // True if the lock's holder (not tid) is itself waiting on a tracked variable and no other thread can run
      bool holderIsDeadlocked(const LockState &lock, thread_id_t tid);
      // Seconds (wall clock) since the condition tracked in 'since' started for tid (starts it if new)
      double heldFor(std::unordered_map<thread_id_t, std::chrono::steady_clock::time_point> &since, thread_id_t tid);
      // Formats one event line, keeps it in the variable's recent events, prints it (per-event lines only if m_log_events)
      void logEvent(IntPtr base, bool per_event, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
      // Prints the state of the variable, all threads and its recent events
      void dumpState(IntPtr base, const char *title, thread_id_t tid, double seconds, SubsecondTime time);

      // Update the lock pattern of tid with one event. Returns the violation, or NULL if the pattern still fits a lock.
      const char *observe(SyncVariable &var, thread_id_t tid, PatternEvent ev);
      // The variable at base broke the lock pattern: never a lock again. Demotes it if it is a lock now.
      void notALock(IntPtr base, SyncVariable &var, thread_id_t tid, const char *reason, SubsecondTime time);
      // A lock goes back to being a sync variable: all its waiters are then let through by the sync rule
      SyncVariableMap::iterator demote(LockMap::iterator it, SubsecondTime time);
      // A sync variable that followed the lock pattern for long enough becomes a lock (free: promoted on a release)
      void promote(SyncVariableMap::iterator sv, thread_id_t tid, SubsecondTime time);

   public:
      LockStateTracker(UInt64 lock_detection_rounds, bool log_events, double stuck_timeout, double lock_demote_delay);
      ~LockStateTracker();

      static LockStateTracker* create();

      // Instruction PC of a micro-op, in the address space of the trace
      static IntPtr getPC(const DynamicMicroOp *uop);

      // Called on micro-op commit for each core
      void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time);

      // Reported by the spin loop detector
      // store_counts: writes to lock_addr seen so far, per writing thread
      void onSpinStart(thread_id_t tid, IntPtr lock_addr, UInt32 size, IntPtr load_pc, const std::unordered_map<thread_id_t, UInt64> &store_counts);
      void onSpinEnd(thread_id_t tid);

      // True if a load at pc of mem_addr is a check of a tracked variable:
      // the spinning load of a reported spin loop, or any load of a sync variable
      bool isWaitLoad(IntPtr pc, IntPtr mem_addr);
      // True if mem_addr is a tracked variable that is not (yet) a lock
      bool isSyncVariable(IntPtr mem_addr);
      // True if pc is the spinning load of a spin loop reported by the spin loop detector
      bool isSpinLoad(IntPtr pc);
      // Start of a wait of tid on the tracked variable at var_base, at its first check at a spin load (frontend)
      void onWaitStart(thread_id_t tid, IntPtr var_base, IntPtr pc, SubsecondTime time);

      // Sync variable: a check of the variable at mem_addr by tid. A wait starts at its first check (new_wait)
      // and lasts until the thread's trace leaves the spin loop (decided by the frontend).
      // Returns true if a write by another thread is there for this wait, false if the thread has to keep waiting.
      // in_replay: the check is an instruction of a replay (its PC is the replayed loop's, not where the thread waits natively)
      bool tryPassSync(thread_id_t tid, IntPtr mem_addr, IntPtr pc, SubsecondTime time, UInt64 icount, bool new_wait, bool in_replay = false);
      // The current wait of tid on the sync variable at mem_addr was a read-modify-write (the thread stored to it right
      // after reading it, with no branch in between): not a wait. Gives back the write it used, if any.
      void undoWait(thread_id_t tid, IntPtr mem_addr, IntPtr read_pc, IntPtr store_pc, SubsecondTime time);
      // Earlier waits of tid on the variable, counted by tid's own frontend from its reads (the visit in progress, its
      // current wait, not included). Used when tid first becomes a waiter of the variable; ignored after that.
      void provideEarlierWaits(thread_id_t tid, IntPtr var_base, UInt64 waits);
      // True if tid is in a spin on the variable at var_base reported by the spin loop detector (lock or sync variable)
      bool isSpinningOn(thread_id_t tid, IntPtr var_base);
      // Seconds (wall clock) of the stuck timeout (0: off)
      double getStuckTimeout() const { return m_stuck_timeout; }
      // Prints the dump for tid stuck on the variable (what: why) and aborts the simulation
      void reportStuck(IntPtr var_base, thread_id_t tid, const char *what, double seconds, SubsecondTime time);
      // Base address of the lock or sync variable covering mem_addr, 0 if none
      IntPtr getVariableBase(IntPtr mem_addr);

      // Called at the branch after a load of the tracked variable at mem_addr by a thread that was not detected spinning.
      // Lock: returns true if the thread holds the lock now, false if another thread holds it (the thread joins the contenders).
      // Sync variable: returns true if a write by another thread is available for this wait, false if the thread has to keep waiting.
      bool tryClaimLoad(thread_id_t tid, IntPtr mem_addr, IntPtr pc, SubsecondTime time, UInt64 icount);

      // True if tid is detected spinning on a lock (spins on a sync variable are handled by tryPassSync)
      bool isSpinning(thread_id_t tid);

      // True if the lock a thread is spinning on is known to be released and not taken by anyone since
      bool isLockFree(thread_id_t tid);

      // Called when a spinning thread's trace leaves its spin loop.
      // Returns true if the thread holds the lock now, false if another thread still holds it.
      bool tryClaim(thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount);
};

#endif // __LOCK_STATE_TRACKER_H
