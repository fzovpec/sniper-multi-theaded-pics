#ifndef __INTRABARRIER_MTNG_H__
#define __INTRABARRIER_MTNG_H__
#include "mtng.h"
namespace intrabarrier_mtng{
    class Record{
        public:
        Record()=default;
        ~Record()=default;
//        RecordType record_type;
//        SimMode sim_mode;
        uint64_t sim_fs;
        std::vector<uint64_t> ins_num_vec;
        ADDRINT pc;
//        Record(RecordType p1, SimMode p2, uint64_t sim_fs_, const std::vector< uint64_t> & ins_num_,  ADDRINT pc_ ) : record_type(p1),sim_mode(p2),sim_fs(sim_fs_), ins_num_vec( ins_num_ ),pc(pc_) {
            //std::cout << ins_num_[0] << std::endl; 
//        }
        

    };
    void activate(bool dvfs_enable);
    void collectDataDVFS( uint32_t freq ,ADDRINT pcaddr) ;
};
#endif
