#ifndef __BBV_COUNT_CLUSTER_H__
#define __BBV_COUNT_CLUSTER_H__
#define BBV_DIM 16

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
//#include "threads.h"
#include <cstdint>
#include <set>
#include <utility>
#include <sstream>
//#include "threads.h"
#include "cond.h"
#include <set>
#include "mtng.h"
#include "to_json.h"
using namespace std;
//#define MARKER_INSERT
#define UNDEFINE_CLUSTER_ID ((uint64_t)-1)
//#define THREDSHOLD 1000000
//#define THREDSHOLD 20000000
//#define THREDSHOLD 100000
//#define LOCAL
//#ifdef LOCAL
//    #define THREDSHOLD            1000000
//    #define SMALL_REGION_INS_NUM (1000000)
//#else
//    #define THREDSHOLD            50000000
//    #define SMALL_REGION_INS_NUM (20000000)
//#endif

#include "mtng.h"
//#define SMALL_REGION_INS_NUM (THREDSHOLD/100)
#define SMALL_REGION_LABEL ((uint64_t) (-2) )
UInt64 * get_bbv_space(bool requre_init=true ) ;

double * intrabarrier_get_bbv(uint32_t idx);
void print_bbv( UInt64 * bbv) ;
uint64_t add_bbv_intra_bbv(const std::vector< UInt64 > & bbv , std::vector<uint64_t> & bbv_interval_insnum,int max_thread_num, mtng::SimMode sim_mode, uint32_t freq = 0) ;
uint64_t add_bbv_inter_bbv( const std::vector<UInt64> & bbv , std::vector<uint64_t> & bbv_interval_insnum,int max_thread_num, mtng::SimMode sim_mode ) ;

double get_intra_bbv_distance(uint32_t idx1,uint32_t idx2,const int max_thread_num) ;

uint64_t final_intrabarrier_cluster_id( 
                      uint32_t idx,
                      uint32_t freq,
                       const int max_thread_num
//                       const double threshold=THRESHOLD_RECLUSTERING_DISTANCE
        ) ;
void final_intrabarrier_remove_uselesscluster_id( 
        std::vector<int> & useful_cluster
        ) ;
void onlinecluster_activate(); 
#endif
