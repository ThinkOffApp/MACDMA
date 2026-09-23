/* libmcdma-rpc: ordered loads and stores of mcdma-rpcd mailbox words.
 *
 * Applications that cannot express acquire/release ordering themselves (for
 * example Python through ctypes) call these instead of touching mailbox words
 * directly. The library holds no state and opens no device. */
#define _POSIX_C_SOURCE 200809L
#include "mcdma_rpc.h"

#include <time.h>

#define WORD_SEQ(w) ((uint32_t)((w) >> 32))

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline void relax(void) {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

uint32_t mcdma_rpc_abi(void) { return MCDMA_RPC_ABI; }

uint64_t mcdma_rpc_wait_word(const volatile uint64_t *word, uint32_t seq, int want_equal, uint64_t spin_ns,
                             uint64_t timeout_ns) {
    uint64_t start = now_ns();
    for (;;) {
        uint64_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
        uint32_t current = WORD_SEQ(value);
        if (want_equal ? current == seq : (current != 0 && current != seq)) return value;
        uint64_t elapsed = now_ns() - start;
        if (elapsed >= timeout_ns) return 0;
        if (elapsed < spin_ns) {
            relax();
        } else {
            struct timespec pause = {0, 20000};
            nanosleep(&pause, NULL);
        }
    }
}

void mcdma_rpc_store_word(volatile uint64_t *word, uint64_t value) { __atomic_store_n(word, value, __ATOMIC_RELEASE); }
