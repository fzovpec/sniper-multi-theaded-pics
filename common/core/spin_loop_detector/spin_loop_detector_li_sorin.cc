// Implements Li & Sorin (TPDS '06) Spin Detection Hardware

#include "spin_loop_detector_li_sorin.h"
#include "dynamic_micro_op.h"
#include "instruction.h"
#include "core.h"
#include "performance_model.h"

void SpinLoopDetectorLiSorin::onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time)
{
   if (!uop || !uop->getMicroOp())
      return;

   // Li & Sorin tracks Backward Control Transfers (branches)
   if (!uop->getMicroOp()->isBranch())
      return;

   const Instruction *ins = uop->getMicroOp()->getInstruction();
   IntPtr pc = ins ? ins->getAddress() : uop->getMicroOp()->getInstructionPointer().phys;

   ScopedLock sl(m_lock);

   ThreadState &state = m_thread_states[tid];
      // Backward Control Transfer (BCT)
      std::deque<SdtEntry>::iterator entry = state.sdt.begin();
      for (; entry != state.sdt.end(); ++entry)
      {
         if (entry->eip == pc)
            break;
      }

      if (entry == state.sdt.end())
      {
         state.sdt.push_back(SdtEntry(pc, state.next_id, core->getInstructionCount(), time));
         state.next_id = (state.next_id + 1) % SDT_MAX_SIZE;
         if (state.sdt.size() > SDT_MAX_SIZE)
            state.sdt.pop_front();
      }
      else
      {
         entry->iteration_count++;
         if (entry->iteration_count >= 2)
         {
            if (!entry->in_spin)
            {
               entry->in_spin = true;
               core->recordSpinInstance();
            }
            UInt64 delta_ins = (core->getInstructionCount() > entry->icount) ? (core->getInstructionCount() - entry->icount) : 0;
            SubsecondTime delta_t = (time > entry->tcount) ? (time - entry->tcount) : SubsecondTime::Zero();
            core->updateSpinCount(delta_ins, delta_t);
         }
         entry->icount = core->getInstructionCount();
         entry->tcount = time;
      }
}
