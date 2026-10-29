#ifndef __SPIN_LOOP_DETECTOR_TIAN_H
#define __SPIN_LOOP_DETECTOR_TIAN_H

#include "spin_loop_detector.h"
#include "lock.h"
#include <deque>
#include <vector>
#include <unordered_map>

class DynamicMicroOp;

// Implements Tian et al. (ISSTA '08) Dynamic Recognition of Synchronization Operations
class SpinLoopDetectorTian : public SpinLoopDetector
{
   private:
      static const size_t LOAD_TABLE_MAX_SIZE = 3;
      static const UInt64 MAX_SPIN_BODY_INSNS = 64;

      struct ShadowMemoryEntry
      {
         IntPtr writepc;
         thread_id_t writetid;
         UInt64 writeseq;
         bool is_atomic;

         ShadowMemoryEntry() : writepc(0), writetid(-1), writeseq(0), is_atomic(false) {}
         ShadowMemoryEntry(IntPtr pc, thread_id_t tid, UInt64 seq, bool atomic)
            : writepc(pc), writetid(tid), writeseq(seq), is_atomic(atomic) {}
      };

      struct LoadTableEntry
      {
         IntPtr pc;
         IntPtr addr;
         uint32_t counter;
         bool possible_spin;
         UInt64 start_writeseq;
         UInt64 accum_ins;
         SubsecondTime accum_time;
         UInt64 last_icount;
         SubsecondTime last_tcount;

         LoadTableEntry()
            : pc(0), addr(0), counter(0), possible_spin(false), start_writeseq(0),
              accum_ins(0), accum_time(SubsecondTime::Zero()),
              last_icount(0), last_tcount(SubsecondTime::Zero()) {}
      };

      struct ActiveSpinInfo
      {
         IntPtr load_pc;
         IntPtr lock_addr;
         bool active;
         ActiveSpinInfo() : load_pc(0), lock_addr(0), active(false) {}
         ActiveSpinInfo(IntPtr pc, IntPtr addr) : load_pc(pc), lock_addr(addr), active(true) {}
      };

   public:
      struct RecordedInstruction
      {
         IntPtr pc;
         String disas;
         RecordedInstruction() : pc(0), disas("") {}
         RecordedInstruction(IntPtr _pc, String _disas) : pc(_pc), disas(_disas) {}
      };

   private:
      Lock m_lock;
      UInt32 m_threshold;
      UInt64 m_write_seq;

      // Per-thread load table
      std::unordered_map<thread_id_t, std::deque<LoadTableEntry>> m_load_tables;
      // Per-thread confirmed active spins
      std::unordered_map<thread_id_t, ActiveSpinInfo> m_active_spins;
      // Recorded loop body iterations: current in-progress vs previous completed
      std::unordered_map<thread_id_t, std::vector<RecordedInstruction>> m_current_iteration;
      std::unordered_map<thread_id_t, std::vector<RecordedInstruction>> m_prev_iteration;
      // Global shadow memory: addr -> (tid -> last store by tid)
      std::unordered_map<IntPtr, std::unordered_map<thread_id_t, ShadowMemoryEntry>> m_shadow_mem;
      std::unordered_map<IntPtr, IntPtr> m_syn_table;

      bool findRemoteWriter(IntPtr addr, thread_id_t reader_tid, ShadowMemoryEntry &out_sme) const;
      void resolveEpisode(Core *core, thread_id_t tid, const LoadTableEntry &entry);

   public:
      SpinLoopDetectorTian(UInt32 threshold = 10) : m_threshold(threshold), m_write_seq(0) {}
      virtual ~SpinLoopDetectorTian() {}

      virtual void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time);
      virtual bool getActiveSpin(thread_id_t tid, IntPtr *out_load_pc, IntPtr *out_lock_addr) override;
      virtual bool isSpinning(thread_id_t tid) override;
      virtual bool anyOtherThreadHoldsLock(thread_id_t tid) override;
      virtual void clearActiveSpin(thread_id_t tid) override;
      virtual bool getRecordedLoopPCs(thread_id_t tid, std::vector<IntPtr> &out_pcs) override;
      virtual bool getWaitingThread(IntPtr lock_addr, thread_id_t exclude_tid, thread_id_t &out_waiter_tid) override;
      bool getRecordedIteration(thread_id_t tid, std::vector<RecordedInstruction> &out_iter);
};

#endif // __SPIN_LOOP_DETECTOR_TIAN_H
