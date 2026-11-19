#include "sync_variable.h"

UInt64 SyncVariable::otherWrites(thread_id_t tid) const
{
   std::unordered_map<thread_id_t, UInt64>::const_iterator own = stores_by.find(tid);
   return stores_total - ((own != stores_by.end()) ? own->second : 0);
}

SyncVariable::WaiterState &SyncVariable::waiter(thread_id_t tid)
{
   std::unordered_map<thread_id_t, WaiterState>::iterator w = waiters.find(tid);
   if (w != waiters.end())
      return w->second;

   WaiterState &state = waiters[tid];
   std::unordered_map<thread_id_t, UInt64>::const_iterator e = earlier_waits.find(tid);
   // Not capped by the writes by others seen so far: the thread's frontend can be past waits whose writes have not
   // committed yet; those writes are then still to come and used up already
   state.consumed = (e != earlier_waits.end()) ? e->second : 0;
   return state;
}

bool SyncVariable::canPass(thread_id_t tid) const
{
   std::unordered_map<thread_id_t, WaiterState>::const_iterator w = waiters.find(tid);
   if (w == waiters.end())
      return false;
   return w->second.passed || otherWrites(tid) > w->second.consumed;
}

void SyncVariable::pass(thread_id_t tid, IntPtr pc, SubsecondTime time, UInt64 icount)
{
   WaiterState &w = waiter(tid);

   if (otherWrites(tid) > w.consumed)
   {
      w.consumed++;
      w.used_write = true;
   }

   w.passed = true;
   w.pass_pc = pc;
   w.pass_time = time;
   w.pass_icount = icount;
}
