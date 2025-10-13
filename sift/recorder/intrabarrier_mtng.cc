
/*! @file
 *  This is an example of the PIN tool that demonstrates some basic PIN APIs 
 *  and could serve as the starting point for developing your first PIN tool
 */
//#include "globals.h"
#include "pin.H"
#include "lock.h"
#include "cond.h"
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
#include "log2.h"
#include "bbv_count_cluster.h"
//#define tuple pair
#include "tuple_hash.h"
using namespace std;
//#define MARKER_INSERT
#include <queue>
#include <deque>
#include <iostream>
#include <functional>
//#include <tuple>
#include <utility>
#include <cmath>
#include "to_json.h"  
#include "trietree.h"
#include "intrabarrier_common.h"
THREADID master_thread_id = 0;
uint32_t global_maxthread_num;

namespace intrabarrier_mtng{
//**common mtng
VOID sendInstruction(THREADID threadid, ADDRINT addr, UINT32 size,  BOOL is_branch, BOOL taken, BOOL is_predicate, BOOL executing, BOOL isbefore, BOOL ispause)
{
   // We're still called for instructions in the same basic block as ROI end, ignore these
   if (!thread_data[threadid].output)
   {
      thread_data[threadid].num_dyn_addresses = 0;
      return;
   }

   // Reconstruct basic blocks (we could ask Pin, but do it the same way as TraceThread will do it)
   if (thread_data[threadid].bbv_end || thread_data[threadid].bbv_last != addr)
   {
      // We're the start of a new basic block
      thread_data[threadid].bbv->count(thread_data[threadid].bbv_base, thread_data[threadid].bbv_count);
      thread_data[threadid].bbv_base = addr;
      thread_data[threadid].bbv_count = 0;
   }
   thread_data[threadid].bbv_count++;
   thread_data[threadid].bbv_last = addr + size;
   // Force BBV end on non-taken branches
   thread_data[threadid].bbv_end = is_branch;
}
//
VOID insertCall(INS ins, IPOINT ipoint,  BOOL is_branch, BOOL taken)
{
   INS_InsertCall(ins, ipoint,
      AFUNPTR(sendInstruction),
      IARG_THREAD_ID,
      IARG_ADDRINT, INS_Address(ins),
      IARG_UINT32, UINT32(INS_Size(ins)),
      IARG_BOOL, is_branch,
      IARG_BOOL, taken,
      IARG_BOOL, INS_IsPredicated(ins),
      IARG_EXECUTING,
      IARG_BOOL, ipoint == IPOINT_BEFORE,
      IARG_BOOL, INS_Opcode(ins) == XED_ICLASS_PAUSE,
      IARG_END);
}


static VOID traceCallback(TRACE trace, void *v) {

      if (current_mode != Sift::ModeDetailed) {
        BBL bbl_head = TRACE_BblHead(trace);
        for (BBL bbl = bbl_head; BBL_Valid(bbl); bbl = BBL_Next(bbl))
         for(INS ins = BBL_InsHead(bbl); ; ins = INS_Next(ins))
         {
            // For memory instructions, collect all addresses at IPOINT_BEFORE

            bool is_branch = INS_IsBranch(ins) && INS_HasFallThrough(ins);

            if (is_branch && INS_IsValidForIpointTakenBranch(ins) && INS_IsValidForIpointAfter(ins))
            {
               insertCall(ins, IPOINT_AFTER,         true  /* is_branch */, false /* taken */);
               insertCall(ins, IPOINT_TAKEN_BRANCH,  true  /* is_branch */, true  /* taken */);
            }
            else
            {
               // Whenever possible, use IPOINT_AFTER as this allows us to process addresses after the application has used them.
               // This ensures that their logical to physical mapping has been set up.
               insertCall(ins, INS_IsValidForIpointAfter(ins) ? IPOINT_AFTER : IPOINT_BEFORE,  false /* is_branch */, false /* taken */);
            }

            if (ins == BBL_InsTail(bbl))
               break;
         }
      }
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
//    enum class FeatureLevel {
//        Unknown = 0 ,
//        Detailed = 1,
//        FFW_From_Detailed = 2,
//        FFW_From_Repaired = 3,
//        FFW_From_Global = 4,
//    };
    enum class DataLevel: uint32_t {
        Unknown = 0,
        FromSelf = 1,
        FromLocal = 2, // from prior barrier
        FromGlobal = 3, 
        FromReClustering = 4,
        FromOtherCluster = 5,
        FromGlobalAvg = 6,
    };
enum class PredictResultType:uint8_t{
    //predict 2 real 
    F2F = 0, 
    F2D = 1,
    D2F = 2,
    D2D = 3,
};


    template<bool dvfs_enable>
    class Feature {
        public:
            FeatureKey<dvfs_enable> feature_key;
            uint32_t idx;
            uint64_t fs;
            double timeperins;
            uint64_t max_ins_num;
            uint64_t ins_nums[MAX_NUM_THREADS_DEFAULT];
            uint64_t  cluster_id;
            uint32_t freq;
            PredictResultType predict_result_type;
            ADDRINT pc;
            DataLevel feature_level;
            mtng::RecordType record_type;
            bool solved;
            uint64_t l2_count;
            uint64_t l2_miss_count;

            std::string toJson() {
                std::stringstream out;
                out << " { \n";
                out << "\"max_ins_num\" : " << max_ins_num << " , \n";
                out << "\"fs\" : " << fs << " , \n";
                out << "\"l2count\" : " << l2_count << " , \n";
                out << "\"l2misscount\" : " << l2_miss_count << " , \n";
                out << "\"clusterid\" : " << cluster_id << " , \n";
                out << "\"pc\" : " << pc << " , \n";
                out << "\"freq\" : " << freq << " , \n";
                out << "\"predict_result_type\" : " << (uint32_t)predict_result_type << " , \n";
                out << "\"recordtype\" : " << (int)record_type << " , \n";
                out << insertElem("insnums",ins_nums,global_maxthread_num) << " , \n";
                out << insertElem("bbv",intrabarrier_get_bbv(idx),global_maxthread_num * BBV_DIM) << " , \n";
                out << "\"feature_level\" : " << (int)feature_level << " \n";
                out << " }";
                return out.str();
            }
            void SetFreq(uint32_t freq_in) {
                freq= freq_in;
                feature_key = FeatureKey<dvfs_enable>(cluster_id,freq);
            }
            


            Feature( uint64_t fs_, uint64_t max_ins_num_ ): fs(fs_),max_ins_num(max_ins_num_){
            
                timeperins = (double)fs / (double)max_ins_num;
                solved = true;
            }


            Feature( uint64_t fs_, const std::vector<uint64_t> & insnums, mtng::RecordType record_type_in, uint64_t l2_count_,uint64_t l2_miss_count_ ):
            fs(fs_), record_type(record_type_in),solved(true),l2_count(l2_count_),l2_miss_count(l2_miss_count_) //use for detailed mode
            {
                feature_level = DataLevel::FromSelf; 
                max_ins_num = 0;
                for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                    ins_nums[i] = insnums[i];
                    max_ins_num = std::max(max_ins_num,ins_nums[i]);
//                    max_ins_num = max_ins_num + ins_nums[i];
                }
                timeperins = (double)fs / (double)max_ins_num;
            }
            Feature(): fs(0), feature_level( DataLevel::Unknown ), solved(false){
                 
            }
            void extrapolate( const Feature & detailed ,DataLevel feature_level_in = DataLevel::FromLocal) {
                solved = true;
                double fs_double = detailed.timeperins * max_ins_num;
                fs = (uint64_t) fs_double;
                timeperins = detailed.timeperins;
                feature_level = feature_level_in; 
            }
            
            void extrapolate( const Feature * last_feature, const Feature * next_feature ,DataLevel feature_level_in = DataLevel::FromLocal) {
                solved = true;
                if( next_feature != NULL && next_feature->solved ) {
                 //   timeperins = ( last_feature->timeperins + next_feature->timeperins ) / 2;
                 if( last_feature == NULL ) {

                    timeperins = next_feature->timeperins ;
                 } else {
                     double similar1 =  get_intra_bbv_distance( last_feature->idx, idx ,global_maxthread_num); 
                     double similar2 =  get_intra_bbv_distance( next_feature->idx, idx ,global_maxthread_num); 
                     if( similar1 > similar2 ) {
                         timeperins = next_feature->timeperins ;
                     } else {
                         timeperins = last_feature->timeperins ;
                     }
                 }
                } else {
                    timeperins = last_feature->timeperins ;
                }
                
                double fs_double = timeperins * max_ins_num;
                fs = (uint64_t) fs_double;
                feature_level = feature_level_in; 
            }


            void set_cluster_id( uint64_t cluster_id_) {
                cluster_id = cluster_id_;
            }
            Feature(  const std::vector<uint64_t> & insnums, mtng::RecordType record_type_in ): record_type(record_type_in),solved(false){
                 max_ins_num = 0;
                 for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                    ins_nums[i] = insnums[i];
//                    max_ins_num = max_ins_num + ins_nums[i];
                    max_ins_num = std::max(max_ins_num,ins_nums[i]);
                }
                fs = 0;
            }
            void merge( const Feature & r ) {
                if( solved ) {
                    if(r.solved) {
                        max_ins_num += r.max_ins_num;
                        fs += r.fs;
                    } else {
                        
                        max_ins_num += r.max_ins_num;
                        double fs_double = timeperins  * (  max_ins_num );
                        fs = (uint64_t) fs_double;        
                    }
                } else {
                    max_ins_num += r.max_ins_num;
                }
            }
            void add( const Feature & l ) {
                fs += l.fs;
                max_ins_num += l.max_ins_num;
                timeperins = (double)fs / (double)max_ins_num;
            }
    };
    std::ostream & operator<<( std::ostream & out, const Feature<false> & feature ) {
        out << "fs : " << feature.fs/1e15 << " pc: " << feature.pc << " insnum: " << feature.max_ins_num << "clusterid: " << feature.cluster_id << " solved " << feature.solved;
        return out;
    }

    
    std::ostream & operator<<( std::ostream & out, const Feature<true> & feature ) {
        out << "fs : " << feature.fs/1e15 << " pc: " << feature.pc << " insnum: " << feature.max_ins_num << "clusterid: " << feature.cluster_id << " solved " << feature.solved;
        return out;
    }
//std::vector<uint64_t> no_pointer;
template<bool dvfs_enable>
class Predictor { 
        int32_t history_queue_size = 0;
//        std::unordered_map<uint64_t, uint64_t> hash2clusterid;//use history to predict cluster id
//        std::unordered_map<uint64_t, Feature> clusterid2feature;//use history to predict cluster id
        std::unordered_map<FeatureKey<dvfs_enable>, Feature<dvfs_enable>> clusterid2feature;//use history to predict cluster id
        std::unordered_map<FeatureKey<dvfs_enable>, int> unknown_clusterid;//unknown cluster id
        std::unordered_map<uint64_t, uint32_t> clusterid2number;//use history to predict cluster id
        uint64_t most_frequent_cluster_id;
        uint64_t max_num_clusterid = 0;
        std::vector<uint32_t> idxes_in_historyqueue;
        uint64_t last_predicted_cluster_id=0;
        bool first_update = true;
        std::vector<uint64_t> & cluster_id_queue;
        mtng::SimMode & mtng_current_mode;
        TrieClass trietree;
    public:
//            Predictor()= delete;
//
            Predictor( std::vector<uint64_t>& cluster_id_queue_para, mtng::SimMode & mtng_current_mode_para  ): cluster_id_queue(cluster_id_queue_para), mtng_current_mode(mtng_current_mode_para) {
            };
//            Predictor():cluster_id_queue( no_pointer ){
//            };
//            Predictor & operator=(const Predictor & predictor ) {
//                this->cluster_id_queue = predictor.cluster_id_queue;
//                return *this;
//            }
            void InitFeature( Feature<dvfs_enable> * feature ) {
                clusterid2feature[feature->feature_key] = *feature;
                most_frequent_cluster_id = feature->cluster_id;
                max_num_clusterid=1;
                last_predicted_cluster_id=most_frequent_cluster_id;
                clusterid2number[last_predicted_cluster_id]=1;
            }
            bool try2FixFeature( Feature<dvfs_enable> & feature , DataLevel datalevel=DataLevel::FromLocal) {
                uint64_t cluster_id = feature.cluster_id;
                uint32_t freq = feature.freq;
                auto feature_key = FeatureKey<dvfs_enable>(cluster_id,freq);
                auto clusterid2feature_it =  clusterid2feature.find( feature_key );
                if(clusterid2feature_it == clusterid2feature.end()) return false;
                feature.extrapolate( clusterid2feature_it->second , datalevel);
                return true;
            }
            
            uint64_t get_cluster_id_from_history() {
                if(cluster_id_queue.size()<=0) return -1; 
                return trietree.search_trie( cluster_id_queue );
            }


            std::pair<mtng::SimMode,uint32_t> decideNextSimMode( uint32_t freq ) {
                uint64_t cluster_id = 0;
//                uint64_t hash = 0;
                mtng::SimMode next_mode;
                if( unknown_clusterid.size() == 0 ) { // only have one cluster
                    cluster_id = last_predicted_cluster_id;
                    if( !dvfs_enable ) { 
                        next_mode =  mtng::SimMode::FastForwardMode;
                        return std::pair<mtng::SimMode,uint32_t>( next_mode,cluster_id );
                    }
                } else {
                    cluster_id = get_cluster_id_from_history();

                    if( cluster_id == (uint64_t)-1 ) {
                       // cluster_id = most_frequent_cluster_id;
                        cluster_id = last_predicted_cluster_id;
                    }
                }
                //LOG2(INFO) << "predict cluster id : " << cluster_id << " "<<hash <<" " << history_queue_size << "\n";
                
                //last_predicted_cluster_id = cluster_id;
                auto feature_key = FeatureKey<dvfs_enable>(cluster_id,freq);
                auto clusterid2feature_it = clusterid2feature.find(feature_key);
                
                if( clusterid2feature_it == clusterid2feature.end()  ) {
                    next_mode = mtng::SimMode::DetailMode;
                } else {
                    next_mode =  mtng::SimMode::FastForwardMode;
                }
                return std::pair<mtng::SimMode,uint32_t>( next_mode,cluster_id );
            }
            bool if_predict_right( uint64_t cluster_id ) {
                if(first_update) {
                    first_update = false;
                    return true;
                } 
                if( last_predicted_cluster_id == cluster_id ) {
                    return true;
                } else {
                    return false;
                }
            }
            uint64_t get_hash_value( int32_t idx ) {
                uint64_t seed = 0;
                int32_t j = 0; 
                for( int i = idx; j < history_queue_size ; i--, j++ ) {
                    auto value = i >= 0 ? cluster_id_queue[ i ] : -1;
                    seed ^= std::hash<uint64_t>()( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2); 
                }
                
                return seed;
            }
#define MAX_HISTORY_QUEUE (16)
            void onlyUpdateClusterFeature( uint64_t cluster_id,const Feature<dvfs_enable> & feature ) { // give up if exiting cluster id
               auto freq = feature.freq;

               auto  feature_key = FeatureKey<dvfs_enable>( cluster_id,freq);
               auto cluster_it = clusterid2feature.find(feature_key );
               if( cluster_it == clusterid2feature.end() ) {
                   clusterid2feature[ feature_key ] = feature;

                    auto unknowncluster_it = unknown_clusterid.find( feature_key );
                    if( unknowncluster_it != unknown_clusterid.end() ) {
                        unknown_clusterid.erase(unknowncluster_it);
                    }

               }

            }

            PredictResultType updateCurPredictor( uint64_t cluster_id, Feature<dvfs_enable> & feature ) {
                //update feature data
                idxes_in_historyqueue.push_back( cluster_id_queue.size()-1 );
                trietree.insert_trie(cluster_id_queue);
                PredictResultType predict_result_type;
                assert( cluster_id_queue[idxes_in_historyqueue[idxes_in_historyqueue.size()-1]] ==cluster_id );
                if( mtng_current_mode == mtng::SimMode::DetailMode ) {
                    uint32_t freq = feature.freq;
                    auto feature_key = FeatureKey<dvfs_enable>(cluster_id,freq);
                    auto cluster_it = clusterid2feature.find( feature_key);
                    if( cluster_it == clusterid2feature.end() ) {
                        clusterid2feature[feature_key] = feature;

                        predict_result_type = PredictResultType::D2D;
                    } else {
                        cluster_it->second.add(feature);
                        predict_result_type = PredictResultType::D2F;
                    }
                    auto unknowncluster_it = unknown_clusterid.find( feature_key );
                    if( unknowncluster_it != unknown_clusterid.end() ) {
                        unknown_clusterid.erase(unknowncluster_it);
                    }
                   
                } else {
                    uint32_t freq = feature.freq;
                    auto feature_key = FeatureKey<dvfs_enable>(cluster_id,freq);
                    auto cluster_it = clusterid2feature.find( feature_key);
 
                    if( cluster_it == clusterid2feature.end() ) { //find unknown cluster id, enable history queue to find that
                        auto unknowncluster_it = unknown_clusterid.find( feature_key );
                        if( unknowncluster_it == unknown_clusterid.end() ) {
                            unknown_clusterid[ feature_key ] = 1;
                        } else {
                            unknowncluster_it->second++;
                        }

                        predict_result_type = PredictResultType::F2D;
                    } else {
                        feature.extrapolate( cluster_it->second );    
                        predict_result_type = PredictResultType::F2F;
                    }
                }
//                auto clusterid2number_it = clusterid2number.find(cluster_id);
//                if(clusterid2number_it == clusterid2number.end() ) {
//                    clusterid2number[cluster_id] = 1;
//                    if(max_num_clusterid == 0) {
//                        most_frequent_cluster_id = cluster_id;
//                    }
//                } else {
//                    auto & num = clusterid2number[cluster_id] ;
//                    num++;
//                    if(num > max_num_clusterid) {
//                        max_num_clusterid = num;
//                        most_frequent_cluster_id = cluster_id; 
//                    }
//                }
                //update predictor
                if( ! if_predict_right( cluster_id ) ) {
//                    LOG2(INFO) << "predict : " << last_predicted_cluster_id << " actual cluster id : " << cluster_id << "\n"; 
                    //update_predictor_size(); 
                } else {
                    
  //                  LOG2(INFO) << "predict success : " <<  cluster_id << "\n"; 
                }

                last_predicted_cluster_id = cluster_id;
                return predict_result_type;
            }
    };
    class VirtualMtng{
        public:    
        virtual void intraBarrierInstruction( THREADID tid, ADDRINT addr ) = 0 ;
        virtual void recordInsCount( THREADID tid, uint32_t insnum ,ADDRINT addr) =0;
        virtual void changeSimMode( mtng::SimMode ret ) =0;
        virtual void fini( ADDRINT pcaddr ) = 0 ;
        virtual void processOmpBegin( ADDRINT pcaddr  )= 0 ;
        virtual void processOmpEnd( ADDRINT pcaddr  )= 0 ;
        virtual void collectData(ADDRINT pcaddr,uint32_t freq) = 0 ;
        virtual void threadStart( THREADID threadid ) =0 ;
        virtual void threadEnd( THREADID threadid ) = 0 ;
        virtual void init_ipc(uint32_t tid) = 0;

        virtual void SetFreq(uint32_t freq_in) = 0;
        static VirtualMtng * toMtng( void * v) {
            VirtualMtng * mtng_ptr = reinterpret_cast<VirtualMtng*>(v);
            if( mtng_ptr == nullptr ) {
                cerr << "null Failed";
            }
            return mtng_ptr;
        }
        virtual ~VirtualMtng()=default;
    };

    inline double getSystemTime()
    {
        struct timeval timer;
        gettimeofday(&timer, 0);
        return ((double)(timer.tv_sec) + (double)(timer.tv_usec)*1.0e-6);
    }



    template<bool dvfs_enable>
    class Mtng : public VirtualMtng
    {
        mtng::RecordType last_record_type = mtng::RecordType::SingleThread;
        std::vector<uint64_t> m_threads_to_signal;
        const uint32_t maximum_thread_num = MAX_NUM_THREADS_DEFAULT;
//        std::unordered_map< Key , bool > pc_record;
        std::map< Key , bool > pc_record;
//        std::vector<Record> record_vec;
        uint64_t prior_key;
        std::vector<ADDRINT> prior_call_pc_vec;
        std::vector<uint64_t> ins_num_vec;
        std::vector<uint64_t> ins_num_interval_vec;
        std::vector<uint64_t> bbv_ins_num_vec;
        std::vector<uint64_t> bbv_ins_interval_num_vec;
#define TIME_CLUSTER (0)
#define TIME_PREDICTION (1)
#define TIMES_SIZE (2)
        std::vector<double>  time_distribution;

        mtng::SimMode mtng_current_mode = mtng::SimMode::DetailMode;
        std::vector<int> detect_unknow_block_in_fast_forward_mode;
        bool record_unknow_enable; 
        const uint64_t threshold ;
        const uint64_t minimum_threshold ;
//        const static uint32_t threshold = 1000000;
        uint64_t next_target = 0;
        std::vector<uint64_t> interval_ins_num_vec;
        PIN_LOCK intra_lock;
        uint32_t global_count = 0;
        Lock lock;
        std::vector<ConditionVariable*> m_core_cond;

        bool enter_lock = false;
        ADDRINT region_begin_pc;
        ADDRINT region_end_pc;
        ///////for dvfs support
        uint32_t freq;
        uint32_t next_freq;
        //
        uint64_t fs;
        uint64_t l2_count;
        uint64_t l2_miss_count;
        uint64_t prior_cluster_id;
        uint64_t prior_fs;
        uint64_t prior_l2_miss_count;
        uint64_t prior_l2_count;
        uint64_t current_barrier;
        uint64_t next_pc_if_not_branch;
        bool is_branch_inst; 
        double time_sum;
        std::vector<uint32_t> enable_warmup_vec;
        std::vector<std::unordered_map< uint64_t , uint32_t>> intrabarrier_bbvs;
        std::vector<std::unordered_map<uint64_t, uint32_t>> record_unknown_bbvs;
        uint64_t intra_id = 0;
        std::unordered_map<uint64_t,Predictor<dvfs_enable>*> hash2predictor;
        /////////unknown cluster
        std::unordered_map<FeatureKey<dvfs_enable>, int> unknown_clusters;
        std::unordered_map<FeatureKey<dvfs_enable>, Feature<dvfs_enable>> cluster2feature;
        std::vector<uint64_t> cluster_id_queue;
        std::vector<uint32_t>fast_forward_sync_with_backend;
#define queue_size 1
        FixedQueue<uint64_t,queue_size> feature_queue;
        ///output
        std::vector<Feature<dvfs_enable>> feature_stack;
        Feature<dvfs_enable> feature;
        public:
        
        uint32_t max_thread_num;
        std::string toJson() {
            std::stringstream out ;
            //////////feature stack
            out << "{";
            out << "\"time_sum\": " << time_sum << ",\n";
            out << "\"time_cluster\": " << time_distribution[TIME_CLUSTER] << ",\n";
            out << "\"time_prediction\": " << time_distribution[TIME_PREDICTION] << ",\n";
            out << "\"feature_stack\" : " << "\n";
            out << "[ \n";
            if(feature_stack.size() > 0) {
                for( uint32_t i = 0 ; i < feature_stack.size() - 1; i++ ) {
                    auto & elem = feature_stack[i];
                    out << elem.toJson() << ",\n";
                }
                out << feature_stack[feature_stack.size()-1].toJson() << "\n";
            }

            out << "] \n";
            
            out << "}";
            return out.str();
        }
        std::vector<int> thread_active_vec;
        void reset_feature_queue() {
            feature_queue.reset(); 
        }

        Mtng() : threshold(KnobSampledRegionSize.Value() * 1000),minimum_threshold(KnobMinimumSampledRegionSize.Value()*1000),current_barrier(0)  {
            std::cout << "Sampled Region Size : " << threshold << std::endl;

            std::cout << "Minimum Sampled Region Size : " << minimum_threshold << std::endl;

            prior_cluster_id = 0;
            prior_key = 0;
            region_begin_pc = 0;
            region_end_pc = 0;
            max_thread_num = 1;
            prior_call_pc_vec.resize( maximum_thread_num,0 ); 
            ins_num_vec.resize( maximum_thread_num,0 );
            detect_unknow_block_in_fast_forward_mode.resize( maximum_thread_num,0 );
            record_unknow_enable = false;
            next_pc_if_not_branch = 0;
            is_branch_inst = false;
            record_unknown_bbvs.resize(maximum_thread_num);

            ins_num_interval_vec.resize( maximum_thread_num,0 );
            bbv_ins_num_vec.resize( maximum_thread_num,0 );
            bbv_ins_interval_num_vec.resize( maximum_thread_num,0 );
            interval_ins_num_vec.resize( maximum_thread_num,0 );
            thread_active_vec.resize( maximum_thread_num,0 );
            intrabarrier_bbvs.resize(maximum_thread_num);

            enable_warmup_vec.resize(maximum_thread_num,0);
            //start_warmup_vec.resize(maximum_thread_num,0);
            fast_forward_sync_with_backend.resize(maximum_thread_num,0);

            hash2predictor[region_begin_pc] = new Predictor<dvfs_enable>( cluster_id_queue,mtng_current_mode );
            PIN_InitLock( & intra_lock );
            enter_lock = false;
            next_target = threshold;
            intra_id =0;
            m_core_cond.resize(maximum_thread_num);
			time_distribution.resize( TIMES_SIZE );
            for (uint32_t i = 0 ; i < maximum_thread_num ; i++) {
              m_core_cond[i] = new ConditionVariable();
            }

            reset_feature_queue();
        }

    void intraBarrierInstruction( THREADID tid, ADDRINT addr ) {

            if(tid!=0) return;
                //detectUnknownBB( tid, 0 , addr) ;
                if( interval_ins_num_vec[tid] > next_target ) {
//                    PIN_GetLock( &intra_lock, tid ) ;
//                    enter_lock = true;          
                    auto ret = processRegion<mtng::RecordType::IntraBarrier>( addr ); 

//                    enter_lock = false;          
  //                  PIN_ReleaseLock(&intra_lock);
                    
                    changeSimMode( ret);
                }

    }

        void resetCount() {
            next_target = interval_ins_num_vec[0] + threshold;
        }



        uint64_t get_insnum() {
            uint64_t insnum_sum = 0;
            for(uint32_t ti = 0 ; ti < maximum_thread_num ; ti++ ) {
                uint64_t insnum = 0 ;
//              if(thread_data[ti].bbv != NULL && thread_data[ti].running) 
//              {
//                    insnum = thread_data[master_thread_id].output->Magic(SIM_CMD_GET_INS_NUM, ti, 0 );
//                    insnum = interval_ins_num_vec[ti] ;
                    insnum = get_bbv_thread_counter(ti);
                                        
                    auto insnum_sum_interval =   insnum - ins_num_vec[ti];                
                    ins_num_interval_vec[ti] = insnum_sum_interval;
                    ins_num_vec[ti] = insnum;
                
                    insnum_sum += insnum_sum_interval;
//                } else {
//                    ins_num_interval_vec[ti] = 0;
//                }
            }
            return insnum_sum;
        }

        Feature<dvfs_enable> get_ipc() {
            get_insnum();

            fs = thread_data[master_thread_id].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0 );
            l2_miss_count = thread_data[master_thread_id].output->Magic(SIM_CMD_GET_L2_MISS_COUNT, 0, 0 );
            l2_count = thread_data[master_thread_id].output->Magic(SIM_CMD_GET_L2_COUNT, 0, 0 );
            //LOG2(INFO) << "time: " <<  fs/1e15 << std::endl;
            uint64_t interval = fs - prior_fs;
            uint64_t interval_l2 = l2_count - prior_l2_count;
            uint64_t interval_l2_miss = l2_miss_count - prior_l2_miss_count;
            prior_fs = fs;
            prior_l2_count = l2_count;
            prior_l2_miss_count = l2_miss_count;
            return Feature<dvfs_enable>(  interval,  ins_num_interval_vec,last_record_type, interval_l2,interval_l2_miss );
        }
        void init_ipc(uint32_t tid) {
            prior_fs = thread_data[tid].output->Magic(SIM_CMD_GET_SIM_TIME, 0, 0 );
            prior_l2_miss_count = thread_data[tid].output->Magic(SIM_CMD_GET_L2_MISS_COUNT, 0, 0 );
            prior_l2_count = thread_data[tid].output->Magic(SIM_CMD_GET_L2_COUNT, 0, 0 );
            //LOG2(INFO) << "time: " <<  prior_fs/1e15 << std::endl;
        }
        void recordState( ADDRINT addr , uint64_t cluster_id ) {
            if( mtng_current_mode == mtng::SimMode::DetailMode) {
                feature = get_ipc(); 

                updateGlobalInfo( region_begin_pc, cluster_id,freq ); 
            } else {
                get_insnum();
                feature =  Feature<dvfs_enable>(ins_num_interval_vec,last_record_type);
            }
            feature.pc = addr;
            feature.cluster_id = cluster_id;
        }
        
        
        void updateCurPredictor( ADDRINT pcaddr, uint64_t cluster_id ) {
            
            auto  predictor_it = hash2predictor.find(pcaddr); ///different pc has different predictor
            assert(predictor_it!=hash2predictor.end());
            auto & predictor = predictor_it->second;
            cluster_id_queue.push_back(cluster_id);
            PredictResultType predict_result_type = predictor->updateCurPredictor( cluster_id, feature );
            feature.predict_result_type = predict_result_type;

        }
        void updateGlobalInfo( ADDRINT pcaddr, uint64_t cluster_id, uint32_t freq ) {
            FeatureKey<dvfs_enable> feature_key = FeatureKey<dvfs_enable>(cluster_id,freq); 
            auto cluster2feature_it = cluster2feature.find( feature_key ); ///different pc has different predictor
            if( cluster2feature_it == cluster2feature.end() ) {
                cluster2feature[feature_key] = feature;
            } else {
                cluster2feature_it->second.add(feature);
            }
        }
        std::pair<Feature<dvfs_enable>,bool> getGlobalInfo( ADDRINT pcaddr, uint64_t cluster_id,uint32_t freq ) {
            
            FeatureKey<dvfs_enable> feature_key = FeatureKey<dvfs_enable>(cluster_id,freq); 
            auto cluster2feature_it = cluster2feature.find( feature_key ); ///different pc has different predictor

            if( cluster2feature_it == cluster2feature.end() ) {

                return std::pair<Feature<dvfs_enable>,bool>( Feature<dvfs_enable>(), false);
            } else {
                return std::pair<Feature<dvfs_enable>,bool>( cluster2feature_it->second, true );
            }
        }



        void fini( ADDRINT pcaddr ) {
            
            processRegion<mtng::RecordType::EndPgm>(pcaddr);

            global_maxthread_num = max_thread_num;
            repairData();
            fflush();

//            update_sim();
//            RecordType record_type = RecordType::SingleThread;
//            Record record = Record(record_type, SimMode::DetailMode, fs,ins_num_vec,0 );
//            record_vec.push_back(record);
        }
        void fflush() {
//            std::ofstream output ;
//            std::string res_dir = KnobMtngDir.Value();
//            std::string full_path = res_dir + "/" + "mtng.json";
//            
//            output.open( full_path.c_str() , std::ofstream::out );
////            output << RecordtoJson( record_vec );
//            output.close();
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

        void detectUnknownBB( THREADID tid, uint32_t insnum ,ADDRINT addr) {
///////////////record bbv recorded before or not
            if(tid!=0) return;
            //auto key_cur = std::tuple<uint64_t, uint32_t>(addr, insnum);
            auto key_cur = addr;
            auto key = combinehash( prior_key, key_cur );
            auto elem = intrabarrier_bbvs[tid][key_cur] ;
            
            if ( elem == 0 ) {
//                std::cout << "Unknown BBV detected\n";
                intrabarrier_bbvs[tid][key_cur] = 1;
                if(mtng_current_mode == mtng::SimMode::FastForwardMode) {
                    detect_unknow_block_in_fast_forward_mode[ tid ] = 1;

                    if(record_unknow_enable) {
                        record_unknown_bbvs[tid][key] = 1;
                    }
                }
            } else {
                if(record_unknow_enable) {
                
                    if(mtng_current_mode == mtng::SimMode::FastForwardMode) {
                        auto find_record =  record_unknown_bbvs[tid].find( key);
                        if( find_record != record_unknown_bbvs[tid].end() ) {
                            detect_unknow_block_in_fast_forward_mode[tid] = 2;
                        }
                    }
                }
            }

            prior_key = key_cur;

            if( tid == 0 && mtng_current_mode == mtng::SimMode::FastForwardMode ) {
            
                int detect_unknown_level = 0;
                for( auto elem : detect_unknow_block_in_fast_forward_mode ) {
                    if( elem == 1 ) {
                        detect_unknown_level = 1;
                        break;
                    } else if (elem == 2) {
                        detect_unknown_level = 2;
                        break;
                    }
                }
                if( detect_unknown_level ) {
                    mtng::SimMode ret ;
                    if(detect_unknown_level == 1) {
                        ret = processRegion<mtng::RecordType::UnknownBlock>(addr);
                    } else {
                        ret = processRegion<mtng::RecordType::UnknownBlockRecord>(addr);
                    }
                    changeSimMode( ret);
                }
            }
        }
        void enable_warmup() {
            //LOG2(INFO) << "Start Warmup\n";
            getWarmupTool()->startWriteWarmupInstructions();
            global_count = 0;
            for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                enable_warmup_vec[i] = 1;
            }
        }
        void disable_warmup() {
            getWarmupTool()->stopWriteWarmupInstructions();
            //LOG2(INFO) << "End Warmup\n";
            
            for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                enable_warmup_vec[i] = 0;
                //start_warmup_vec[i] = 0;
            }
        }

        void changeSimMode( mtng::SimMode ret ) {
              if( mtng_current_mode == ret ) {
                    return;
              }
              mtng_current_mode = ret;
              if( ret == mtng::SimMode::FastForwardMode ) {
                  //LOG2(INFO) << "here1\n";
                   gotoFastForwardMode();
              } else if (ret == mtng::SimMode::DetailMode) {
//                   gotoDetailMode();
                   //LOG2(INFO) << "here2\n";
                    enable_warmup();                 
              }

        }
        void gotoDetailMode(uint32_t tid) {
            for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                fast_forward_sync_with_backend[ i ] = 1;
            }
            if (thread_data[tid].icount_reported > 0)
            {
               thread_data[tid].output->InstructionCount(thread_data[tid].icount_reported);
               thread_data[tid].icount_reported = 0;
            }
            fast_forward_sync_with_backend[master_thread_id] = 0;

            setInstrumentationMode( Sift::ModeDetailed );
            thread_data[tid].output->Magic(SIM_CMD_ROI_START, 0, 0);
            init_ipc(tid);
        }

//        #define ONLY_DETAILED_MODE
        void gotoFastForwardMode() {
            for (uint32_t i = 0 ; i < detect_unknow_block_in_fast_forward_mode.size() ; i++) {
                detect_unknow_block_in_fast_forward_mode[i] = 0;
            }
#ifndef ONLY_DETAILED_MODE
            setInstrumentationMode(Sift::ModeIcount);
            thread_data[master_thread_id].output->Magic(SIM_CMD_ROI_END, 0, 0);
            for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                fast_forward_sync_with_backend[ i ] = 1;
            }
            uint32_t tid = master_thread_id;
            if (thread_data[tid].icount_reported > 0)
            {
               thread_data[tid].output->InstructionCount(thread_data[tid].icount_reported);
               thread_data[tid].icount_reported = 0;
            }
            fast_forward_sync_with_backend[tid] = 0;

#endif
        }
        bool warmupfinish() {
             bool warmup_finished = true;
             for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                 if(thread_active_vec[i]&&enable_warmup_vec[i]==2) { // just wait these warmup thread that started
//                     LOG2(INFO) << i << " " << thread_active_vec[i] << " " << enable_warmup_vec[i];
                     warmup_finished = false;
                     break;
                 }
             }

             if(warmup_finished) {
                return true;
             } else {
                return false;
             }
        }
        void finishRelease() {
            std::vector<uint64_t> m_lcl_tts(m_threads_to_signal);
            m_threads_to_signal.clear();

            for (auto tts : m_lcl_tts) {
              m_core_cond[tts]->signal();
            }
        }

        void recordInsCount( THREADID tid, uint32_t insnum ,ADDRINT addr) {
            
            interval_ins_num_vec[tid] += insnum;
            if(fast_forward_sync_with_backend[tid]) {
               if (thread_data[tid].icount_reported > 0)
               {
                  thread_data[tid].output->InstructionCount(thread_data[tid].icount_reported);
                  thread_data[tid].icount_reported = 0;
               }
               fast_forward_sync_with_backend[tid] = 0;
            }

            ///for warmup
            if(enable_warmup_vec[tid]==1) {
//                getWarmupTool()->writeWarmupInstructions(tid); //not compatible with sde, disable at first
                if( tid == 0 ){
                     
                    disable_warmup();
                    gotoDetailMode(tid);
                } 
            }

        }

        UInt64 * updateBBV( ){

            UInt64 * bbv = get_bbv_space();   
            memcpy( bbv, &global_m_bbv_counts[0], MAX_NUM_THREADS_DEFAULT * NUM_BBV * sizeof(uint64_t) );
            return bbv;
        }

        uint64_t getLastRegionClusterID() {
            {
                uint64_t current_ins = get_bbv_thread_counter(0);
                uint64_t ins_interval = current_ins - bbv_ins_num_vec[0];
                //LOG
                //LOG2(INFO) << ins_interval << "\n";
                if( ins_interval < minimum_threshold ) { // we just has a small region and want to change the simulation mode
                    return SMALL_REGION_LABEL;
                } 
            }

            for(uint32_t ti = 0 ; ti < maximum_thread_num ; ti++ ) {
//                uint64_t current_ins = interval_ins_num_vec[ti];
                //uint64_t current_ins = thread_data[ti].bbv->getInstructionCount();
                uint64_t current_ins = get_bbv_thread_counter(ti);
                

//                uint64_t ins_interval = current_ins - bbv_ins_num_vec[ti];
                bbv_ins_num_vec[ti] = current_ins;
//                bbv_ins_interval_num_vec[ti] = ins_interval ;

            }
//            UInt64 * bbv;
//            bbv = updateBBV(  );
            uint64_t cluster_id; 
            if(dvfs_enable) {
                cluster_id= add_bbv_intra_bbv( global_m_bbv_counts, bbv_ins_num_vec,max_thread_num, mtng_current_mode, 0 );
            } else {
            
                cluster_id= add_bbv_intra_bbv( global_m_bbv_counts, bbv_ins_num_vec,max_thread_num, mtng_current_mode, freq );
            }
            return cluster_id;

        }
        #define STABLE_THRESHOLD 2
        Feature<dvfs_enable> * detectStableRegion(  ) {
            uint32_t feature_stack_size = feature_stack.size();
            if(feature_stack_size == 0) {
                return NULL;
            }
            auto target_feature_key = feature_stack.back().feature_key;
            uint32_t solved_idx = -1;
            if(feature_stack.back().solved) {
                solved_idx = feature_stack_size - 1;
            }
            uint32_t similar_idx = 1;
            for( int32_t init_idx = feature_stack_size - 2; init_idx >= 0 ; init_idx--  ) {
                auto & feature = feature_stack[init_idx];
                if(feature.feature_key == target_feature_key) {
                    if( feature.solved && (solved_idx == (uint32_t)-1) )  {
                        solved_idx = init_idx;
                    }
                    similar_idx++;
                } else {
                    break;
                }
                if(similar_idx>=STABLE_THRESHOLD && (solved_idx!=(uint32_t)-1)) {
                    return &feature_stack[solved_idx];
                }
                
            }
            return NULL;
        }
//#define DISABLE_STABLE_DETECTOR
        mtng::SimMode decideNextSimMode( ADDRINT pcaddr ) {
            ///get predictor
                 
            //LOG2(INFO) <<"predict pc : " <<  pcaddr << std::endl;
            auto predictor_it = hash2predictor.find(pcaddr); ///different pc has different predictor
            Predictor<dvfs_enable> * predictor;
            if( predictor_it == hash2predictor.end() ) { //never saw this pc before; go to detailed mode
#ifndef DISABLE_STABLE_DETECTOR
                Feature<dvfs_enable> * feature = detectStableRegion();
#endif
                predictor = new Predictor<dvfs_enable>( cluster_id_queue, mtng_current_mode);
                hash2predictor[pcaddr] = predictor ;
#ifndef DISABLE_STABLE_DETECTOR
                if(feature!=NULL) {
                    predictor->InitFeature( feature );
                } else 
#endif
                {
                    return mtng::SimMode::DetailMode;
                }
            } else { 
                predictor = predictor_it->second;
            }
                auto result_mode_cluster = predictor->decideNextSimMode(freq);
                auto next_mode = result_mode_cluster.first;
                if( next_mode == mtng::SimMode::DetailMode   ) { //disable use global infomation at first
                    auto result_global_feature_cluster = getGlobalInfo( pcaddr, result_mode_cluster.second,freq);
                    if( result_global_feature_cluster.second ) {
                        next_mode = mtng::SimMode::FastForwardMode;
                        predictor->onlyUpdateClusterFeature( result_mode_cluster.second , result_global_feature_cluster.first );
                    }
                }
                return next_mode;
            
        }
        //dvfs support
        void collectData(ADDRINT pcaddr, uint32_t freq) {
            
            next_freq = freq;
            auto ret = processRegion<mtng::RecordType::HardwareEvent>(pcaddr);
            changeSimMode( ret);
        }

        template< mtng::RecordType record_type >
        mtng::SimMode processRegion( ADDRINT pcaddr  ) {
//            Record record;

			double cluster_time_begin = getSystemTime();
            uint64_t cluster_id = getLastRegionClusterID();
			double cluster_time_end = getSystemTime();
            time_distribution[TIME_CLUSTER] += cluster_time_end-cluster_time_begin;
            //LOG2(INFO) << record_type << " "<< cluster_id << "\n";
            if( cluster_id == SMALL_REGION_LABEL) {
                if( record_type != mtng::RecordType::HardwareEvent ) {
                    return mtng_current_mode; 
                } else { // it is hardware event; we must process this small region!!!!

                    assert(feature_stack.size() > 0 || mtng_current_mode == mtng::SimMode::DetailMode) ;
                    region_end_pc = pcaddr;
                    freq = next_freq;
                    resetCount();
                    mtng::SimMode sim_mode_ret;
                    if( mtng_current_mode == mtng::SimMode::DetailMode )  { 
                        Feature<dvfs_enable> local_feature = get_ipc(); 
                        if( feature_stack.size() ==0  ) {
                            local_feature.idx = feature_stack.size();
                            feature_stack.push_back(local_feature);
                            sim_mode_ret = mtng_current_mode;
                        } else {
                            feature_stack.back().merge(local_feature);
                            auto ret = decideNextSimMode(region_end_pc);
                            sim_mode_ret = ret;
                        } 
                    } else {
                        get_insnum();
                        Feature<dvfs_enable> local_feature =  Feature<dvfs_enable>(ins_num_interval_vec,last_record_type);
                        feature_stack.back().merge(local_feature); 
                        auto ret = decideNextSimMode(region_end_pc);
                        sim_mode_ret = ret;
                    }
                    last_record_type = record_type;
                    return sim_mode_ret;
                }
            } else {
                region_begin_pc = region_end_pc;
                region_end_pc = pcaddr;
            }

            resetCount();
//            next_target = interval_ins_num_vec[0] + threshold;
           // LOG2(INFO) << "goto Analysis" << "\n";
            
            //LOG2(INFO) << "clusterid:" << cluster_id << "\n";
            recordState( region_begin_pc,cluster_id );        

            feature.SetFreq( freq );
            last_record_type = record_type;
            feature.idx = feature_stack.size();
            updateCurPredictor( region_begin_pc, cluster_id);

            feature_stack.push_back(feature);
            //LOG2(INFO) << feature << "\n";

            if( record_type == mtng::RecordType::HardwareEvent ) {
                freq = next_freq;
            }
            auto ret = decideNextSimMode(region_end_pc);
            //LOG2(INFO) << "next mode:" << ret << "\n";
			double predict_time_end = getSystemTime();
            time_distribution[TIME_PREDICTION] += ( predict_time_end - cluster_time_end);
            return ret;
        }
        bool trySolveFeatureByLocalInfo( Feature<dvfs_enable> & feature, DataLevel datalevel = DataLevel::FromLocal ) {
            auto pc = feature.pc;
            auto predictor_it = hash2predictor.find(pc);
            if( predictor_it == hash2predictor.end() ) return false;
            auto & predictor = predictor_it->second;
            return predictor->try2FixFeature( feature,datalevel);
        }

        bool trySolveFeatureByGlobalInfo( Feature<dvfs_enable> & feature, DataLevel datalevel=DataLevel::FromGlobal ) {
            auto cluster_id = feature.cluster_id;
            auto freq = feature.freq;
            auto find_feature_it = cluster2feature.find( FeatureKey<dvfs_enable>(cluster_id, freq ) );
            if(find_feature_it != cluster2feature.end()) {
                feature.extrapolate( find_feature_it->second, datalevel );
                return true;
            }
            return false;

        }

        bool trySolveFeatureByReclustering( uint32_t index, Feature<dvfs_enable> & feature ) {
            auto cluster_id = final_intrabarrier_cluster_id( index,feature.freq , max_thread_num);


            if(cluster_id == (uint64_t)-1) {
                return false;
            }
            feature.cluster_id = cluster_id;
            bool ret = trySolveFeatureByLocalInfo(feature, DataLevel::FromReClustering);
            if(ret) {
                return ret;
            }
            ret = trySolveFeatureByGlobalInfo(feature,DataLevel::FromReClustering);
            return ret;
        }


        bool trySolveFeatureByClosedFeature( Feature<dvfs_enable> & feature, Feature<dvfs_enable> * last_feature, Feature<dvfs_enable> *next_feature ) {
             
            feature.extrapolate( last_feature, next_feature, DataLevel::FromOtherCluster );
            return true;
        }

        void processOmpBegin(ADDRINT pc_addr) {
            auto ret = processRegion<mtng::RecordType::MultiThread>(pc_addr);

            //LOG2(INFO) << "change mode to " << ret << "\n";

            changeSimMode( ret);
        }
        void processOmpEnd(ADDRINT pc_addr) {
            auto ret = processRegion<mtng::RecordType::SingleThread>(pc_addr);
            changeSimMode( ret);
        }

        void repairData() {
            time_sum=0;
            
            std::vector<uint32_t> unsolved_feature;
            uint64_t ins_num_sum = 0 ;

            for( uint32_t i = 0 ; i < feature_stack.size() ; i++ ) {
                auto & feature = feature_stack[i];
                bool ret = true;
                if(!feature.solved) {
                    ret = trySolveFeatureByLocalInfo(feature);
                    if(!ret) {

                        ret = trySolveFeatureByGlobalInfo( feature );
                        if(!ret) {

                        ret = trySolveFeatureByReclustering( i,feature );    
                        
                        }
                    }
                }
                if(ret) {
                    time_sum += feature.fs;
                    ins_num_sum += feature.max_ins_num;
                }
            }
            std::unordered_map<uint32_t, Feature<dvfs_enable>*> freq2lastfeature;

            for( int32_t i = 1 ; i < (int32_t)feature_stack.size() ; i++ ) {
                auto & feature = feature_stack[i];
                if(!feature.solved) {
                    Feature<dvfs_enable> * last_feature = &feature_stack[i-1] ;
                        
                    Feature<dvfs_enable> * next_feature = NULL;
                    if ( i + 1 < (int32_t)feature_stack.size() ) {
                        next_feature = &feature_stack[i+1];
                    }
                    if(dvfs_enable) {
                        if(next_feature != NULL) {
                            if(next_feature->freq!=feature.freq) {
                                next_feature = NULL;
                            }
                        }
                        if(last_feature->freq!=feature.freq) {
                            last_feature = NULL;
                        }
                        if( next_feature == NULL && last_feature == NULL ) {
                            last_feature = freq2lastfeature[feature.freq];
                        }
                    }
                    
                    bool ret = trySolveFeatureByClosedFeature( feature , last_feature, next_feature);
                    if(!ret) {
                        unsolved_feature.push_back( i );
                        continue;
                    } else {
                        time_sum += feature.fs;
                        ins_num_sum += feature.max_ins_num;
                    }
                } else {
                    if(dvfs_enable) {
                        uint32_t freq=  feature.freq;
                        freq2lastfeature[freq] = &feature;
                    }
                }
                
            }


            Feature<dvfs_enable> average = Feature<dvfs_enable>( time_sum,ins_num_sum);
            for( auto elem : unsolved_feature ) { //these feature are really hard
                feature_stack[elem].extrapolate( average, DataLevel::FromGlobalAvg );
                time_sum += feature_stack[elem].fs;
                ins_num_sum += feature_stack[elem].max_ins_num;
            }
            //std::cout << "time sum: " << time_sum /1e15 << std::endl;
            std::ofstream final_file;
            std::string res_dir = KnobMtngDir.Value();
            std::string full_path = res_dir + "/" + "intra_mtng.json";
            final_file.open( full_path.c_str(),std::ofstream::out );
            final_file << toJson();
            final_file.close();
        }
        static Mtng * toMtng( VOID * v ) {
            Mtng * mtng_ptr = reinterpret_cast<Mtng*>(v); 
            if( mtng_ptr == nullptr ) {
                cerr << "null Failed";
            }
            return mtng_ptr;
        }
        void SetFreq( uint32_t freq_in ) {
            freq = freq_in;
        }
        void threadStart( THREADID threadid ) {
            thread_active_vec[threadid] = 1;
           if(threadid > max_thread_num - 1) {
                max_thread_num = threadid + 1;
           }
            for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                if( thread_active_vec[i] ) {
                    master_thread_id = i;
                    break;
                }
            }

            //std::cerr << "thread " << threadid << " master id: " << master_thread_id << " begin\n " ;


        }
        void threadEnd( THREADID threadid ) {
            thread_active_vec[threadid] = 0;
            for( uint32_t i = 0 ; i < MAX_NUM_THREADS_DEFAULT ; i++ ) {
                if( thread_active_vec[i] ) {
                    master_thread_id = i;
                    break;
                }
            }
             //std::cerr << "thread " << threadid << " master id: " << master_thread_id << " end\n " ;

        }
};
     void recordInsCount( THREADID threadid, uint32_t insnum, ADDRINT pc_addr, VOID * v  ) {
//        if(threadid != 0 ) return;
        auto * mtng_ptr = VirtualMtng::toMtng(v);
        mtng_ptr->recordInsCount(threadid, insnum, pc_addr);

    }
   
    void processOmpBegin( THREADID threadid, ADDRINT pc_addr, VOID * v  ) {
        if(threadid!=0) return;
        auto * mtng_ptr = VirtualMtng::toMtng(v);
        mtng_ptr->processOmpBegin( pc_addr );
//        mtng_ptr->detectUnknownBB( threadid, 0 , pc_addr) ;
       

    }
    void processOmpEnd( THREADID threadid, ADDRINT pc_addr, VOID * v  ) {
        if(threadid!=0) return;
        auto * mtng_ptr = VirtualMtng::toMtng(v);

        mtng_ptr->processOmpEnd( pc_addr );
    }


    VOID sendIntraBarrierInstruction(THREADID threadid, ADDRINT addr, void * v)
    {
        if(threadid != 0 ) return;
    //void intraBarrierInstruction( THREADID tid, ADDRINT addr, UINT32 size, BOOL is_branch ) {
        auto * mtng_ptr = VirtualMtng::toMtng(v);
        mtng_ptr->intraBarrierInstruction(threadid, addr);

    }



VOID
traceCallbackForBarrierPoint(TRACE trace, void* v)
{
  RTN rtn = TRACE_Rtn(trace);

  {
    ///comments: for intra-barrier
      for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) 
      {
         INS tail = BBL_InsTail(bbl);
               ///comments: keep this function;
            INS_InsertPredicatedCall( tail, IPOINT_BEFORE, (AFUNPTR) recordInsCount ,
            IARG_THREAD_ID,
            IARG_UINT32, BBL_NumIns(bbl),
            IARG_INST_PTR,
            IARG_ADDRINT, v,
            IARG_END);

      }
      {
      BBL bbl = TRACE_BblTail(trace);
            //comments : this is for intra barrier; detect branch instructions

      INS tail = BBL_InsTail(bbl);
         if (INS_IsDirectControlFlow(tail) && INS_HasFallThrough(tail) )
         {
            const ADDRINT target = INS_DirectControlFlowTargetAddress(tail);
            const ADDRINT current_pc = INS_Address(tail);
            if(target < current_pc) {
                INS_InsertPredicatedCall(tail, IPOINT_BEFORE,
                      AFUNPTR(sendIntraBarrierInstruction),
                      IARG_THREAD_ID,
                      IARG_INST_PTR,
                      IARG_ADDRINT, v,
                      IARG_END);
            }

         }
      }
            //comments: end

  }
  //comments: for small barrier region
  if (RTN_Valid(rtn)    
      && RTN_Address(rtn) == TRACE_Address(trace))
  {
      std::string rtn_name = RTN_Name(rtn).c_str();
      BBL bbl = TRACE_BblHead(trace);
      INS ins = BBL_InsHead(bbl);
#define OMP_FN_BEGIN "_omp_fn"
      if (
           (rtn_name.find( OMP_FN_BEGIN ) != std::string::npos)
         )
      {
                 INS_InsertPredicatedCall( ins, IPOINT_BEFORE, (AFUNPTR)processOmpBegin,
                     IARG_THREAD_ID,
                     IARG_INST_PTR,
                     IARG_ADDRINT, v,
                     IARG_END);
           
      }

  }
}

///this function is for intel compiler to detect barrier boundary
VOID
iccRoutineStartCallback(RTN rtn,void *v)
{
    RTN_Open(rtn);
    if(RTN_Valid(rtn)) {

        std::string rtn_name = RTN_Name(rtn).c_str();
        if(rtn_name == "__kmp_invoke_microtask") {
//        std::string * rtn_name_ptr = new string(rtn_name);
        for (INS tail = RTN_InsHead(rtn); INS_Valid(tail); tail = INS_Next(tail) ) {

            if( INS_IsCall(tail) )
            {
                 INS_InsertPredicatedCall( tail, IPOINT_BEFORE, (AFUNPTR)processOmpBegin,
                     IARG_THREAD_ID,
                     IARG_INST_PTR,
                     IARG_ADDRINT, v,
                     IARG_END);


            }

        }
        }
    }
    RTN_Close(rtn);
}




VOID
processExit( THREADID threadid, ADDRINT pcaddr, void *v)
    {

        if(threadid!=0) return;
        VirtualMtng * mtng_ptr = VirtualMtng::toMtng( v );
        mtng_ptr->fini(pcaddr);
        
    }
VOID Fini( INT32 code,void *v) {
    
        VirtualMtng * mtng_ptr = VirtualMtng::toMtng( v );
        delete mtng_ptr;
}
static void routineCallback(RTN rtn, VOID *v)
{

  RTN_Open(rtn);
  if (RTN_Valid(rtn) )
  {

      std::string rtn_name = RTN_Name(rtn).c_str();
      if ( rtn_name == "_exit" || rtn_name == "_Exit" )
      {
                 RTN_InsertCall( rtn, IPOINT_BEFORE, (AFUNPTR)processExit,
                     IARG_THREAD_ID,
                     IARG_INST_PTR,
                     IARG_ADDRINT,v,
                     IARG_END);
           
      }
  }

   RTN_Close(rtn);
}
    VOID threadEnd(THREADID threadid, const CONTEXT *ctxt, INT32 flags, VOID *v) {

        auto * mtng_ptr = VirtualMtng::toMtng( v );
        mtng_ptr->threadEnd(threadid);
    }
    VOID threadStart(THREADID threadid, CONTEXT *ctxt, INT32 flags, VOID *v) {
        auto * mtng_ptr = VirtualMtng::toMtng( v );
        mtng_ptr->threadStart(threadid);
    }

    void * mtng_global_void;
    template<bool dvfs_enable2>
    void activate( ) {
        VirtualMtng * mtng_global = new Mtng<dvfs_enable2>();
        mtng_global_void = mtng_global; 

        uint32_t freq = thread_data[0].output->Magic( SIM_CMD_MHZ_GET, 0,0);

        mtng_global->init_ipc(0);
        mtng_global->SetFreq( freq );
        TRACE_AddInstrumentFunction(traceCallback, 0);
    //    TRACE_AddInstrumentFunction(detectUnknownBlock, mtng_global);
//        RTN_AddInstrumentFunction(routineCallback, 0);
        TRACE_AddInstrumentFunction(traceCallbackForBarrierPoint, (void*)mtng_global ); 
        PIN_AddFiniFunction(Fini, (void*)mtng_global);

        RTN_AddInstrumentFunction(routineCallback, mtng_global);
        RTN_AddInstrumentFunction(iccRoutineStartCallback, mtng_global);
        PIN_AddThreadStartFunction(threadStart, mtng_global);
        PIN_AddThreadFiniFunction(threadEnd, mtng_global);

    }
    void collectDataDVFS( uint32_t freq ,ADDRINT pcaddr) {
        VirtualMtng * mtng_global = VirtualMtng::toMtng( mtng_global_void );
        mtng_global->collectData(pcaddr,freq);
    }

    void activate(bool dvfs_enable) {
        onlinecluster_activate();
        if(dvfs_enable) {
            activate<true>();
        } else {
            activate<false>();
        }
    }
};
