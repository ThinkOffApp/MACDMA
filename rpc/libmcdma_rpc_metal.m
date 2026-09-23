/* libmcdma-rpc on macOS: Metal buffers over mailbox memory, so the GPU reads what the NIC
 * wrote without a copy. Callers import the buffer into their framework (MLX through DLPack). */
#import <Metal/Metal.h>

#include "mcdma_rpc.h"

void *mcdma_rpc_metal_wrap(void *memory, size_t length) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || memory == NULL || length == 0) return NULL;
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:memory
                                                     length:length
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
    // A buffer that is not the caller's memory would defeat the point; callers get NULL instead.
    if (buffer == nil || [buffer contents] != memory) return NULL;
    return (__bridge_retained void *)buffer;
}

void *mcdma_rpc_metal_contents(void *buffer) {
    return buffer == NULL ? NULL : [(__bridge id<MTLBuffer>)buffer contents];
}

void mcdma_rpc_metal_release(void *buffer) {
    if (buffer != NULL) CFRelease(buffer);
}
