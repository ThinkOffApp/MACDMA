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
};
// Firmware reports a 16-bit WQE counter, whereas software's producer is 32-bit.
// A slot cannot be reused until that exact work request has completed.
class WorkQueue {
public:
    bool can_post(uint32_t producer) const {
        return pending_ < 31 && !records_[producer & 31].occupied;
    }
    bool post(uint32_t producer,uint64_t id,uint8_t opcode,uint32_t length,uint32_t lkey) {
        if (!can_post(producer) || !length || !lkey) return false;
        records_[producer & 31]={id,length,lkey,uint16_t(producer),opcode,true};
        ++pending_; return true;
    }
    bool complete(uint16_t counter,WorkRecord &result) {
        auto &record=records_[counter & 31];
        if (!record.occupied || record.counter!=counter) return false;
        result=record; record={}; --pending_; return true;
    }
    bool references(uint32_t lkey) const {
        for (const auto &record:records_) if (record.occupied && record.lkey==lkey) return true;
        return false;
    }
    uint32_t pending() const { return pending_; }
    void removed() { for (auto &record:records_) record={}; pending_=0; }
private:
    WorkRecord records_[32]{};
    uint32_t pending_ = 0;
};
}
