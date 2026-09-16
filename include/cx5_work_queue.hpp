#pragma once
#include <stdint.h>
#include <stddef.h>

namespace cx5 {
struct WorkRecord {
    uint64_t id = 0;
    uint32_t length = 0, lkey = 0;
    uint16_t counter = 0;
    uint8_t opcode = 0;
    bool occupied = false;
    // Every memory key the request references (up to three scatter entries);
    // lkey mirrors the first for the original single-entry callers.
    uint32_t lkeys[3] = {0,0,0};
    uint8_t lkey_count = 0;
};
// Firmware reports a 16-bit WQE counter, whereas software's producer is 32-bit.
// A slot cannot be reused until that exact work request has completed.
class WorkQueue {
public:
    bool can_post(uint32_t producer) const {
        return pending_ < 31 && !records_[producer & 31].occupied;
    }
    bool post(uint32_t producer,uint64_t id,uint8_t opcode,uint32_t length,const uint32_t *lkeys,unsigned count) {
        if (!can_post(producer) || !length || !lkeys || !count || count>3) return false;
        for (unsigned i=0;i<count;++i) if (!lkeys[i]) return false;
        WorkRecord record{id,length,lkeys[0],uint16_t(producer),opcode,true,{0,0,0},uint8_t(count)};
        for (unsigned i=0;i<count;++i) record.lkeys[i]=lkeys[i];
        records_[producer & 31]=record;
        ++pending_; return true;
    }
    bool post(uint32_t producer,uint64_t id,uint8_t opcode,uint32_t length,uint32_t lkey) {
        return post(producer,id,opcode,length,&lkey,1);
    }
    bool complete(uint16_t counter,WorkRecord &result) {
        auto &record=records_[counter & 31];
        if (!record.occupied || record.counter!=counter) return false;
        result=record; record={}; --pending_; return true;
    }
    bool references(uint32_t lkey) const {
        for (const auto &record:records_)
            if (record.occupied)
                for (unsigned i=0;i<record.lkey_count;++i) if (record.lkeys[i]==lkey) return true;
        return false;
    }
    uint32_t pending() const { return pending_; }
    void removed() { for (auto &record:records_) record={}; pending_=0; }
private:
    WorkRecord records_[32]{};
    uint32_t pending_ = 0;
};
}
