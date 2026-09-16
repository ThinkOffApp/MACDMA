#pragma once
#include <stddef.h>
#include <stdint.h>

namespace cx5 {
// Original fixed storage index for at most `Limit` live driver objects (a
// power of two; twice as many slots keep probes short). Callers retain
// ownership and serialize all accesses; this adds no allocation or lock.
// Empty slots use the pointer, so every 32-bit key, including zero, is valid.
template<class T,size_t Limit=64> class PointerIndex {
    static_assert(Limit>=2 && (Limit&(Limit-1))==0,"limit must be a power of two");
    static constexpr size_t slots=Limit*2;
    static constexpr unsigned log2_slots() { unsigned n=0; for (size_t s=slots;s>1;s>>=1) ++n; return n; }
    static constexpr unsigned shift=32-log2_slots();
    static constexpr size_t mask=slots-1;
    struct Slot { uint32_t key=0; T *value=nullptr; } slots_[slots]{};
    size_t size_=0;
    static size_t bucket(uint32_t key) { return (key*uint32_t(2654435761u))>>shift; }
public:
    static constexpr size_t limit=Limit;
    size_t size() const { return size_; }
    T *find(uint32_t key) const {
        size_t at=bucket(key);
        for(size_t n=0;n<slots;++n,at=(at+1)&mask) {
            if(!slots_[at].value) return nullptr;
            if(slots_[at].key==key) return slots_[at].value;
        }
        return nullptr;
    }
    bool insert(uint32_t key,T *value) {
        if(!value || size_==limit) return false;
        size_t at=bucket(key);
        for(size_t n=0;n<slots;++n,at=(at+1)&mask) {
            if(!slots_[at].value) { slots_[at]={key,value}; ++size_; return true; }
            if(slots_[at].key==key) return false;
        }
        return false;
    }
    bool erase(uint32_t key,T *expected) {
        size_t hole=bucket(key);
        for(size_t n=0;n<slots;++n,hole=(hole+1)&mask) {
            if(!slots_[hole].value) return false;
            if(slots_[hole].key!=key) continue;
            if(slots_[hole].value!=expected) return false;
            slots_[hole]={}; --size_;
            // Close the cluster without tombstones, including wrap at the last slot.
            for(size_t scan=(hole+1)&mask;slots_[scan].value;scan=(scan+1)&mask) {
                size_t home=bucket(slots_[scan].key);
                if(((scan-home)&mask)>=((scan-hole)&mask)) {
                    slots_[hole]=slots_[scan]; slots_[scan]={}; hole=scan;
                }
            }
            return true;
        }
        return false;
    }
};
}
