#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cx5 {
// Original fixed storage index for at most 64 live driver objects. Callers
// retain ownership and serialize all accesses; this adds no allocation or lock.
// Empty slots use the pointer, so every 32-bit key, including zero, is valid.
template<class T> class PointerIndex {
    struct Slot { uint32_t key=0; T *value=nullptr; } slots_[128]{};
    size_t size_=0;
    static size_t bucket(uint32_t key) { return (key*uint32_t(2654435761u))>>25; }
public:
    static constexpr size_t limit=64;
    size_t size() const { return size_; }
    T *find(uint32_t key) const {
        size_t at=bucket(key);
        for(size_t n=0;n<128;++n,at=(at+1)&127) {
            if(!slots_[at].value) return nullptr;
            if(slots_[at].key==key) return slots_[at].value;
        }
        return nullptr;
    }
    bool insert(uint32_t key,T *value) {
        if(!value || size_==limit) return false;
        size_t at=bucket(key);
        for(size_t n=0;n<128;++n,at=(at+1)&127) {
            if(!slots_[at].value) { slots_[at]={key,value}; ++size_; return true; }
            if(slots_[at].key==key) return false;
        }
        return false;
    }
    bool erase(uint32_t key,T *expected) {
        size_t hole=bucket(key);
        for(size_t n=0;n<128;++n,hole=(hole+1)&127) {
            if(!slots_[hole].value) return false;
            if(slots_[hole].key!=key) continue;
            if(slots_[hole].value!=expected) return false;
            slots_[hole]={}; --size_;
            // Close the cluster without tombstones, including wrap at slot127.
            for(size_t scan=(hole+1)&127;slots_[scan].value;scan=(scan+1)&127) {
                size_t home=bucket(slots_[scan].key);
                if(((scan-home)&127)>=((scan-hole)&127)) {
                    slots_[hole]=slots_[scan]; slots_[scan]={}; hole=scan;
                }
            }
            return true;
        }
        return false;
    }
};
}
