#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vector>
// The number of children for each node
// We will construct a N-ary tree and make it
// a Trie
// Since we have 26 english letters, we need
// 26 children per node
#define N 512
#define MAX_DEPTH 16
#include <queue>
#include "trietree.h"

    TrieClass::TrieClass() {
        root = new TrieNode();
    }
uint64_t TrieClass::search_trie( const std::vector<uint64_t> & word)
{
    // Searches for word in the Trie
    TrieNode* temp = root;
    int32_t idx = word.size()-1;
    while( idx >= 0 ) {
        uint64_t position = word[idx];
        if ( position >= temp->children.size() || temp->children[position] == NULL )
            break;
        temp = temp->children[position];
        idx--;
    }
     
//    if (temp != root )
    return temp->max_data;
//    return (uint32_t)-1;
}

void TrieClass::insert_trie(  const std::vector<uint64_t> & word) {
    // Inserts the word onto the Trie
    // ASSUMPTION: The word only has lower case characters
    TrieNode* temp = root;
    int32_t idx = word.size()-1;
    uint32_t predictValue = word[idx];
    idx--;
    int max_depth_i = 0;
    uint32_t newsize = predictValue+1;
    for ( ;idx >= 0 && max_depth_i < MAX_DEPTH; max_depth_i++,idx-- ) {
        // Get the relative position in the alphabet list
        uint32_t word_elem =  word[idx];
        newsize = std::max(word_elem+1, newsize);
        if( newsize > temp->children.size()) {
            temp->resize( newsize   );
        }
        if ( temp->children[word_elem] == NULL) {
            // If the corresponding child doesn't exist,
            // simply create that child!
            temp->children[word_elem] = new TrieNode(predictValue,newsize);
        } 
        
        temp->update( predictValue );
        
        // Go down a level, to the child referenced by idx
        // since we have a prefix match
        temp = temp->children[word_elem];
    }
    // At the end of the word, mark this node as the leaf node
}


void TrieClass::print_trie() { // Breadth-First Search
    std::queue<TrieNode*> queue1;
    std::queue<TrieNode*> queue2;

    std::queue<TrieNode*> & current_queue = queue1;
    std::queue<TrieNode*> & next_queue = queue2;
    current_queue.push(root);
    int level_i = 0;
    printf("level : %d :\n",level_i);
    while(current_queue.size()!=0) {
        TrieNode * node = current_queue.front();
        current_queue.pop();
        for( uint32_t i = 0 ; i < node->children.size() ; i++ ) {
            TrieNode * nextlevel_node = node->children[i];
            if(nextlevel_node != NULL) {
                printf( "%d ", i );
                next_queue.push(nextlevel_node); 
            }
        }
        if( current_queue.size() == 0 ) {
//            std::queue<TrieNode*> &  tmp = current_queue ;
//            current_queue = next_queue;
//            next_queue = tmp;
            std::swap(current_queue,next_queue);            
            level_i++;
            printf("\nlevel : %d :\n",level_i);
        }
    }
}
//void print_search( uint32_t* word) {
//    if (search_trie( word) == 0)
//        printf("Not Found\n");
//    else
//        printf("Found!\n");
//}
/*
int main() {
    // Driver program for the Trie Data Structure Operations
    std::vector<uint32_t> history_queue;
    history_queue.push_back(-1);
    TrieClass trieclass;
    history_queue.push_back(1);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(2);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(3);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(4);
    trieclass.insert_trie( &history_queue.back() );

    history_queue.push_back(4);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(3);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(2);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(2);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(2);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(2);
    trieclass.insert_trie( &history_queue.back() );
    history_queue.push_back(3);
    trieclass.insert_trie( &history_queue.back() );


    auto * ret = trieclass.search_trie( &history_queue.back() );
    if(ret!=NULL) {
        printf("predict: %d\n",ret->data);
    }
//    trieclass.search_trie( &history_queue.back() );
    trieclass.print_trie( );
    return 0;
}
*/
