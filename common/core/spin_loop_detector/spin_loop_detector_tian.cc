// Implements Tian et al. (ISSTA '08) Dynamic Recognition of Synchronization Operations

#include "spin_loop_detector_tian.h"
#include "lock_state_tracker.h"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"
#include "performance_model.h"
#include "simulator.h"
#include "stats.h"
#include <inttypes.h>
#include <cstdio>

// Find the preferred store by another thread that overlaps [addr, addr + size).
// Stores made before the reader started are ignored: the reader cannot be waiting for them
// (e.g. the creating thread initializing the new thread's thread-local variables, or a lock before the workers start).
bool SpinLoopDetectorTian::findRemoteWriter(IntPtr addr, UInt32 size, thread_id_t reader_tid, ShadowMemoryEntry &out_sme) const
{
   bool found = false;

   std::unordered_map<thread_id_t, UInt64>::const_iterator start_it = m_thread_start_seq.find(reader_tid);
   UInt64 reader_start = (start_it != m_thread_start_seq.end()) ? start_it->second : 0;

   // Shadow memory is keyed by store address: a store overlaps if it starts inside the range or less than MAX_ACCESS_SIZE before it
   for (IntPtr store_addr = addr - (MAX_ACCESS_SIZE - 1); store_addr < addr + size; ++store_addr)
   {
      std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ShadowMemoryEntry>>::const_iterator addr_it = m_shadow_mem.find(store_addr);
      if (addr_it == m_shadow_mem.end())
         continue;

      const std::unordered_map<thread_id_t, ShadowMemoryEntry> &writers = addr_it->second;
      for (std::unordered_map<thread_id_t, ShadowMemoryEntry>::const_iterator it = writers.begin(); it != writers.end(); ++it)
      {
         if (it->first == reader_tid)
            continue;

         const ShadowMemoryEntry &cand = it->second;
         if (store_addr + cand.size <= addr)
            continue;
         if (cand.writeseq <= reader_start)
            continue;

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
   }
   return found;
}

SpinLoopDetectorTian::~SpinLoopDetectorTian()
{
}

void SpinLoopDetectorTian::resolveEpisode(Core *core, thread_id_t tid, const LoadTableEntry &entry)
{
   if (!entry.possible_spin)
      return;

   ShadowMemoryEntry sme;
   if (findRemoteWriter(entry.addr, entry.size, tid, sme))
   {
      core->recordSpinInstance();
      core->updateSpinCount(entry.counter, entry.accum_ins, entry.accum_time);

      SubsecondTime spin_start = (entry.last_tcount > entry.accum_time) ? (entry.last_tcount - entry.accum_time) : SubsecondTime::Zero();

      // Record synchronization events in SQLite stats for timeline plotting
      if (Sim()->getStatsManager())
      {
         Sim()->getStatsManager()->recordSyncEvent(tid, "begin_lock_acquisition", spin_start.getNS(), entry.addr);
         Sim()->getStatsManager()->recordSyncEvent(tid, "end_lock_acquisition", entry.last_tcount.getNS(), entry.addr);
      }
   }
}

void SpinLoopDetectorTian::endActiveSpin(thread_id_t tid)
{
   m_active_spins[tid].active = false;
   m_current_iteration[tid].clear();

   if (Sim()->getLockStateTracker())
      Sim()->getLockStateTracker()->onSpinEnd(tid);
}

void SpinLoopDetectorTian::onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time)
{
   if (!uop || !uop->getMicroOp())
      return;

   const Instruction *ins = uop->getMicroOp()->getInstruction();
   IntPtr pc = LockStateTracker::getPC(uop);
   bool is_atomic = ins ? ins->isAtomic() : false;

   ScopedLock sl(m_lock);

   if (m_thread_start_seq.find(tid) == m_thread_start_seq.end())
      m_thread_start_seq[tid] = m_write_seq;

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

   UInt32 mem_size = uop->getMicroOp()->getMemoryAccessSize();
   if (mem_size == 0)
      mem_size = 1;

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
            }
            // Activate once per episode, and report the spin if another thread writes the loaded variable
            if (loc->counter == m_threshold)
            {
               m_active_spins[tid] = ActiveSpinInfo(loc->pc);

               ShadowMemoryEntry sme;
               if (Sim()->getLockStateTracker() && findRemoteWriter(loc->addr, loc->size, tid, sme))
               {
                  // Stores to the variable seen so far, per thread
                  std::unordered_map<thread_id_t, UInt64> store_counts;
                  std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ShadowMemoryEntry>>::const_iterator sm = m_shadow_mem.find(loc->addr);
                  if (sm != m_shadow_mem.end())
                  {
                     for (std::unordered_map<thread_id_t, ShadowMemoryEntry>::const_iterator s = sm->second.begin(); s != sm->second.end(); ++s)
                        store_counts[s->first] = s->second.count;
                  }
                  Sim()->getLockStateTracker()->onSpinStart(tid, loc->addr, loc->size, loc->pc, store_counts);
               }
            }
         }
         else
         {
            // Either loaded a different address, or re-entered this PC in a new spin episode after leaving the loop
            resolveEpisode(core, tid, *loc);
            if (m_active_spins.count(tid) > 0 && m_active_spins[tid].active && m_active_spins[tid].load_pc == loc->pc)
               endActiveSpin(tid);

            loc->addr = mem_addr;
            loc->size = mem_size;
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
         new_entry.size = mem_size;
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
      // Spin-wait loops do not execute stores in their loop body:
      // - Erase any unconfirmed (!possible_spin) load candidates so normal loops with stores don't accumulate loop-invariant loads.
      // - If possible_spin is true and the store overlaps the loaded variable (e.g. lock cmpxchg acquiring the lock), resolve and erase it.
      if (m_load_tables.count(tid) > 0)
      {
         std::deque<LoadTableEntry> &load_table = m_load_tables[tid];
         for (std::deque<LoadTableEntry>::iterator it = load_table.begin(); it != load_table.end(); )
         {
            if (mem_addr < it->addr + it->size && it->addr < mem_addr + mem_size)
            {
               resolveEpisode(core, tid, *it);
               it = load_table.erase(it);
            }
            else if (!it->possible_spin)
            {
               it = load_table.erase(it);
            }
            else
            {
               ++it;
            }
         }
      }

      // A store by the spinning thread itself means it left the spin loop
      if (m_active_spins.count(tid) > 0 && m_active_spins[tid].active)
         endActiveSpin(tid);

      // Record store in shadow memory (prefer pure release stores over atomic RMW stores)
      m_write_seq++;
      std::unordered_map<thread_id_t, ShadowMemoryEntry> &writers = m_shadow_mem[mem_addr];
      UInt64 store_count = (writers.find(tid) == writers.end()) ? 0 : writers[tid].count;
      if (!is_atomic || writers.find(tid) == writers.end())
      {
         writers[tid] = ShadowMemoryEntry(pc, tid, m_write_seq, is_atomic, mem_size);
      }
      writers[tid].count = store_count + 1;
   }
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
