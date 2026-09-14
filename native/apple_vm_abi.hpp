#pragma once
// The public Kernel.framework SDK strips the KERNEL_PRIVATE branch of
// mach/vm_types.h, even in SDK 27: vm_map_t becomes ipc_port*. The installed
// 26A5425a IOMemoryDescriptor vtable instead exports doMap/doUnmap(_vm_map*).
// Preserve the SDK's other declarations and replace only these opaque aliases
// before any IOKit class is parsed, consistently across kernel translation units.
// No private struct layout is declared, and no address conversion is performed.
// Reference: Apple's public XNU osfmk/mach/vm_types.h, KERNEL_PRIVATE branch;
// installed kernel symbolsets are the actual build-specific ABI authority.
#if !defined(KERNEL) || !defined(KERNEL_PRIVATE)
#error This alias correction is only for our build-gated kernel backend
#endif
#define vm_map_t mcdma_sdk_vm_map_t
#define vm_map_read_t mcdma_sdk_vm_map_read_t
#define vm_map_inspect_t mcdma_sdk_vm_map_inspect_t
#include <mach/vm_types.h>
#undef vm_map_t
#undef vm_map_read_t
#undef vm_map_inspect_t
struct _vm_map;
typedef _vm_map *vm_map_t, *vm_map_read_t, *vm_map_inspect_t;
