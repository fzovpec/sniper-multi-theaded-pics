
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include "mtng.h"
#include <set>
#include "to_json.h"
using namespace std;
using namespace mtng;
//    template<typename DataType>
//    std::string insertElem( std::string  value,const DataType data ) 
//    {
//        std::stringstream ret;
//        ret << "\""<< value  << "\":" << data;
//        return ret.str();
//    }
//    template<>
//    string insertElem(string  value, const std::set<std::string> data ) 
//    {
//        std::stringstream ret;
//        ret << STRING( value ) << ":";
//        ret << "[";
//        uint32_t size = data.size();
//        if(size >= 1) {
//            std::set<std::string>::const_iterator it= data.begin();
//            for ( ;  it != data.end() ; ++it)
//            {
//                if( it != data.begin() )
//                    ret << ",";
//
//                const std::string & tmp = *it;
//                ret <<"\"" << tmp  << "\"";
//            }
//        }
//        ret << "]";
//        return ret.str();
//    }
//
//    template<typename BASIC_TYPE>
//    string insertElem(string  value, const std::vector<BASIC_TYPE> data ) 
//    {
//        std::stringstream ret;
//        ret << STRING( value ) << ":";
//        ret << "[";
//        uint32_t size = data.size();
//        if(size >= 1) {
//
//        for(uint32_t i = 0 ; i < size - 1; i++) 
//        {
//            auto & elem = data[i];
//            ret << elem << ",";
//        }
//        auto & elem = data[size-1];
//        ret << elem;
//        }
//        ret << "]";
//        return ret.str();
//    }
//    template<>
    std::ostream & operator<<( std::ostream & out , const std::vector<uint64_t>& io_vec ) {
        out << "[";
        if(io_vec.size() > 0 ) {
            for( uint32_t i = 0 ; i < io_vec.size() - 1 ; i++ ) {
                out << io_vec[i] << ", ";
            }
            out << io_vec[io_vec.size()-1] << "]";
        }

        return out;
    }
    string insertElem(string value, const std::vector<Record> & data) {
        std::stringstream ret;
        ret << STRING( value ) << ":";
        ret << "["<<std::endl;
        uint32_t size = data.size();

        auto  func = [](const Record & elem )->string {
            std::stringstream ret_tmp ; 
            ret_tmp << "[ "
                 << (int)elem.record_type << ","
                 << (int)elem.sim_mode    << ","
                 << elem.pc << ","
                 << elem.sim_fs << ","
                 << elem.ins_num_vec //<< ","
                 << "]"; 

            return ret_tmp.str();
        };


        if(size >= 1) {

            for(uint32_t i = 0 ; i < size - 1; i++) 
            {
                ret << func( data[i] ) << " ," << std::endl;
            }
            auto & elem = data[size-1];
            ret << func(elem) << std::endl;
        }
        ret << "]";
        return ret.str();
    }
//    template<>
//    string insertElem(string  value, const string data ) 
//    {
//        std::stringstream ret;
//        ret << "\""<< value  << "\":" << "\""<< data << "\"";
//       
//        return ret.str();
//    }

    std::string RecordtoJson( const std::vector<Record> & record_pc )
    {
        std::stringstream ret;
        ret << "{";
        ret << insertElem( "record" , record_pc);
        ret << "}";
        return ret.str();
    }
std::string insertElem(const std::string & value, const uint64_t  data[MAX_NUM_THREADS_DEFAULT],const int num ) 
    {
        std::stringstream ret;
        ret << STRING( value ) << ":";
        ret << "[";
        int32_t size = num;
        if(size >= 1) {

        for(int32_t i = 0 ; i < size - 1; i++) 
        {
 	    if (i >= MAX_NUM_THREADS_DEFAULT) {
                std::cerr << "[ERROR] Invalid size " << i << " " << num << " " << MAX_NUM_THREADS_DEFAULT << "\n";
		return ret.str();
            }

            auto & elem = data[i];
            ret << elem << ",";
        }
        auto & elem = data[size-1];
        ret << elem;
        }
        ret << "]";
        return ret.str();
    }
std::string insertElem(const std::string & value, const double*  data,const int num ) 
    {
        std::stringstream ret;
        ret << STRING( value ) << ":";
        ret << "[";
        int32_t size = num;
        if(size >= 1) {

        for(int32_t i = 0 ; i < size - 1; i++) 
        {
            auto & elem = data[i];
            ret << elem << ",";
        }
        auto & elem = data[size-1];
        ret << elem;
        }
        ret << "]";
        return ret.str();
    }



