#include "lock_state_tracker.h"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"
#include "simulator.h"
#include "stats.h"
#include "config.hpp"
#include <inttypes.h>
#include <cstdio>

LockStateTracker* LockStateTracker::create()
{
   if (!Sim()->getCfg()->getBool("core/lock_state_tracking"))
      return NULL;

   return new LockStateTracker();
}

LockStateTracker::LockStateTracker()
   : m_total_acquires(0),
     m_total_unlocks(0),
     m_total_held_time(SubsecondTime::Zero()),
     m_total_held_insns(0)
{
   printf("[LOCK_STATE_TRACKER] Initialized Lock State Tracker\n");
}

LockStateTracker::~LockStateTracker()
{
   printf("\n============= [Lock State Tracker Final Summary] =============\n");
   printf("  Unique Lock Addresses Tracked:     %zu\n", m_locks.size());
   for (LockMap::iterator it = m_locks.begin(); it != m_locks.end(); ++it)
   {
      printf("    - Lock Address: 0x%lx (%u bytes)\n", (unsigned long)it->first, it->second.size);
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

// Find the known lock overlapping [addr, addr + size). Must be called with m_lock held.
LockStateTracker::LockMap::iterator LockStateTracker::findLock(IntPtr addr, UInt32 size)
{
   if (size == 0)
      size = 1;

   LockMap::iterator it = m_locks.upper_bound(addr + size - 1);
   if (it == m_locks.begin())
      return m_locks.end();
   --it;
   if (it->first + it->second.size > addr)
      return it;
   return m_locks.end();
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
   m_total_acquires++;
}

void LockStateTracker::onSpinStart(thread_id_t tid, IntPtr lock_addr, UInt32 size, IntPtr load_pc)
{
   ScopedLock sl(m_lock);

   m_spin_load_pcs.insert(load_pc);

   LockMap::iterator it = findLock(lock_addr, size);
   if (it == m_locks.end())
   {
      it = m_locks.insert(std::make_pair(lock_addr, LockState())).first;
      it->second.size = size ? size : 1;

      printf("[Lock State Tracker] Tracking lock 0x%lx (%u bytes)\n", (unsigned long)lock_addr, it->second.size);
      fflush(stdout);
   }

   LockState &lock = it->second;

   // A holder that left its spin loop really holds the lock: this is a late report of spin loads committing after the claim
   if (lock.holder == tid && lock.from_spin)
      return;

   // A holder does not spin on its own lock: its atomic store was an attempt, not an acquire
   if (lock.holder == tid)
   {
      lock.holder = INVALID_THREAD_ID;
      m_total_acquires--;
      printf("[Lock State Tracker] Thread %d is spinning on Lock 0x%lx, no longer its holder\n", tid, (unsigned long)it->first);
      fflush(stdout);
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
   return m_spinning.count(tid) > 0;
}

bool LockStateTracker::isLockFree(thread_id_t tid)
{
   ScopedLock sl(m_lock);

   std::unordered_map<thread_id_t, IntPtr>::iterator spin_it = m_spinning.find(tid);
   if (spin_it == m_spinning.end())
      return false;

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

   LockMap::iterator it = m_locks.find(spin_it->second);
   if (it == m_locks.end())
      return true;

   LockState &lock = it->second;
   if (lock.holder == tid)
      return true;
   if (lock.holder != INVALID_THREAD_ID)
      return false;

   setHolder(lock, tid, pc, time, icount);
   lock.from_spin = true;

   printf("[Lock State Tracker] Thread %d ACQUIRED Lock 0x%lx at T=%" PRIu64 " ns | Acq PC 0x%lx (from spin)\n",
          tid, (unsigned long)it->first, time.getNS(), (unsigned long)pc);
   fflush(stdout);
   return true;
}

bool LockStateTracker::isSpinLoad(IntPtr pc)
{
   ScopedLock sl(m_lock);
   return m_spin_load_pcs.count(pc) > 0;
}

bool LockStateTracker::tryClaimLoad(thread_id_t tid, IntPtr mem_addr, IntPtr pc, SubsecondTime time, UInt64 icount)
{
   ScopedLock sl(m_lock);

   LockMap::iterator it = findLock(mem_addr, 1);
   if (it == m_locks.end())
      return true;

   LockState &lock = it->second;
   if (lock.holder == tid)
      return true;

   if (lock.holder != INVALID_THREAD_ID)
   {
      if (lock.contenders.insert(tid).second)
      {
         printf("[Lock State Tracker] Thread %d JOINED contenders of Lock 0x%lx (held by Thread %d) at T=%" PRIu64 " ns\n",
                tid, (unsigned long)it->first, lock.holder, time.getNS());
         fflush(stdout);
      }
      return false;
   }

   setHolder(lock, tid, pc, time, icount);
   lock.from_spin = true;

   printf("[Lock State Tracker] Thread %d ACQUIRED Lock 0x%lx at T=%" PRIu64 " ns | Acq PC 0x%lx (no detected spin)\n",
          tid, (unsigned long)it->first, time.getNS(), (unsigned long)pc);
   fflush(stdout);

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

   LockMap::iterator it = findLock(mem_addr, uop->getMicroOp()->getMemoryAccessSize());
   if (it == m_locks.end())
      return;

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

      printf("[Lock State Tracker] Core %d (Thread %d) UNLOCKED Lock 0x%lx at T=%" PRIu64 " ns (Acquired at T=%" PRIu64 " ns) | Held: %" PRIu64 " ns (%" PRIu64 " insns) | Acq PC 0x%lx -> Unlock PC 0x%lx\n",
             core->getId(), tid, (unsigned long)lock_addr, time.getNS(), lock.acq_time.getNS(), held_time.getNS(), held_insns,
             (unsigned long)lock.acq_pc, (unsigned long)pc);
      fflush(stdout);

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

         printf("[Lock State Tracker] Core %d (Thread %d) ACQUIRED Lock 0x%lx (atomic) at T=%" PRIu64 " ns | Acq PC 0x%lx\n",
                core->getId(), tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)pc);
         fflush(stdout);

         // Record uncontended lock acquisition in SQLite stats
         if (Sim()->getStatsManager())
         {
            Sim()->getStatsManager()->recordSyncEvent(tid, "lock", time.getNS(), lock_addr);
         }
      }
      else if (lock.contenders.insert(tid).second)
      {
         printf("[Lock State Tracker] Core %d (Thread %d) JOINED contenders of Lock 0x%lx (held by Thread %d) at T=%" PRIu64 " ns\n",
                core->getId(), tid, (unsigned long)lock_addr, lock.holder, time.getNS());
         fflush(stdout);
      }
      return;
   }

   // 3. Non-atomic store by a contender: it acquired the lock without a detected spin and now releases it.
   //    Stores by any other thread are not unlocks and are ignored.
   if (lock.contenders.erase(tid) > 0)
   {
      m_total_acquires++;
      m_total_unlocks++;

      printf("[Lock State Tracker] Core %d (Thread %d) UNLOCKED Lock 0x%lx at T=%" PRIu64 " ns (acquired without detected spin) | Unlock PC 0x%lx\n",
             core->getId(), tid, (unsigned long)lock_addr, time.getNS(), (unsigned long)pc);
      fflush(stdout);

      if (Sim()->getStatsManager())
      {
         Sim()->getStatsManager()->recordSyncEvent(tid, "unlock", time.getNS(), lock_addr);
      }
   }
}
