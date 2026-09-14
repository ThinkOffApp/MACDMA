#pragma once
#include "kernel_hca.hpp"
#include <stddef.h>

namespace cx5_native {
// Layouts observed in the owner's 26A5425a IORDMAFamily request decoding and
// AppleThunderboltRDMAKernelCQ::poll, not imported private kernel headers.
// These are kernel callback arguments AFTER core copyin/handle validation;
// they must never be applied directly to untrusted userspace pointers.
struct AppleSGE { uint64_t address; uint32_t length, lkey; };
struct AppleSendWR {
    const AppleSendWR *next;
    uint64_t id;
    const AppleSGE *sge;
    int32_t sge_count, opcode;
    uint32_t flags, immediate;
};
struct AppleRDMAWR { AppleSendWR base; uint64_t remote; uint32_t rkey, reserved; };
struct AppleRecvWR {
    const AppleRecvWR *next;
    uint64_t id;
    const AppleSGE *sge;
    int32_t sge_count;
};
struct AppleWC {
    uint64_t id;
    uint32_t status, opcode, vendor, bytes;
    void *qp;
    uint32_t immediate, source_qp, slid, flags;
    uint16_t pkey;
    uint8_t sl, path;
    uint32_t port;
    uint8_t remaining[16];
};
static_assert(sizeof(AppleSGE)==16 && sizeof(AppleSendWR)==0x28);
static_assert(offsetof(AppleSendWR,sge)==0x10 && offsetof(AppleSendWR,opcode)==0x1c);
static_assert(offsetof(AppleRDMAWR,remote)==0x28 && offsetof(AppleRDMAWR,rkey)==0x30);
static_assert(offsetof(AppleRecvWR,sge_count)==0x18);
static_assert(sizeof(AppleWC)==0x48 && offsetof(AppleWC,qp)==0x18);
static_assert(offsetof(AppleWC,immediate)==0x20 && offsetof(AppleWC,flags)==0x2c);
static_assert(offsetof(AppleWC,pkey)==0x30 && offsetof(AppleWC,port)==0x34);

// The enclosing native provider must hold its HCA lock for each call, validate
// its Apple build, bind the CQ/QP to that device, and keep qp.client_context
// pointing at the real ib_qp until all completions have been consumed.
// Limited first data path: RC SEND/WRITE/READ, one SGE, signalled/polled work.
int apple_post_send(Hca &,HardwareQP &,bool signal_all,const AppleSendWR *,const AppleSendWR **bad);
int apple_post_recv(Hca &,HardwareQP &,const AppleRecvWR *,const AppleRecvWR **bad);
int apple_poll_cq(Hca &,HardwareCQ &,int maximum,AppleWC *);
}
