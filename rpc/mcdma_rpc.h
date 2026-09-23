/* libmcdma-rpc, ABI 1: the ordered word operations an application needs to
 * use an mcdma-rpcd mailbox without opening a verbs context itself.
 *
 * A mailbox word is (seq << 32 | length); sequence 0 means empty. Payloads are
 * written before their word, so words are stored with release ordering and
 * read with acquire ordering. docs/link-daemon.md describes the mailbox. */
#ifndef MCDMA_RPC_H
#define MCDMA_RPC_H

#include <stdint.h>

#define MCDMA_RPC_ABI 1u

#ifdef __cplusplus
extern "C" {
#endif

/* The ABI this library implements; callers refuse any other value. */
uint32_t mcdma_rpc_abi(void);

/* Wait until the word's sequence equals `seq` (want_equal != 0) or is non-zero
 * and differs from `seq` (want_equal == 0). Returns the word, or 0 once
 * timeout_ns has passed. Spins for spin_ns, then sleeps in short steps. */
uint64_t mcdma_rpc_wait_word(const volatile uint64_t *word, uint32_t seq, int want_equal, uint64_t spin_ns,
                             uint64_t timeout_ns);

/* Store `value` so that every earlier store to the mailbox is visible first. */
void mcdma_rpc_store_word(volatile uint64_t *word, uint64_t value);

#if defined(__APPLE__)
#include <stddef.h>

/* A Metal buffer over `length` bytes at `memory` without a copy, for a GPU to read mailbox memory
 * directly; NULL unless it aliases `memory`. Pass page-aligned memory, as Metal documents.
 * Optional: older helpers lack these three functions. */
void *mcdma_rpc_metal_wrap(void *memory, size_t length);

/* The buffer's CPU address, which equals `memory` for a buffer from mcdma_rpc_metal_wrap. */
void *mcdma_rpc_metal_contents(void *buffer);

/* Release a buffer from mcdma_rpc_metal_wrap; the memory itself stays mapped. */
void mcdma_rpc_metal_release(void *buffer);
#endif

#ifdef __cplusplus
}
#endif

#endif
