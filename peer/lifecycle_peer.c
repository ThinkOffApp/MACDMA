/* Original Linux libibverbs responder for the lifecycle torture harness.
 *
 * Stays up for a whole campaign.  For each cycle it creates a fresh RC QP,
 * hands its descriptor to the driver, connects to the Mac endpoint the
 * driver relays, and after the Mac client has been killed (or has exited
 * cleanly) it verifies that this side is still healthy: the QP state can be
 * queried, the QP is destroyed without error, and new CQ/QP/MR objects can
 * still be created and destroyed on the long-lived context.
 *
 * Commands on stdin, replies on stdout:
 *   CYCLE <n>                       -> ENDPOINT qpn psn rkey address 16384 gid
 *   CONNECT <qpn> <psn> <gid>       -> READY
 *   VERIFY <seed>                   -> PEER_VERIFY bytes=<n>
 *   FINISH                          -> PEER_HEALTH cycle=.. qp_state=.. ok=..
 *   QUIT                            -> PEER_DONE cleanup=<n>
 *
 * Build on the peer from the repository root:
 *   cc -std=c11 -O2 -Wall -Wextra -Werror peer/lifecycle_peer.c -libverbs -o build/lifecycle-peer
 */
#define _POSIX_C_SOURCE 200809L
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned payload_bytes=4096;
static enum ibv_mtu path_mtu=IBV_MTU_1024;
static int gid_index;

static struct {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    unsigned char *memory;
    unsigned cycle;
    int connected;
} peer;

static void fail(const char *where) {
    fprintf(stderr,"%s: %s\n",where,strerror(errno));
    exit(2);
}

static int modify(struct ibv_qp *qp,struct ibv_qp_attr *attr,int mask,const char *stage) {
    const int result=ibv_modify_qp(qp,attr,mask);
    if (result) fprintf(stderr,"%s: modify_qp result=%d (%s)\n",stage,result,strerror(result>0?result:errno));
    return result;
}

static struct ibv_qp *create_init_qp(struct ibv_cq *cq,struct ibv_pd *pd) {
    struct ibv_qp_init_attr init={0};
    init.send_cq=cq; init.recv_cq=cq; init.qp_type=IBV_QPT_RC;
    init.cap.max_send_wr=31; init.cap.max_recv_wr=31; init.cap.max_send_sge=1; init.cap.max_recv_sge=1;
    struct ibv_qp *qp=ibv_create_qp(pd,&init);
    if (!qp) return NULL;
    struct ibv_qp_attr attr; memset(&attr,0,sizeof(attr));
    attr.qp_state=IBV_QPS_INIT; attr.port_num=1;
    attr.qp_access_flags=IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE;
    if (modify(qp,&attr,IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS,"INIT")) {
        ibv_destroy_qp(qp); return NULL;
    }
    return qp;
}

static int release_all(void) {
    int error=0;
    if (peer.qp && ibv_destroy_qp(peer.qp)) error|=1;
    peer.qp=NULL;
    if (peer.cq && ibv_destroy_cq(peer.cq)) error|=2;
    peer.cq=NULL;
    if (peer.mr && ibv_dereg_mr(peer.mr)) error|=4;
    peer.mr=NULL;
    if (!(error&4)) { free(peer.memory); peer.memory=NULL; }
    if (peer.pd && ibv_dealloc_pd(peer.pd)) error|=8;
    peer.pd=NULL;
    if (peer.ctx && ibv_close_device(peer.ctx)) error|=16;
    peer.ctx=NULL;
    return error;
}

/* Proves the context still hands out and takes back every object type. */
static void probe_resources(int *cq_ok,int *qp_ok,int *mr_ok) {
    *cq_ok=*qp_ok=*mr_ok=0;
    struct ibv_cq *cq=ibv_create_cq(peer.ctx,7,NULL,NULL,0);
    if (cq) {
        struct ibv_qp *qp=create_init_qp(cq,peer.pd);
        if (qp) *qp_ok=!ibv_destroy_qp(qp);
        *cq_ok=!ibv_destroy_cq(cq);
    }
    unsigned char *page=NULL;
    if (!posix_memalign((void **)&page,4096,4096)) {
        struct ibv_mr *mr=ibv_reg_mr(peer.pd,page,4096,IBV_ACCESS_LOCAL_WRITE);
        if (mr) *mr_ok=!ibv_dereg_mr(mr);
        free(page);
    }
}

static void begin_cycle(unsigned cycle) {
    if (peer.qp) { puts("PEER_ERROR cycle_already_open"); fflush(stdout); exit(2); }
    peer.qp=create_init_qp(peer.cq,peer.pd);
    if (!peer.qp) { printf("PEER_ERROR create_qp errno=%d\n",errno); fflush(stdout); exit(2); }
    peer.cycle=cycle; peer.connected=0;
    memset(peer.memory,0,16384);
    char gid_text[INET6_ADDRSTRLEN]; union ibv_gid gid;
    if (ibv_query_gid(peer.ctx,1,gid_index,&gid)) fail("query gid");
    inet_ntop(AF_INET6,&gid,gid_text,sizeof(gid_text));
    printf("ENDPOINT %u %u %u %llu 16384 %s\n",peer.qp->qp_num,(0x100000u+cycle)&0xffffffu,peer.mr->rkey,
           (unsigned long long)(uintptr_t)peer.memory,gid_text);
    fflush(stdout);
}

static void connect_cycle(const char *arguments) {
    unsigned remote_qpn,remote_psn; char remote_gid[80];
    if (!peer.qp || peer.connected) { puts("PEER_ERROR connect_without_cycle"); fflush(stdout); exit(2); }
    if (sscanf(arguments,"%u %u %79s",&remote_qpn,&remote_psn,remote_gid)!=3 || remote_qpn>0xffffff || remote_psn>0xffffff) {
        puts("PEER_ERROR bad_connect"); fflush(stdout); exit(2);
    }
    struct ibv_qp_attr attr; memset(&attr,0,sizeof(attr));
    attr.qp_state=IBV_QPS_RTR; attr.path_mtu=path_mtu; attr.dest_qp_num=remote_qpn; attr.rq_psn=remote_psn;
    attr.max_dest_rd_atomic=1; attr.min_rnr_timer=12;
    attr.ah_attr.is_global=1; attr.ah_attr.port_num=1; attr.ah_attr.grh.sgid_index=(uint8_t)gid_index; attr.ah_attr.grh.hop_limit=64;
    if (inet_pton(AF_INET6,remote_gid,&attr.ah_attr.grh.dgid)!=1) { puts("PEER_ERROR bad_gid"); fflush(stdout); exit(2); }
    if (modify(peer.qp,&attr,IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|
               IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER,"RTR")) { puts("PEER_ERROR rtr"); fflush(stdout); exit(2); }
    memset(&attr,0,sizeof(attr));
    attr.qp_state=IBV_QPS_RTS; attr.timeout=14; attr.retry_cnt=7; attr.rnr_retry=7;
    attr.sq_psn=(0x100000u+peer.cycle)&0xffffffu; attr.max_rd_atomic=1;
    if (modify(peer.qp,&attr,IBV_QP_STATE|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_SQ_PSN|
               IBV_QP_MAX_QP_RD_ATOMIC,"RTS")) { puts("PEER_ERROR rts"); fflush(stdout); exit(2); }
    peer.connected=1;
    puts("READY"); fflush(stdout);
}

static void verify(const char *arguments) {
    unsigned seed=0;
    if (sscanf(arguments,"%u",&seed)!=1) { puts("PEER_ERROR bad_verify"); fflush(stdout); exit(2); }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    int good=1;
    for (unsigned i=0;i<payload_bytes;++i) if (peer.memory[i]!=(unsigned char)(i*37+19+seed)) good=0;
    printf("PEER_VERIFY bytes=%u\n",good?payload_bytes:0); fflush(stdout);
}

static void finish_cycle(void) {
    int qp_state=-1,cur_state=-1,query=-1,destroy=-1;
    if (peer.qp) {
        struct ibv_qp_attr attr; struct ibv_qp_init_attr init;
        memset(&attr,0,sizeof(attr)); memset(&init,0,sizeof(init));
        query=ibv_query_qp(peer.qp,&attr,IBV_QP_STATE|IBV_QP_CUR_STATE,&init);
        if (!query) { qp_state=attr.qp_state; cur_state=attr.cur_qp_state; }
        destroy=ibv_destroy_qp(peer.qp);
        peer.qp=NULL;
    }
    int cq_ok,qp_ok,mr_ok;
    probe_resources(&cq_ok,&qp_ok,&mr_ok);
    memset(peer.memory,0,16384);
    const int ok=query==0 && destroy==0 && cq_ok && qp_ok && mr_ok;
    printf("PEER_HEALTH cycle=%u connected=%d query=%d qp_state=%d cur_state=%d destroy=%d probe_cq=%d probe_qp=%d probe_mr=%d ok=%d\n",
           peer.cycle,peer.connected,query,qp_state,cur_state,destroy,cq_ok,qp_ok,mr_ok,ok);
    fflush(stdout);
    peer.connected=0;
}

int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if (argc!=3) { fputs("Usage: lifecycle-peer RDMA_DEVICE GID_INDEX\n",stderr); return 2; }
    const char *bytes=getenv("MCDMA_PAYLOAD_BYTES");
    if (bytes && strcmp(bytes,"1024") && strcmp(bytes,"4096")) { fputs("Payload must be 1024 or 4096 bytes\n",stderr); return 2; }
    if (bytes) payload_bytes=(unsigned)strtoul(bytes,NULL,10);
    const char *mtu=getenv("MCDMA_PATH_MTU");
    if (mtu && strcmp(mtu,"1024") && strcmp(mtu,"4096")) { fputs("Path MTU must be 1024 or 4096 bytes\n",stderr); return 2; }
    if (mtu && !strcmp(mtu,"4096")) path_mtu=IBV_MTU_4096;
    gid_index=atoi(argv[2]);
    if (gid_index<0 || gid_index>255) { fputs("Invalid GID index\n",stderr); return 2; }

    int count=0; struct ibv_device **list=ibv_get_device_list(&count);
    if (!list) fail("device list");
    for (int i=0;i<count;++i) if (!strcmp(ibv_get_device_name(list[i]),argv[1])) peer.ctx=ibv_open_device(list[i]);
    ibv_free_device_list(list);
    if (!peer.ctx) fail("open device");
    struct ibv_port_attr port;
    if (ibv_query_port(peer.ctx,1,&port)) fail("query port");
    if (port.state!=IBV_PORT_ACTIVE || port.link_layer!=IBV_LINK_LAYER_ETHERNET) { fputs("Port is not active Ethernet\n",stderr); return 2; }
    if (path_mtu>port.active_mtu || path_mtu>port.max_mtu) {
        fprintf(stderr,"Requested path MTU exceeds port active/max MTU (%u/%u)\n",128u<<port.active_mtu,128u<<port.max_mtu);
        return 2;
    }
    peer.pd=ibv_alloc_pd(peer.ctx); if (!peer.pd) fail("PD");
    if (posix_memalign((void **)&peer.memory,16384,16384)) fail("memory");
    memset(peer.memory,0,16384);
    peer.mr=ibv_reg_mr(peer.pd,peer.memory,16384,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ|IBV_ACCESS_REMOTE_WRITE);
    if (!peer.mr) fail("MR");
    peer.cq=ibv_create_cq(peer.ctx,31,NULL,NULL,0); if (!peer.cq) fail("CQ");
    printf("PEER_UP payload_bytes=%u path_mtu=%u\n",payload_bytes,128u<<path_mtu); fflush(stdout);

    char line[256];
    while (fgets(line,sizeof(line),stdin)) {
        line[strcspn(line,"\r\n")]=0;
        if (!strncmp(line,"CYCLE ",6)) {
            char *end=NULL; const unsigned long cycle=strtoul(line+6,&end,10);
            if (end==line+6 || *end || cycle>0xffffffful) { puts("PEER_ERROR bad_cycle"); fflush(stdout); return 2; }
            begin_cycle((unsigned)cycle);
        } else if (!strncmp(line,"CONNECT ",8)) connect_cycle(line+8);
        else if (!strncmp(line,"VERIFY ",7)) verify(line+7);
        else if (!strcmp(line,"FINISH")) finish_cycle();
        else if (!strcmp(line,"QUIT")) break;
        else { printf("PEER_ERROR unknown_command\n"); fflush(stdout); return 2; }
    }
    const int cleanup=release_all();
    printf("PEER_DONE cleanup=%d\n",cleanup); fflush(stdout);
    return cleanup ? 2 : 0;
}
