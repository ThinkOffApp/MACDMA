/* Original native-librdma acceptance check; no substitute device enumeration. */
#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <arpa/inet.h>

int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    int require_active=0,require_gid=0,list_only=0;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--require-active")) require_active=1;
        else if (!strcmp(argv[i],"--require-gid")) require_active=require_gid=1;
        else if (!strcmp(argv[i],"--list-only")) list_only=1;
        else if (!strcmp(argv[i],"--provider") && i+1<argc) {
            const char *path=argv[++i];
            void *library=dlopen(path,RTLD_NOW|RTLD_GLOBAL);
            if (!library) { fprintf(stderr,"provider_load_error=%s\n",dlerror()); return 2; }
            /* The registered provider must remain loaded for the process lifetime. */
            printf("provider_loaded=%s\n",path);
            int (*check)(void)=dlsym(library,"mcdma_provider_abi_check");
            if (check) {
                int error=check(); printf("provider_ops_abi_check=%d hardware_test=0\n",error);
                if (error) return 2;
            }
        } else { fprintf(stderr,"usage: %s [--require-active | --require-gid | --list-only] [--provider /path/to/provider]\n",argv[0]); return 2; }
    }
    if (list_only && require_active) { fprintf(stderr,"--list-only cannot verify active ports\n"); return 2; }
    int count = 0, cx5 = 0, active = 0, errors = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices) { perror("ibv_get_device_list"); return 2; }
    printf("native_devices=%d\n", count);
    for (int i = 0; i < count; ++i) {
        printf("discovered=%s\n",ibv_get_device_name(devices[i]));
        if (list_only) continue;
        struct ibv_context *context = ibv_open_device(devices[i]);
        if (!context) { perror(ibv_get_device_name(devices[i])); ++errors; continue; }
        struct ibv_device_attr attr = {0};
        if (ibv_query_device(context, &attr)) {
            perror("ibv_query_device"); ++errors; ibv_close_device(context); continue;
        }
        const int match = attr.vendor_id == 0x15b3 && attr.vendor_part_id == 0x1019;
        cx5 += match;
        printf("device=%s vendor=0x%x part=0x%x ports=%u cx5=%d\n",
               ibv_get_device_name(devices[i]), attr.vendor_id,
               attr.vendor_part_id, attr.phys_port_cnt, match);
        for (unsigned p = 1; p <= attr.phys_port_cnt; ++p) {
            struct ibv_port_attr port = {0};
            int rc = ibv_query_port(context, (uint8_t)p, &port);
            if (rc) { fprintf(stderr, "query_port=%u error=%d\n", p, rc); ++errors; continue; }
            printf("port=%u state=%d physical=%u link_layer=%u\n",
                   p, port.state, port.phys_state, port.link_layer);
            if (match && port.state == IBV_PORT_ACTIVE) {
                ++active;
                if (require_gid) {
                    struct ibv_gid_entry gid={0};
                    char address[INET6_ADDRSTRLEN]={0};
                    const unsigned char zero[16]={0};
                    rc=ibv_query_gid_ex(context,p,0,&gid,0);
                    inet_ntop(AF_INET6,gid.gid.raw,address,sizeof(address));
                    printf("gid_index=0 error=%d address=%s type=%u ifindex=%u\n",
                           rc,address,gid.gid_type,gid.ndev_ifindex);
                    if (rc || !memcmp(gid.gid.raw,zero,16) ||
                        gid.gid_type!=IBV_GID_TYPE_ROCE_V2 || !gid.ndev_ifindex) ++errors;
                }
            }
        }
        if (ibv_close_device(context)) ++errors;
    }
    ibv_free_device_list(devices);
    if (list_only) { printf("enumeration_only=1 hardware_verbs_test=0\n"); return 0; }
    printf("native_cx5=%d native_cx5_active_ports=%d errors=%d\n", cx5, active, errors);
    /* Passing proves native discovery/query only; WRITE/READ requires a separate test. */
    return errors ? 2 : ((require_active ? active : cx5) ? 0 : 1);
}
