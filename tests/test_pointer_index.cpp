#include "cx5_pointer_index.hpp"
#include <assert.h>
#include <map>
#include <random>
#include <stdio.h>
#include <vector>
int main() {
    cx5::PointerIndex<int> index; int values[65]{};
    // Force a 64-item cluster across the end of the array, then remove every
    // possible position and verify all surviving keys and reclaimed capacity.
    std::vector<uint32_t> keys;
    for(uint32_t key=0;keys.size()<64;++key)
        if((key*uint32_t(2654435761u)>>25)==127) keys.push_back(key);
    for(size_t removed=0;removed<keys.size();++removed) {
        for(size_t i=0;i<64;++i) assert(index.insert(keys[i],&values[i]));
        assert(!index.insert(123456,&values[64]));
        assert(!index.erase(keys[removed],&values[64]));
        assert(index.erase(keys[removed],&values[removed]));
        for(size_t i=0;i<64;++i) assert(index.find(keys[i])==(i==removed?nullptr:&values[i]));
        assert(index.insert(keys[removed],&values[removed]));
        for(size_t i=0;i<64;++i) assert(index.erase(keys[i],&values[i]));
        assert(index.size()==0);
    }
    // Deterministic model-based churn catches stale aliases and failed erases.
    std::mt19937 random(0x435835); std::map<uint32_t,int *> model;
    for(unsigned i=0;i<20000;++i) {
        uint32_t key=random()%96; int *value=&values[key%65];
        if(random()&1) {
            bool expected=!model.count(key) && model.size()<64;
            assert(index.insert(key,value)==expected);
            if(expected) model[key]=value;
        } else {
            bool expected=model.count(key);
            assert(index.erase(key,value)==expected); model.erase(key);
        }
        for(uint32_t key2=0;key2<96;++key2) {
            auto at=model.find(key2);
            assert(index.find(key2)==(at==model.end()?nullptr:at->second));
        }
        assert(index.size()==model.size());
    }
    for(auto entry:model) assert(index.erase(entry.first,entry.second));
    assert(index.insert(0,&values[0]) && index.insert(UINT32_MAX,&values[1]));
    assert(!index.insert(0,&values[2]) && !index.insert(17,nullptr));
    assert(index.find(0)==&values[0] && index.find(UINT32_MAX)==&values[1]);
    puts("PASS object index collisions, cluster wrap, limits, stale-pointer rejection and model-based churn");
}
