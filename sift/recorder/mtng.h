#ifndef __MTNG_H__
#define __MTNG_H__

//#define THRESHOLD_DISTANCE 0.1
//#define THRESHOLD_RECLUSTERING_DISTANCE 0.2
namespace mtng{

    enum class RecordType:uint32_t{
        PointUndef = 0,
        MultiThread = 1,
        SingleThread = 2,
        IntraBarrier = 3,
        UnknownBlock = 4,
        UnknownBlockRecord = 5, ///repeaded unknown block pc
        HardwareEvent = 6,
        EndPgm = 7,

    };

    enum class SimMode : char {
        DetailMode = 0,
        FastForwardMode = 1,
        WarmUpMode = 2,
        NoChange = 3,

    };
    class Record{
        public:
        Record()=default;
        ~Record()=default;
        RecordType record_type;
        SimMode sim_mode;
        uint64_t sim_fs;
        std::vector<uint64_t> ins_num_vec;
        ADDRINT pc;
        Record(RecordType p1, SimMode p2, uint64_t sim_fs_, const std::vector< uint64_t> & ins_num_,  ADDRINT pc_ ) : record_type(p1),sim_mode(p2),sim_fs(sim_fs_), ins_num_vec( ins_num_ ),pc(pc_) {
            //std::cout << ins_num_[0] << std::endl; 
        }
        

    };
    
    void activate();
std::ostream & operator<<(std::ostream&out ,const RecordType& record_type) ;
std::ostream & operator<<(std::ostream&out ,const SimMode& record_type) ;

};
#endif
