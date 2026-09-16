/* Original lifecycle torture client for the native Apple verbs provider.
 *
 * Walks one RC endpoint through open -> pd -> mr -> cq -> qp -> init -> rtr
 * -> rts -> posted -> polled -> mapped, announcing "STATE <name>" on stdout
 * as each state is reached, and then leaves the process in the requested way
 * at the requested state.  The point is to abandon kernel objects in every
 * possible lifecycle position so a driver fix for stale provider callbacks
 * can be proven: the driver script kills this process thousands of times and
 * checks that the device stays healthy.
 *
 * Handshake (line oriented, same shape as peer/verbs_peer.c):
 *   out: PID <pid>
 *   out: CONFIG ...
 *   out: STATE open|pd|mr|cq|qp|init
 *   out: ENDPOINT qpn psn rkey address 16384 gid
 *   in:  <remote_qpn> <remote_psn> <remote_gid>
 *   out: STATE rtr, STATE rts, READY
 *   in:  INITIATE <rkey> <address> <length> <seed>
 *   out: STATE posted, WRITE status=<wc status> bytes=<n>, STATE polled
 *   out: MAPPED ..., STATE mapped
 *   out: TEARDOWN ... (clean mode only)
 *
 * Build on the Mac from the repository root (tools/build.py native does this):
 *   clang -std=c11 -O2 -Wall -Wextra -Werror -isysroot "$(xcrun --sdk macosx --show-sdk-path)" \
 *       client/lifecycle_client.c -lrdma -o build/lifecycle-client
 */
#ifndef __APPLE__
#error "lifecycle_client.c targets the Apple RDMA userspace library only"
#endif
#define _DARWIN_C_SOURCE 1
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include "../include/cx5_cq_observer.h"
#include "../include/cx5_user_post.h"
extern void *darwin_mmap(void *,size_t,int,int,int,int64_t);
extern int darwin_munmap(int,void *);

static const char *const STATES[]={"open","pd","mr","cq","qp","init","rtr","rts","posted","polled","mapped"};
enum { STATE_COUNT=sizeof(STATES)/sizeof(STATES[0]) };
enum exit_mode { MODE_KILL, MODE_ABORT, MODE_EXIT_NO_TEARDOWN, MODE_CLEAN, MODE_HANG };
static const char *const MODES[]={"kill","abort","exit-no-teardown","clean","hang"};
enum { MODE_COUNT=sizeof(MODES)/sizeof(MODES[0]) };

static int stop_state=-1;
static enum exit_mode mode=MODE_CLEAN;
static unsigned payload_bytes=4096;
static enum ibv_mtu path_mtu=IBV_MTU_1024;

/* Everything that may exist when the process leaves; teardown walks it. */
static struct {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    unsigned char *memory;
    void *cq_page,*queue_page,*uar_page;
} live;

static void fail(const char *stage,int error) __attribute__((noreturn));
static void fail(const char *stage,int error) {
    printf("FAIL stage=%s errno=%d text=%s\n",stage,error,strerror(error));
    fflush(stdout);
    /* Failed setup exits without teardown on purpose: the driver records the
     * stage, and a partial object set is itself a lifecycle case worth leaving. */
    _exit(2);
}

/* Announces a state.  Returns 1 when the caller must stop walking and tear
 * down (clean mode at the requested state); performs the abrupt exits itself. */
static int reached(int state) {
    printf("STATE %s\n",STATES[state]); fflush(stdout);
    if (state!=stop_state) return 0;
    switch (mode) {
    case MODE_KILL: raise(SIGKILL); _exit(99);
    case MODE_ABORT: abort();
    case MODE_EXIT_NO_TEARDOWN: _exit(0);
    case MODE_HANG:
        printf("HANG pid=%ld\n",(long)getpid()); fflush(stdout);
        for (;;) pause();
    case MODE_CLEAN: return 1;
    }
    return 1;
}

static int read_line(char *buffer,size_t size) {
    if (!fgets(buffer,(int)size,stdin)) return 0;
    buffer[strcspn(buffer,"\r\n")]=0;
    return 1;
}

static int mapping_supported(struct ibv_context *ctx) {
    Dl_info origin={0};
    if (!ctx || !dladdr((const void *)ctx->ops.poll_cq,&origin) || !origin.dli_fname) return 0;
    void *library=dlopen(origin.dli_fname,RTLD_NOW|RTLD_NOLOAD);
    if (!library) return 0;
    int (*supports)(struct ibv_context *)=dlsym(library,"mcdma_context_supports_cq_mapping");
    const int ok=supports && supports(ctx);
    dlclose(library);
    return ok;
}

/* Maps a provider page and reports 1 on success or the negative errno.  The
 * harness never writes through these mappings; it only holds them at exit. */
static int hold_page(struct ibv_context *ctx,uint64_t page_offset,int prot,void **out) {
    void *p=darwin_mmap(NULL,MCDMA_QUEUE_PAGE_BYTES,prot,MAP_SHARED,ctx->cmd_fd,(int64_t)page_offset);
    if (p==MAP_FAILED) return -errno;
    *out=p; return 1;
}

static int poll_write(uint64_t wr_id,unsigned *status_out) {
    struct timespec start,now,pause_ns={.tv_sec=0,.tv_nsec=100000};
    if (clock_gettime(CLOCK_MONOTONIC,&start)) return 0;
    for (;;) {
        struct ibv_wc wc={0}; const int n=ibv_poll_cq(live.cq,1,&wc);
        if (n<0) { *status_out=0xffffu; return 0; }
        if (n==1) {
            *status_out=wc.status;
            return wc.status==IBV_WC_SUCCESS && wc.wr_id==wr_id && wc.opcode==IBV_WC_RDMA_WRITE;
        }
        if (clock_gettime(CLOCK_MONOTONIC,&now)) return 0;
        if (now.tv_sec-start.tv_sec>=5) { *status_out=0xfffeu; return 0; }
        nanosleep(&pause_ns,NULL);
    }
}

static int teardown(void) {
    int error=0,unmap=0;
    if (live.qp && ibv_destroy_qp(live.qp)) error|=1; live.qp=NULL;
    if (live.cq && ibv_destroy_cq(live.cq)) error|=2; live.cq=NULL;
    if (live.mr && ibv_dereg_mr(live.mr)) error|=4; live.mr=NULL;
    if (live.pd && ibv_dealloc_pd(live.pd)) error|=8; live.pd=NULL;
    /* Explicit pages are released after their objects, matching the order the
     * mapping acceptance tests use (destroy while mapped, then unmap). */
    if (live.ctx) {
        if (live.cq_page && darwin_munmap(live.ctx->cmd_fd,live.cq_page)) unmap|=1;
        if (live.queue_page && darwin_munmap(live.ctx->cmd_fd,live.queue_page)) unmap|=2;
        if (live.uar_page && darwin_munmap(live.ctx->cmd_fd,live.uar_page)) unmap|=4;
        if (ibv_close_device(live.ctx)) error|=16;
    }
    live.ctx=NULL;
    free(live.memory); live.memory=NULL;
    printf("TEARDOWN error=%d unmap_error=%d\n",error,unmap); fflush(stdout);
    return error || unmap;
}

static void usage(const char *program) {
    fprintf(stderr,"usage: %s RDMA_DEVICE --stop-at STATE --exit-mode MODE [--gid-index N] [--pattern-seed N]\n"
            "  STATE: open pd mr cq qp init rtr rts posted polled mapped\n"
            "  MODE:  kill abort exit-no-teardown clean hang\n",program);
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    const char *device_name=NULL;
    int gid_index=0;
    unsigned pattern_seed=0;
    int have_mode=0;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--stop-at") && i+1<argc) {
            const char *name=argv[++i];
            for (int s=0;s<STATE_COUNT;++s) if (!strcmp(name,STATES[s])) stop_state=s;
            if (stop_state<0) { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i],"--exit-mode") && i+1<argc) {
            const char *name=argv[++i];
            int found=-1;
            for (int m=0;m<MODE_COUNT;++m) if (!strcmp(name,MODES[m])) found=m;
            if (found<0) { usage(argv[0]); return 2; }
            mode=(enum exit_mode)found; have_mode=1;
        } else if (!strcmp(argv[i],"--gid-index") && i+1<argc) {
            gid_index=atoi(argv[++i]);
            if (gid_index<0 || gid_index>255) { usage(argv[0]); return 2; }
        } else if (!strcmp(argv[i],"--pattern-seed") && i+1<argc) {
            pattern_seed=(unsigned)strtoul(argv[++i],NULL,10);
        } else if (argv[i][0]!='-' && !device_name) device_name=argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!device_name || stop_state<0 || !have_mode) { usage(argv[0]); return 2; }
    const char *bytes=getenv("MCDMA_PAYLOAD_BYTES");
    if (bytes && strcmp(bytes,"1024") && strcmp(bytes,"4096")) { fputs("Payload must be 1024 or 4096 bytes\n",stderr); return 2; }
    if (bytes) payload_bytes=(unsigned)strtoul(bytes,NULL,10);
    const char *mtu=getenv("MCDMA_PATH_MTU");
    if (mtu && strcmp(mtu,"1024") && strcmp(mtu,"4096")) { fputs("Path MTU must be 1024 or 4096 bytes\n",stderr); return 2; }
    if (mtu && !strcmp(mtu,"4096")) path_mtu=IBV_MTU_4096;
    /* Abort cycles must not fill the disk with core images of mapped device pages. */
    struct rlimit core={0,0};
    (void)setrlimit(RLIMIT_CORE,&core);
    const char *cq_map=getenv("MCDMA_CQ_MAP"),*user_post=getenv("MCDMA_USER_POST"),*user_bf=getenv("MCDMA_USER_BF");
    printf("PID %ld\n",(long)getpid());
    printf("CONFIG stop_at=%s exit_mode=%s cq_map=%s user_post=%s user_bf=%s payload_bytes=%u path_mtu=%u seed=%u\n",
           STATES[stop_state],MODES[mode],cq_map?cq_map:"unset",user_post?user_post:"unset",
           user_bf?user_bf:"unset",payload_bytes,128u<<path_mtu,pattern_seed);
    fflush(stdout);

    int count=0; struct ibv_device **list=ibv_get_device_list(&count);
    if (!list) fail("device_list",errno);
    struct ibv_device *device=NULL;
    for (int i=0;i<count;++i) if (!strcmp(ibv_get_device_name(list[i]),device_name)) device=list[i];
    if (!device) { ibv_free_device_list(list); fail("device_lookup",ENODEV); }
    live.ctx=ibv_open_device(device);
    ibv_free_device_list(list);
    if (!live.ctx) fail("open_device",errno);
    if (reached(0)) goto clean;

    live.pd=ibv_alloc_pd(live.ctx);
    if (!live.pd) fail("alloc_pd",errno);
    if (reached(1)) goto clean;

    if (posix_memalign((void **)&live.memory,16384,16384)) fail("memory",ENOMEM);
    memset(live.memory,0,16384);
    live.mr=ibv_reg_mr(live.pd,live.memory,16384,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE);
    if (!live.mr) fail("reg_mr",errno);
    if (reached(2)) goto clean;

    live.cq=ibv_create_cq(live.ctx,31,NULL,NULL,0);
    if (!live.cq) fail("create_cq",errno);
    if (reached(3)) goto clean;

    {
        struct ibv_qp_init_attr init={0};
        init.send_cq=live.cq; init.recv_cq=live.cq; init.qp_type=IBV_QPT_RC;
        init.cap.max_send_wr=31; init.cap.max_recv_wr=31; init.cap.max_send_sge=1; init.cap.max_recv_sge=1;
        live.qp=ibv_create_qp(live.pd,&init);
        if (!live.qp) fail("create_qp",errno);
    }
    if (reached(4)) goto clean;

    struct ibv_qp_attr attr;
    memset(&attr,0,sizeof(attr));
    attr.qp_state=IBV_QPS_INIT; attr.port_num=1;
    attr.qp_access_flags=IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE;
    {
        const int error=ibv_modify_qp(live.qp,&attr,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS);
        if (error) fail("modify_init",error>0?error:errno);
    }
    if (reached(5)) goto clean;

    union ibv_gid gid;
    if (ibv_query_gid(live.ctx,1,gid_index,&gid)) fail("query_gid",errno);
    char gid_text[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6,&gid,gid_text,sizeof(gid_text));
    const unsigned local_psn=(0x200000u+pattern_seed)&0xffffffu;
    printf("ENDPOINT %u %u %u %llu 16384 %s\n",live.qp->qp_num,local_psn,live.mr->rkey,
           (unsigned long long)(uintptr_t)live.memory,gid_text);
    fflush(stdout);

    char line[256];
    unsigned remote_qpn,remote_psn; char remote_gid[80];
    if (!read_line(line,sizeof(line)) || sscanf(line,"%u %u %79s",&remote_qpn,&remote_psn,remote_gid)!=3 ||
        remote_qpn>0xffffff || remote_psn>0xffffff) fail("peer_descriptor",EPROTO);
    memset(&attr,0,sizeof(attr));
    attr.qp_state=IBV_QPS_RTR; attr.path_mtu=path_mtu; attr.dest_qp_num=remote_qpn; attr.rq_psn=remote_psn;
    attr.max_dest_rd_atomic=1; attr.min_rnr_timer=12;
    attr.ah_attr.is_global=1; attr.ah_attr.port_num=1; attr.ah_attr.grh.sgid_index=(uint8_t)gid_index; attr.ah_attr.grh.hop_limit=64;
    if (inet_pton(AF_INET6,remote_gid,&attr.ah_attr.grh.dgid)!=1) fail("peer_gid",EINVAL);
    {
        const int error=ibv_modify_qp(live.qp,&attr,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|
                                      IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);
        if (error) fail("modify_rtr",error>0?error:errno);
    }
    if (reached(6)) goto clean;

    memset(&attr,0,sizeof(attr));
    attr.qp_state=IBV_QPS_RTS; attr.timeout=14; attr.retry_cnt=7; attr.rnr_retry=7; attr.sq_psn=local_psn; attr.max_rd_atomic=1;
    {
        const int error=ibv_modify_qp(live.qp,&attr,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC);
        if (error) fail("modify_rts",error>0?error:errno);
    }
    if (reached(7)) goto clean;
    puts("READY"); fflush(stdout);

    unsigned remote_key,remote_length,seed; unsigned long long remote_address; char verb[16];
    if (!read_line(line,sizeof(line)) || sscanf(line,"%15s %u %llu %u %u",verb,&remote_key,&remote_address,&remote_length,&seed)!=5 ||
        strcmp(verb,"INITIATE") || !remote_key || remote_length<payload_bytes || remote_address>UINT64_MAX-remote_length)
        fail("initiate",EPROTO);
    for (unsigned i=0;i<payload_bytes;++i) live.memory[i]=(unsigned char)(i*37+19+seed);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    struct ibv_sge sge={.addr=(uintptr_t)live.memory,.length=payload_bytes,.lkey=live.mr->lkey};
    struct ibv_send_wr wr={0},*bad=NULL;
    wr.wr_id=0x4c494645u; wr.sg_list=&sge; wr.num_sge=1; wr.opcode=IBV_WR_RDMA_WRITE; wr.send_flags=IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr=remote_address; wr.wr.rdma.rkey=remote_key;
    {
        const int error=ibv_post_send(live.qp,&wr,&bad);
        if (error) fail("post_send",error>0?error:errno);
    }
    if (reached(8)) goto clean;

    unsigned status=0;
    const int completed=poll_write(wr.wr_id,&status);
    printf("WRITE status=%u bytes=%u\n",status,completed?payload_bytes:0); fflush(stdout);
    if (!completed) fail("poll_cq",EIO);
    if (reached(9)) goto clean;

    /* Hold extra process-side mappings of kernel-owned pages.  When the
     * provider already holds one (an enabled knob), the duplicate request is
     * expected to be refused and the knob-held mapping counts instead. */
    {
        const int supported=mapping_supported(live.ctx);
        int cq_result=0,queue_result=0,uar_result=0;
        if (supported) {
            cq_result=hold_page(live.ctx,((uint64_t)live.cq->handle+1)*MCDMA_CQ_MAP_BYTES,PROT_READ,&live.cq_page);
            queue_result=hold_page(live.ctx,(MCDMA_QUEUE_PAGE_BASE+live.qp->qp_num)*MCDMA_QUEUE_PAGE_BYTES,PROT_READ|PROT_WRITE,&live.queue_page);
            uar_result=hold_page(live.ctx,MCDMA_UAR_PAGE_NUMBER*MCDMA_QUEUE_PAGE_BYTES,PROT_READ|PROT_WRITE,&live.uar_page);
        }
        const int knob_held=(cq_map && strcmp(cq_map,"0")) || (user_post && !strcmp(user_post,"1"));
        const int held=cq_result==1 || queue_result==1 || uar_result==1 || knob_held;
        printf("MAPPED supported=%d cq=%d queue=%d uar=%d knob_held=%d held=%d\n",
               supported,cq_result,queue_result,uar_result,knob_held,held); fflush(stdout);
        if (!held) { printf("FAIL stage=map errno=%d text=no mapping held\n",EOPNOTSUPP); fflush(stdout); _exit(4); }
    }
    if (reached(10)) goto clean;
    /* Every state was walked without a stop: only clean mode gets here. */
clean:
    return teardown() ? 3 : 0;
}
