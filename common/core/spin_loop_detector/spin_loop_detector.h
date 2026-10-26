#ifndef __SPIN_LOOP_DETECTOR_H
#define __SPIN_LOOP_DETECTOR_H

#include "fixed_types.h"
#include "subsecond_time.h"

class Core;
class DynamicMicroOp;

class SpinLoopDetector
{
   public:
      virtual ~SpinLoopDetector() {}

      // Called on micro-op commit for each core
      virtual void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time) = 0;

      // Factory method following standard Sniper pattern (like RoutineTracer::create, BranchPredictor::create)
      static SpinLoopDetector* create();
};

#endif // __SPIN_LOOP_DETECTOR_H
