// Exercise the actual provider's error paths with ordinary memory standing
// in for mapped device pages. No hardware commands or MMIO are issued.
#define ibv_cmd_poll_cq test_cmd_poll_cq
#define ibv_cmd_destroy_cq test_cmd_destroy_cq
#define ibv_cmd_destroy_qp test_cmd_destroy_qp
#define darwin_munmap test_munmap
#include "../native/user_provider.c"
#undef ibv_cmd_poll_cq
#undef ibv_cmd_destroy_cq
#undef ibv_cmd_destroy_qp
#undef darwin_munmap
#include <assert.h>

static unsigned kernel_polls;
static unsigned cq_destroys,qp_destroys,unmaps;
static int cq_destroy_error,qp_destroy_error,unmap_error;
int test_cmd_destroy_cq(struct ibv_cq *cq) { (void)cq; ++cq_destroys; return cq_destroy_error; }
int test_cmd_destroy_qp(struct ibv_qp *qp) { (void)qp; ++qp_destroys; return qp_destroy_error; }
int test_munmap(int fd,void *address) { (void)fd; (void)address; ++unmaps; return unmap_error; }
int test_cmd_poll_cq(struct ibv_cq *cq,int maximum,struct ibv_wc *wc) {
    (void)cq;
    ++kernel_polls;
    if (maximum<=0) return 0;
    memset(wc,0,sizeof(*wc));
    wc->wr_id=7; // A raw hardware counter, never the application's WR ID.
    return 1;
}

static void retired_post(void) {
    uint8_t queue[MCDMA_QUEUE_PAGE_BYTES]={0},uar[MCDMA_QUEUE_PAGE_BYTES]={0};
    uint8_t cq_page[MCDMA_CQ_MAP_BYTES]={0};
    uint32_t live=MCDMA_CQ_LIVE_MAGIC;
    memcpy(cq_page+MCDMA_CQ_LIVE_OFFSET,&live,sizeof(live));
    struct mcdma_cq cq={0}; cq.observation=cq_page; cq.consume=1; cq.retired=1;
    struct mcdma_context mc={0}; mc.uar=uar;
    struct mcdma_qp qp={0}; qp.queue=queue; qp.user_posted=1;
    qp.qp.qp_num=17; qp.qp.send_cq=qp.qp.recv_cq=&cq.cq;
    struct ibv_sge sge={.addr=0x10000,.length=16,.lkey=123};
    struct ibv_send_wr wr={0},*bad=NULL;
    wr.wr_id=0x1122334455667788ull; wr.sg_list=&sge; wr.num_sge=1;
    wr.opcode=IBV_WR_RDMA_WRITE; wr.send_flags=IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr=0x20000; wr.wr.rdma.rkey=456;
    assert(user_post_send(&mc,&qp,&wr,&bad)==EIO);
    assert(bad==&wr && qp.sends.producer==0);
    struct ibv_recv_wr recv={0},*bad_recv=NULL;
    recv.sg_list=&sge; recv.num_sge=1;
    assert(user_post_recv(&mc,&qp,&recv,&bad_recv)==EIO);
    assert(bad_recv==&recv && qp.receives.producer==0);
    for (unsigned i=0;i<sizeof(queue);++i) assert(queue[i]==0 && uar[i]==0);

    // A mismatch discovered in the next post's catch-up must stop that post,
    // not just emit a log and continue writing the device's doorbell.
    cq.retired=0; cq.ahead=cq.pending_count=MCDMA_ACK_BATCH;
    for (unsigned i=0;i<MCDMA_ACK_BATCH;++i) cq.pending[i].id=100+i;
    kernel_polls=0;
    assert(user_post_send(&mc,&qp,&wr,&bad)==EIO);
    assert(cq.retired && kernel_polls && bad==&wr && qp.sends.producer==0);
    for (unsigned i=0;i<sizeof(queue);++i) assert(queue[i]==0 && uar[i]==0);
}

static void retired_poll(void) {
    struct mcdma_context mc={0};
    struct mcdma_cq cq={0}; cq.retired=1; cq.consume=1;
    struct ibv_wc wc={0}; wc.wr_id=0x1122334455667788ull;
    kernel_polls=0;
    assert(user_poll(&mc,&cq,1,&wc)==-EIO);
    assert(kernel_polls==0 && wc.wr_id==0x1122334455667788ull);
    cq.ahead=cq.pending_count=1;
    assert(user_poll(&mc,&cq,1,&wc)==-EIO);
    assert(kernel_polls==0 && cq.ahead==1 && cq.pending_count==1);
}

static void fresh_mismatch(void) {
    uint8_t page[MCDMA_CQ_MAP_BYTES]={0};
    struct mcdma_context mc={0};
    struct mcdma_cq cq={0}; cq.observation=page; cq.consume=1;
    struct ibv_wc wc={0};
    // There is no matching QP/work record for this CQE. The raw command's
    // positive count must not escape as a successful application completion.
    kernel_polls=0;
    assert(kernel_fresh(&mc,&cq,1,&wc)==-EIO);
    assert(cq.retired && kernel_polls==1);
}

static void destroy_retries(void) {
    struct mcdma_context mc={0};
    mc.vctx.context.abi_compat=__VERBS_ABI_IS_EXTENDED;
    mc.vctx.context.ops.post_send=mcdma_post_send;
    assert(!pthread_mutex_init(&mc.lock,NULL));
    struct mcdma_cq *cq=calloc(1,sizeof(*cq)); assert(cq);
    assert(!pthread_mutex_init(&cq->poll_lock,NULL));
    uint8_t cq_page[MCDMA_CQ_MAP_BYTES]={0};
    cq->cq.context=&mc.vctx.context; cq->consume=1; cq->observation=cq_page;
    mc.cqs[0]=cq;
    cq_destroys=unmaps=0; cq_destroy_error=EBUSY; unmap_error=0;
    assert(destroy_cq(&cq->cq)==EBUSY);
    assert(cq_destroys==1 && unmaps==0 && cq->consume && cq->observation==cq_page && mc.cqs[0]==cq);
    cq_destroy_error=0; unmap_error=EIO;
    assert(destroy_cq(&cq->cq)==EIO);
    assert(cq_destroys==2 && unmaps==1 && cq->observation==cq_page);
    struct ibv_wc wc={0}; kernel_polls=0;
    assert(poll_cq(&cq->cq,1,&wc)==-EIO && kernel_polls==0);
    unmap_error=0;
    assert(destroy_cq(&cq->cq)==0);
    assert(cq_destroys==2 && unmaps==2 && mc.cqs[0]==NULL);

    struct mcdma_qp *qp=calloc(1,sizeof(*qp)); assert(qp);
    uint8_t queue[MCDMA_QUEUE_PAGE_BYTES]={0};
    qp->qp.context=&mc.vctx.context; qp->queue=queue; qp->user_posted=1; mc.qps[0]=qp;
    qp_destroys=unmaps=0; qp_destroy_error=EBUSY;
    assert(destroy_qp(&qp->qp)==EBUSY);
    assert(qp_destroys==1 && unmaps==0 && qp->queue==queue && qp->user_posted && mc.qps[0]==qp);
    qp_destroy_error=0; unmap_error=EIO;
    assert(destroy_qp(&qp->qp)==EIO);
    assert(qp_destroys==2 && unmaps==1 && qp->queue==queue && qp->user_posted);
    unmap_error=0;
    assert(destroy_qp(&qp->qp)==0);
    assert(qp_destroys==2 && unmaps==2 && mc.qps[0]==NULL);
    pthread_mutex_destroy(&mc.lock);
}

static void blueflame_arms(void) {
    // Environment parsing: every arm needs direct posting; unknown values fail.
    unsetenv("MCDMA_USER_BF"); assert(bf_mode_from_environment()==MCDMA_BF_OFF);
    setenv("MCDMA_USER_BF","0",1); assert(bf_mode_from_environment()==MCDMA_BF_OFF);
    setenv("MCDMA_USER_BF","64",1); assert(bf_mode_from_environment()==MCDMA_BF_64);
    setenv("MCDMA_USER_BF","128",1); assert(bf_mode_from_environment()==MCDMA_BF_128);
    setenv("MCDMA_USER_BF","64s",1); assert(bf_mode_from_environment()==MCDMA_BF_64S);
    setenv("MCDMA_USER_BF","db",1); assert(bf_mode_from_environment()==MCDMA_BF_DOORBELL);
    setenv("MCDMA_USER_BF","32",1); assert(bf_mode_from_environment()==-1);
    setenv("MCDMA_USER_BF","1",1); assert(bf_mode_from_environment()==-1);
    unsetenv("MCDMA_USER_BF");
    assert(!strcmp(bf_mode_name(MCDMA_BF_64),"64") && !strcmp(bf_mode_name(MCDMA_BF_128),"128") &&
           !strcmp(bf_mode_name(MCDMA_BF_64S),"64s") && !strcmp(bf_mode_name(MCDMA_BF_DOORBELL),"db"));

    // Posting through the real provider function with ordinary memory as the
    // mapped pages: a single WQE goes through alternating banks, a list of
    // two rings the ordinary doorbell, and a doorbell-only context never
    // writes beyond the first eight bytes of the register.
    static uint8_t queue[MCDMA_QUEUE_PAGE_BYTES],uar[MCDMA_QUEUE_PAGE_BYTES],cq_page[MCDMA_CQ_MAP_BYTES];
    memset(queue,0,sizeof(queue)); memset(uar,0,sizeof(uar)); memset(cq_page,0,sizeof(cq_page));
    uint32_t live=MCDMA_CQ_LIVE_MAGIC; memcpy(cq_page+MCDMA_CQ_LIVE_OFFSET,&live,sizeof(live));
    struct mcdma_cq cq={0}; cq.observation=cq_page; cq.consume=1;
    struct mcdma_context mc={0}; mc.uar=uar; mc.uar_wc=1; mc.bf_mode=MCDMA_BF_64; mc.bf_bytes=64; mc.bf_vector=1; mc.bf_bank_bytes=256;
    struct mcdma_qp qp={0}; qp.queue=queue; qp.user_posted=1; qp.blueflame=1;
    qp.qp.qp_num=17; qp.qp.send_cq=qp.qp.recv_cq=&cq.cq;
    struct ibv_sge sge={.addr=0x10000,.length=4096,.lkey=123};
    struct ibv_send_wr wr={0},*bad=NULL;
    wr.wr_id=1; wr.sg_list=&sge; wr.num_sge=1; wr.opcode=IBV_WR_RDMA_WRITE; wr.send_flags=IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr=0x20000; wr.wr.rdma.rkey=456;
    assert(user_post_send(&mc,&qp,&wr,&bad)==0 && bad==NULL && qp.sends.producer==1 && mc.bf_bank==1);
    assert(!memcmp(uar+MCDMA_UAR_DOORBELL,queue+MCDMA_SQ_OFFSET,64));
    for (unsigned i=0;i<sizeof(uar);++i) if (i<MCDMA_UAR_DOORBELL || i>=MCDMA_UAR_DOORBELL+64) assert(uar[i]==0);
    assert(mcdma_get_le32(queue+MCDMA_DBR_OFFSET+4)==0x01000000u); // Big-endian producer 1.
    memset(uar,0,sizeof(uar)); wr.wr_id=2;
    assert(user_post_send(&mc,&qp,&wr,&bad)==0 && qp.sends.producer==2 && mc.bf_bank==0);
    assert(!memcmp(uar+MCDMA_UAR_DOORBELL+256,queue+MCDMA_SQ_OFFSET+64,64));
    for (unsigned i=0;i<sizeof(uar);++i) if (i<MCDMA_UAR_DOORBELL+256 || i>=MCDMA_UAR_DOORBELL+256+64) assert(uar[i]==0);
    // Two requests in one call: ordinary doorbell with the last control qword.
    memset(uar,0,sizeof(uar)); struct ibv_send_wr second=wr; second.wr_id=4; wr.wr_id=3; wr.next=&second;
    assert(user_post_send(&mc,&qp,&wr,&bad)==0 && qp.sends.producer==4 && mc.bf_bank==1);
    assert(!memcmp(uar+MCDMA_UAR_DOORBELL,queue+MCDMA_SQ_OFFSET+3*64,8));
    for (unsigned i=0;i<sizeof(uar);++i) if (i<MCDMA_UAR_DOORBELL || i>=MCDMA_UAR_DOORBELL+8) assert(uar[i]==0);
    wr.next=NULL;
    // The 128-byte kernel pattern: WQEBB plus zero padding, scalar stores.
    memset(uar,0x77,sizeof(uar)); mc.bf_mode=MCDMA_BF_128; mc.bf_bytes=128; mc.bf_vector=0; wr.wr_id=5;
    assert(user_post_send(&mc,&qp,&wr,&bad)==0 && qp.sends.producer==5 && mc.bf_bank==0);
    assert(!memcmp(uar+MCDMA_UAR_DOORBELL+256,queue+MCDMA_SQ_OFFSET+4*64,64));
    for (unsigned i=64;i<128;++i) assert(uar[MCDMA_UAR_DOORBELL+256+i]==0);
    for (unsigned i=0;i<sizeof(uar);++i) if (i<MCDMA_UAR_DOORBELL+256 || i>=MCDMA_UAR_DOORBELL+256+128) assert(uar[i]==0x77);
    // An odd number of pushes followed by a list must ring bank 1, then
    // advance once for that doorbell, just as for a full BlueFlame push.
    memset(uar,0,sizeof(uar)); mc.bf_bank=1; wr.next=&second;
    assert(user_post_send(&mc,&qp,&wr,&bad)==0 && qp.sends.producer==7 && mc.bf_bank==0);
    assert(!memcmp(uar+MCDMA_UAR_DOORBELL+256,queue+MCDMA_SQ_OFFSET+6*64,8));
    for (unsigned i=0;i<sizeof(uar);++i) if (i<MCDMA_UAR_DOORBELL+256 || i>=MCDMA_UAR_DOORBELL+256+8) assert(uar[i]==0);
    wr.next=NULL;
    // A write-combined page without BlueFlame (doorbell arm, or no bank).
    memset(uar,0,sizeof(uar)); qp.blueflame=0; mc.bf_bank=0; wr.wr_id=6;
    assert(user_post_send(&mc,&qp,&wr,&bad)==0 && qp.sends.producer==8 && mc.bf_bank==0);
    assert(!memcmp(uar+MCDMA_UAR_DOORBELL,queue+MCDMA_SQ_OFFSET+7*64,8));
    for (unsigned i=0;i<sizeof(uar);++i) if (i<MCDMA_UAR_DOORBELL || i>=MCDMA_UAR_DOORBELL+8) assert(uar[i]==0);
    // The capability block decides the QP's push eligibility exactly as
    // create_qp evaluates it: a bank only counts on a write-combined page.
    uint32_t flags=0,bank=0;
    assert(!mcdma_info_read(queue,&flags,&bank));
    mcdma_info_write(queue,MCDMA_INFO_KERNEL_BLUEFLAME,256);
    assert(mcdma_info_read(queue,&flags,&bank) && !(flags&MCDMA_INFO_UAR_WRITE_COMBINED) && bank==0);
    mcdma_info_write(queue,MCDMA_INFO_UAR_WRITE_COMBINED|MCDMA_INFO_KERNEL_BLUEFLAME,256);
    assert(mcdma_info_read(queue,&flags,&bank) && (flags&MCDMA_INFO_UAR_WRITE_COMBINED) && bank==256);
}

int main(int argc,char **argv) {
    if (argc==1 || !strcmp(argv[1],"blueflame")) blueflame_arms();
    if (argc==1 || !strcmp(argv[1],"post")) retired_post();
    if (argc==1 || !strcmp(argv[1],"poll")) retired_poll();
    if (argc==1 || !strcmp(argv[1],"mismatch")) fresh_mismatch();
    if (argc==1 || !strcmp(argv[1],"destroy")) destroy_retries();
    puts("PASS real userspace provider rejects retired-CQ posting and never exposes raw completion counters");
    return 0;
}
