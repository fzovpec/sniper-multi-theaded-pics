#ifndef __SPIN_LOOP_DETECTOR_LI_SORIN_H
#define __SPIN_LOOP_DETECTOR_LI_SORIN_H

#include "spin_loop_detector.h"
#include "lock.h"
#include <deque>
#include <unordered_map>

class DynamicMicroOp;

// Implements Li & Sorin (TPDS '06) Spin Detection Hardware
class SpinLoopDetectorLiSorin : public SpinLoopDetector
{
   private:
      static const size_t SDT_MAX_SIZE = 16;

      struct SdtEntry
      {
         IntPtr eip;
         uint8_t id;
         UInt64 icount;
         SubsecondTime tcount;
         UInt32 iteration_count;
         bool in_spin;

         SdtEntry(IntPtr _eip, uint8_t _id, UInt64 _icount, SubsecondTime _tcount)
            : eip(_eip), id(_id), icount(_icount), tcount(_tcount), iteration_count(0), in_spin(false) {}
      };

      struct ThreadState
      {
         std::deque<SdtEntry> sdt;
         uint8_t next_id;
         ThreadState() : sdt(), next_id(0) {}
      };

      Lock m_lock;
      std::unordered_map<thread_id_t, ThreadState> m_thread_states;

   public:
      SpinLoopDetectorLiSorin() {}
      virtual ~SpinLoopDetectorLiSorin() {}

      virtual void onMicroOpCommit(Core *core, thread_id_t tid, const DynamicMicroOp *uop, SubsecondTime time);
};

#endif // __SPIN_LOOP_DETECTOR_LI_SORIN_H
