/* Original hardware acceptance test for the candidate's read-only CQ mapping.
 * No QPs or data transfers: validates real VM permissions and retained storage. */
#include <infiniband/verbs.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <spawn.h>
#include <time.h>
extern char **environ;
#include "../include/cx5_cq_observer.h"
extern void *darwin_mmap(void *,size_t,int,int,int,int64_t);
extern int darwin_munmap(int,void *);

static int failures;
static void check(int ok,const char *name) {
    printf("CQ_MAP_CHECK %s=%s\n",name,ok?"PASS":"FAIL");
    failures+=!ok;
}
static void rejected(struct ibv_context *ctx,uint64_t offset,size_t bytes,int prot,int flags,const char *name) {
    void *p=darwin_mmap(NULL,bytes,prot,flags,ctx->cmd_fd,(int64_t)offset);
    check(p==MAP_FAILED,name);
    if (p!=MAP_FAILED) check(darwin_munmap(ctx->cmd_fd,p)==0,"unexpected_map_cleanup");
}
static int mapping_preflight(struct ibv_context *ctx) {
    Dl_info origin={0};
    if (!ctx || !dladdr((const void *)ctx->ops.poll_cq,&origin) || !origin.dli_fname) return 0;
    void *library=dlopen(origin.dli_fname,RTLD_NOW|RTLD_NOLOAD);
    if (!library) return 0;
    int (*supports)(struct ibv_context *)=dlsym(library,"mcdma_context_supports_cq_mapping");
    int ok=supports && supports(ctx);
    dlclose(library); return ok;
}
static void write_fault_child(const char *name) {
    // IOKit mappings need not survive fork: open a fresh context after exec.
    // The child owns no QP, posts no DMA work, and exits via the tested fault.
    alarm(5);
    struct rlimit core={0};
    if(getrlimit(RLIMIT_CORE,&core)) _exit(70);
    core.rlim_cur=0; if(setrlimit(RLIMIT_CORE,&core)) _exit(71);
    int count=0; struct ibv_device **devices=ibv_get_device_list(&count);
    struct ibv_device *device=NULL;
    for(int i=0;devices && i<count;++i)
        if(!strcmp(ibv_get_device_name(devices[i]),name)) device=devices[i];
    struct ibv_context *ctx=device?ibv_open_device(device):NULL;
    if(!mapping_preflight(ctx)) _exit(72);
    struct ibv_cq *cq=ibv_create_cq(ctx,31,NULL,NULL,0);
    if(!cq) _exit(73);
    uint64_t offset=((uint64_t)cq->handle+1)*MCDMA_CQ_MAP_BYTES;
    void *p=darwin_mmap(NULL,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_SHARED,ctx->cmd_fd,(int64_t)offset);
    if(p==MAP_FAILED) _exit(74);
    volatile unsigned char *bytes=p;
    unsigned char before=bytes[0];
    const char readable='R';
    if(write(STDOUT_FILENO,&readable,1)!=1) _exit(75);
    bytes[0]=(unsigned char)(before^1u);
    _exit(76); // Reaching this is a failed protection check.
}
static void child_write_fault(const char *executable,const char *device) {
    int channel[2];
    if (pipe(channel)) { check(0,"child_pipe"); return; }
    posix_spawn_file_actions_t actions;
    int error=posix_spawn_file_actions_init(&actions);
    if(error) { close(channel[0]);close(channel[1]);check(0,"child_spawn_actions");return; }
    error=posix_spawn_file_actions_addclose(&actions,channel[0]);
    if(!error) error=posix_spawn_file_actions_adddup2(&actions,channel[1],STDOUT_FILENO);
    if(!error && channel[1]!=STDOUT_FILENO) error=posix_spawn_file_actions_addclose(&actions,channel[1]);
    char *args[]={(char *)executable,"--write-fault",(char *)device,NULL};
    pid_t child=-1;
    posix_spawnattr_t attributes;
    int attr_error=posix_spawnattr_init(&attributes);
    if(!error) error=attr_error;
    if(!attr_error) {
        sigset_t mask,defaults; sigemptyset(&mask); sigemptyset(&defaults);
        sigaddset(&defaults,SIGBUS);sigaddset(&defaults,SIGSEGV);
        sigaddset(&defaults,SIGALRM);sigaddset(&defaults,SIGPIPE);
        if(!error) error=posix_spawnattr_setsigmask(&attributes,&mask);
        if(!error) error=posix_spawnattr_setsigdefault(&attributes,&defaults);
        if(!error) error=posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
        if(!error) error=posix_spawn(&child,executable,&actions,&attributes,args,environ);
        posix_spawnattr_destroy(&attributes);
    }
    posix_spawn_file_actions_destroy(&actions);close(channel[1]);
    int status=0; pid_t waited=-1;
    int timed_out=0;
    if(!error) {
        struct timespec start={0},now={0};
        int clock_error=clock_gettime(CLOCK_MONOTONIC,&start);
        for(;;) {
            waited=waitpid(child,&status,WNOHANG);
            if(waited==child || (waited<0 && errno!=EINTR)) break;
            if(clock_error || clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec-start.tv_sec>=8) {
                timed_out=1;kill(child,SIGKILL);
                do {waited=waitpid(child,&status,0);} while(waited<0 && errno==EINTR);
                break;
            }
            usleep(10000);
        }
    }
    char readable=0; ssize_t got=read(channel[0],&readable,1);close(channel[0]);
    printf("CQ_MAP_CHILD spawn=%d waited=%d read_marker=%d signal=%d exit=%d timeout=%d\n",
           error,waited==child,got==1 && readable=='R',
           waited==child && WIFSIGNALED(status)?WTERMSIG(status):0,
           waited==child && WIFEXITED(status)?WEXITSTATUS(status):-1,timed_out);
    check(!error && !timed_out && waited==child && got==1 && readable=='R' && WIFSIGNALED(status) &&
          (WTERMSIG(status)==SIGBUS || WTERMSIG(status)==SIGSEGV),"child_read_succeeds_write_faults");
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    setenv("MCDMA_CQ_MAP","0",1);
    if(argc==3 && !strcmp(argv[1],"--write-fault")) write_fault_child(argv[2]);
    if (argc!=2) { fprintf(stderr,"usage: %s rdma_mcrdma1\n",argv[0]); return 2; }
    setenv("MCDMA_CQ_MAP","0",1);
    int count=0; struct ibv_device **devices=ibv_get_device_list(&count);
    struct ibv_device *device=NULL;
    for (int i=0;devices && i<count;++i) if (!strcmp(ibv_get_device_name(devices[i]),argv[1])) device=devices[i];
    if (!device) { fprintf(stderr,"Device not found\n"); return 2; }
    struct ibv_context *ctx=ibv_open_device(device),*other=ibv_open_device(device);
    if (!ctx || !other) { fprintf(stderr,"Context open failed\n"); return 2; }
    if (!mapping_preflight(ctx) || !mapping_preflight(other)) {
        fputs("CQ_MAP_PREFLIGHT rejected: requires this ABI2 mapping provider; no mmap attempted\n",stderr);
        ibv_close_device(other);ibv_close_device(ctx);ibv_free_device_list(devices);return 2;
    }
    puts("CQ_MAP_PREFLIGHT supported=1");
    struct ibv_cq *cq=ibv_create_cq(ctx,31,NULL,NULL,0);
    if (!cq) { perror("ibv_create_cq"); return 2; }
    uint64_t offset=((uint64_t)cq->handle+1)*MCDMA_CQ_MAP_BYTES;
    rejected(other,offset,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_SHARED,"foreign_context");
    rejected(ctx,offset,MCDMA_CQ_MAP_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,"writable_request");
    rejected(ctx,offset,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_PRIVATE,"private_request");
    rejected(ctx,offset,8192,PROT_READ,MAP_SHARED,"wrong_length");
    rejected(ctx,0,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_SHARED,"invalid_handle");
    void *p=darwin_mmap(NULL,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_SHARED,ctx->cmd_fd,(int64_t)offset);
    check(p!=MAP_FAILED,"read_mapping");
    if (p==MAP_FAILED) goto cleanup;
    check(mcdma_cq_observe(p)==0,"empty_live_cq");
    child_write_fault(argv[0],argv[1]);
    mach_vm_address_t region=(mach_vm_address_t)p; mach_vm_size_t size=0;
    vm_region_basic_info_data_64_t info={0}; mach_msg_type_number_t entries=VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object=MACH_PORT_NULL;
    kern_return_t kr=mach_vm_region(mach_task_self(),&region,&size,VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&info,&entries,&object);
    if (object!=MACH_PORT_NULL) mach_port_deallocate(mach_task_self(),object);
    printf("CQ_MAP_VM result=%d current=%d maximum=%d size=%llu\n",kr,info.protection,info.max_protection,(unsigned long long)size);
    check(kr==KERN_SUCCESS && !(info.protection&(VM_PROT_WRITE|VM_PROT_EXECUTE)) &&
          !(info.max_protection&(VM_PROT_WRITE|VM_PROT_EXECUTE)),"maximum_read_only");
    errno=0; int upgraded=mprotect(p,MCDMA_CQ_MAP_BYTES,PROT_READ|PROT_WRITE);
    check(upgraded!=0,"mprotect_write_rejected");
    if (!upgraded) (void)mprotect(p,MCDMA_CQ_MAP_BYTES,PROT_READ);
    kr=mach_vm_protect(mach_task_self(),(mach_vm_address_t)p,MCDMA_CQ_MAP_BYTES,FALSE,VM_PROT_READ|VM_PROT_WRITE);
    check(kr!=KERN_SUCCESS,"mach_write_rejected");
    if (!kr) (void)mach_vm_protect(mach_task_self(),(mach_vm_address_t)p,MCDMA_CQ_MAP_BYTES,FALSE,VM_PROT_READ);
    struct ibv_pd *pd=ibv_alloc_pd(ctx);
    check(pd!=NULL,"pd_for_permission_test");
    if (pd) {
        struct ibv_mr *mr=ibv_reg_mr(pd,p,MCDMA_CQ_MAP_BYTES,IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE);
        check(mr==NULL,"writable_mr_rejected");
        if (mr) check(ibv_dereg_mr(mr)==0,"unexpected_mr_cleanup");
        check(ibv_dealloc_pd(pd)==0,"pd_cleanup");
    }
    check(ibv_destroy_cq(cq)==0,"destroy_with_external_mapping"); cq=NULL;
    check(mcdma_cq_observe(p)==-1,"retired_mapping_invalidated");
    rejected(ctx,offset,MCDMA_CQ_MAP_BYTES,PROT_READ,MAP_SHARED,"map_after_destroy");
    check(darwin_munmap(ctx->cmd_fd,p)==0,"unmap_retired_cq");
cleanup:
    if (cq) check(ibv_destroy_cq(cq)==0,"cq_cleanup");
    check(ibv_close_device(other)==0,"other_context_cleanup");
    check(ibv_close_device(ctx)==0,"context_cleanup"); ibv_free_device_list(devices);
    printf("CQ_MAP_CHECK failures=%d transfers_tested=0\n",failures);
    return failures?1:0;
}
