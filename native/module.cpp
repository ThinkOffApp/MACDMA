#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *,void *);
extern "C" kern_return_t _stop(kmod_info_t *,void *);
extern "C" {
KMOD_EXPLICIT_DECL(org.mcdma.cx5.native,"0.1.16",_start,_stop)
__private_extern__ kmod_start_func_t *_realmain=nullptr;
__private_extern__ kmod_stop_func_t *_antimain=nullptr;
__private_extern__ int _kext_apple_cc=__APPLE_CC__;
}
