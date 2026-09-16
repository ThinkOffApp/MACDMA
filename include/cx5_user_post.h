// Original userspace posting helpers for a user-posted QP whose work-queue
// page (receive queue, send queue, doorbell record) and whose context UAR
// page are mapped into the process. Byte layouts follow the public NVIDIA
// formats already used by the kernel encoder (core/cx5_verbs.cpp), so the
// hardware sees identical WQEs whichever side wrote them.
#pragma once
#include <stdint.h>
#include <string.h>
#if !defined(KERNEL) && defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#define MCDMA_QUEUE_PAGE_BYTES 16384u
#define MCDMA_RQ_OFFSET 0u          /* 32 receive entries of 16 bytes. */
#define MCDMA_SQ_OFFSET 512u        /* 32 send WQEBBs of 64 bytes. */
#define MCDMA_DBR_OFFSET 4096u      /* Doorbell record: RQ counter, SQ counter. */
#define MCDMA_UAR_DOORBELL 0x800u   /* First BlueFlame register of a 4 KiB UAR. */
#define MCDMA_WQE_WRITE 0x08u
#define MCDMA_WQE_WRITE_IMM 0x09u
#define MCDMA_WQE_SEND 0x0au
#define MCDMA_WQE_SEND_IMM 0x0bu
#define MCDMA_WQE_READ 0x10u
/* Single-WQEBB limits shared with the kernel encoder (cx5_verbs.hpp). */
#define MCDMA_MAX_SEND_SGE 3u
#define MCDMA_MAX_RDMA_SGE 2u
#define MCDMA_MAX_INLINE_SEND 44u
#define MCDMA_MAX_INLINE_RDMA 28u
#define MCDMA_SEND_FENCE 1u
#define MCDMA_SEND_SIGNALED 2u
#define MCDMA_SEND_SOLICITED 4u
#define MCDMA_SEND_INLINE 8u
/* mmap page numbers shared with the kernel provider (16 KiB units). The
 * write-combined UAR page is the same firmware UAR mapped with the cache
 * attribute the kernel's own BlueFlame page uses; the kernel grants it only
 * when its personality allows userspace BlueFlame and its own BlueFlame
 * path is enabled on this HCA. A context maps one of the two, once. */
#define MCDMA_UAR_PAGE_NUMBER (1ull<<32)
#define MCDMA_UAR_WC_PAGE_NUMBER ((1ull<<32)+1)
#define MCDMA_QUEUE_PAGE_BASE (2ull<<32)
/* Capability block the kernel writes into a user-posted QP's work-queue page
 * beyond everything the hardware reads (queues, doorbell record) and beyond
 * what a reset clears. Little-endian; the provider reads it after mapping. */
#define MCDMA_INFO_OFFSET 8192u
#define MCDMA_INFO_MAGIC 0x4d434246u          /* "MCBF" */
#define MCDMA_INFO_UAR_WRITE_COMBINED 1u       /* This context's UAR page is write-combined. */
#define MCDMA_INFO_KERNEL_BLUEFLAME 2u         /* The kernel's own posts use BlueFlame. */
#define MCDMA_BF_BANKS 2u

static inline void mcdma_put_be32(uint8_t *p,uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static inline void mcdma_put_be64(uint8_t *p,uint64_t v) { mcdma_put_be32(p,(uint32_t)(v>>32)); mcdma_put_be32(p+4,(uint32_t)v); }
static inline void mcdma_put_le32(uint8_t *p,uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline uint32_t mcdma_get_le32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

// One send request in the hardware's terms, byte for byte the kernel
// encoder's SendRequest (cx5_verbs.hpp): opcode, fence/solicited flags,
// immediate (raw network-order bits), RDMA target, up to three scatter
// entries or inline bytes that fit the single WQEBB.
struct mcdma_send_sge { uint64_t address; uint32_t lkey,length; };
struct mcdma_send_request {
    uint8_t opcode,flags;
    uint32_t immediate;
    uint64_t remote; uint32_t rkey;
    struct mcdma_send_sge sge[3]; unsigned sge_count;
    const uint8_t *inline_data; unsigned inline_bytes;
};
static inline int mcdma_rdma_opcode(uint8_t opcode) {
    return opcode==MCDMA_WQE_WRITE || opcode==MCDMA_WQE_WRITE_IMM || opcode==MCDMA_WQE_READ;
}
// Total payload bytes of an encodable request, else 0 (same rules as the kernel).
static inline uint32_t mcdma_request_bytes(const struct mcdma_send_request *r) {
    const int rdma=mcdma_rdma_opcode(r->opcode);
    if (!rdma && r->opcode!=MCDMA_WQE_SEND && r->opcode!=MCDMA_WQE_SEND_IMM) return 0;
    if (r->flags&~(MCDMA_SEND_FENCE|MCDMA_SEND_SIGNALED|MCDMA_SEND_SOLICITED)) return 0;
    if (rdma && !r->rkey) return 0;
    uint64_t total=0;
    if (r->inline_data || r->inline_bytes) {
        if (!r->inline_data || r->sge_count || r->opcode==MCDMA_WQE_READ) return 0;
        if (r->inline_bytes<1 || r->inline_bytes>(rdma ? MCDMA_MAX_INLINE_RDMA : MCDMA_MAX_INLINE_SEND)) return 0;
        total=r->inline_bytes;
    } else {
        if (r->sge_count<1 || r->sge_count>(rdma ? MCDMA_MAX_RDMA_SGE : MCDMA_MAX_SEND_SGE)) return 0;
        for (unsigned i=0;i<r->sge_count;++i) {
            if (!r->sge[i].length || !r->sge[i].lkey || r->sge[i].address>UINT64_MAX-r->sge[i].length) return 0;
            total+=r->sge[i].length;
        }
    }
    if (!total || total>0x7fffffffu || (rdma && r->remote>UINT64_MAX-total)) return 0;
    return (uint32_t)total;
}
// Writes one 64-byte RC WQE at send-queue index `producer`; returns the first
// eight bytes as the doorbell value, or 0 when the request is not encodable.
static inline uint64_t mcdma_encode_send_request(uint8_t *queue_page,uint32_t qpn,uint32_t producer,
                                                 const struct mcdma_send_request *r) {
    const uint32_t total=mcdma_request_bytes(r);
    if (!queue_page || qpn>0xffffffu || !total) return 0;
    const int rdma=mcdma_rdma_opcode(r->opcode), inlined=r->inline_data!=NULL;
    const unsigned data_units=inlined ? (4u+r->inline_bytes+15u)/16u : r->sge_count;
    const unsigned ds=1u+(unsigned)rdma+data_units;
    if (ds>4u) return 0;
    uint8_t *wqe=queue_page+MCDMA_SQ_OFFSET+(producer&31u)*64;
    memset(wqe,0,64);
    mcdma_put_be32(wqe,((uint32_t)(uint16_t)producer<<8)|r->opcode);
    mcdma_put_be32(wqe+4,(qpn<<8)|ds);
    wqe[11]=(uint8_t)(8u|((r->flags&MCDMA_SEND_FENCE) ? 0x80u : 0u)|((r->flags&MCDMA_SEND_SOLICITED) ? 0x02u : 0u));
    if (r->opcode==MCDMA_WQE_WRITE_IMM || r->opcode==MCDMA_WQE_SEND_IMM) memcpy(wqe+12,&r->immediate,4);
    unsigned at=16;
    if (rdma) { mcdma_put_be64(wqe+16,r->remote); mcdma_put_be32(wqe+24,r->rkey); at=32; }
    if (inlined) {
        mcdma_put_be32(wqe+at,0x80000000u|r->inline_bytes);
        memcpy(wqe+at+4,r->inline_data,r->inline_bytes);
    } else for (unsigned i=0;i<r->sge_count;++i,at+=16) {
        mcdma_put_be32(wqe+at,r->sge[i].length); mcdma_put_be32(wqe+at+4,r->sge[i].lkey); mcdma_put_be64(wqe+at+8,r->sge[i].address);
    }
    uint64_t doorbell; memcpy(&doorbell,wqe,8); return doorbell;
}
// One-entry convenience (WRITE, SEND, READ), kept for the original callers.
static inline uint64_t mcdma_encode_send_wqe(uint8_t *queue_page,uint32_t qpn,uint32_t producer,uint8_t opcode,
                                             uint64_t local,uint32_t lkey,uint32_t length,uint64_t remote,uint32_t rkey) {
    if (opcode!=MCDMA_WQE_WRITE && opcode!=MCDMA_WQE_SEND && opcode!=MCDMA_WQE_READ) return 0;
    struct mcdma_send_request r; memset(&r,0,sizeof(r));
    r.opcode=opcode; r.remote=remote; r.rkey=rkey;
    r.sge[0].address=local; r.sge[0].lkey=lkey; r.sge[0].length=length; r.sge_count=1;
    return mcdma_encode_send_request(queue_page,qpn,producer,&r);
}
// Writes one 16-byte receive entry at receive-queue index `producer`.
static inline int mcdma_encode_recv_wqe(uint8_t *queue_page,uint32_t producer,uint64_t address,uint32_t length,uint32_t lkey) {
    if (!queue_page || !length || !lkey || address>UINT64_MAX-length) return 0;
    uint8_t *entry=queue_page+MCDMA_RQ_OFFSET+(producer&31u)*16;
    mcdma_put_be32(entry,length); mcdma_put_be32(entry+4,lkey); mcdma_put_be64(entry+8,address);
    return 1;
}
// Publishes the send producer count to the doorbell record, then rings the
// UAR doorbell with the last WQE's control qword. Ordering follows the kernel
// path: WQE bytes, then the record, then the device register.
static inline void mcdma_ring_send(uint8_t *queue_page,volatile uint8_t *uar_page,uint32_t producer_after,uint64_t doorbell) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
    mcdma_put_be32(queue_page+MCDMA_DBR_OFFSET+4,producer_after);
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
    *(volatile uint64_t *)(uar_page+MCDMA_UAR_DOORBELL)=doorbell;
}
static inline void mcdma_publish_recv(uint8_t *queue_page,uint32_t producer_after) {
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
    mcdma_put_be32(queue_page+MCDMA_DBR_OFFSET,producer_after);
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
}

// Kernel side: publishes the capability block of one user-posted QP. Bank
// bytes are reported only with a write-combined UAR page, so a context with
// an ordinary device-memory mapping never attempts a BlueFlame push.
static inline void mcdma_info_write(uint8_t *queue_page,uint32_t flags,uint32_t bank_bytes) {
    uint8_t *p=queue_page+MCDMA_INFO_OFFSET;
    memset(p,0,32);
    mcdma_put_le32(p,MCDMA_INFO_MAGIC); mcdma_put_le32(p+4,flags);
    mcdma_put_le32(p+8,(flags&MCDMA_INFO_UAR_WRITE_COMBINED) ? bank_bytes : 0);
    mcdma_put_le32(p+12,MCDMA_BF_BANKS); mcdma_put_le32(p+16,MCDMA_UAR_DOORBELL);
}
// Provider side: 1 with the block's fields when it is valid, else 0.
static inline int mcdma_info_read(const uint8_t *queue_page,uint32_t *flags,uint32_t *bank_bytes) {
    const uint8_t *p=queue_page+MCDMA_INFO_OFFSET;
    if (mcdma_get_le32(p)!=MCDMA_INFO_MAGIC || mcdma_get_le32(p+12)!=MCDMA_BF_BANKS ||
        mcdma_get_le32(p+16)!=MCDMA_UAR_DOORBELL) return 0;
    const uint32_t bytes=mcdma_get_le32(p+8);
    if (bytes && (bytes<128u || bytes>1024u || (bytes&(bytes-1)))) return 0;
    *flags=mcdma_get_le32(p+4); *bank_bytes=bytes; return 1;
}
// Ordinary doorbell through a write-combined UAR page: identical to
// mcdma_ring_send, plus the store barrier that drains the write-combining
// buffer immediately instead of whenever the core decides to.
static inline void mcdma_ring_send_wc(uint8_t *queue_page,volatile uint8_t *uar_page,uint32_t producer_after,uint64_t doorbell) {
    mcdma_ring_send(queue_page,uar_page,producer_after,doorbell);
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
}
// BlueFlame push of the single WQE at send-queue index `index`: doorbell
// record first (the fetch fallback stays valid), then `bytes` to one bank of
// the write-combined UAR page: 64 (exactly the WQEBB, as the vendor's
// userspace provider writes on arm64) or 128 (the WQEBB followed by 64 zero
// bytes, exactly what this driver's kernel path writes). With `vector` the
// 64-byte chunks go out as one four-register NEON store each; otherwise as
// eight-byte scalar stores like the kernel. The trailing barrier drains the
// write-combining buffer before the bank can be reused.
static inline void mcdma_ring_send_bf(uint8_t *queue_page,volatile uint8_t *uar_page,uint32_t producer_after,
                                      uint32_t index,uint32_t bank_offset,unsigned bytes,int vector) {
    const uint8_t *wqe=queue_page+MCDMA_SQ_OFFSET+(index&31u)*64;
    volatile uint8_t *bank=uar_page+bank_offset;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
    mcdma_put_be32(queue_page+MCDMA_DBR_OFFSET+4,producer_after);
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
    if (bytes!=64u && bytes!=128u) bytes=64u;
#if !defined(KERNEL) && defined(__aarch64__) && defined(__ARM_NEON)
    if (vector) {
        static const uint64_t zero[8]={0,0,0,0,0,0,0,0};
        for (unsigned chunk=0;chunk<bytes;chunk+=64u) {
            const uint64_t *source=chunk ? zero : (const uint64_t *)(const void *)wqe;
            vst4q_u64((uint64_t *)(bank+chunk),vld4q_u64(source));
        }
    } else
#else
    (void)vector;
#endif
    {
        for (unsigned i=0;i<bytes/8u;++i) {
            uint64_t word=0;
            if (i<8u) memcpy(&word,wqe+i*8u,8);
            *(volatile uint64_t *)(bank+i*8u)=word;
        }
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb oshst" ::: "memory");
#endif
}
