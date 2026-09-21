/* Run the real trial state machines with shared memory in place of RDMA.
 * The Python test relays control messages between two independent processes.
 * This tests protocol and file correctness, not hardware ordering or timing. */
#include <infiniband/verbs.h>
#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
static struct ibv_wc pending[128];
static unsigned head, tail;
static const char *fault;
static unsigned char *destination_guard;
static int copied;

static int copy_post(struct ibv_qp *qp, struct ibv_send_wr *wr, struct ibv_send_wr **bad) {
    (void)qp; (void)bad;
    assert(tail < 128);
    void *local = (void *)(uintptr_t)wr->sg_list->addr;
    void *remote = (void *)(uintptr_t)wr->wr.rdma.remote_addr;
    const int read_op = wr->opcode == IBV_WR_RDMA_READ;
    assert(read_op || wr->opcode == IBV_WR_RDMA_WRITE);
    memcpy(read_op ? local : remote, read_op ? remote : local, wr->sg_list->length);
    if (!copied++) {
        if (!strcmp(fault, "corrupt")) ((unsigned char *)(read_op ? local : remote))[0] ^= 1;
        if (!strcmp(fault, "guard")) *destination_guard ^= 1;
    }
    struct ibv_wc *wc = &pending[tail++];
    wc->wr_id = wr->wr_id;
    wc->opcode = read_op ? IBV_WC_RDMA_READ : IBV_WC_RDMA_WRITE;
    wc->status = !strcmp(fault, "completion") ? IBV_WC_GENERAL_ERR : IBV_WC_SUCCESS;
    return 0;
}
static int copy_poll(struct ibv_cq *cq, int count, struct ibv_wc *wc) {
    (void)cq;
    int n = 0;
    while (head < tail && n < count) wc[n++] = pending[head++];
    return n;
}
#define ibv_post_send copy_post
#define ibv_poll_cq copy_poll
#define main benchmark_main
#include "../benchmarks/mcdma_bw.c"
#undef main

int main(int argc, char **argv) {
    assert(argc == 6);
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct bench b = {0};
    b.o.initiator = !strcmp(argv[1], "initiator");
    b.o.op = !strcmp(argv[2], "read") ? OP_READ : OP_WRITE;
    b.o.qps = b.cqs = 1;
    b.o.bytes = 4096;
    b.o.depth = b.depth = b.depth_effective = 4;
    b.o.timeout_s = 3;
    b.o.verify_bytes = 16384;
    b.cq_cap[0] = 32;
    b.region_bytes = 4 * 4096 + GUARD_BYTES + FLAG_BYTES;
    int fd = open(argv[3], O_RDWR); assert(fd >= 0);
    unsigned char *regions = mmap(NULL, 2 * b.region_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(regions != MAP_FAILED); close(fd);
    b.region = regions + (b.o.initiator ? 0 : b.region_bytes);
    b.remote.addr = (uintptr_t)(regions + (b.o.initiator ? b.region_bytes : 0));
    b.remote.length = b.region_bytes;
    const int source = b.o.op == OP_READ ? !b.o.initiator : b.o.initiator;
    if (source) b.o.payload_path = argv[4]; else b.o.dump_path = argv[4];
    fault = argv[5];
    destination_guard = (b.o.op == OP_READ ? b.region : (unsigned char *)(uintptr_t)b.remote.addr) + 16384;
    struct ibv_mr mr = {0}; b.mr = &mr;
    b.nonce = 123;
    build_windows(&b);
    prepare_payload(&b);
    const int ok = b.o.initiator ? run_initiator_trial(&b, 0, 0) : run_responder_trial(&b, 0, 0);
    free(b.windows);
    assert(!munmap(regions, 2 * b.region_bytes));
    return ok ? 0 : 3;
}
