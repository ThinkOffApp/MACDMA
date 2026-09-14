// Offline test of the userspace completion engine against a synthetic CQ
// page written the way the NIC and kernel write it. No hardware, no verbs.
#include "cx5_cq_observer.h"
#include "cx5_user_completion.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

struct fake { struct mcdma_user_queue sends,receives; uint32_t qpn; } qps[2];
static struct mcdma_user_qp_view lookup(void *context,uint32_t qpn) {
    (void)context; struct mcdma_user_qp_view view={NULL,NULL,0};
    for (unsigned i=0;i<2;++i) if (qps[i].qpn==qpn) { view.sends=&qps[i].sends; view.receives=&qps[i].receives; }
    return view;
}
static void write_be32(uint8_t *p,uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
// Emits a hardware-style CQE at ring index `index`, owner byte last.
static void emit(uint8_t *page,uint32_t index,uint32_t qpn,uint16_t counter,uint8_t opcode,uint8_t syndrome,uint32_t bytes) {
    uint8_t *entry=page+(index&31u)*64;
    memset(entry,0,63);
    write_be32(entry+56,qpn); write_be32(entry+44,bytes);
    entry[60]=(uint8_t)(counter>>8); entry[61]=(uint8_t)counter; entry[55]=syndrome; entry[54]=0x42;
    __atomic_store_n(entry+63,(uint8_t)((opcode<<4)|((index>>5)&1u)),__ATOMIC_RELEASE);
}
int main(void) {
    static uint8_t page[MCDMA_CQ_MAP_BYTES] __attribute__((aligned(16384)));
    for (unsigned i=0;i<32;++i) page[i*64+63]=0xf1; // Fresh CQ: invalid opcode, owner 1.
    qps[0].qpn=0x123456; qps[1].qpn=0x000042;
    struct mcdma_user_completion c; uint8_t cqe[64];
    // Not live until the kernel publishes the marker.
    CHECK(mcdma_cq_entry(page,0,cqe)==-1);
    mcdma_cq_set_live(page,1); mcdma_cq_set_consumer(page,0);
    CHECK(mcdma_cq_consumer(page)==0 && mcdma_cq_entry(page,0,cqe)==0 && mcdma_cq_observe(page)==0);
    // Queue mirror: credits, slot reuse and rollback follow the kernel rules.
    struct mcdma_user_queue *q=&qps[0].sends;
    for (unsigned i=0;i<31;++i) CHECK(mcdma_user_queue_post(q,1000+i,0x08,4096));
    CHECK(!mcdma_user_queue_post(q,2000,0x10,4096) && q->pending==31 && q->producer==31);
    mcdma_user_queue_rollback(q,2); CHECK(q->pending==29 && q->producer==29 && !q->records[29].occupied);
    CHECK(mcdma_user_queue_post(q,1029,0x10,64) && mcdma_user_queue_post(q,1030,0x0a,16));
    // A WRITE, a READ and a SEND complete in order through hardware CQEs.
    emit(page,0,0x123456,0,0,0,0);
    CHECK(mcdma_cq_entry(page,0,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1);
    CHECK(c.id==1000 && c.status==0 && c.opcode==1 && c.bytes==0 && c.qpn==0x123456 && c.vendor==0);
    CHECK(mcdma_cq_entry(page,0,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==-1); // Duplicate CQE.
    for (uint16_t i=1;i<29;++i) { emit(page,i,0x123456,i,0,0,0); CHECK(mcdma_cq_entry(page,i,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==1000u+i); }
    emit(page,29,0x123456,29,0,0,0);
    CHECK(mcdma_cq_entry(page,29,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==1029 && c.opcode==2 && c.bytes==64);
    emit(page,30,0x123456,30,0,0,0);
    CHECK(mcdma_cq_entry(page,30,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==1030 && c.opcode==0 && c.bytes==0);
    CHECK(q->pending==0);
    // Error completion keeps the work ID and carries the hardware syndrome.
    CHECK(mcdma_user_queue_post(q,77,0x10,4096));
    emit(page,31,0x123456,31,13,0x13,0);
    CHECK(mcdma_cq_entry(page,31,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1);
    CHECK(c.id==77 && c.status==10 && c.vendor==0x4213 && c.opcode==2 && c.bytes==0);
    // Wraparound: index 32 uses owner phase 1 on slot 0, and the stale entry
    // at slot 0 (phase 0) must read as empty before the NIC rewrites it.
    CHECK(mcdma_cq_entry(page,32,cqe)==0);
    CHECK(mcdma_user_queue_post(q,88,0x08,4096));
    emit(page,32,0x123456,32,0,0,0);
    CHECK(mcdma_cq_entry(page,32,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==88);
    // Receive completions: byte count, oversize, immediate formats.
    struct mcdma_user_queue *r=&qps[1].receives;
    CHECK(mcdma_user_queue_post(r,500,0x0a,4096) && mcdma_user_queue_post(r,501,0x0a,64) && mcdma_user_queue_post(r,502,0x0a,64));
    emit(page,33,0x42,0,2,0,4000);
    CHECK(mcdma_cq_entry(page,33,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==500 && c.opcode==128 && c.bytes==4000 && c.status==0);
    emit(page,34,0x42,1,2,0,65);
    CHECK(mcdma_cq_entry(page,34,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==501 && c.status==1 && c.bytes==0);
    emit(page,35,0x42,2,3,0,16);
    CHECK(mcdma_cq_entry(page,35,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==502 && c.status==21 && c.vendor==3);
    // A responder error goes to the receive queue.
    CHECK(mcdma_user_queue_post(r,503,0x0a,64));
    emit(page,36,0x42,3,14,0x05,0);
    CHECK(mcdma_cq_entry(page,36,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==503 && c.status==5 && c.opcode==128);
    // Unknown QP, wrong queue and unsupported opcodes are rejected.
    emit(page,37,0x999999,0,0,0,0);
    CHECK(mcdma_cq_entry(page,37,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==-1);
    emit(page,38,0x42,9,0,0,0); // No send record with counter 9.
    CHECK(mcdma_cq_entry(page,38,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==-1);
    emit(page,39,0x42,0,7,0,0);
    CHECK(mcdma_cq_entry(page,39,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==-1);
    // 16-bit counter rollover and 24-bit consumer rollover stay consistent.
    mcdma_user_queue_reset(q); q->producer=65535;
    CHECK(mcdma_user_queue_post(q,9001,0x08,1) && mcdma_user_queue_post(q,9002,0x08,1));
    emit(page,40,0x123456,65535,0,0,0);
    CHECK(mcdma_cq_entry(page,40,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==9001);
    emit(page,41,0x123456,0,0,0,0);
    CHECK(mcdma_cq_entry(page,41,cqe)==1 && mcdma_user_decode(cqe,lookup,NULL,&c)==1 && c.id==9002);
    mcdma_cq_set_consumer(page,0xffffffu); CHECK(mcdma_cq_consumer(page)==0xffffffu);
    CHECK(((mcdma_cq_consumer(page)+1u)&0xffffffu)==0);
    // Retired CQ reads as not live.
    mcdma_cq_set_live(page,0); CHECK(mcdma_cq_entry(page,41,cqe)==-1 && mcdma_cq_observe(page)==-1);
    printf("PASS %u userspace completion mirror/decode checks (synthetic CQ page, no hardware)\n",checks);
    return 0;
}
