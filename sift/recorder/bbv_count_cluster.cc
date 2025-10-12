#include "bbv_count_cluster.h"
#include "onlinebbv_count.h"
//#define THRESHOLD_DISTANCE 1e9
#include <cmath>
UInt64 * get_bbv_space(bool requre_init) {

        UInt64 * bbv = (UInt64*)malloc(sizeof(UInt64)*MAX_NUM_THREADS_DEFAULT*BBV_DIM);
        if (requre_init) {
            memset(bbv,0,sizeof(UInt64) * MAX_NUM_THREADS_DEFAULT * BBV_DIM);
        }
        return bbv;
}

double * get_normalized_bbv_space(bool requre_init=true) {

        double * bbv = (double*)malloc(sizeof(double)*MAX_NUM_THREADS_DEFAULT*BBV_DIM);
        if (requre_init) {
            memset(bbv,0,sizeof(double) * MAX_NUM_THREADS_DEFAULT * BBV_DIM);
        }
        return bbv;
}
    double manhattan_distance(const double * bbv_base, const double *realBbv,const int max_thread_num) {
             // Get Manhattan distance 
         // (i.e. summation of absolute difference b/w respective coordinates)
//         int active_num = 0; 

         double distance = 0;
         for ( int j = 0; j < BBV_DIM * max_thread_num; j++) {
              auto baseelem = bbv_base[j];
              auto toelem = realBbv[j];
                                          
              distance += fabs( baseelem-toelem )  ;//* 1.0 / (double)baseelem;
//              double tmp = baseelem - toelem;          
//              distance += tmp * tmp;
              
         }
//         distance = sqrt(distance);
        return distance;
    }




class BBVStruct{
    public:
    double * bbv;
    uint32_t freq;
    uint32_t cluster_id;
    std::vector<uint32_t> queue_ids;
    BBVStruct( double * bbv_in , uint32_t cluster_id_in,uint32_t freq_in) {
        bbv=bbv_in;
        cluster_id=cluster_id_in;
        freq = freq_in;
    }
    bool is_new_bbv() {
        return queue_ids.size()==0;
    }
    void average( double * bbv_in,uint32_t max_thread_num ) {
        for(uint32_t i = 0 ; i < BBV_DIM * max_thread_num ; i++) {
            
            bbv[i]= (bbv[i]+bbv_in[i])/2;

        }
    }
};
class DistanceMatrix {
    public:
    std::vector<double* >distanceMatrix;
    uint32_t distance_matrix_size = 0;
    double * distance_buffer;
    DistanceMatrix() {
        distance_matrix_size = 0;
        resetdistancebuffer();
    }

 
    inline void update_distance_buffer ( double new_distance, uint64_t i  ) {
        distance_buffer[i] = new_distance;
    }
    inline void resetdistancebuffer() {
        distance_buffer = new double[distance_matrix_size+1];
        reset_distance_buffer();
    }
    inline void update_distance_matrix (  ) {
//        for( uint32_t i = 0 ; i < distance_matrix_size + 1; i++) {
//            std::cout << distance_buffer[i] << " ";
//        }
        std::cout << "\n";
        distanceMatrix.push_back( distance_buffer );
        distance_matrix_size = distanceMatrix.size();
        resetdistancebuffer();
    }
    inline void reset_distance_buffer (  ) {
        for( uint32_t i = 0 ; i < distance_matrix_size +1 ; i++ ) {
            distance_buffer[i] = -1;
        }
    }

    // Skip check condition
    inline bool skip_check (uint64_t cluster_id, double distance, uint64_t current_best_cluster_id) {
        if( cluster_id >= distanceMatrix.size() ) {
            return false;
        }

        double d1 = fabs(distanceMatrix[current_best_cluster_id][cluster_id] - distance) ;
        
//        std::cout << "opt:"<< cluster_id << " "<<current_best_cluster_id<< " " << distanceMatrix[current_best_cluster_id][cluster_id]<<" " <<d1 << " " << distance << std::endl;
        if ( d1 > distance ) {

            return true;
        } else {
            return false;
        }
    }
};


class BBVCluster {
    std::vector<UInt64>  prior_bbv;
    std::vector<BBVStruct*> bbv_cluster_vec_detailed;
    std::vector<BBVStruct*> bbv_cluster_vec_ffw;
    std::vector<uint32_t> cluster_queue;
    std::vector<uint64_t> insnum_vec;
    DistanceMatrix distance_matrix;
    uint32_t cluster_id ;
    uint32_t queue_id;
    double threshold;
    std::vector<double*> bbv_vec;
    public:
    double * get_bbv( uint32_t  idx ) {
        return bbv_vec[idx];
    }

    BBVCluster() {

        prior_bbv = std::vector<UInt64>( MAX_NUM_THREADS_DEFAULT*NUM_BBV,0 );
        cluster_id = 0;
        queue_id = 0;
        insnum_vec.resize(MAX_NUM_THREADS_DEFAULT,0);
        threshold = KnobClusterThreshold.Value();
        std::cout << "threshold : " << KnobClusterThreshold.Value() << std::endl; 
    }
    std::pair<double,bool> is_same_cluster( const double * bbv_base, const double * realBbv, const int max_thread_num,double threshold) {
         // If distance is less than THRESHOLD_DISTANCE
         // Update number of regions compared, minimum distance recorded and BBV index (ret)
         double distance = manhattan_distance(bbv_base,realBbv,max_thread_num);
         if (distance < threshold) {
             return std::pair<double,bool>(distance,true);
         } else {
             return std::pair<double,bool>(distance,false);
         }
}
template<bool dvfs_enable,bool check_all>
BBVStruct * find_closed_one(
            double * interval_bbv,
            uint32_t freq,
            std::vector<BBVStruct*> & target_bbv_struct,
            const int max_thread_num,
            double threshold
        ) {
#define INIT_VALUE 10
                    double min_distance = INIT_VALUE;
                    int32_t finally_result = 0;
                    bool result_inited = false;
                    for (int32_t i =  (int32_t)(target_bbv_struct.size()) - 1; i >= 0 ; i--) {

                       BBVStruct & bbv_struct = *target_bbv_struct[i];
                    
                       double * bbv_cluster = bbv_struct.bbv;
                       if(bbv_cluster==NULL) {
                        continue;
                       }
                       if(dvfs_enable) {
                           if( bbv_struct.freq != freq ) {
                                continue;
                           }
                       }
                       //std::cout << bbv_struct.cluster_id << "\n";
                       if( min_distance != INIT_VALUE && distance_matrix.skip_check( bbv_struct.cluster_id, min_distance, target_bbv_struct[finally_result]->cluster_id ) ) {
                            
                            continue;
                       }

                       auto ret_back = is_same_cluster( bbv_cluster, interval_bbv, max_thread_num,threshold );
            
                       bool is_same_cluster_ret = ret_back.second;
                           //is_same_cluster( bbv_cluster, interval_bbv, max_thread_num,threshold );

                       distance_matrix.update_distance_buffer( ret_back.first, bbv_struct.cluster_id);

                       if(min_distance > ret_back.first) {
                           min_distance = ret_back.first;
                           finally_result = i;
                       }

                       if (is_same_cluster_ret) {
                            result_inited = true;
                            
                            if( !check_all)  {
                                return target_bbv_struct[i]; 
                            }
                       }
                       
                    }
                    if( result_inited) {
                        return target_bbv_struct[finally_result];
                    } else {
                        return NULL;
                    }
}
                    // Check distance from BBVs corresponding to each entry in the bbvIdxList
//                    if( reclustering ) { //directly return cluster id
//                        if(minDistance > THRESHOLD_RECLUSTERING_DISTANCE ) {
//                            ret = -1;
//                        } else {
//                            ret = cluster_id;
//                        }
//
//                        std::cout << "distance: "<< minDistance << " cluster id: " << ret << std::endl;
//                        return ret;
//                    }
template<bool dvfs_enable>
std::pair<BBVStruct*, bool> get_cluster_id( 
                       double * realBbv, 
                       uint32_t freq,
                       const int max_thread_num,
                       bool only_search_detailed_vec = false
        ) {

        BBVStruct * bbv_struct = find_closed_one<dvfs_enable,true>( realBbv,freq,bbv_cluster_vec_detailed, max_thread_num,threshold );
        bool is_detailed_queue = true;
        if ( bbv_struct == NULL) {
           is_detailed_queue = false;
           if( only_search_detailed_vec ) {
                /*do nothing*/
           } else {
                bbv_struct = find_closed_one<dvfs_enable,false>( realBbv,freq,bbv_cluster_vec_ffw, max_thread_num,threshold );
                if(bbv_struct == NULL ) {
                    bbv_struct = new BBVStruct(  realBbv, cluster_id,freq);
    //std::vector<BBVStruct*> bbv_cluster_vec_ffw;
                    for( auto &elem : bbv_cluster_vec_detailed) {
                        uint32_t i = elem->cluster_id;
                        if( distance_matrix.distance_buffer[i] == -1 ) {
                            
                            double distance = manhattan_distance(elem->bbv,realBbv,max_thread_num);
                            distance_matrix.distance_buffer[i] = distance;
                        }

                    }
                    for( auto &elem : bbv_cluster_vec_ffw) {
                        uint32_t i = elem->cluster_id;
                        if( distance_matrix.distance_buffer[i] == -1 ) {
                            
                            double distance = manhattan_distance( elem->bbv,realBbv,max_thread_num);
                            distance_matrix.distance_buffer[i] = distance;
                        }

                    }
                    distance_matrix.distance_buffer[cluster_id] = 0;

                    cluster_id++; 
                    distance_matrix.update_distance_matrix();
                }  else {
                
                    distance_matrix.reset_distance_buffer();
                }
           }
        } else {
            distance_matrix.reset_distance_buffer();
        }
        

        return std::pair<BBVStruct*,bool>( bbv_struct, is_detailed_queue);
}

double * minus2( const UInt64 * bbv1, const UInt64*bbv2, std::vector<uint64_t> &bbv_interval_insnum ) {
    std::vector<UInt64> ret;
    ret.resize( bbv_interval_insnum.size() * BBV_DIM );
//    UInt64 * ret = get_bbv_space();
    int index = 0;
    double sum = 0;
    double * norm_bbv = get_normalized_bbv_space();
    for( auto elem : bbv_interval_insnum ) {
        
        if(elem > 1000000 ) { //filter out small region
            sum = 0 ;
            for( int i = 0 ; i < BBV_DIM ; i++ ) {
                UInt64 tmp = bbv1[index+i] - bbv2[index+i] ;
                UInt64 div_res = tmp / elem;
                ret[index + i] = div_res;
                double div_res_double = div_res;
                sum += div_res_double * div_res_double;
            }
            sum = sqrt(sum);
            for( uint32_t i = 0 ; i < BBV_DIM ; i++ ) {
                norm_bbv[i+index] = ret[i+index] / sum;
            }


        } 
        index += BBV_DIM;
    }

    return norm_bbv;
}

void print_bbv( UInt64 * bbv) {
    for( int ti = 0 ; ti < MAX_NUM_THREADS_DEFAULT ; ti++ ) {
            auto index = ti * BBV_DIM;      
            for( int i = 0 ; i < BBV_DIM ; i++ ) {
                std::cout << bbv[index+i] << " ";
            }
            std::cout << " ** ";
    }
    std::cout << "\n";

}


double * intervalBBV( const std::vector<UInt64 > & bbv , std::vector<uint64_t>&bbv_insnum, const int max_thread_num  ) {
    double * real_bbv;
    
    std::vector<uint64_t>bbv_interval_insnum ;

    bbv_interval_insnum.resize(MAX_NUM_THREADS_DEFAULT,0);

    for( int i = 0 ; i < max_thread_num ; i++ ) {
        bbv_interval_insnum[i] = bbv_insnum[i] - insnum_vec[i];
    }

    real_bbv = minus2( &bbv[0],&prior_bbv[0], bbv_interval_insnum ); 
    memcpy( &insnum_vec[0] , &bbv_insnum[0], sizeof(uint64_t) * MAX_NUM_THREADS_DEFAULT );
    //prior_bbv = bbv;
    prior_bbv = bbv;
    return real_bbv;
}

template<bool dvfs_enable>
uint64_t add_bbv( const std::vector<UInt64> & bbv , std::vector<uint64_t> &bbv_interval_insnum, const int max_thread_num, mtng::SimMode sim_mode, uint32_t freq) {
    double * real_bbv = intervalBBV( bbv, bbv_interval_insnum, max_thread_num );
    bbv_vec.push_back(real_bbv);     
    prior_bbv = bbv;
    auto ret = get_cluster_id<dvfs_enable>(  real_bbv, freq,max_thread_num );
    BBVStruct * bbv_data = ret.first;
    bool is_detailed_queue = ret.second;
    if ( sim_mode == mtng::SimMode::DetailMode ) {
        if( bbv_data->is_new_bbv() ) { //no close data
            bbv_cluster_vec_detailed.push_back( bbv_data ); 
        } else {
            if( is_detailed_queue ) { // try to merge two bbv struct together
                //bbv_data->average(real_bbv,max_thread_num);   
            } else {
            
             //   bbv_cluster_vec_detailed.push_back( bbv_data );
            }
            /// try to use new bbv to solve ffw// next plan 
//            UInt64 * bbv_base = bbv_data->bbv;
//            double sum_base = bbv_data->sum;
//            for( auto bbv_cluster_ffw : bbv_cluster_vec_ffw) {
//                UInt64 * bbv_ffw = bbv_cluster_ffw.bbv;
//                if( is_same_cluster( bbv_base, bbv_ffw ,sum_base ,max_thread_num ) ) {
//                    
//                }
//            }
            
        }
    } else {
        if( bbv_data->is_new_bbv() ) {
            bbv_cluster_vec_ffw.push_back( bbv_data ); 
        } else {
        
            if( is_detailed_queue ) { // try to merge two bbv struct together
                /*do nothing*/
            } else {
                
              //  bbv_data->average(real_bbv,max_thread_num);
            }
        }
    }

    cluster_queue.push_back( bbv_data->cluster_id );
    bbv_data->queue_ids.push_back(queue_id);
    queue_id++;
    return bbv_data->cluster_id;
}

};

BBVCluster * intra_bbv_cluster;
BBVCluster * inter_bbv_cluster;

uint64_t add_bbv_intra_bbv( const std::vector<UInt64> & bbv , std::vector<uint64_t> &bbv_interval_insnum, const int max_thread_num, mtng::SimMode sim_mode,uint32_t freq) {
    if(freq == 0) {
        return intra_bbv_cluster->add_bbv<false>( bbv,bbv_interval_insnum,max_thread_num,sim_mode,freq );
    } else {
    
        return intra_bbv_cluster->add_bbv<true>( bbv,bbv_interval_insnum,max_thread_num,sim_mode , freq);
    }
}

//uint64_t add_bbv_inter_bbv( const std::vector<UInt64>&  bbv , std::vector<uint64_t> &bbv_interval_insnum, const int max_thread_num, mtng::SimMode sim_mode) {
//
//    return inter_bbv_cluster.add_bbv( bbv,bbv_interval_insnum,max_thread_num,sim_mode );
//}
//

double * intrabarrier_get_bbv(uint32_t idx) {
    return intra_bbv_cluster->get_bbv(idx);
}

uint64_t final_intrabarrier_cluster_id( 
                      uint32_t idx,
                      uint32_t freq,
                       const int max_thread_num
        ) {
    auto realBbv = intra_bbv_cluster->get_bbv(idx);
    std::pair<BBVStruct*, bool>  ret;
    if(freq == 0) {
        ret = intra_bbv_cluster->get_cluster_id<false>( realBbv, freq, max_thread_num); 
    } else {
    
        ret = intra_bbv_cluster->get_cluster_id<true>( realBbv, freq, max_thread_num ); 
    }
    if(ret.second && ret.first!=NULL) {
        return ret.first->cluster_id;
    } else {
        return -1;
    }
}

double get_intra_bbv_distance(uint32_t idx1,uint32_t idx2,const int max_thread_num) {
    auto bbv1 = intra_bbv_cluster->get_bbv(idx1);
    auto bbv2 = intra_bbv_cluster->get_bbv(idx2);
    return manhattan_distance(bbv1,bbv2,max_thread_num);
}

void onlinecluster_activate() {
        intra_bbv_cluster = new BBVCluster();
        inter_bbv_cluster = new BBVCluster();

}
