#include "unlock_detector.h"
#include "spin_loop_detector.h"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"
#include "simulator.h"
#include "stats.h"
#include "config.hpp"
#include <inttypes.h>
#include <cstdio>
#include <cstdlib>

UnlockDetector* UnlockDetector::s_instance = NULL;

UnlockDetector* UnlockDetector::getSingleton()
{
   if (!s_instance)
   {
      bool enabled = Sim()->getCfg()->getBoolDefault("core/unlock_detection", true);
      if (enabled)
      {
         s_instance = new UnlockDetector();
         std::atexit(UnlockDetector::release);
      }
   }
   return s_instance;
}

void UnlockDetector::release()
{
   if (s_instance)
   {
      UnlockDetector *det = s_instance;
      s_instance = NULL;
      delete det;
   }
}

UnlockDetector::UnlockDetector()
   : m_release_seq(0),
     m_total_acquires(0),
     m_total_unlocks(0),
     m_total_held_time(SubsecondTime::Zero()),
     m_total_held_insns(0)
{
   printf("[UNLOCK_DETECTOR] Initialized Unlock Detector\n");
}

UnlockDetector::~UnlockDetector()
{
   printf("\n============= [Unlock Detector Final Summary] =============\n");
   printf("  Unique Lock Addresses Tracked:     %zu\n", m_known_locks.size());
   for (std::unordered_set<IntPtr>::iterator it = m_known_locks.begin(); it != m_known_locks.end(); ++it)
   {
      printf("    - Lock Address: 0x%lx\n", (unsigned long)*it);
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
   printf("===========================================================\n\n");
   fflush(stdout);
}


void UnlockDetector::recordAcquire(thread_id_t tid, IntPtr lock_addr, IntPtr acq_pc,
                                   SubsecondTime time, UInt64 icount, bool from_spin)
{
   ScopedLock sl(m_lock);
   m_known_locks.insert(lock_addr);
   bool already_held = (m_held_locks[tid].find(lock_addr) != m_held_locks[tid].end());
   m_held_locks[tid][lock_addr] = HeldLock(lock_addr, acq_pc, time, icount, from_spin);
   if (!already_held)
      m_total_acquires++;

   printf("[Unlock Detector] Thread %d ACQUIRED Lock 0x%lx at T=%" PRIu64 " ns | Acq PC 0x%lx%s\n",
          tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)acq_pc,
          from_spin ? " (from spin)" : "");
   fflush(stdout);
}

bool UnlockDetector::isKnownLock(IntPtr addr) const
{
   return m_known_locks.find(addr) != m_known_locks.end();
}

void UnlockDetector::addKnownLock(IntPtr addr)
{
   ScopedLock sl(m_lock);
   if (m_known_locks.insert(addr).second)
   {
      printf("[Unlock Detector] Permanently saved lock address: 0x%lx\n", (unsigned long)addr);
      fflush(stdout);
   }
}

void UnlockDetector::ensureHeldLock(thread_id_t tid, IntPtr lock_addr, IntPtr acq_pc)
{
   ScopedLock sl(m_lock);
   m_known_locks.insert(lock_addr);
   if (m_held_locks[tid].find(lock_addr) == m_held_locks[tid].end())
   {
      m_held_locks[tid][lock_addr] = HeldLock(lock_addr, acq_pc, SubsecondTime::Zero(), 0, false);
   }
}

bool UnlockDetector::isLockHeldByOther(thread_id_t caller_tid, IntPtr lock_addr, thread_id_t &out_owner_tid)
{
   ScopedLock sl(m_lock);
   for (std::unordered_map<thread_id_t, std::unordered_map<IntPtr, HeldLock>>::const_iterator it = m_held_locks.begin(); it != m_held_locks.end(); ++it)
   {
      if (it->first == caller_tid)
         continue;
      if (it->second.count(lock_addr) > 0)
      {
         out_owner_tid = it->first;
         return true;
      }
   }
   return false;
}

bool UnlockDetector::anyOtherThreadHoldsLock(thread_id_t caller_tid)
{
   ScopedLock sl(m_lock);
   for (std::unordered_map<thread_id_t, std::unordered_map<IntPtr, HeldLock>>::const_iterator it = m_held_locks.begin(); it != m_held_locks.end(); ++it)
   {
      if (it->first != caller_tid && !it->second.empty())
         return true;
   }
   return false;
}

bool UnlockDetector::getLastUnlockTime(IntPtr lock_addr, SubsecondTime &out_time)
{
   ScopedLock sl(m_lock);
   std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ReleaseInfo>>::const_iterator it = m_last_releases.find(lock_addr);
   if (it == m_last_releases.end() || it->second.empty())
      return false;
   SubsecondTime max_time = SubsecondTime::Zero();
   for (std::unordered_map<thread_id_t, ReleaseInfo>::const_iterator rit = it->second.begin(); rit != it->second.end(); ++rit)
   {
      if (rit->second.unlock_time > max_time)
         max_time = rit->second.unlock_time;
   }
   out_time = max_time;
   return true;
}

bool UnlockDetector::canExitSpin(thread_id_t tid, IntPtr lock_addr, SubsecondTime now)
{
   ScopedLock sl(m_lock);

   for (std::unordered_map<thread_id_t, std::unordered_map<IntPtr, HeldLock>>::const_iterator it = m_held_locks.begin(); it != m_held_locks.end(); ++it)
   {
      if (it->first != tid && it->second.count(lock_addr) > 0)
         return false;
   }

   std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ReleaseInfo>>::const_iterator it = m_last_releases.find(lock_addr);
   if (it != m_last_releases.end() && !it->second.empty())
   {
      SubsecondTime latest_unlock = SubsecondTime::Zero();
      thread_id_t latest_releaser = -1;
      for (std::unordered_map<thread_id_t, ReleaseInfo>::const_iterator rit = it->second.begin(); rit != it->second.end(); ++rit)
      {
         if (rit->first != tid && rit->second.unlock_time > latest_unlock)
         {
            latest_unlock = rit->second.unlock_time;
            latest_releaser = rit->first;
         }
      }
      if (latest_releaser != -1 && now < latest_unlock)
         return false;
   }

   return true;
}

bool UnlockDetector::findLatestRelease(IntPtr addr, thread_id_t reader_tid, IntPtr &out_unlock_pc) const
{
   std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ReleaseInfo>>::const_iterator addr_it = m_last_releases.find(addr);
   if (addr_it == m_last_releases.end())
      return false;

   bool found = false;
   UInt64 max_seq = 0;
   const std::unordered_map<thread_id_t, ReleaseInfo> &writers = addr_it->second;
   for (std::unordered_map<thread_id_t, ReleaseInfo>::const_iterator it = writers.begin(); it != writers.end(); ++it)
   {
      if (it->first == reader_tid)
         continue;
      if (!found || it->second.seq > max_seq)
      {
         out_unlock_pc = it->second.unlock_pc;
         max_seq = it->second.seq;
         found = true;
      }
   }
   return found;
}

void UnlockDetector::onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time)
{
   if (!uop || !uop->getMicroOp())
      return;

   // Unlocks are always STORE operations!
   if (!uop->getMicroOp()->isStore())
      return;

   IntPtr mem_addr = uop->getAddress().address;
   if (mem_addr == 0)
      return;

   const Instruction *ins = uop->getMicroOp()->getInstruction();
   IntPtr pc = ins ? ins->getAddress() : uop->getMicroOp()->getInstructionPointer().phys;
   bool is_atomic = ins ? ins->isAtomic() : false;

   ScopedLock sl(m_lock);

   if (m_known_locks.count(mem_addr) == 0)
      return;

   IntPtr lock_addr = mem_addr;

   UInt64 cur_icount = core->getInstructionCount();

   // 1. Check if this thread currently holds lock_addr as an acquired lock
   std::unordered_map<thread_id_t, std::unordered_map<IntPtr, HeldLock>>::iterator thread_it = m_held_locks.find(tid);
   if (thread_it != m_held_locks.end())
   {
      std::unordered_map<IntPtr, HeldLock>::iterator lock_it = thread_it->second.find(lock_addr);
      if (lock_it != thread_it->second.end())
      {
         const HeldLock &held = lock_it->second;
         // In spinlocks, release is a plain store (is_atomic == false)
         // Or any store strictly after the acquire instruction
         if (!is_atomic || cur_icount > held.acq_icount)
         {
            // THIS STORE IS THE UNLOCK!
            SubsecondTime held_time = (time > held.acq_time) ? (time - held.acq_time) : SubsecondTime::Zero();
            UInt64 held_insns = (cur_icount > held.acq_icount) ? (cur_icount - held.acq_icount) : 0;

            m_total_unlocks++;
            m_total_held_time += held_time;
            m_total_held_insns += held_insns;
            m_known_locks.insert(lock_addr);

            // Record release store in shadow table
            m_release_seq++;
            m_last_releases[lock_addr][tid] = ReleaseInfo(pc, tid, m_release_seq, time);

            printf("[Unlock Detector] Core %d (Thread %d) UNLOCKED Lock 0x%lx at T=%" PRIu64 " ns (Acquired at T=%" PRIu64 " ns) | Held: %" PRIu64 " ns (%" PRIu64 " insns) | Acq PC 0x%lx -> Unlock PC 0x%lx\n",
                   core->getId(), tid, (unsigned long)lock_addr, time.getNS(), held.acq_time.getNS(), held_time.getNS(), held_insns,
                   (unsigned long)held.acq_pc, (unsigned long)pc);
            fflush(stdout);

            // Record unlock event in SQLite stats for timeline plotting
            if (Sim()->getStatsManager())
            {
               Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", time.getNS(), lock_addr);
            }

            thread_it->second.erase(lock_it);

            // DIRECT HANDOFF: If another thread is currently waiting on this lock, transfer ownership immediately!
            thread_id_t waiter_tid = -1;
            if (Sim()->getSpinLoopDetector() &&
                Sim()->getSpinLoopDetector()->getWaitingThread(lock_addr, tid, waiter_tid))
            {
               m_held_locks[waiter_tid][lock_addr] = HeldLock(lock_addr, pc, time, 0, /*from_spin=*/true);
               m_total_acquires++;
               printf("[Unlock Detector] Lock 0x%lx handed off to waiting Thread %d at T=%" PRIu64 " ns\n",
                      (unsigned long)lock_addr, waiter_tid, time.getNS());
               fflush(stdout);
            }
            return;
         }
      }
   }

   // 2. Uncontended or fresh acquire via atomic RMW store to a known lock variable
   if (is_atomic)
   {
      if (m_held_locks[tid].find(lock_addr) == m_held_locks[tid].end())
      {
         m_held_locks[tid][lock_addr] = HeldLock(lock_addr, pc, time, cur_icount, /*from_spin=*/false);
         m_total_acquires++;

         printf("[Unlock Detector] Core %d (Thread %d) ACQUIRED Lock 0x%lx (atomic) at T=%" PRIu64 " ns | Acq PC 0x%lx\n",
                core->getId(), tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)pc);
         fflush(stdout);

         // Record uncontended lock acquisition in SQLite stats
         if (Sim()->getStatsManager())
         {
            Sim()->getStatsManager()->recordSyncEvent(tid, "lock", time.getNS(), lock_addr);
         }
         return;
      }
   }

   // 3. Fallback: non-atomic store to a known lock variable is an unlock store
   if (!is_atomic)
   {
      m_total_unlocks++;
      m_release_seq++;
      m_last_releases[lock_addr][tid] = ReleaseInfo(pc, tid, m_release_seq, time);

      printf("[Unlock Detector] Core %d (Thread %d) UNLOCKED Known Lock 0x%lx at T=%" PRIu64 " ns | Unlock PC 0x%lx\n",
             core->getId(), tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)pc);
      fflush(stdout);

      if (Sim()->getStatsManager())
      {
         Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", time.getNS(), lock_addr);
      }

      // DIRECT HANDOFF: If another thread is currently waiting on this lock, transfer ownership immediately!
      thread_id_t waiter_tid = -1;
      if (Sim()->getSpinLoopDetector() &&
          Sim()->getSpinLoopDetector()->getWaitingThread(lock_addr, tid, waiter_tid))
      {
         m_held_locks[waiter_tid][lock_addr] = HeldLock(lock_addr, pc, time, 0, /*from_spin=*/true);
         m_total_acquires++;
         printf("[Unlock Detector] Lock 0x%lx handed off to waiting Thread %d at T=%" PRIu64 " ns\n",
                (unsigned long)lock_addr, waiter_tid, time.getNS());
         fflush(stdout);
      }
      return;
   }
}

