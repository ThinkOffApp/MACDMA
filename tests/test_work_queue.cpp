#include "cx5_work_queue.hpp"
#include <assert.h>
#include <stdio.h>

int main() {
    cx5::WorkQueue queue;
    cx5::WorkRecord completion;
    assert(!queue.complete(0,completion));
    assert(!queue.post(0,42,0x08,0,7));
    assert(!queue.post(0,42,0x08,4096,0));
    for (unsigned i=0;i<31;++i) assert(queue.post(i,1000+i,0x08,4096,7));
    assert(!queue.post(31,2000,0x10,4096,8));
    assert(queue.references(7) && !queue.references(8));
    assert(!queue.complete(32,completion));
    assert(queue.complete(0,completion) && completion.id==1000 && completion.length==4096);
    assert(!queue.complete(0,completion));
    assert(queue.post(31,2000,0x10,4096,8));
    for (unsigned i=1;i<31;++i) assert(queue.complete(uint16_t(i),completion) && completion.id==1000+i);
    assert(queue.complete(31,completion) && completion.id==2000 && completion.opcode==0x10);
    assert(!queue.pending() && !queue.references(7));
    // Cross both the hardware counter rollover and the 32-bit producer rollover.
    const uint32_t starts[]={65500,UINT32_MAX-64};
    for (uint32_t start:starts) for (uint32_t i=0;i<200;++i) {
        const uint32_t producer=start+i;
        const uint64_t id=(uint64_t(start)<<32)|i;
        assert(queue.post(producer,id,0x10,1024,99));
        assert(!queue.post(producer,id,0x10,1024,99));
        assert(queue.complete(uint16_t(producer),completion));
        assert(completion.id==id && completion.counter==uint16_t(producer));
        assert(!queue.complete(uint16_t(producer),completion));
    }
    assert(queue.post(0,1,0x0a,16,22)); queue.removed();
    assert(!queue.pending() && !queue.references(22));
    puts("PASS work IDs, queue exhaustion, stale/duplicate completions, counter rollover and removal");
}
