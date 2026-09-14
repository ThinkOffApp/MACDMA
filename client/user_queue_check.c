/* Original mapping acceptance test. Creates reset-state QPs only; does not
 * ring any doorbell, post DMA work, change networking, or touch another app. */
#include <infiniband/verbs.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <errno.h>
#include "../include/cx5_user_post.h"
extern void *darwin_mmap(void *,size_t,int,int,int,int64_t);
extern int darwin_munmap(int,void *);
static unsigned failures;
static void check(int success,const char *name) {
    printf("USER_QUEUE_CHECK %s=%s\n",name,success ? "PASS" : "FAIL");
    failures+=!success;
}
static void *map(struct ibv_context *c,uint64_t page,int prot) {
    return darwin_mmap(NULL,16384,prot,MAP_SHARED,c->cmd_fd,(int64_t)(page*16384));
}
static void denied(struct ibv_context *c,uint64_t page,int prot,const char *name) {
    void *p=map(c,page,prot); check(p==MAP_FAILED,name);
    if (p!=MAP_FAILED) check(!darwin_munmap(c->cmd_fd,p),"unexpected_map_cleanup");
}
static int supported(struct ibv_context *c) {
    Dl_info origin={0};
    if (!c || !dladdr((void *)c->ops.poll_cq,&origin)) return 0;
    void *library=dlopen(origin.dli_fname,RTLD_NOW|RTLD_NOLOAD);
    if (!library) return 0;
    int (*probe)(struct ibv_context *)=dlsym(library,"mcdma_context_supports_cq_mapping");
    int ok=probe && probe(c); dlclose(library); return ok;
}
int main(int argc,char **argv) {
    if (argc!=2) return 2;
    setvbuf(stdout,NULL,_IOLBF,0);
    setenv("MCDMA_USER_POST","0",1); setenv("MCDMA_CQ_MAP","0",1);
    int count=0; struct ibv_device **devices=ibv_get_device_list(&count),*device=NULL;
    for (int i=0;devices && i<count;++i)
        if (!strcmp(ibv_get_device_name(devices[i]),argv[1])) device=devices[i];
    struct ibv_context *a=device ? ibv_open_device(device) : NULL;
    struct ibv_context *b=device ? ibv_open_device(device) : NULL;
    if (!supported(a) || !supported(b)) { fputs("Unsupported provider\n",stderr); return 2; }
    denied(a,MCDMA_UAR_PAGE_NUMBER,PROT_READ,"uar_readonly_rejected");
    denied(a,MCDMA_UAR_PAGE_NUMBER,PROT_READ|PROT_WRITE|PROT_EXEC,"uar_executable_rejected");
    denied(a,MCDMA_UAR_WC_PAGE_NUMBER,PROT_READ,"uar_wc_readonly_rejected");
    void *uar=map(a,MCDMA_UAR_PAGE_NUMBER,PROT_READ|PROT_WRITE);
    check(uar!=MAP_FAILED,"own_uar_mapping");
    if (uar==MAP_FAILED) goto contexts;
    denied(a,MCDMA_UAR_PAGE_NUMBER,PROT_READ|PROT_WRITE,"duplicate_uar_rejected");
    denied(a,MCDMA_UAR_WC_PAGE_NUMBER,PROT_READ|PROT_WRITE,"uar_wc_after_uar_rejected");
    {
        // The write-combined page maps only when the kernel granted userspace
        // BlueFlame (EOPNOTSUPP otherwise); either answer is a pass here, and
        // a second such mapping of the same context must be refused.
        struct ibv_context *c=device ? ibv_open_device(device) : NULL;
        if (supported(c)) {
            void *wc=map(c,MCDMA_UAR_WC_PAGE_NUMBER,PROT_READ|PROT_WRITE);
            const int refused=wc==MAP_FAILED ? errno : 0;
            check(wc!=MAP_FAILED || refused==EOPNOTSUPP,"uar_wc_mapping_or_unsupported");
            printf("USER_QUEUE_CHECK uar_wc_granted=%d\n",wc!=MAP_FAILED);
            if (wc!=MAP_FAILED) {
                denied(c,MCDMA_UAR_WC_PAGE_NUMBER,PROT_READ|PROT_WRITE,"duplicate_uar_wc_rejected");
                denied(c,MCDMA_UAR_PAGE_NUMBER,PROT_READ|PROT_WRITE,"uar_after_uar_wc_rejected");
                check(!darwin_munmap(c->cmd_fd,wc),"uar_wc_unmap");
            }
            check(!ibv_close_device(c),"uar_wc_context_close");
        } else if (c) ibv_close_device(c);
    }
    struct ibv_pd *pd=ibv_alloc_pd(a);
    struct ibv_cq *cq=ibv_create_cq(a,31,NULL,NULL,0);
    struct ibv_qp *qp=NULL;
    if (pd && cq) {
        struct ibv_qp_init_attr attr={0}; attr.send_cq=attr.recv_cq=cq; attr.qp_type=IBV_QPT_RC;
        attr.cap.max_send_wr=attr.cap.max_recv_wr=31;
        attr.cap.max_send_sge=attr.cap.max_recv_sge=1;
        qp=ibv_create_qp(pd,&attr);
    }
    check(qp!=NULL,"user_qp_created");
    if (qp) {
        uint64_t page=MCDMA_QUEUE_PAGE_BASE+qp->qp_num;
        denied(b,page,PROT_READ|PROT_WRITE,"foreign_context_queue_rejected");
        denied(a,page,PROT_READ|PROT_WRITE|PROT_EXEC,"queue_executable_rejected");
        void *queue=map(a,page,PROT_READ|PROT_WRITE);
        check(queue!=MAP_FAILED,"own_queue_mapping");
        int error=ibv_destroy_qp(qp); check(!error,"qp_destroy_while_mapped");
        if (!error) {
            denied(a,page,PROT_READ|PROT_WRITE,"destroyed_queue_mapping_rejected");
            if (queue!=MAP_FAILED) {
                volatile uint8_t *bytes=queue;
                bytes[16383]=0x5a; check(bytes[16383]==0x5a,"retained_queue_storage");
            }
        }
        if (queue!=MAP_FAILED) check(!darwin_munmap(a->cmd_fd,queue),"queue_unmap");
    }
    if (cq) check(!ibv_destroy_cq(cq),"cq_destroy");
    if (pd) check(!ibv_dealloc_pd(pd),"pd_destroy");
    check(!darwin_munmap(a->cmd_fd,uar),"uar_unmap");
contexts:
    check(!ibv_close_device(b),"other_context_close");
    check(!ibv_close_device(a),"own_context_close");
    ibv_free_device_list(devices);
    puts("USER_QUEUE_SCOPE mapping checks only; foreign-doorbell execution and process death remain separate");
    return failures ? 1 : 0;
}
