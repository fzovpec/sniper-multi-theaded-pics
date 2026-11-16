#ifndef __SPIN_LOOP_DETECTOR_H
#define __SPIN_LOOP_DETECTOR_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include <vector>

class Core;
class DynamicMicroOp;

// Detects spin loops and reports their start and end to the LockStateTracker
class SpinLoopDetector
{
   public:
      virtual ~SpinLoopDetector() {}

      // Called on micro-op commit for each core
      virtual void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time) = 0;

      // Factory method following standard Sniper pattern (like RoutineTracer::create, BranchPredictor::create)
      static SpinLoopDetector* create();

      // Instruction PCs of the last complete iteration of the spin loop detected for a thread
      virtual bool getRecordedLoopPCs(thread_id_t tid, std::vector<IntPtr> &out_pcs) { return false; }
};

#endif // __SPIN_LOOP_DETECTOR_H
