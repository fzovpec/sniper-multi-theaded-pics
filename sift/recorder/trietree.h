#ifndef __TRIETREE__
#define __TRIETREE__
#include <vector>
#define TRIBUFFER_N 512
#define REFINE_N(size) (( size / TRIBUFFER_N + 1 ) * TRIBUFFER_N )
class TrieNode {
    // The Trie Node Structure
    // Each node has N children, starting from the root
    // and a flag to check if it's a leaf node
    public:
    std::vector< TrieNode* > children;
    std::vector<uint32_t > cluster_count;
    uint64_t max_data; // Storing for printing purposes only
    uint32_t max_num;
    TrieNode(uint32_t data1,uint32_t newsize) {
    // Allocate memory for a TrieNode
        
        resize(newsize);
        max_data = data1;
        max_num = 1;
        cluster_count[data1]++;
    }
    TrieNode() {
    // Allocate memory for a TrieNode 
        resize(0);
        max_num = 0;
        max_data = -1;
    }

    void resize(uint32_t resize){
        children.resize( REFINE_N( resize ), NULL  );
        cluster_count.resize( REFINE_N( resize ), 0 );
    }
    void update(uint32_t wordelem) {
        uint32_t new_number = cluster_count[wordelem] + 1;
        if( new_number > max_num) {
            max_num = new_number;
            max_data = wordelem;
        }
        cluster_count[wordelem] = new_number;
    }
};
class TrieClass {

    TrieNode * root;
    public:
    TrieClass() ;

    void print_trie() ; // Breadth-First Search
    uint64_t search_trie( const std::vector< uint64_t> & word);

    void insert_trie(  const std::vector<uint64_t >& word) ;
};
#endif
