#ifndef __LOCK_STATE_TRACKER_H
#define __LOCK_STATE_TRACKER_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "lock.h"
#include <map>
#include <unordered_map>
#include <unordered_set>

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

         LockState() : size(0), holder(INVALID_THREAD_ID), released(false), from_spin(false), acq_pc(0), acq_time(SubsecondTime::Zero()), acq_icount(0) {}
      };

      typedef std::map<IntPtr, LockState> LockMap;

      Lock m_lock;

      // Known locks: base address -> state, covering [base, base + size)
      LockMap m_locks;
      // Threads that are currently spinning: tid -> base address of the lock
      std::unordered_map<thread_id_t, IntPtr> m_spinning;
      // PCs of atomic stores executed by threads that did not hold the lock (acquire attempts)
      std::unordered_set<IntPtr> m_acquire_pcs;
      // PCs of the spinning loads reported by the spin loop detector
      std::unordered_set<IntPtr> m_spin_load_pcs;

      // Aggregated statistics
      UInt64 m_total_acquires;
      UInt64 m_total_unlocks;
      SubsecondTime m_total_held_time;
      UInt64 m_total_held_insns;

      LockMap::iterator findLock(IntPtr addr, UInt32 size);
      void setHolder(LockState &lock, thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount);

   public:
      LockStateTracker();
      ~LockStateTracker();

      static LockStateTracker* create();

      // Instruction PC of a micro-op, in the address space of the trace
      static IntPtr getPC(const DynamicMicroOp *uop);

      // Called on micro-op commit for each core
      void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time);

      // Reported by the spin loop detector
      void onSpinStart(thread_id_t tid, IntPtr lock_addr, UInt32 size, IntPtr load_pc);
      void onSpinEnd(thread_id_t tid);

      // True if pc is the spinning load of a spin loop reported by the spin loop detector
      bool isSpinLoad(IntPtr pc);

      // Called when a thread's trace leaves a spin loop it was not detected spinning in, after loading the lock variable at mem_addr.
      // Returns true if the thread holds the lock now, false if another thread holds it (the thread joins the contenders).
      bool tryClaimLoad(thread_id_t tid, IntPtr mem_addr, IntPtr pc, SubsecondTime time, UInt64 icount);

      bool isSpinning(thread_id_t tid);

      // True if the lock a thread is spinning on is known to be released and not taken by anyone since
      bool isLockFree(thread_id_t tid);

      // Called when a spinning thread's trace leaves its spin loop.
      // Returns true if the thread holds the lock now, false if another thread still holds it.
      bool tryClaim(thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount);
};

#endif // __LOCK_STATE_TRACKER_H
