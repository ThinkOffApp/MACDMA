/* Offline test of libmcdma-rpc: waits see stores made by another thread, match
 * the requested sequence rule and time out without a device. */
#define _POSIX_C_SOURCE 200809L
#include "../rpc/mcdma_rpc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WORD(seq, len) (((uint64_t)(uint32_t)(seq) << 32) | (uint32_t)(len))

static volatile uint64_t g_word;
static unsigned char g_payload[4096];

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "test_rpc_helper: %s\n", what);
        exit(1);
    }
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Writes a payload, then publishes its word; the waiter must see the payload. */
static void *publisher(void *arg) {
    uint32_t seq = *(const uint32_t *)arg;
    struct timespec pause = {0, 2000000};
    nanosleep(&pause, NULL);
    for (size_t i = 0; i < sizeof(g_payload); ++i) g_payload[i] = (unsigned char)(seq + i);
    mcdma_rpc_store_word(&g_word, WORD(seq, sizeof(g_payload)));
    return NULL;
}

int main(void) {
    check(mcdma_rpc_abi() == MCDMA_RPC_ABI, "ABI");

    g_word = 0;
    check(mcdma_rpc_wait_word(&g_word, 7, 1, 1000, 2000000) == 0, "an empty word must time out");
    check(mcdma_rpc_wait_word(&g_word, 7, 0, 1000, 2000000) == 0, "an empty word is never a new request");

    mcdma_rpc_store_word(&g_word, WORD(7, 12));
    check(mcdma_rpc_wait_word(&g_word, 7, 1, 1000, 2000000) == WORD(7, 12), "equal match returns the word");
    check(mcdma_rpc_wait_word(&g_word, 7, 0, 1000, 2000000) == 0, "the same sequence is not a new request");
    check(mcdma_rpc_wait_word(&g_word, 6, 0, 1000, 2000000) == WORD(7, 12), "a different sequence is new");

    uint64_t began = now_ns();
    check(mcdma_rpc_wait_word(&g_word, 9, 1, 0, 30000000) == 0, "a wrong sequence times out");
    check(now_ns() - began >= 30000000, "the timeout is honoured");

    for (uint32_t seq = 100; seq < 164; ++seq) {
        pthread_t thread;
        check(pthread_create(&thread, NULL, publisher, &seq) == 0, "thread");
        uint64_t got = mcdma_rpc_wait_word(&g_word, seq, 1, 1000000, 2000000000ull);
        check(got == WORD(seq, sizeof(g_payload)), "a store from another thread is seen");
        for (size_t i = 0; i < sizeof(g_payload); ++i)
            check(g_payload[i] == (unsigned char)(seq + i), "the payload is visible before its word");
        pthread_join(thread, NULL);
    }
    puts("test_rpc_helper: ok");
    return 0;
}
