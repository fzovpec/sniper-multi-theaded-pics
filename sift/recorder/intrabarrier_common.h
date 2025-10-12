#ifndef __INTRA_BARRIER_COMMON_H__
#define __INTRA_BARRIER_COMMON_H__

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
#include "hooks_manager.h"
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


template <typename T, int MaxLen >
class FixedQueue  {
    std::deque<T> container;
    uint64_t hash_value;
public:
    FixedQueue() {
        container.resize( MaxLen,-1);
        renew_hash();
    } 
    void push(const T& value) {
        if (container.size() == MaxLen) {
            container.pop_front();
        }
        container.push_back(value);
        renew_hash();
    }
    uint64_t hash() {
        return hash_value;
    }
    void renew_hash() {
        uint64_t seed = 0;
        for( uint32_t i = 0 ;  i < container.size() ; i++ ) {
            seed ^= std::hash<T>()( container[i] ) + 0x9e3779b9 + (seed << 6) + (seed >> 2); 
        }
        hash_value = seed;
    }
    void reset() {
        for( uint32_t i = 0 ; i < MaxLen ; i++ ) {
            container[i] = -1;
        }
        renew_hash();
    }
};
uint64_t combinehash(
        std::tuple<uint64_t, uint32_t> key1,
        std::tuple<uint64_t, uint32_t> key2
        ) ;
uint64_t combinehash(
        uint64_t key1,
        uint64_t key2
        ) ;

    template<bool dvfs_enable>
    class FeatureKey{
		public:
        uint64_t cluster_id;
        uint32_t freq;
        FeatureKey(uint64_t cluster_id_in, uint32_t freq_in) : cluster_id(cluster_id_in),freq(freq_in){};
        FeatureKey(){}
       	bool operator==(const  FeatureKey<dvfs_enable>  right )const  {
			if(dvfs_enable) {
        		if ( right.cluster_id==this->cluster_id && right.freq==this->freq ) {
    	        	return true;
		        } else {
    	        	return false;
        		}
			} else {
			
				return this->cluster_id == right.cluster_id;
			}
    	} 
   
    };



	namespace std {
	template<bool dvfs_enable>
	  struct hash<FeatureKey<dvfs_enable>>
	  {

	    std::size_t operator()(const FeatureKey<dvfs_enable>& k) const
	    {
	      using std::size_t;
	      using std::hash;
	      using std::string;
	
	      // Compute individual hash values for first,
	      // second and third and combine them using XOR
	      // and bit shifting:
			if(dvfs_enable) {
    		  return combinehash( k.cluster_id, (uint64_t)k.freq );
			} else {
				return k.cluster_id;
			}
	    }
	 };
	
	}


#endif
