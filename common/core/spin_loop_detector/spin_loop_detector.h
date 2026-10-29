#ifndef __SPIN_LOOP_DETECTOR_H
#define __SPIN_LOOP_DETECTOR_H

#include "fixed_types.h"
#include "subsecond_time.h"
#include <vector>

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

      // Query if a thread is actively spinning and get its load PC and lock address
      virtual bool getActiveSpin(thread_id_t tid, IntPtr *out_load_pc, IntPtr *out_lock_addr) { return false; }
      virtual bool isSpinning(thread_id_t tid) { return false; }
      virtual bool anyOtherThreadHoldsLock(thread_id_t tid) { return false; }
      virtual void clearActiveSpin(thread_id_t tid) {}
      virtual bool getRecordedLoopPCs(thread_id_t tid, std::vector<IntPtr> &out_pcs) { return false; }
      virtual bool getWaitingThread(IntPtr lock_addr, thread_id_t exclude_tid, thread_id_t &out_waiter_tid) { return false; }
};

#endif // __SPIN_LOOP_DETECTOR_H
