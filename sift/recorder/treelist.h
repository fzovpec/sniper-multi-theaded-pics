#ifndef __TREELIST__
#define __TREELIST__
#include<list>
#include<unordered_map>
#include<iostream>
template<typename KeyType, typename ElemType> //ElemType should define < operator and hash function
class TreeList {
    using ListType = typename std::list<std::pair<KeyType,ElemType>>;
    using MapElemType=typename ListType::iterator;
    uint32_t  capacity_;
    std::unordered_map< KeyType , MapElemType> map2listelem;
    public:
    ListType elemlist;
    TreeList( )=default;
    TreeList(uint32_t capacity ):capacity_(capacity) {
    }

    void update( KeyType keyvalue, uint64_t e1,uint64_t e2) {
        ///try to find elem from map
        auto map2listelem_find = map2listelem.find(keyvalue);
        std::pair<uint64_t,uint64_t> elemvalue;
        if (map2listelem_find == map2listelem.end() ) { //this value is never be seen before
            if (e1 > e2) {
                elemvalue = std::pair<uint64_t,uint64_t>( 0,e2 );
            } else {
                elemvalue = std::pair<uint64_t,uint64_t>( e1,0 );
            }
            elemlist.push_back( std::pair<KeyType,ElemType>(keyvalue,elemvalue) );
            auto endit = elemlist.end();
            endit--;
            map2listelem[keyvalue] = endit;
            if( map2listelem.size() > capacity_ ) { //remove the elem to keep the capcity
                auto firstelem = elemlist.begin();
                map2listelem.erase( (*firstelem).first );
                elemlist.erase(firstelem);
            }
        } else {
            //to update this elem; we first remove old data inside list
            auto elemlist_it = (*map2listelem_find).second;
            if (e1 > e2) {
                elemvalue = std::pair<uint64_t,uint64_t>( (*elemlist_it).second.first,e2 );
            } else {
                elemvalue = std::pair<uint64_t,uint64_t>( e1,(*elemlist_it).second.second );
            }

            elemlist.erase(elemlist_it);
            elemlist.push_back( std::pair<KeyType,ElemType>(keyvalue,elemvalue) );
            auto endit = elemlist.end();
            endit--;          
            (*map2listelem_find).second = (endit);
 
        }

    }
    void update( KeyType keyvalue, ElemType elemvalue ) {
        ///try to find elem from map
        auto map2listelem_find = map2listelem.find(keyvalue);
        if (map2listelem_find == map2listelem.end() ) { //this value is never be seen before
            elemlist.push_back( std::pair<KeyType,ElemType>(keyvalue,elemvalue) );
            auto endit = elemlist.end();
            endit--;
            map2listelem[keyvalue] = endit;
            if( map2listelem.size() > capacity_ ) { //remove the elem to keep the capcity
                auto firstelem = elemlist.begin();
                map2listelem.erase( (*firstelem).first );
                elemlist.erase(firstelem);
            }
        } else {
            //to update this elem; we first remove old data inside list
            auto elemlist_it = (*map2listelem_find).second;
            elemlist.erase(elemlist_it);
            elemlist.push_back( std::pair<KeyType,ElemType>(keyvalue,elemvalue) );
            auto endit = elemlist.end();
            endit--;          
            (*map2listelem_find).second = (endit);
 
        }
    }
    void clear(){
        map2listelem.clear();
        elemlist.clear();
    }
    void print() {
        for(auto elem : elemlist) {
            std::cout << elem.first << " " << elem.second << std::endl;
        }
        std::cout << std::endl;
    }
    
};
#endif
