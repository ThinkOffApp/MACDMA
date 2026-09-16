// Original macOS provider using Apple's exported command helpers and public
// verbs.h. No interposition, fake device enumeration or private provider code.
// The ABI-34 offsets below were checked against the owner's Apple binaries;
// the upstream rdma-core API declarations served as a signature reference.
#include <infiniband/verbs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/sysctl.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../include/cx5_cq_observer.h"
#include "../include/cx5_user_completion.h"
#include "../include/cx5_user_post.h"
#include "../include/apple_build_ids.h"

#define MCDMA_DRIVER_ID 0x4d434435u
#define MCDMA_ABI 2u
// CQ mapping modes selected by MCDMA_CQ_MAP at context creation:
//   0  kernel polling only, no mapping;
//   1  read-only observation: userspace skips empty kernel polls (default);
//   2  observation plus userspace completion reporting from the mirrored work
//      records, with every reported completion verified against the kernel's
//      own consumption of the same ring entry.
#define MCDMA_MODE_OFF 0
#define MCDMA_MODE_OBSERVE 1
#define MCDMA_MODE_CONSUME 2
// Kernel catch-up cadence for mode 2: acknowledge reported completions at the
// next post once this many are outstanding, and inside poll as a safety valve.
#define MCDMA_ACK_BATCH 8u
#define MCDMA_ACK_SAFETY 24u
#define MCDMA_OBJECTS 256u
struct mcdma_vmr { struct ibv_mr mr; int type,access; };
struct mcdma_cq {
    struct ibv_cq cq;
    void *observation;
    pthread_mutex_t poll_lock;
    unsigned empty_polls;
    // Mode 2 state, guarded by the owning context lock.
    int consume,retired,kernel_destroyed;
    unsigned ahead;               // Reported to the application, not yet consumed by the kernel.
    struct mcdma_user_completion pending[MCDMA_USER_SLOTS];
    unsigned pending_head,pending_count;
};
struct mcdma_qp {
    struct ibv_qp qp;
    struct mcdma_user_queue sends,receives;
    // User-posted QP: its own work-queue page is mapped writable and the
    // kernel refuses to post to it; the mirror producer is the real producer.
    uint8_t *queue; int user_posted,kernel_destroyed;
    // Single-WQE posts go through a BlueFlame bank of the context's
    // write-combined UAR page when the kernel published a bank size for it.
    int blueflame;
};
// Userspace BlueFlame arms selected by MCDMA_USER_BF (requires MCDMA_USER_POST=1):
//   0    unset: ordinary device-memory UAR page and 8-byte doorbell (0.1.15 behaviour);
//   64   write-combined UAR page, one 64-byte WQEBB per post as a NEON burst;
//   128  write-combined UAR page, the WQEBB plus 64 zero bytes as scalar
//        stores, the kernel path's exact pattern;
//   64s  write-combined UAR page, 64 bytes as scalar stores;
//   db   write-combined UAR page, ordinary 8-byte doorbell (mapping effect only).
#define MCDMA_BF_OFF 0
#define MCDMA_BF_64 1
#define MCDMA_BF_128 2
#define MCDMA_BF_64S 3
#define MCDMA_BF_DOORBELL 4
// Apple allocates the verbs context at offset zero of our request; the
// remainder is provider-private state for the mirrored queues of mode 2.
struct mcdma_context {
    struct verbs_context vctx;
    int cq_mode;
    // MCDMA_USER_POST=1: the context's UAR page is mapped writable and every
    // QP created afterwards is posted from userspace with direct doorbells.
    int user_post; volatile uint8_t *uar;
    // MCDMA_USER_BF: the UAR page was mapped write-combined (uar_wc) and
    // BlueFlame posts alternate between the two banks of its register.
    int bf_mode,uar_wc,bf_vector; unsigned bf_bytes,bf_bank_bytes,bf_bank;
    pthread_mutex_t lock;
    struct mcdma_qp *qps[MCDMA_OBJECTS];
    struct mcdma_cq *cqs[MCDMA_OBJECTS];
};
// Apple's discovery fast path walks 24-byte entries before match_device;
// a null table is not accepted when a kernel interface has a driver ID.
struct mcdma_match {
    void *data;
    uint64_t driver_id;
    uint16_t vendor,device;
    uint8_t kind;
};
_Static_assert(sizeof(struct mcdma_match)==0x18,"Apple match stride changed");
_Static_assert(offsetof(struct mcdma_match,kind)==0x14,"Apple match kind changed");
static const struct mcdma_match device_matches[]={
    {.driver_id=MCDMA_DRIVER_ID,.kind=3},
    {0}
};
struct mcdma_device_ops {
    const char *name;
    uint32_t minimum,maximum;
    const void *match_table,*static_providers;
    bool (*match_device)(const void *);
    struct verbs_context *(*alloc_context)(struct ibv_device *,int,void *);
    void *import_context;
    void *(*alloc_device)(const void *);
    void (*uninit_device)(void *);
};
struct mcdma_device {
    struct ibv_device device;
    const struct mcdma_device_ops *ops;
    int references;
    void *entry[2];
    void *sysfs;
    uint64_t support;
};
_Static_assert(offsetof(struct mcdma_device,ops)==0x298,"Apple device ABI changed");
_Static_assert(offsetof(struct mcdma_device,sysfs)==0x2b8,"Apple device ABI changed");
_Static_assert(offsetof(struct mcdma_device_ops,alloc_context)==0x28,"Apple provider ABI changed");
_Static_assert(offsetof(struct mcdma_device_ops,alloc_device)==0x38,"Apple provider ABI changed");
_Static_assert(offsetof(struct verbs_context,context)==0x140,"Apple context ABI changed");
_Static_assert(sizeof(struct verbs_context)==0x2a0,"Apple context size changed");
_Static_assert(offsetof(struct mcdma_context,vctx)==0,"Provider context must start with the verbs context");
_Static_assert(offsetof(struct mcdma_vmr,type)==0x30,"Apple MR ABI changed");

extern void verbs_register_driver_34(const struct mcdma_device_ops *);
extern void *_verbs_init_and_alloc_context(struct ibv_device *,int,size_t,void *,uint32_t);
extern void verbs_uninit_context(struct verbs_context *);
extern void verbs_set_ops(struct verbs_context *,const void *);
extern int ibv_cmd_get_context(struct verbs_context *,void *,size_t,void *,size_t);
extern int ibv_cmd_query_device_any(struct ibv_context *,const struct ibv_query_device_ex_input *,struct ibv_device_attr_ex *,size_t,void *,size_t *);
extern int ibv_cmd_query_port(struct ibv_context *,uint8_t,struct ibv_port_attr *,void *,size_t);
extern int ibv_cmd_alloc_pd(struct ibv_context *,struct ibv_pd *,void *,size_t,void *,size_t);
extern int ibv_cmd_dealloc_pd(struct ibv_pd *);
extern int ibv_cmd_create_cq(struct ibv_context *,int,struct ibv_comp_channel *,int,struct ibv_cq *,void *,size_t,void *,size_t);
extern int ibv_cmd_destroy_cq(struct ibv_cq *);
extern int ibv_cmd_create_qp(struct ibv_pd *,struct ibv_qp *,struct ibv_qp_init_attr *,void *,size_t,void *,size_t);
extern int ibv_cmd_modify_qp(struct ibv_qp *,struct ibv_qp_attr *,int,void *,size_t);
extern int ibv_cmd_destroy_qp(struct ibv_qp *);
extern int ibv_cmd_reg_mr(struct ibv_pd *,void *,size_t,uint64_t,int,struct mcdma_vmr *,void *,size_t,void *,size_t);
extern int ibv_cmd_dereg_mr(struct mcdma_vmr *);
extern int ibv_cmd_poll_cq(struct ibv_cq *,int,struct ibv_wc *);
extern int ibv_cmd_post_send(struct ibv_qp *,struct ibv_send_wr *,struct ibv_send_wr **);
extern int ibv_cmd_post_recv(struct ibv_qp *,struct ibv_recv_wr *,struct ibv_recv_wr **);
// Exported Apple helpers, signatures checked in the gated build's binaries.
extern void *darwin_mmap(void *,size_t,int,int,int,int64_t);
extern int darwin_munmap(int,void *);

static void *context_ops[0x260/8];
int mcdma_provider_abi_check(void);
static int mcdma_post_send(struct ibv_qp *,struct ibv_send_wr *,struct ibv_send_wr **);
static int mcdma_post_recv(struct ibv_qp *,struct ibv_recv_wr *,struct ibv_recv_wr **);
static uint32_t field(const void *object,size_t offset) {
    uint32_t result; memcpy(&result,(const uint8_t *)object+offset,4); return result;
}
static bool supported_build(void) {
    char build[32]={0}; size_t size=sizeof(build);
    return sysctlbyname("kern.osversion",build,&size,NULL,0)==0 &&
        size<=sizeof(build) && mcdma_verified_apple_build(build,size)!=NULL;
}
static bool match_device(const void *sysfs) {
    // Only our own driver ID and command ABI; never claim Apple's MLX5 or TB.
    return sysfs && supported_build() && field(sysfs,0x3b0)==MCDMA_DRIVER_ID && field(sysfs,0x3c0)==MCDMA_ABI;
}
static void *alloc_device(const void *sysfs) {
    if (!match_device(sysfs)) { errno=ENODEV; return NULL; }
    return calloc(1,sizeof(struct mcdma_device));
}
static void uninit_device(void *device) { free(device); }
static struct mcdma_context *context_of(struct ibv_context *ctx) {
    // Only contexts allocated by alloc_context below carry our private tail.
    struct verbs_context *vctx=ctx ? verbs_get_ctx(ctx) : NULL;
    return vctx && ctx->ops.post_send==mcdma_post_send ? (struct mcdma_context *)vctx : NULL;
}
static int cq_mode_from_environment(void) {
    const char *mode=getenv("MCDMA_CQ_MAP");
    if (!mode) return MCDMA_MODE_OBSERVE;
    if (!strcmp(mode,"0")) return MCDMA_MODE_OFF;
    if (!strcmp(mode,"1")) return MCDMA_MODE_OBSERVE;
    if (!strcmp(mode,"2")) return MCDMA_MODE_CONSUME;
    return -1;
}
static int bf_mode_from_environment(void) {
    const char *value=getenv("MCDMA_USER_BF");
    if (!value || !strcmp(value,"0")) return MCDMA_BF_OFF;
    if (!strcmp(value,"64")) return MCDMA_BF_64;
    if (!strcmp(value,"128")) return MCDMA_BF_128;
    if (!strcmp(value,"64s")) return MCDMA_BF_64S;
    if (!strcmp(value,"db")) return MCDMA_BF_DOORBELL;
    return -1;
}
static const char *bf_mode_name(int mode) {
    return mode==MCDMA_BF_64 ? "64" : mode==MCDMA_BF_128 ? "128" : mode==MCDMA_BF_64S ? "64s" :
           mode==MCDMA_BF_DOORBELL ? "db" : "0";
}
static int query_device(struct ibv_context *ctx,const struct ibv_query_device_ex_input *input,struct ibv_device_attr_ex *attr,size_t size) {
    return ibv_cmd_query_device_any(ctx,input,attr,size,NULL,NULL);
}
static int query_port(struct ibv_context *ctx,uint8_t port,struct ibv_port_attr *attr) {
    uint64_t command[3]={0}; return ibv_cmd_query_port(ctx,port,attr,command,sizeof(command));
}
static struct ibv_pd *alloc_pd(struct ibv_context *ctx) {
    struct ibv_pd *pd=calloc(1,sizeof(*pd)); if (!pd) return NULL;
    uint64_t command[2]={0}; uint32_t response=0;
    int error=ibv_cmd_alloc_pd(ctx,pd,command,sizeof(command),&response,sizeof(response));
    if (error) { free(pd); errno=error; return NULL; } return pd;
}
static int dealloc_pd(struct ibv_pd *pd) {
    int error=ibv_cmd_dealloc_pd(pd); if (!error) free(pd); return error;
}

// ---- Mode 2: userspace completion reporting, kernel-verified -------------
static struct mcdma_user_qp_view lookup_qp(void *context,uint32_t qpn) {
    struct mcdma_context *mc=context;
    struct mcdma_user_qp_view view={NULL,NULL,0};
    for (unsigned i=0;i<MCDMA_OBJECTS;++i) {
        struct mcdma_qp *node=mc->qps[i];
        if (node && !node->kernel_destroyed && node->qp.qp_num==qpn) { view.sends=&node->sends; view.receives=&node->receives; view.user_posted=node->user_posted; break; }
    }
    return view;
}
static void retire(struct mcdma_cq *node,const char *reason) {
    // A failed mirror is terminal for this CQ. In user-post mode the kernel
    // returns hardware counters, not application WR IDs, so raw kernel
    // polling cannot safely replace the failed userspace decoder.
    if (!node->retired) fprintf(stderr,"MCDMA_CQ_CONSUME retired cq=%u reason=%s\n",node->cq.handle,reason);
    node->retired=1;
}
static void publish(struct ibv_wc *wc,const struct mcdma_user_completion *c) {
    memset(wc,0,sizeof(*wc));
    wc->wr_id=c->id; wc->status=(enum ibv_wc_status)c->status; wc->opcode=(enum ibv_wc_opcode)c->opcode;
    wc->vendor_err=c->vendor; wc->byte_len=c->bytes; wc->qp_num=c->qpn;
    wc->wc_flags=(unsigned)c->flags; wc->imm_data=c->immediate;
}
static int same(const struct ibv_wc *wc,const struct mcdma_user_completion *c) {
    if (c->user) {
        // The kernel holds no record for a user-posted QP: it reports the
        // hardware counter, the queue side and the raw byte count only.
        return wc->wr_id==c->counter && ((uint32_t)wc->opcode&128u)==(c->opcode&128u) &&
               (wc->status==IBV_WC_SUCCESS ? c->status<=1 : (uint32_t)wc->status==c->status) &&
               (c->status ? 1 : ((uint32_t)wc->wc_flags==c->flags && (!c->flags || wc->imm_data==c->immediate)));
    }
    return wc->wr_id==c->id && (uint32_t)wc->status==c->status &&
           (uint32_t)wc->opcode==c->opcode && wc->byte_len==c->bytes &&
           (c->status ? 1 : ((uint32_t)wc->wc_flags==c->flags && (!c->flags || wc->imm_data==c->immediate)));
}
// Public send opcodes to the hardware's: WRITE 0, WRITE_WITH_IMM 1, SEND 2,
// SEND_WITH_IMM 3, READ 4.
static uint8_t hardware_opcode(enum ibv_wr_opcode opcode) {
    switch ((int)opcode) {
    case 0: return MCDMA_WQE_WRITE; case 1: return MCDMA_WQE_WRITE_IMM;
    case 2: return MCDMA_WQE_SEND; case 3: return MCDMA_WQE_SEND_IMM;
    case 4: return MCDMA_WQE_READ; default: return 0;
    }
}
// Builds the hardware request for one public WR. Inline bytes are gathered
// from the caller's scatter list into `scratch` (userspace posting only).
static int build_request(const struct ibv_send_wr *w,int allow_inline,uint8_t *scratch,struct mcdma_send_request *r) {
    memset(r,0,sizeof(*r));
    r->opcode=hardware_opcode(w->opcode);
    if (!r->opcode) return EOPNOTSUPP;
    if (w->send_flags&~(unsigned)(IBV_SEND_FENCE|IBV_SEND_SIGNALED|IBV_SEND_SOLICITED|IBV_SEND_INLINE)) return EOPNOTSUPP;
    if (!(w->send_flags&IBV_SEND_SIGNALED)) return EOPNOTSUPP; // Every request completes signalled.
    r->flags=(uint8_t)(w->send_flags&(IBV_SEND_FENCE|IBV_SEND_SOLICITED));
    r->immediate=w->imm_data;
    if (mcdma_rdma_opcode(r->opcode)) { r->remote=w->wr.rdma.remote_addr; r->rkey=w->wr.rdma.rkey; }
    if (!w->sg_list || w->num_sge<1) return EINVAL;
    if (w->send_flags&IBV_SEND_INLINE) {
        if (!allow_inline || r->opcode==MCDMA_WQE_READ) return EOPNOTSUPP;
        const unsigned limit=mcdma_rdma_opcode(r->opcode) ? MCDMA_MAX_INLINE_RDMA : MCDMA_MAX_INLINE_SEND;
        unsigned total=0;
        for (int i=0;i<w->num_sge;++i) {
            if (w->sg_list[i].length>limit-total) return EINVAL;
            memcpy(scratch+total,(const void *)(uintptr_t)w->sg_list[i].addr,w->sg_list[i].length);
            total+=w->sg_list[i].length;
        }
        if (!total) return EINVAL;
        r->inline_data=scratch; r->inline_bytes=total;
    } else {
        if (w->num_sge>(int)(mcdma_rdma_opcode(r->opcode) ? MCDMA_MAX_RDMA_SGE : MCDMA_MAX_SEND_SGE)) return EOPNOTSUPP;
        for (int i=0;i<w->num_sge;++i) {
            r->sge[i].address=w->sg_list[i].addr; r->sge[i].lkey=w->sg_list[i].lkey; r->sge[i].length=w->sg_list[i].length;
        }
        r->sge_count=(unsigned)w->num_sge;
    }
    return mcdma_request_bytes(r) ? 0 : EINVAL;
}
static int cq_live(const struct mcdma_cq *node) {
    return node->observation && __atomic_load_n((const uint32_t *)((const uint8_t *)node->observation+MCDMA_CQ_LIVE_OFFSET),
                                                 __ATOMIC_ACQUIRE)==MCDMA_CQ_LIVE_MAGIC;
}
// Lets the kernel consume the ring entries already reported to the
// application and checks that it derived the same completion from each.
// Returns 0 once the CQ has been retired; the kernel may have advanced.
static int acknowledge(struct mcdma_cq *node) {
    struct ibv_wc tmp[MCDMA_USER_SLOTS];
    while (node->ahead) {
        if (node->retired) {
            // Cleanup may drain already-reported entries, but an application
            // poll on this retired CQ returns EIO instead of raw kernel WCs.
            const int k=ibv_cmd_poll_cq(&node->cq,(int)node->ahead,tmp);
            if (k<=0) return 0;
            node->ahead-=(unsigned)k; node->pending_head+=(unsigned)k; node->pending_count-=(unsigned)k;
            continue;
        }
        const uint32_t before=mcdma_cq_consumer(node->observation);
        const int k=ibv_cmd_poll_cq(&node->cq,(int)node->ahead,tmp);
        if (k<=0 || (unsigned)k>node->ahead) { retire(node,k<0?"kernel_error":"kernel_disagrees"); continue; }
        for (int i=0;i<k;++i) {
            if (!same(&tmp[i],&node->pending[node->pending_head&31u])) { retire(node,"completion_mismatch"); }
            ++node->pending_head; --node->pending_count; --node->ahead;
        }
        if (!node->retired && ((mcdma_cq_consumer(node->observation)-before)&0xffffffu)!=(uint32_t)k)
            retire(node,"consumer_mismatch");
    }
    return !node->retired;
}
// Ordinary kernel polling with a mirror check on whatever the kernel consumed.
static int kernel_fresh(struct mcdma_context *mc,struct mcdma_cq *node,int maximum,struct ibv_wc *completions) {
    const uint32_t base=mcdma_cq_consumer(node->observation);
    const int k=ibv_cmd_poll_cq(&node->cq,maximum,completions);
    for (int i=0;i<k && !node->retired;++i) {
        uint8_t cqe[64];
        const volatile uint8_t *entry=(const volatile uint8_t *)node->observation+((base+(uint32_t)i)&31u)*64;
        for (unsigned j=0;j<64;++j) cqe[j]=entry[j];
        struct mcdma_user_completion c;
        if (mcdma_user_decode(cqe,lookup_qp,mc,&c)!=1 || !same(&completions[i],&c)) { retire(node,"mirror_mismatch"); break; }
        // For a user-posted QP only this side knows the request behind the
        // counter the kernel reported; hand the application the real one.
        if (c.user) publish(&completions[i],&c);
    }
    return node->retired ? -EIO : k;
}
static int user_poll(struct mcdma_context *mc,struct mcdma_cq *node,int maximum,struct ibv_wc *completions) {
    if (node->retired) return -EIO;
    int n=0;
    const uint32_t base=mcdma_cq_consumer(node->observation);
    while (n<maximum && node->ahead<MCDMA_USER_MAX_PENDING) {
        uint8_t cqe[64];
        const int ready=mcdma_cq_entry(node->observation,base+node->ahead,cqe);
        if (ready<0) { retire(node,"cq_not_live"); break; }
        if (ready==0) break;
        struct mcdma_user_completion c;
        if (mcdma_user_decode(cqe,lookup_qp,mc,&c)!=1) { retire(node,"unmatched_cqe"); break; }
        node->pending[(node->pending_head+node->pending_count)&31u]=c;
        ++node->pending_count; ++node->ahead;
        publish(&completions[n++],&c);
    }
    if (node->retired) return -EIO;
    if (n) {
        node->empty_polls=0;
        if (node->ahead>=MCDMA_ACK_SAFETY && !acknowledge(node)) return -EIO;
        return n;
    }
    // Every 256 empty observations the kernel still checks error/removal
    // state, exactly as in observation mode.
    if (++node->empty_polls<256) return 0;
    node->empty_polls=0;
    if (node->ahead) return acknowledge(node) ? 0 : -EIO;
    return kernel_fresh(mc,node,maximum,completions);
}
static void acknowledge_all(struct mcdma_context *mc) {
    for (unsigned i=0;i<MCDMA_OBJECTS;++i)
        if (mc->cqs[i] && mc->cqs[i]->consume && mc->cqs[i]->ahead) acknowledge(mc->cqs[i]);
}
static void acknowledge_qp(struct mcdma_qp *node) {
    struct mcdma_cq *send=(struct mcdma_cq *)node->qp.send_cq,*recv=(struct mcdma_cq *)node->qp.recv_cq;
    if (send && send->consume) acknowledge(send);
    if (recv && recv!=send && recv->consume) acknowledge(recv);
}
static int register_object(void **table,void *object) {
    for (unsigned i=0;i<MCDMA_OBJECTS;++i) if (!table[i]) { table[i]=object; return 1; }
    return 0;
}
static void unregister_object(void **table,const void *object) {
    for (unsigned i=0;i<MCDMA_OBJECTS;++i) if (table[i]==object) table[i]=NULL;
}

static struct ibv_cq *create_cq(struct ibv_context *ctx,int cqe,struct ibv_comp_channel *channel,int vector) {
    if (channel || vector || cqe<1 || cqe>31) { errno=EOPNOTSUPP; return NULL; }
    struct mcdma_context *mc=context_of(ctx);
    const int mode=mc ? mc->cq_mode : cq_mode_from_environment();
    if (mode<0) { errno=EINVAL; return NULL; }
    struct mcdma_cq *node=calloc(1,sizeof(*node)); if (!node) return NULL;
    int error=pthread_mutex_init(&node->poll_lock,NULL);
    if (error) { free(node); errno=error; return NULL; }
    struct ibv_cq *cq=&node->cq;
    uint64_t command[5]={0},response=0;
    error=ibv_cmd_create_cq(ctx,cqe,NULL,0,cq,command,sizeof(command),&response,sizeof(response));
    if (error) { pthread_mutex_destroy(&node->poll_lock); free(node); errno=error; return NULL; }
    if (mode!=MCDMA_MODE_OFF) {
        const uint64_t offset=((uint64_t)cq->handle+1)*MCDMA_CQ_MAP_BYTES;
        node->observation=darwin_mmap(NULL,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_SHARED,
                                     ctx->cmd_fd,(int64_t)offset);
        if (node->observation==MAP_FAILED) {
            node->observation=NULL;
            // Keep a valid CQ and the ordinary kernel path on mapping failure.
            // Benchmarks require the explicit successful mapping marker below.
            fprintf(stderr,"MCDMA_CQ_OBSERVER mapped=0 reason=mmap_failed\n");
        } else if (mode==MCDMA_MODE_CONSUME && mc) {
            node->consume=1;
            fprintf(stderr,"MCDMA_CQ_OBSERVER mapped=1 bytes=%u readonly=1 consume=user\n",MCDMA_CQ_MAP_BYTES);
        } else fprintf(stderr,"MCDMA_CQ_OBSERVER mapped=1 bytes=%u readonly=1\n",MCDMA_CQ_MAP_BYTES);
    } else fprintf(stderr,"MCDMA_CQ_OBSERVER mapped=0 reason=disabled\n");
    if (mc) {
        pthread_mutex_lock(&mc->lock);
        if (!register_object((void **)mc->cqs,node)) node->consume=0;
        pthread_mutex_unlock(&mc->lock);
    }
    return cq;
}
static int destroy_cq(struct ibv_cq *cq) {
    struct mcdma_cq *node=(struct mcdma_cq *)cq;
    struct mcdma_context *mc=context_of(cq->context);
    // Destruction requires callers to stop polling. Preserve all mappings and
    // mirror state if the kernel refuses destruction (for example, EBUSY).
    if (!node->kernel_destroyed) {
        if (mc) {
            pthread_mutex_lock(&mc->lock);
            if (node->consume && node->ahead) acknowledge(node);
            pthread_mutex_unlock(&mc->lock);
        }
        const int error=ibv_cmd_destroy_cq(cq);
        if (error) return error;
        node->kernel_destroyed=1;
        node->retired=1;
    }
    // The hardware object is gone; an unmap failure leaves only cleanup to
    // retry, without issuing destruction for a potentially reused handle.
    if (node->observation) {
        int error=darwin_munmap(cq->context->cmd_fd,node->observation);
        if (error) return error;
        node->observation=NULL;
    }
    if (mc) {
        pthread_mutex_lock(&mc->lock);
        unregister_object((void **)mc->cqs,node);
        pthread_mutex_unlock(&mc->lock);
    }
    pthread_mutex_destroy(&node->poll_lock); free(node); return 0;
}
static int poll_cq(struct ibv_cq *cq,int maximum,struct ibv_wc *completions) {
    struct mcdma_cq *node=(struct mcdma_cq *)cq;
    if (node->kernel_destroyed) return -EIO;
    if (maximum<=0 || !completions || !node->observation)
        return ibv_cmd_poll_cq(cq,maximum,completions);
    if (node->consume) {
        struct mcdma_context *mc=context_of(cq->context);
        if (!mc) return ibv_cmd_poll_cq(cq,maximum,completions);
        pthread_mutex_lock(&mc->lock);
        const int result=user_poll(mc,node,maximum,completions);
        pthread_mutex_unlock(&mc->lock);
        return result;
    }
    pthread_mutex_lock(&node->poll_lock);
    int result=0;
    // Every 256 empty observations still checks kernel error/removal state.
    // A visible CQE always goes through Apple's normal command and our full
    // kernel CQ/WR validation; no completion is manufactured in userspace.
    if (mcdma_cq_observe(node->observation)!=0 || ++node->empty_polls>=256) {
        node->empty_polls=0;
        result=ibv_cmd_poll_cq(cq,maximum,completions);
    }
    pthread_mutex_unlock(&node->poll_lock);
    return result;
}
static struct ibv_qp *create_qp(struct ibv_pd *pd,struct ibv_qp_init_attr *attr) {
    struct mcdma_context *pre=pd ? context_of(pd->context) : NULL;
    // RC only, up to two send scatter entries, one receive entry; inline
    // data (up to the WQEBB's room) only where this process posts directly.
    if (!attr || attr->qp_type!=IBV_QPT_RC || attr->srq ||
        attr->cap.max_inline_data>MCDMA_MAX_INLINE_RDMA || (attr->cap.max_inline_data && !(pre && pre->user_post)) ||
        attr->cap.max_send_wr>31 || attr->cap.max_recv_wr>31 ||
        attr->cap.max_send_sge<1 || attr->cap.max_send_sge>MCDMA_MAX_RDMA_SGE || attr->cap.max_recv_sge!=1) { errno=EOPNOTSUPP; return NULL; }
    if ((attr->send_cq && ((struct mcdma_cq *)attr->send_cq)->kernel_destroyed) ||
        (attr->recv_cq && ((struct mcdma_cq *)attr->recv_cq)->kernel_destroyed)) { errno=EIO; return NULL; }
    struct mcdma_qp *node=calloc(1,sizeof(*node)); if (!node) return NULL;
    struct ibv_qp *qp=&node->qp;
    uint64_t command[8]={0},response[4]={0};
    int error=ibv_cmd_create_qp(pd,qp,attr,command,sizeof(command),response,sizeof(response));
    if (error) { free(node); errno=error; return NULL; }
    struct mcdma_context *mc=context_of(pd->context);
    if (mc) {
        pthread_mutex_lock(&mc->lock);
        const int registered=register_object((void **)mc->qps,node);
        pthread_mutex_unlock(&mc->lock);
        if (!registered) {
            // The kernel bounds objects per context; mirror the same bound.
            ibv_cmd_destroy_qp(qp); free(node); errno=ENOMEM; return NULL;
        }
        if (mc->user_post && mc->uar) {
            // The kernel created this QP on our UAR: map its work queues, or
            // give it back rather than leave a queue nobody can post to.
            struct mcdma_cq *scq=(struct mcdma_cq *)qp->send_cq,*rcq=(struct mcdma_cq *)qp->recv_cq;
            const int64_t offset=(int64_t)((MCDMA_QUEUE_PAGE_BASE+qp->qp_num)*MCDMA_QUEUE_PAGE_BYTES);
            void *queue=(scq && scq->consume && rcq && rcq->consume)
                ? darwin_mmap(NULL,MCDMA_QUEUE_PAGE_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,pd->context->cmd_fd,offset) : MAP_FAILED;
            if (queue==MAP_FAILED) {
                fprintf(stderr,"MCDMA_USER_POST qp=%u queue_mapped=0 reason=%s\n",qp->qp_num,
                        (scq && scq->consume && rcq && rcq->consume) ? "mmap_failed" : "cq_not_consumed");
                pthread_mutex_lock(&mc->lock); unregister_object((void **)mc->qps,node); pthread_mutex_unlock(&mc->lock);
                ibv_cmd_destroy_qp(qp); free(node); errno=EIO; return NULL;
            }
            node->queue=queue; node->user_posted=1;
            fprintf(stderr,"MCDMA_USER_POST qp=%u queue_mapped=1 bytes=%u\n",qp->qp_num,MCDMA_QUEUE_PAGE_BYTES);
            if (mc->bf_mode) {
                // BlueFlame only when the kernel published a bank size for a
                // write-combined UAR page this context actually holds.
                uint32_t flags=0,bank_bytes=0;
                const int valid=mcdma_info_read(queue,&flags,&bank_bytes);
                const int push=mc->bf_mode!=MCDMA_BF_DOORBELL;
                node->blueflame=push && valid && mc->uar_wc && (flags&MCDMA_INFO_UAR_WRITE_COMBINED) &&
                                bank_bytes>=mc->bf_bytes;
                if (node->blueflame) mc->bf_bank_bytes=bank_bytes;
                fprintf(stderr,"MCDMA_USER_BF qp=%u bank_bytes=%u bytes=%u store=%s\n",qp->qp_num,
                        valid ? bank_bytes : 0u,node->blueflame ? mc->bf_bytes : 0u,
                        node->blueflame ? (mc->bf_vector ? "neon" : "scalar") : "doorbell");
            }
        }
    }
    return qp;
}
static int modify_qp(struct ibv_qp *qp,struct ibv_qp_attr *attr,int mask) {
    if (((struct mcdma_qp *)qp)->kernel_destroyed) return EIO;
    struct mcdma_context *mc=context_of(qp->context);
    const int reset=attr && (mask&IBV_QP_STATE) && attr->qp_state==IBV_QPS_RESET;
    if (mc && reset) {
        // A kernel RESET purges and compacts this QP's completions: nothing
        // reported from userspace may remain unacknowledged when it happens.
        pthread_mutex_lock(&mc->lock); acknowledge_qp((struct mcdma_qp *)qp); pthread_mutex_unlock(&mc->lock);
    }
    uint64_t command[15]={0};
    const int error=ibv_cmd_modify_qp(qp,attr,mask,command,sizeof(command));
    if (mc && reset && !error) {
        struct mcdma_qp *node=(struct mcdma_qp *)qp;
        pthread_mutex_lock(&mc->lock);
        mcdma_user_queue_reset(&node->sends); mcdma_user_queue_reset(&node->receives);
        pthread_mutex_unlock(&mc->lock);
    }
    return error;
}
static int destroy_qp(struct ibv_qp *qp) {
    struct mcdma_context *mc=context_of(qp->context);
    struct mcdma_qp *node=(struct mcdma_qp *)qp;
    if (!node->kernel_destroyed) {
        if (mc) {
            pthread_mutex_lock(&mc->lock);
            acknowledge_qp(node);
            pthread_mutex_unlock(&mc->lock);
        }
        const int error=ibv_cmd_destroy_qp(qp);
        if (error) return error;
        node->kernel_destroyed=1;
    }
    if (node->queue) {
        // The kernel has stopped this QP; retain a failed unmap for retry.
        const int error=darwin_munmap(qp->context->cmd_fd,node->queue);
        if (error) return error;
        node->queue=NULL;
    }
    if (mc) {
        pthread_mutex_lock(&mc->lock);
        unregister_object((void **)mc->qps,node);
        pthread_mutex_unlock(&mc->lock);
    }
    free(node); return 0;
}
// Requests the kernel accepted: on error, those before the reported bad WR;
// an error without a bad WR means the command never reached the kernel.
static unsigned accepted_sends(const struct ibv_send_wr *wr,const struct ibv_send_wr *bad,int error) {
    unsigned n=0;
    if (error && !bad) return 0;
    for (const struct ibv_send_wr *w=wr;w && !(error && w==bad);w=w->next) ++n;
    return n;
}
static unsigned accepted_recvs(const struct ibv_recv_wr *wr,const struct ibv_recv_wr *bad,int error) {
    unsigned n=0;
    if (error && !bad) return 0;
    for (const struct ibv_recv_wr *w=wr;w && !(error && w==bad);w=w->next) ++n;
    return n;
}
// Direct posting for a user-posted QP: the WQE goes into the mapped send
// queue, the doorbell record is updated and the UAR register is rung, all
// from this process. The kernel keeps consuming CQEs and verifying them.
static int user_post_send(struct mcdma_context *mc,struct mcdma_qp *node,struct ibv_send_wr *wr,struct ibv_send_wr **bad) {
    struct mcdma_cq *scq=(struct mcdma_cq *)node->qp.send_cq;
    if (bad) *bad=wr;
    if (node->kernel_destroyed) return EIO;
    if (scq && scq->retired) return EIO;
    if (!scq || !cq_live(scq)) return ENODEV; // Quiesced, retired or removed device.
    if (scq->ahead>=MCDMA_ACK_BATCH && !acknowledge(scq)) return EIO;
    unsigned posted=0; uint64_t doorbell=0; int error=0;
    for (struct ibv_send_wr *w=wr;w;w=w->next) {
        uint8_t scratch[MCDMA_MAX_INLINE_SEND]; struct mcdma_send_request request;
        error=build_request(w,1,scratch,&request);
        if (error) { if (bad) *bad=w; break; }
        if (!mcdma_user_queue_can_post(&node->sends) && scq->ahead && !acknowledge(scq)) {
            error=EIO; if (bad) *bad=w; break;
        }
        if (!mcdma_user_queue_can_post(&node->sends)) { error=ENOMEM; if (bad) *bad=w; break; }
        const uint32_t bytes=mcdma_request_bytes(&request);
        const uint64_t db=mcdma_encode_send_request(node->queue,node->qp.qp_num,node->sends.producer,&request);
        if (!db || !mcdma_user_queue_post(&node->sends,w->wr_id,request.opcode,bytes)) { error=EINVAL; if (bad) *bad=w; break; }
        doorbell=db; ++posted;
    }
    if (posted==1 && node->blueflame) {
        // One WQE: push it through the next bank; the doorbell record was
        // written first, so a fetch remains correct if the push is not taken.
        mcdma_ring_send_bf(node->queue,mc->uar,node->sends.producer,node->sends.producer-1u,
                           MCDMA_UAR_DOORBELL+mc->bf_bank*mc->bf_bank_bytes,mc->bf_bytes,mc->bf_vector);
        mc->bf_bank^=1u;
    } else if (posted && mc->uar_wc) {
        // A list still uses the current BlueFlame bank for its ordinary
        // doorbell, and advances that bank after the WC stores are flushed.
        // Single and list submissions share the same context lock/cadence.
        const uint32_t offset=node->blueflame ? mc->bf_bank*mc->bf_bank_bytes : 0u;
        mcdma_ring_send_wc(node->queue,mc->uar+offset,node->sends.producer,doorbell);
        if (node->blueflame) mc->bf_bank^=1u;
    }
    else if (posted) mcdma_ring_send(node->queue,mc->uar,node->sends.producer,doorbell);
    if (!error && bad) *bad=NULL;
    return error;
}
static int user_post_recv(struct mcdma_context *mc,struct mcdma_qp *node,struct ibv_recv_wr *wr,struct ibv_recv_wr **bad) {
    struct mcdma_cq *rcq=(struct mcdma_cq *)node->qp.recv_cq;
    if (bad) *bad=wr;
    if (node->kernel_destroyed) return EIO;
    if (rcq && rcq->retired) return EIO;
    if (!rcq || !cq_live(rcq)) return ENODEV;
    if (rcq->ahead>=MCDMA_ACK_BATCH && !acknowledge(rcq)) return EIO;
    unsigned posted=0; int error=0;
    for (struct ibv_recv_wr *w=wr;w;w=w->next) {
        if (w->num_sge!=1 || !w->sg_list) { error=EOPNOTSUPP; if (bad) *bad=w; break; }
        if (!mcdma_user_queue_can_post(&node->receives) && rcq->ahead && !acknowledge(rcq)) {
            error=EIO; if (bad) *bad=w; break;
        }
        if (!mcdma_user_queue_can_post(&node->receives)) { error=ENOMEM; if (bad) *bad=w; break; }
        if (!mcdma_encode_recv_wqe(node->queue,node->receives.producer,w->sg_list->addr,w->sg_list->length,w->sg_list->lkey) ||
            !mcdma_user_queue_post(&node->receives,w->wr_id,MCDMA_WQE_SEND,w->sg_list->length)) { error=EINVAL; if (bad) *bad=w; break; }
        ++posted;
    }
    (void)mc;
    if (posted) mcdma_publish_recv(node->queue,node->receives.producer);
    if (!error && bad) *bad=NULL;
    return error;
}
static int mcdma_post_send(struct ibv_qp *qp,struct ibv_send_wr *wr,struct ibv_send_wr **bad) {
    if (((struct mcdma_qp *)qp)->kernel_destroyed) { if (bad) *bad=wr; return EIO; }
    struct mcdma_context *mc=context_of(qp->context);
    struct mcdma_cq *scq=(struct mcdma_cq *)qp->send_cq;
    if (!mc || !scq || !scq->consume) return ibv_cmd_post_send(qp,wr,bad);
    struct mcdma_qp *node=(struct mcdma_qp *)qp;
    pthread_mutex_lock(&mc->lock);
    if (scq->retired) {
        if (bad) *bad=wr;
        pthread_mutex_unlock(&mc->lock); return EIO;
    }
    if (node->user_posted) {
        const int direct=user_post_send(mc,node,wr,bad);
        pthread_mutex_unlock(&mc->lock); return direct;
    }
    if (scq->ahead>=MCDMA_ACK_BATCH && !acknowledge(scq)) {
        if (bad) *bad=wr;
        pthread_mutex_unlock(&mc->lock); return EIO;
    }
    int error=0;
    for (int attempt=0;attempt<2;++attempt) {
        // Mirror exactly the requests the kernel can accept, in kernel order.
        unsigned recorded=0;
        for (const struct ibv_send_wr *w=wr;w;w=w->next) {
            struct mcdma_send_request request;
            if (build_request(w,0,NULL,&request) ||
                !mcdma_user_queue_post(&node->sends,w->wr_id,request.opcode,mcdma_request_bytes(&request))) break;
            ++recorded;
        }
        error=ibv_cmd_post_send(qp,wr,bad);
        const unsigned accepted=accepted_sends(wr,bad ? *bad : NULL,error);
        if (accepted<recorded) mcdma_user_queue_rollback(&node->sends,recorded-accepted);
        else if (accepted>recorded) retire(scq,"mirror_desync");
        // Kernel credits are held by completions it has not consumed yet;
        // retry only a list that was refused before any request was posted.
        if (error!=ENOMEM || attempt || accepted || !scq->ahead) break;
        if (!acknowledge(scq)) { error=EIO; break; }
    }
    pthread_mutex_unlock(&mc->lock);
    return error;
}
static int mcdma_post_recv(struct ibv_qp *qp,struct ibv_recv_wr *wr,struct ibv_recv_wr **bad) {
    if (((struct mcdma_qp *)qp)->kernel_destroyed) { if (bad) *bad=wr; return EIO; }
    struct mcdma_context *mc=context_of(qp->context);
    struct mcdma_cq *rcq=(struct mcdma_cq *)qp->recv_cq;
    if (!mc || !rcq || !rcq->consume) return ibv_cmd_post_recv(qp,wr,bad);
    struct mcdma_qp *node=(struct mcdma_qp *)qp;
    pthread_mutex_lock(&mc->lock);
    if (rcq->retired) {
        if (bad) *bad=wr;
        pthread_mutex_unlock(&mc->lock); return EIO;
    }
    if (node->user_posted) {
        const int direct=user_post_recv(mc,node,wr,bad);
        pthread_mutex_unlock(&mc->lock); return direct;
    }
    if (rcq->ahead>=MCDMA_ACK_BATCH && !acknowledge(rcq)) {
        if (bad) *bad=wr;
        pthread_mutex_unlock(&mc->lock); return EIO;
    }
    int error=0;
    for (int attempt=0;attempt<2;++attempt) {
        unsigned recorded=0;
        for (const struct ibv_recv_wr *w=wr;w;w=w->next) {
            if (w->num_sge!=1 || !w->sg_list ||
                !mcdma_user_queue_post(&node->receives,w->wr_id,0x0a,w->sg_list->length)) break;
            ++recorded;
        }
        error=ibv_cmd_post_recv(qp,wr,bad);
        const unsigned accepted=accepted_recvs(wr,bad ? *bad : NULL,error);
        if (accepted<recorded) mcdma_user_queue_rollback(&node->receives,recorded-accepted);
        else if (accepted>recorded) retire(rcq,"mirror_desync");
        if (error!=ENOMEM || attempt || accepted || !rcq->ahead) break;
        if (!acknowledge(rcq)) { error=EIO; break; }
    }
    pthread_mutex_unlock(&mc->lock);
    return error;
}
static struct ibv_mr *register_mr(struct ibv_pd *pd,void *address,size_t length,uint64_t iova,int access) {
    // The kernel decides what a mapping can describe; only the shared 16 KiB
    // page offset of the HCA address and the access bits are checked here.
    if (!length || (uintptr_t)address>UINT64_MAX-length || iova>UINT64_MAX-length ||
        ((iova^(uintptr_t)address)&0x3fff) || (access&~7) || ((access&2)&&!(access&1))) {
        errno=EINVAL; return NULL;
    }
    struct mcdma_vmr *mr=calloc(1,sizeof(*mr)); if (!mr) return NULL;
    uint64_t command[6]={0}; uint32_t response[3]={0};
    int error=ibv_cmd_reg_mr(pd,address,length,iova,access,mr,command,sizeof(command),response,sizeof(response));
    if (error) { free(mr); errno=error; return NULL; }
    return &mr->mr;
}
static int deregister_mr(struct mcdma_vmr *mr) {
    struct mcdma_context *mc=mr->mr.context ? context_of(mr->mr.context) : NULL;
    if (mc) {
        // The kernel refuses to deregister a key while work it has not yet
        // consumed still references it; let it consume what we reported.
        pthread_mutex_lock(&mc->lock); acknowledge_all(mc); pthread_mutex_unlock(&mc->lock);
    }
    int error=ibv_cmd_dereg_mr(mr); if (!error) free(mr); return error;
}
static int notify_cq(struct ibv_cq *cq,int solicited) { (void)cq; (void)solicited; return EOPNOTSUPP; }
static void free_context(struct ibv_context *ctx) {
    struct verbs_context *vctx=verbs_get_ctx(ctx);
    struct mcdma_context *mc=(struct mcdma_context *)vctx;
    pthread_mutex_destroy(&mc->lock);
    verbs_uninit_context(vctx); free(vctx);
}
static int user_post_requested(void) {
    const char *value=getenv("MCDMA_USER_POST");
    if (!value || !strcmp(value,"0")) return 0;
    return strcmp(value,"1") ? -1 : 1;
}
static struct verbs_context *alloc_context(struct ibv_device *device,int connection,void *private_data) {
    (void)private_data;
    if (!supported_build()) { errno=ENODEV; return NULL; }
    int mode=cq_mode_from_environment();
    const int user_post=user_post_requested();
    const int bf_mode=bf_mode_from_environment();
    // A BlueFlame arm without direct posting would silently measure the
    // kernel path under the wrong label.
    if (mode<0 || user_post<0 || bf_mode<0 || (bf_mode && !user_post)) { errno=EINVAL; return NULL; }
    // Direct posting reports completions from userspace, so it needs mode 2.
    if (user_post) mode=MCDMA_MODE_CONSUME;
    struct mcdma_context *mc=_verbs_init_and_alloc_context(device,connection,sizeof(*mc),NULL,MCDMA_DRIVER_ID);
    if (!mc) return NULL;
    struct verbs_context *ctx=&mc->vctx;
    mc->cq_mode=mode; mc->user_post=0; mc->uar=NULL;
    mc->bf_mode=bf_mode; mc->uar_wc=0; mc->bf_bank=0; mc->bf_bank_bytes=0;
    mc->bf_bytes=bf_mode==MCDMA_BF_128 ? 128u : (bf_mode==MCDMA_BF_64 || bf_mode==MCDMA_BF_64S) ? 64u : 0u;
    mc->bf_vector=bf_mode==MCDMA_BF_64;
    memset(mc->qps,0,sizeof(mc->qps)); memset(mc->cqs,0,sizeof(mc->cqs));
    int error=pthread_mutex_init(&mc->lock,NULL);
    if (!error) {
        uint64_t command[2]={0},response=0;
        error=ibv_cmd_get_context(ctx,command,sizeof(command),&response,sizeof(response));
        if (error) pthread_mutex_destroy(&mc->lock);
    }
    if (error) {
        // On alloc_context failure Apple's caller closes the command handle.
        ctx->context.cmd_fd=-1;
        verbs_uninit_context(ctx); free(ctx); errno=error; return NULL;
    }
    if (user_post) {
        // Map this context's UAR page before any QP exists: the kernel makes
        // every later QP user-posted on it. Failure keeps kernel posting.
        // A BlueFlame arm asks for the write-combined page first and falls
        // back to the ordinary page (reported, so a run cannot be mislabelled).
        void *uar=MAP_FAILED;
        if (bf_mode) {
            uar=darwin_mmap(NULL,MCDMA_QUEUE_PAGE_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,ctx->context.cmd_fd,
                            (int64_t)(MCDMA_UAR_WC_PAGE_NUMBER*MCDMA_QUEUE_PAGE_BYTES));
            if (uar==MAP_FAILED) fprintf(stderr,"MCDMA_USER_BF mode=%s uar_wc=0 reason=wc_mmap_failed\n",bf_mode_name(bf_mode));
            else { mc->uar_wc=1; fprintf(stderr,"MCDMA_USER_BF mode=%s uar_wc=1\n",bf_mode_name(bf_mode)); }
        }
        if (uar==MAP_FAILED)
            uar=darwin_mmap(NULL,MCDMA_QUEUE_PAGE_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,ctx->context.cmd_fd,
                            (int64_t)(MCDMA_UAR_PAGE_NUMBER*MCDMA_QUEUE_PAGE_BYTES));
        if (uar==MAP_FAILED) fprintf(stderr,"MCDMA_USER_POST enabled=0 reason=uar_mmap_failed\n");
        else { mc->uar=uar; mc->user_post=1; fprintf(stderr,"MCDMA_USER_POST enabled=1 uar_bytes=%u\n",MCDMA_QUEUE_PAGE_BYTES); }
    }
    verbs_set_ops(ctx,context_ops); return ctx;
}
static const struct mcdma_device_ops device_ops={
    .name="mcdma",.minimum=MCDMA_ABI,.maximum=MCDMA_ABI,
    .match_table=device_matches,
    .match_device=match_device,.alloc_context=alloc_context,
    .alloc_device=alloc_device,.uninit_device=uninit_device
};
#define OP(offset,function) context_ops[(offset)/8]=(void *)(function)
int mcdma_provider_abi_check(void) {
    if (!supported_build()) return ENOTSUP;
    // Exercise the REAL Apple ops installer on isolated storage, without
    // claiming or enumerating a fabricated device and without issuing I/O.
    struct { struct verbs_context ctx; uint64_t guard; } local={0};
    struct { uint8_t bytes[0x278]; uint64_t guard; } private={0};
    local.guard=private.guard=0x6d63646d615f6162ull;
    local.ctx.priv=(struct verbs_ex_private *)private.bytes;
    verbs_set_ops(&local.ctx,context_ops);
    int error=0;
    if (local.guard!=0x6d63646d615f6162ull || private.guard!=local.guard ||
        local.ctx.query_device_ex!=query_device ||
        local.ctx.context.ops.poll_cq!=poll_cq ||
        local.ctx.context.ops.post_send!=mcdma_post_send ||
        local.ctx.context.ops.post_recv!=mcdma_post_recv ||
        local.ctx.context.ops.req_notify_cq!=notify_cq) error=EPROTO;
    // Private ops begin at +0x18; check every installed entry, including
    // lifecycle and MR functions not exposed as typed public context fields.
    for (size_t i=0;i<sizeof(context_ops)/sizeof(context_ops[0]);++i) {
        if (!context_ops[i]) continue;
        void *actual=NULL; memcpy(&actual,private.bytes+0x18+i*8,8);
        if (actual!=context_ops[i]) error=EPROTO;
    }
    return error;
}
/* Acceptance tools use this before any raw mmap. The context must have been
 * opened by this build-gated ABI2 provider, not an older or foreign plugin. */
int mcdma_context_supports_cq_mapping(struct ibv_context *ctx) {
    return supported_build() && MCDMA_ABI==2 && ctx && ctx->ops.poll_cq==poll_cq &&
           ctx->ops.post_send==mcdma_post_send && ctx->ops.post_recv==mcdma_post_recv;
}
__attribute__((constructor)) static void register_provider(void) {
    if (!supported_build()) return;
    OP(0x028,alloc_pd); OP(0x078,create_cq); OP(0x098,create_qp);
    OP(0x0d0,dealloc_pd); OP(0x0e0,deregister_mr); OP(0x0f8,destroy_cq);
    OP(0x110,destroy_qp); OP(0x138,free_context); OP(0x178,modify_qp);
    OP(0x1a8,poll_cq); OP(0x1b0,mcdma_post_recv); OP(0x1b8,mcdma_post_send);
    OP(0x1d0,query_device); OP(0x1e0,query_port); OP(0x220,register_mr); OP(0x228,notify_cq);
    if (!mcdma_provider_abi_check()) verbs_register_driver_34(&device_ops);
}
