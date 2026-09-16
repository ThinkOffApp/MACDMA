// Original userspace completion engine for the read-only CQ mapping.
//
// The kernel remains the owner of every queue: it validates and posts WRs,
// consumes CQEs, updates the doorbell record and enforces credits. This
// engine lets the userspace provider report a completion as soon as the NIC
// has published its CQE, from an exact mirror of the kernel's work records,
// and then verifies each reported completion against the kernel's own
// consumption of the same ring entries. Any disagreement retires the CQ;
// nothing here fabricates a completion or advances hardware state.
#pragma once
#include <stdint.h>
#include <string.h>

#define MCDMA_USER_SLOTS 32u
#define MCDMA_USER_MAX_PENDING 31u

struct mcdma_user_record {
    uint64_t id;
    uint32_t length;
    uint16_t counter;
    uint8_t opcode;   // Hardware opcode: 0x08 WRITE, 0x10 READ, 0x0a SEND/RECV.
    uint8_t occupied;
};
// Mirrors the kernel WorkQueue: a slot is reusable only once that exact
// 16-bit counter has completed, and at most 31 records are outstanding.
struct mcdma_user_queue {
    struct mcdma_user_record records[MCDMA_USER_SLOTS];
    uint32_t producer;
    uint32_t pending;
};
struct mcdma_user_completion {
    uint64_t id;
    uint32_t status, opcode, vendor, bytes, qpn;
    // Hardware WQE counter and whether the QP is user-posted: for such QPs the
    // kernel can only report the counter, so the cross-check compares that.
    uint32_t counter; int user;
    // Public wc flags (2: with immediate) and the raw immediate bits.
    uint32_t flags, immediate;
};

static inline void mcdma_user_queue_reset(struct mcdma_user_queue *q) {
    memset(q,0,sizeof(*q));
}
static inline int mcdma_user_queue_can_post(const struct mcdma_user_queue *q) {
    return q->pending<MCDMA_USER_MAX_PENDING && !q->records[q->producer&31u].occupied;
}
// Records one accepted WR at the kernel's next index; returns 0 when the
// mirror cannot accept it (the kernel would also refuse the post).
static inline int mcdma_user_queue_post(struct mcdma_user_queue *q,uint64_t id,uint8_t opcode,uint32_t length) {
    if (!mcdma_user_queue_can_post(q) || !length) return 0;
    struct mcdma_user_record *r=&q->records[q->producer&31u];
    r->id=id; r->length=length; r->counter=(uint16_t)q->producer; r->opcode=opcode; r->occupied=1;
    ++q->producer; ++q->pending; return 1;
}
// Removes the newest `count` records that the kernel did not accept.
static inline void mcdma_user_queue_rollback(struct mcdma_user_queue *q,unsigned count) {
    while (count-- && q->producer) {
        struct mcdma_user_record *r=&q->records[(q->producer-1)&31u];
        if (!r->occupied || r->counter!=(uint16_t)(q->producer-1)) break;
        memset(r,0,sizeof(*r)); --q->producer; --q->pending;
    }
}
static inline int mcdma_user_queue_complete(struct mcdma_user_queue *q,uint16_t counter,struct mcdma_user_record *out) {
    struct mcdma_user_record *r=&q->records[counter&31u];
    if (!r->occupied || r->counter!=counter) return 0;
    *out=*r; memset(r,0,sizeof(*r)); --q->pending; return 1;
}

// NVIDIA CQE syndromes mapped to public verbs status values, identical to the
// kernel provider's table so the cross-check below compares like with like.
static inline uint32_t mcdma_user_status(uint8_t syndrome) {
    switch (syndrome) {
    case 0x01: return 1; case 0x02: return 2; case 0x04: return 4;
    case 0x05: return 5; case 0x06: return 6; case 0x10: return 7;
    case 0x11: return 8; case 0x12: return 9; case 0x13: return 10;
    case 0x14: return 11; case 0x15: return 12; case 0x16: return 13;
    case 0x22: return 16; default: return 21;
    }
}
static inline uint32_t mcdma_user_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
struct mcdma_user_qp_view { struct mcdma_user_queue *sends,*receives; int user_posted; };
typedef struct mcdma_user_qp_view (*mcdma_user_lookup)(void *context,uint32_t qpn);

// Decodes one published hardware CQE exactly as the kernel provider does:
// returns 1 with the completion, or -1 when the entry does not correspond to
// a recorded request (unknown QP, wrong queue, stale or duplicate counter,
// unsupported opcode). A -1 means the CQ can no longer be trusted from
// userspace; the caller must stop reporting and let the kernel decide.
static inline int mcdma_user_decode(const uint8_t cqe[64],mcdma_user_lookup lookup,void *context,
                                    struct mcdma_user_completion *out) {
    memset(out,0,sizeof(*out));
    const uint8_t opcode=cqe[63]>>4;
    if (opcode==15) return -1;
    if (opcode>4 && opcode!=13 && opcode!=14) return -1;
    const uint32_t qpn=mcdma_user_be32(cqe+56)&0xffffffu;
    const uint16_t counter=(uint16_t)(((uint16_t)cqe[60]<<8)|cqe[61]);
    const int send=opcode==0 || opcode==13;
    struct mcdma_user_qp_view view=lookup(context,qpn);
    struct mcdma_user_queue *queue=send ? view.sends : view.receives;
    if (!queue) return -1;
    struct mcdma_user_record record;
    if (!mcdma_user_queue_complete(queue,counter,&record)) return -1;
    out->id=record.id; out->qpn=qpn; out->counter=counter; out->user=view.user_posted;
    // Receive formats: 2 SEND, 3 SEND with immediate, 1 WRITE with immediate
    // (public opcode 129, byte count is the written length). Send side: the
    // recorded opcode decides, exactly as in the kernel provider.
    const int with_immediate=!send && (opcode==1 || opcode==3);
    out->opcode=send ? ((record.opcode==0x08 || record.opcode==0x09) ? 1u : record.opcode==0x10 ? 2u : 0u)
                     : (opcode==1 ? 129u : 128u);
    if (opcode==13 || opcode==14) {
        out->status=mcdma_user_status(cqe[55]);
        out->vendor=((uint32_t)cqe[54]<<8)|cqe[55];
    } else if (!send && opcode!=2 && !with_immediate) {
        // Invalidate receive formats are not supported yet.
        out->status=21; out->vendor=opcode;
    } else if (!send) {
        const uint32_t bytes=mcdma_user_be32(cqe+44);
        if (opcode!=1 && bytes>record.length) out->status=1;
        else {
            out->bytes=bytes;
            if (with_immediate) { out->flags=2u; memcpy(&out->immediate,cqe+40,4); }
        }
    } else if (record.opcode==0x10) out->bytes=record.length;
    return 1;
}
