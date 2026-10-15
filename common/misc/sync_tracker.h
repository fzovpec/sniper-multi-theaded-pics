#ifndef SYNC_TRACKER_H
#define SYNC_TRACKER_H

#include "fixed_types.h"
#include "subsecond_time.h"

class Instruction;

class SyncTracker
{
public:
   static void onThreadStart(thread_id_t tid, SubsecondTime time);
   static void onThreadExit(thread_id_t tid);
   static void onThreadWaitStart(thread_id_t tid, SubsecondTime time, IntPtr lock_address = 0);
   static void onThreadWaitEnd(thread_id_t tid, SubsecondTime time);
   static void onLockAcquired(thread_id_t tid, SubsecondTime time, IntPtr lock_address = 0);
   static void onUnlock(thread_id_t tid, SubsecondTime time, IntPtr lock_address = 0);
   static void onInstructionCommit(thread_id_t tid, Instruction *ins, SubsecondTime time, IntPtr mem_addr = 0);
};

#endif // SYNC_TRACKER_H

