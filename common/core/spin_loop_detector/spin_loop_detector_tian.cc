// Implements Tian et al. (ISSTA '08) Dynamic Recognition of Synchronization Operations

#include "spin_loop_detector_tian.h"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"
#include "performance_model.h"
#include "simulator.h"
#include "stats.h"
#include "unlock_detector.h"
#include <inttypes.h>
#include <cstdio>

bool SpinLoopDetectorTian::findRemoteWriter(IntPtr addr, thread_id_t reader_tid, ShadowMemoryEntry &out_sme) const
{
   std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ShadowMemoryEntry>>::const_iterator addr_it = m_shadow_mem.find(addr);
   if (addr_it == m_shadow_mem.end())
      return false;

   bool found = false;
   const std::unordered_map<thread_id_t, ShadowMemoryEntry> &writers = addr_it->second;
   for (std::unordered_map<thread_id_t, ShadowMemoryEntry>::const_iterator it = writers.begin(); it != writers.end(); ++it)
   {
      if (it->first == reader_tid)
         continue;

      const ShadowMemoryEntry &cand = it->second;
      if (!found)
      {
         out_sme = cand;
         found = true;
      }
      else
      {
         // Prefer non-atomic release stores over atomic RMW acquires; break ties by most recent sequence
         if (out_sme.is_atomic && !cand.is_atomic)
            out_sme = cand;
         else if (out_sme.is_atomic == cand.is_atomic && cand.writeseq > out_sme.writeseq)
            out_sme = cand;
      }
   }
   return found;
}

void SpinLoopDetectorTian::resolveEpisode(Core *core, thread_id_t tid, const LoadTableEntry &entry)
{
   if (!entry.possible_spin)
      return;

   ShadowMemoryEntry sme;
   if (findRemoteWriter(entry.addr, tid, sme))
   {
      core->recordSpinInstance();
      core->updateSpinCount(entry.counter, entry.accum_ins, entry.accum_time);
      m_syn_table[entry.pc] = sme.writepc;

      SubsecondTime spin_start = (entry.last_tcount > entry.accum_time) ? (entry.last_tcount - entry.accum_time) : SubsecondTime::Zero();

      printf("[Tian Spin Detector] Confirmed Synchronization Pair: Read PC 0x%lx (Core %d) <--> Write PC 0x%lx (Thread %d) | Iterations: %u | Spin: T=%" PRIu64 " -> T=%" PRIu64 " ns (duration %" PRIu64 " ns)\n",
             (unsigned long)entry.pc, core->getId(), (unsigned long)sme.writepc, sme.writetid,
             entry.counter, spin_start.getNS(), entry.last_tcount.getNS(), entry.accum_time.getNS());
      fflush(stdout);

      // Record synchronization events in SQLite stats for timeline plotting
      if (Sim()->getStatsManager())
      {
         Sim()->getStatsManager()->recordSyncEvent(tid, "begin_lock_acquisition", spin_start.getNS(), entry.addr);
         Sim()->getStatsManager()->recordSyncEvent(tid, "end_lock_acquisition", entry.last_tcount.getNS(), entry.addr);
      }

      if (UnlockDetector::getSingleton())
      {
         UnlockDetector::getSingleton()->recordAcquire(tid, entry.addr, entry.pc, entry.last_tcount, entry.last_icount, true);
      }

      if (m_prev_iteration.count(tid) > 0 && !m_prev_iteration[tid].empty())
      {
         printf("[Tian Loop Body] Thread %d (Core %d) recorded iteration before last (%zu insns):\n",
                tid, core->getId(), m_prev_iteration[tid].size());
         for (size_t i = 0; i < m_prev_iteration[tid].size(); ++i)
         {
            printf("    [%zu] 0x%lx: %s\n", i, (unsigned long)m_prev_iteration[tid][i].pc,
                   m_prev_iteration[tid][i].disas.c_str());
         }
         fflush(stdout);
      }
   }
}

void SpinLoopDetectorTian::onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time)
{
   if (!uop || !uop->getMicroOp())
      return;

   const Instruction *ins = uop->getMicroOp()->getInstruction();
   IntPtr vaddr = ins ? const_cast<Instruction*>(ins)->getVaddress() : 0;
   IntPtr pc = ins ? (vaddr != 0 ? vaddr : ins->getAddress()) : uop->getMicroOp()->getInstructionPointer().phys;
   bool is_atomic = ins ? ins->isAtomic() : false;

   ScopedLock sl(m_lock);

   // Record dynamic loop body instruction if this thread is actively spinning
   if (uop->isFirst() && m_active_spins.count(tid) > 0 && m_active_spins[tid].active)
   {
      IntPtr spin_load_pc = m_active_spins[tid].load_pc;
      if (pc == spin_load_pc)
      {
         // We reached the anchor load: the previous iteration is complete!
         if (!m_current_iteration[tid].empty())
         {
            m_prev_iteration[tid] = m_current_iteration[tid];
            m_current_iteration[tid].clear();
         }
      }

      String disas = ins ? ins->getDisassembly() : "";
      m_current_iteration[tid].push_back(RecordedInstruction(pc, disas));
   }

   bool is_load = uop->getMicroOp()->isLoad();
   bool is_store = uop->getMicroOp()->isStore();

   if (!is_load && !is_store)
      return;

   IntPtr mem_addr = uop->getAddress().address;
   if (mem_addr == 0)
      return;

   // 1. Process LOAD (ignore the load micro-op of atomic RMW instructions like lock cmpxchg)
   if (is_load && !is_atomic)
   {
      std::deque<LoadTableEntry> &load_table = m_load_tables[tid];
      std::deque<LoadTableEntry>::iterator loc = load_table.end();

      for (std::deque<LoadTableEntry>::iterator it = load_table.begin(); it != load_table.end(); ++it)
      {
         if (it->pc == pc)
         {
            loc = it;
            break;
         }
      }

      UInt64 cur_icount = core->getInstructionCount();

      if (loc != load_table.end())
      {
         UInt64 ins_delta = (cur_icount > loc->last_icount) ? (cur_icount - loc->last_icount) : 0;

         if (loc->addr == mem_addr && ins_delta <= MAX_SPIN_BODY_INSNS)
         {
            // Contiguous iteration of the same spin candidate load (Tian Fig. 5 lines 5-12)
            loc->counter++;
            loc->accum_ins += ins_delta;
            if (time > loc->last_tcount)
               loc->accum_time += (time - loc->last_tcount);
            loc->last_icount = cur_icount;
            loc->last_tcount = time;

            if (loc->counter >= m_threshold)
            {
               loc->possible_spin = true;
               m_active_spins[tid] = ActiveSpinInfo(loc->pc, loc->addr);
               if (UnlockDetector::getSingleton())
               {
                  UnlockDetector::getSingleton()->addKnownLock(loc->addr);
               }
            }
         }
         else
         {
            // Either loaded a different address, or re-entered this PC in a new spin episode after leaving the loop
            resolveEpisode(core, tid, *loc);

            loc->addr = mem_addr;
            loc->counter = 1;
            loc->possible_spin = false;
            loc->start_writeseq = m_write_seq;
            loc->accum_ins = 0;
            loc->accum_time = SubsecondTime::Zero();
            loc->last_icount = cur_icount;
            loc->last_tcount = time;
         }
      }
      else
      {
         // New load PC for load_table (Tian Fig. 5 lines 20-30)
         if (load_table.size() >= LOAD_TABLE_MAX_SIZE)
         {
            // Prefer evicting an entry that has not reached possible_spin so stack loads don't evict an active spin
            std::deque<LoadTableEntry>::iterator evict_it = load_table.end();
            for (std::deque<LoadTableEntry>::iterator it = load_table.begin(); it != load_table.end(); ++it)
            {
               if (!it->possible_spin)
               {
                  evict_it = it;
                  break;
               }
            }
            if (evict_it == load_table.end())
               evict_it = load_table.begin();

            resolveEpisode(core, tid, *evict_it);
            load_table.erase(evict_it);
         }

         LoadTableEntry new_entry;
         new_entry.pc = pc;
         new_entry.addr = mem_addr;
         new_entry.counter = 1;
         new_entry.possible_spin = false;
         new_entry.start_writeseq = m_write_seq;
         new_entry.accum_ins = 0;
         new_entry.accum_time = SubsecondTime::Zero();
         new_entry.last_icount = cur_icount;
         new_entry.last_tcount = time;
         load_table.push_back(new_entry);
      }
   }

   // 2. Process STORE (Tian Fig. 5 lines 14-18 & 31-32)
   if (is_store)
   {
      // If this thread itself stores to an address in its own load_table:
      // - If possible_spin is false (e.g. private loop variable d++), reset/erase it (Fig. 5 line 14: writetid == cur_tid -> reset).
      // - If possible_spin is true (e.g. lock cmpxchg acquiring the lock right after spinning at 1302), the spin episode has ended -> resolve and erase it.
      if (m_load_tables.count(tid) > 0)
      {
         std::deque<LoadTableEntry> &load_table = m_load_tables[tid];
         for (std::deque<LoadTableEntry>::iterator it = load_table.begin(); it != load_table.end(); )
         {
            if (it->addr == mem_addr)
            {
               resolveEpisode(core, tid, *it);
               it = load_table.erase(it);
            }
            else
            {
               ++it;
            }
         }
      }

      // Record store in shadow memory (prefer pure release stores over atomic RMW stores)
      m_write_seq++;
      std::unordered_map<thread_id_t, ShadowMemoryEntry> &writers = m_shadow_mem[mem_addr];
      if (!is_atomic || writers.find(tid) == writers.end())
      {
         writers[tid] = ShadowMemoryEntry(pc, tid, m_write_seq, is_atomic);
      }
   }
}

bool SpinLoopDetectorTian::getActiveSpin(thread_id_t tid, IntPtr *out_load_pc, IntPtr *out_lock_addr)
{
   ScopedLock sl(m_lock);
   if (m_active_spins.count(tid) > 0 && m_active_spins[tid].active)
   {
      if (out_load_pc) *out_load_pc = m_active_spins[tid].load_pc;
      if (out_lock_addr) *out_lock_addr = m_active_spins[tid].lock_addr;
      return true;
   }
   if (m_load_tables.count(tid) == 0)
      return false;
   const std::deque<LoadTableEntry> &table = m_load_tables[tid];
   for (std::deque<LoadTableEntry>::const_iterator it = table.begin(); it != table.end(); ++it)
   {
      if (it->possible_spin)
      {
         if (out_load_pc) *out_load_pc = it->pc;
         if (out_lock_addr) *out_lock_addr = it->addr;
         return true;
      }
   }
   return false;
}

void SpinLoopDetectorTian::clearActiveSpin(thread_id_t tid)
{
   ScopedLock sl(m_lock);
   if (m_active_spins.count(tid) > 0)
   {
      m_active_spins[tid].active = false;
   }
}

bool SpinLoopDetectorTian::isSpinning(thread_id_t tid)
{
   ScopedLock sl(m_lock);
   if (m_active_spins.count(tid) > 0 && m_active_spins[tid].active)
      return true;
   if (m_load_tables.count(tid) > 0)
   {
      for (std::deque<LoadTableEntry>::const_iterator it = m_load_tables[tid].begin(); it != m_load_tables[tid].end(); ++it)
      {
         if (it->possible_spin)
            return true;
      }
   }
   return false;
}

bool SpinLoopDetectorTian::anyOtherThreadHoldsLock(thread_id_t tid)
{
   if (UnlockDetector::getSingleton())
      return UnlockDetector::getSingleton()->anyOtherThreadHoldsLock(tid);
   return false;
}

bool SpinLoopDetectorTian::getRecordedIteration(thread_id_t tid, std::vector<RecordedInstruction> &out_iter)
{
   ScopedLock sl(m_lock);
   if (m_prev_iteration.count(tid) > 0 && !m_prev_iteration[tid].empty())
   {
      out_iter = m_prev_iteration[tid];
      return true;
   }
   return false;
}

bool SpinLoopDetectorTian::getRecordedLoopPCs(thread_id_t tid, std::vector<IntPtr> &out_pcs)
{
   ScopedLock sl(m_lock);
   if (m_prev_iteration.count(tid) > 0 && !m_prev_iteration[tid].empty())
   {
      out_pcs.clear();
      for (size_t i = 0; i < m_prev_iteration[tid].size(); ++i)
         out_pcs.push_back(m_prev_iteration[tid][i].pc);
      return true;
   }
   return false;
}

bool SpinLoopDetectorTian::getWaitingThread(IntPtr lock_addr, thread_id_t exclude_tid, thread_id_t &out_waiter_tid)
{
   ScopedLock sl(m_lock);
   for (std::unordered_map<thread_id_t, ActiveSpinInfo>::const_iterator it = m_active_spins.begin(); it != m_active_spins.end(); ++it)
   {
      if (it->first != exclude_tid && it->second.active && it->second.lock_addr == lock_addr)
      {
         out_waiter_tid = it->first;
         return true;
      }
   }
   for (std::unordered_map<thread_id_t, std::deque<LoadTableEntry>>::const_iterator it = m_load_tables.begin(); it != m_load_tables.end(); ++it)
   {
      if (it->first != exclude_tid)
      {
         for (std::deque<LoadTableEntry>::const_iterator lit = it->second.begin(); lit != it->second.end(); ++lit)
         {
            if (lit->possible_spin && lit->addr == lock_addr)
            {
               out_waiter_tid = it->first;
               return true;
            }
         }
      }
   }
   return false;
}
