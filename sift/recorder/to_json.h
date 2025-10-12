#ifndef __TO_JSON_H__
#define __TO_JSON_H__
#include "mtng.h"
#define MAX_NUM_THREADS_DEFAULT 128
#define STRING(data) ("\""+data+"\"")
std::string RecordtoJson( const std::vector<mtng::Record> & record_pc );
std::string insertElem(const std::string & value, const uint64_t  data[MAX_NUM_THREADS_DEFAULT],const int size ) ;

std::string insertElem(const std::string & value, const double*  data,const int num ) ;
#endif
