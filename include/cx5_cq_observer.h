// Original read-only completion observation ABI, shared by our kernel and
// userspace providers. Observation is only a hint: the kernel consumes CQEs.
#pragma once
#include <stdint.h>

#define MCDMA_CQ_MAP_BYTES 16384u
#define MCDMA_CQ_LIVE_OFFSET 8192u
#define MCDMA_CQ_LIVE_MAGIC 0x4d434401u

static inline void mcdma_cq_set_live(void *buffer, int live) {
    __atomic_store_n((uint32_t *)((uint8_t *)buffer+MCDMA_CQ_LIVE_OFFSET),
                     live ? MCDMA_CQ_LIVE_MAGIC : 0, __ATOMIC_RELEASE);
}
static inline void mcdma_cq_set_consumer(void *buffer, uint32_t consumer) {
    // The actual NIC doorbell record, in network byte order; one aligned
    // atomic store also makes observation safe across 24-bit wraparound.
    __atomic_store_n((uint32_t *)((uint8_t *)buffer+4096),
                     __builtin_bswap32(consumer & 0xffffffu), __ATOMIC_RELEASE);
}
// The kernel-published consumer index (24 bits, as the NIC doorbell record).
static inline uint32_t mcdma_cq_consumer(const void *buffer) {
    return __builtin_bswap32(__atomic_load_n(
        (const uint32_t *)((const uint8_t *)buffer+4096),__ATOMIC_ACQUIRE))&0xffffffu;
}
// Whether the hardware has published the entry at ring `index`: 1 when its
// owner phase matches and it is not the invalid opcode, 0 when the slot is
// still empty, -1 when the kernel has retired this CQ or is not ready.
static inline int mcdma_cq_entry_ready(const void *buffer, uint32_t index) {
    const uint8_t *bytes=(const uint8_t *)buffer;
    if (__atomic_load_n((const uint32_t *)(bytes+MCDMA_CQ_LIVE_OFFSET),
                        __ATOMIC_ACQUIRE)!=MCDMA_CQ_LIVE_MAGIC) return -1;
    const uint8_t owner=__atomic_load_n(bytes+(index&31u)*64+63,__ATOMIC_ACQUIRE);
    return (owner>>4)!=15 && (owner&1u)==((index>>5)&1u);
}
// Copies one published 64-byte entry after the owner byte has been observed
// with acquire ordering, so the remaining bytes are read after the NIC's write.
static inline int mcdma_cq_entry(const void *buffer, uint32_t index, uint8_t cqe[64]) {
    const int ready=mcdma_cq_entry_ready(buffer,index);
    if (ready!=1) return ready;
    const volatile uint8_t *entry=(const volatile uint8_t *)buffer+(index&31u)*64;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (unsigned i=0;i<64;++i) cqe[i]=entry[i];
    return 1;
}
static inline int mcdma_cq_observe(const void *buffer) {
    const uint8_t *bytes=(const uint8_t *)buffer;
    if (__atomic_load_n((const uint32_t *)(bytes+MCDMA_CQ_LIVE_OFFSET),
                        __ATOMIC_ACQUIRE)!=MCDMA_CQ_LIVE_MAGIC) return -1;
    const uint32_t consumer=__builtin_bswap32(
        __atomic_load_n((const uint32_t *)(bytes+4096),__ATOMIC_ACQUIRE));
    const uint8_t owner=__atomic_load_n(bytes+(consumer&31u)*64+63,__ATOMIC_ACQUIRE);
    return (owner>>4)!=15 && (owner&1u)==((consumer>>5)&1u);
}
