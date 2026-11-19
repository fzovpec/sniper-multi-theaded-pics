#ifndef __SYNC_VARIABLE_H
#define __SYNC_VARIABLE_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include <unordered_map>
#include <unordered_set>

// A variable that some threads write and other threads wait on, found by the spin loop detector.
// It has no holder: each wait of a thread uses up one write by another thread.
// A wait lasts from the first check of the variable until the thread's trace leaves the spin loop.
class SyncVariable
{
   public:
      // State of a thread that waits on the variable
      struct WaiterState
      {
         UInt64 consumed;     // writes by other threads that this thread's waits have used up
         bool passed;         // the current wait already got its write
         bool used_write;     // the current wait used up a write (given back if the wait turns out to be a read-modify-write)
         IntPtr pass_pc;
         SubsecondTime pass_time;
         UInt64 pass_icount;

         WaiterState() : consumed(0), passed(false), used_write(false), pass_pc(0), pass_time(SubsecondTime::Zero()), pass_icount(0) {}
      };

      UInt32 size;
      // Writes seen so far, in total and per writing thread
      UInt64 stores_total;
      std::unordered_map<thread_id_t, UInt64> stores_by;
      // Threads that waited on the variable
      std::unordered_map<thread_id_t, WaiterState> waiters;
      // Threads detected spinning on the variable: they are the contenders if it turns out to be a lock
      std::unordered_set<thread_id_t> spinners;
      // Waits each thread completed on the variable before its first check of it as a tracked variable (counted by its
      // own frontend from its reads): a thread's 'consumed' starts at this when it first shows up as a waiter
      std::unordered_map<thread_id_t, UInt64> earlier_waits;

      // Lock pattern, per thread, from its own waits and writes (both fixed by its trace):
      // acquire = start of a wait at a spin load, or an atomic store; release = plain store.
      // A lock alternates acquire -> release in every thread.
      std::unordered_map<thread_id_t, bool> holding;        // acquired, not released yet
      std::unordered_map<thread_id_t, bool> pending_wait;   // a new wait started before the release committed
      UInt64 lock_rounds;   // releases seen without a violation of the lock pattern
      bool not_a_lock;      // a thread broke the lock pattern: never (again) a lock

      SyncVariable(UInt32 _size = 0) : size(_size), stores_total(0), lock_rounds(0), not_a_lock(false) {}

      // Writes by threads other than tid
      UInt64 otherWrites(thread_id_t tid) const;
      // True if the current wait of tid already passed, or a write by another thread is there for it
      bool canPass(thread_id_t tid) const;
      // The state of tid as a waiter, created on first use with consumed = its earlier waits (at most the writes by others)
      WaiterState &waiter(thread_id_t tid);
      // tid gets past its current wait, using up a write by another thread if one is there
      void pass(thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount);
};

#endif // __SYNC_VARIABLE_H
