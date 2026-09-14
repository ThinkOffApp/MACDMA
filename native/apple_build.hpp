#pragma once
#ifdef CX5_NATIVE_TEST
namespace cx5_native { bool supported_build(); }
#else
#include <libkern/sysctl.h>
#include "../include/apple_build_ids.h"

namespace cx5_native {
inline const char *verified_build_name() {
    // osversion is declared by the SDK but is not exported by this kernel;
    // use the public kernel sysctl API rather than an unresolved data import.
    char build[32]{}; size_t size=sizeof(build);
    if (sysctlbyname("kern.osversion",build,&size,nullptr,0) || size>sizeof(build)) return nullptr;
    return mcdma_verified_apple_build(build,size);
}
inline bool supported_build() { return verified_build_name()!=nullptr; }
}
#endif
