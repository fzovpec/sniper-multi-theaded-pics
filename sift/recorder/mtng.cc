 
/*! @file
 *  This is an example of the PIN tool that demonstrates some basic PIN APIs 
 *  and could serve as the starting point for developing your first PIN tool
 */
//#include "globals.h"
#include "pin.H"
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <map>
#include "control_manager.H"
#define OMP_BEGIN "GOMP_parallel_start"
#define OMP_END "GOMP_parallel_end"
#include "sim_api.h"
#include "onlinebbv_count.h"
#include "globals.h"
//#include "hooks_manager.h"
#include "recorder_control.h"
#include "sift/sift_format.h"
#include "sift_assert.h"
#include "sim_api.h"
#include "threads.h"
#include <cstdint>
#include <set>
#include <utility>
#include <sstream>
#include "threads.h"
#include "cond.h"
//#include "tool_warmup.h"
#include <set>
#include "mtng.h"
#include "to_json.h"
using namespace std;
//#define MARKER_INSERT

namespace mtng{
    std::ostream & operator<<(std::ostream&out ,const RecordType& record_type) {
        std::string print[] ={
        "PointUndef",
        "MultiThread",
        "SingleThread",
        "IntraBarrier",
        "UnknownBlock",
        "UnknownBlockRecord",
        "HardWareEvent",
        "EndPgm"
        };
        out << print[(int32_t)record_type];
        return out;
    }
    std::ostream & operator<<(std::ostream&out ,const SimMode& record_type) {
        std::string print[] = {
            "Detailed",
            "FastForward",
            "Warmup"
        };
        out << print[(int32_t)record_type];
        return out;
    }

    class Key 
    {
        public:
        ADDRINT pc;
        Key()=default;
        ~Key()=default;
        Key( ADDRINT pc_para ) : pc(pc_para)
        {
        }
	    bool operator <(const Key & rhs) const
    	{
        	return pc < rhs.pc;
	    }
	    bool operator !=(const Key & rhs) const
    	{
        	return pc != rhs.pc;
	    }

    };

    std::ostream & operator<<( std::ostream & out , const Record & in) {
        out << (int)in.record_type << " " << (int)in.sim_mode ;
        return out;
    }
    class Mtng
    {
        const uint32_t maximum_thread_num = MAX_NUM_THREADS_DEFAULT;
        std::map< Key , bool > pc_record;
        std::vector<Record> record_vec;
        std::vector<ADDRINT> prior_call_pc_vec;
        std::vector<uint64_t> ins_num_vec;
        uint64_t fs;
        uint64_t current_barrier;
        public:
        
        Mtng() : current_barrier(0) {
            prior_call_pc_vec.resize( maximum_thread_num,0 ); 
            ins_num_vec.resize( maximum_thread_num,0 );
            RecordType record_type = RecordType::SingleThread;
            update_sim();
//            for(uint32_t ti = 0 ; ti < maximum_thread_num ; ti++ ) {
//                uint64_t insnum = thread_data[ti].output->Magic(SIM_CMD_GET_INS_NUM, 0, 0 );
//                ins_num_vec[ti] = insnum;
//            }
//            fs = thread_data[0].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0 );
//            fs = 0;          

            Record record = Record(record_type, SimMode::DetailMode, fs,ins_num_vec,0 );
            record_vec.push_back(record);
        }
        void update_sim() {
            for(uint32_t ti = 0 ; ti < maximum_thread_num ; ti++ ) {
           // for(uint32_t ti = 0 ; ti < 1 ; ti++ ) {
                uint64_t insnum ;
//                if (thread_data[ti].running && thread_data[ti].output) {
                insnum = thread_data[0].output->Magic(SIM_CMD_GET_INS_NUM, ti, 0 );
  //              } else {
//                    insnum = 0;
//                }
                ins_num_vec[ti] = insnum;
            }
            fs = thread_data[0].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0 );
        }
        void fini() {
            update_sim();
            RecordType record_type = RecordType::SingleThread;
            Record record = Record(record_type, SimMode::DetailMode, fs,ins_num_vec,0 );
            record_vec.push_back(record);
        }
        void fflush() {
            std::ofstream output ;
            std::string res_dir = KnobMtngDir.Value();
//            std::string res_dir = "./";
            std::string full_path = res_dir + "/" + "mtng.json";
            output.open( full_path.c_str() , std::ofstream::out );
            output << RecordtoJson( record_vec );
            output.close();
        }
        void add_pc( ADDRINT pc_addr ) {
            Key key = Key(pc_addr);
            //pc_record[ key ] = true;
            pc_record.insert( std::pair<Key,bool>(key,true));
        }
        bool has_key( ADDRINT pc_addr ) {
//            std::cerr << pc_addr << " " << prior_call_pc_vec[0] << "\n";
            if( pc_record.find( Key(prior_call_pc_vec[0]) ) == pc_record.end() ) {
                add_pc(prior_call_pc_vec[0]);
                return false;
            } else {
                return true;
            }
        }
        void recordCall( THREADID tid, ADDRINT pc) {
            prior_call_pc_vec[tid] = pc;
        }
        template< RecordType record_type >
        void changeMode( ADDRINT pc_addr ) {
            Record record;


            bool ret = (bool)thread_data[0].output->Magic(SIM_CMD_GET_BARRIER_REACHED, 0, 0);
            while(!ret) {
                ret = (bool)thread_data[0].output->Magic(SIM_CMD_GET_BARRIER_REACHED, 0, 0);
                PIN_Sleep(100);
                std::cout << "\044Reached: \044"<< ret<<"\n";
            }
            std::cout << "\044Reached: \044"<< ret<<"\n";
            update_sim();
            if( ! has_key(pc_addr ) ) {
                std::cout << "\033[1;31m[BBV   ] BBV not simulated, start "
                 "Sift::ModeDetailed\033[0m\n"
                << std::endl;



                record = Record(record_type, SimMode::DetailMode,fs,ins_num_vec,prior_call_pc_vec[0] );
                setInstrumentationMode(Sift::ModeDetailed);

                thread_data[0].output->Magic(SIM_CMD_ROI_START, 0, 0);
#ifdef MARKER_INSERT
               thread_data[0].output->Magic(SIM_CMD_MARKER, current_barrier, 2 );
#endif
            } else {
                std::cout
                  << "\033[1;32m[BBV   ] BBV simulated, start Sift::ModeIcount\033[0m"
                  << std::endl;

                record = Record(record_type, SimMode::FastForwardMode,fs,ins_num_vec,prior_call_pc_vec[0] );
                setInstrumentationMode(Sift::ModeIcount);

                thread_data[0].output->Magic(SIM_CMD_ROI_END, 0, 0);
#ifdef MARKER_INSERT
                thread_data[0].output->Magic(SIM_CMD_MARKER, current_barrier, 2 );
#endif
            }
            record_vec.push_back( record );
            current_barrier++;
        }
        static Mtng * toMtng( VOID * v ) {
            Mtng * mtng_ptr = reinterpret_cast<Mtng*>(v); 
            if( mtng_ptr == nullptr ) {
                cerr << "null Failed";
            }
            return mtng_ptr;
        }
        
    };
     void recordCall( THREADID threadid, ADDRINT pc_addr, VOID * v  ) {
        if(threadid != 0 ) return;
        Mtng * mtng_ptr = Mtng::toMtng(v);
        mtng_ptr->recordCall(threadid,pc_addr);
    }
   
    void processOmpBegin( THREADID threadid, ADDRINT pc_addr, VOID * v  ) {
        Mtng * mtng_ptr = Mtng::toMtng(v);
        mtng_ptr->changeMode<RecordType::MultiThread>(pc_addr);
    }
    void processOmpEnd( THREADID threadid, ADDRINT pc_addr, VOID * v  ) {
        Mtng * mtng_ptr = Mtng::toMtng(v);
        mtng_ptr->changeMode<RecordType::SingleThread>(pc_addr);
    }
/*
    PIN_LOCK hooks_lock;
    void printRTNName( THREADID threadid, char *rtn_name ) {
        
        PIN_GetLock(&hooks_lock, 0);
        std::cerr << "threadid" << threadid <<" " << rtn_name<<"\n";
        PIN_ReleaseLock(&hooks_lock);
    }
    static void routineCallback(RTN rtn, VOID *v)
    {
        RTN_Open(rtn);
        std::string rtn_name = RTN_Name(rtn).c_str();
                 RTN_InsertCall( rtn, IPOINT_BEFORE, (AFUNPTR)printRTNName,
                     IARG_THREAD_ID,
                     IARG_PTR,strdup(rtn_name.c_str()),
                     IARG_END);

//        if ( rtn_name == OMP_BEGIN )
//        {
//                 RTN_InsertCall( rtn, IPOINT_BEFORE, (AFUNPTR)processOmpBegin,
//                     IARG_THREAD_ID,
//                     IARG_INST_PTR,
//                     IARG_ADDRINT, v,
//                     IARG_END);
//           
//        } else if( rtn_name == OMP_END ) {
//                  RTN_InsertCall( rtn, IPOINT_BEFORE, (AFUNPTR)processOmpEnd,
//                     IARG_THREAD_ID,
//                     IARG_INST_PTR,
//                     IARG_ADDRINT, v,
//                     IARG_END);
//       
//        }
        RTN_Close(rtn);
    }*/
const string Target2String(ADDRINT target)
{
    string name = RTN_FindNameByAddress(target);
    if (name == "")
        return "";
    else
        return name;
}
VOID
traceCallbackForBarrierPoint(TRACE trace, void* v)
{

  RTN rtn = TRACE_Rtn(trace);

  if( RTN_Valid(rtn) ) 
  {
      for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) 
      {
         INS tail = BBL_InsTail(bbl);
         if( INS_IsCall(tail) ) 
         {

            INS_InsertPredicatedCall( tail, IPOINT_BEFORE, (AFUNPTR) recordCall ,
            IARG_THREAD_ID,
            IARG_INST_PTR,
            IARG_ADDRINT, v,
            IARG_END);

         }
//          if( INS_IsCall(tail ) && INS_IsDirectControlFlow(tail) ) {
//             const ADDRINT target = INS_DirectControlFlowTargetAddress(tail);
//             std::cout << " pc : " << target << "\n";
//          }
      }
  }
  if (RTN_Valid(rtn)    
      && RTN_Address(rtn) == TRACE_Address(trace))
  {
      std::string rtn_name = RTN_Name(rtn).c_str();
      BBL bbl = TRACE_BblHead(trace);
      INS ins = BBL_InsHead(bbl);
      if (
          (
           (rtn_name.find("GOMP_parallel_start") != std::string::npos) ||
           (rtn_name.find("GOMP_parallel") != std::string::npos)
          )
           &&
          !(
           (rtn_name.find("@plt") != std::string::npos) ||
           (rtn_name.find("GOMP_parallel_end") != std::string::npos)
          )
         )
      {
                 INS_InsertPredicatedCall( ins, IPOINT_BEFORE, (AFUNPTR)processOmpBegin,
                     IARG_THREAD_ID,
                     IARG_INST_PTR,
                     IARG_ADDRINT, v,
                     IARG_END);
           
      } else 
//      if( rtn_name == OMP_END )
      if (
          (
           (rtn_name.find("GOMP_parallel_end") != std::string::npos)
          )
           &&
          !(
           (rtn_name.find("@plt") != std::string::npos)
          )
         )
      {
                  INS_InsertPredicatedCall( ins, IPOINT_BEFORE, (AFUNPTR)processOmpEnd,
                     IARG_THREAD_ID,
                     IARG_INST_PTR,
                     IARG_ADDRINT, v,
                     IARG_END);
       
      }

  }
}
    VOID Fini( INT32 code,void *v)
    {
        Mtng * mtng_ptr = Mtng::toMtng( v );
        mtng_ptr->fini();
        mtng_ptr->fflush();
        delete mtng_ptr;
    }



    void activate() {
        Mtng *  mtng_global = new Mtng();
//        RTN_AddInstrumentFunction(routineCallback, 0);
        TRACE_AddInstrumentFunction(traceCallbackForBarrierPoint, (void*)mtng_global ); 
        PIN_AddFiniFunction(Fini, (void*)mtng_global);
    }

};
