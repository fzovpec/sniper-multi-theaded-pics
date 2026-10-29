#ifndef __UNLOCK_DETECTOR_H
#define __UNLOCK_DETECTOR_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include "lock.h"
#include <unordered_map>
#include <unordered_set>

class Core;
class DynamicMicroOp;

struct HeldLock
{
   IntPtr lock_addr;
   IntPtr acq_pc;
   SubsecondTime acq_time;
   UInt64 acq_icount;
   bool from_spin;

   HeldLock()
      : lock_addr(0), acq_pc(0), acq_time(SubsecondTime::Zero()), acq_icount(0), from_spin(false) {}
   HeldLock(IntPtr addr, IntPtr pc, SubsecondTime time, UInt64 icount, bool spin)
      : lock_addr(addr), acq_pc(pc), acq_time(time), acq_icount(icount), from_spin(spin) {}
};

struct ReleaseInfo
{
   IntPtr unlock_pc;
   thread_id_t tid;
   UInt64 seq;
   SubsecondTime unlock_time;

   ReleaseInfo() : unlock_pc(0), tid(-1), seq(0), unlock_time(SubsecondTime::Zero()) {}
   ReleaseInfo(IntPtr pc, thread_id_t t, UInt64 s, SubsecondTime time)
      : unlock_pc(pc), tid(t), seq(s), unlock_time(time) {}
};

class UnlockDetector
{
   private:
      static UnlockDetector *s_instance;

      Lock m_lock;
      UInt64 m_release_seq;

      // Addresses known/suspected to be locks
      std::unordered_set<IntPtr> m_known_locks;

      // Currently held locks: tid -> (lock_addr -> HeldLock)
      std::unordered_map<thread_id_t, std::unordered_map<IntPtr, HeldLock>> m_held_locks;

      // Latest release stores per lock address: addr -> (tid -> ReleaseInfo)
      std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ReleaseInfo>> m_last_releases;

      // Aggregated statistics
      UInt64 m_total_acquires;
      UInt64 m_total_unlocks;
      SubsecondTime m_total_held_time;
      UInt64 m_total_held_insns;

   public:
      UnlockDetector();
      virtual ~UnlockDetector();

      static UnlockDetector* getSingleton();
      static void release();

      // Called when ANY detector detects a lock acquire (contended or uncontended)
      void recordAcquire(thread_id_t tid, IntPtr lock_addr, IntPtr acq_pc,
                         SubsecondTime time, UInt64 icount, bool from_spin);

      // Process instruction commit to detect unlock stores
      void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time);

      // Query if an address is tracked as a lock
      bool isKnownLock(IntPtr addr) const;

      // Query latest release store PC for synchronization pairing
      bool findLatestRelease(IntPtr addr, thread_id_t reader_tid, IntPtr &out_unlock_pc) const;

      void addKnownLock(IntPtr addr);
      void ensureHeldLock(thread_id_t tid, IntPtr lock_addr, IntPtr acq_pc);
      bool isLockHeldByOther(thread_id_t caller_tid, IntPtr lock_addr, thread_id_t &out_owner_tid);
      bool anyOtherThreadHoldsLock(thread_id_t caller_tid);
      bool getLastUnlockTime(IntPtr lock_addr, SubsecondTime &out_time);
      bool canExitSpin(thread_id_t tid, IntPtr lock_addr, SubsecondTime now);
};

#endif // __UNLOCK_DETECTOR_H

