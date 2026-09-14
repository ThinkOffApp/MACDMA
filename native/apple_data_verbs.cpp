#include "apple_data_verbs.hpp"
#include <sys/errno.h>

namespace cx5_native {
namespace {
uint32_t completion_status(uint8_t syndrome) {
    // NVIDIA CQE syndromes mapped to public verbs completion status values.
    switch (syndrome) {
    case 0x01: return 1; case 0x02: return 2; case 0x04: return 4;
    case 0x05: return 5; case 0x06: return 6; case 0x10: return 7;
    case 0x11: return 8; case 0x12: return 9; case 0x13: return 10;
    case 0x14: return 11; case 0x15: return 12; case 0x16: return 13;
    case 0x22: return 16; default: return 21;
    }
}
bool usable_sge(const AppleSGE *sge,int count) {
    return count==1 && sge && sge->length && sge->lkey &&
        sge->address<=UINT64_MAX-sge->length;
}
}

int apple_post_send(Hca &hca,HardwareQP &qp,bool signal_all,const AppleSendWR *wr,const AppleSendWR **bad) {
    if (!bad) return -EINVAL;
    *bad=nullptr;
    for (;wr;wr=wr->next) {
        int error=0; uint8_t opcode=0;
        if (!qp.client_context || !qp.object.live || qp.state!=3 || !hca.transport.ready()) error=-EIO;
        else if (!usable_sge(wr->sge,wr->sge_count)) error=-EINVAL;
        else if ((wr->flags&~2u) || (!signal_all && !(wr->flags&2))) error=-EOPNOTSUPP;
        else {
            // Public verbs WRITE=0, SEND=2, READ=4; CX5 hardware encodings differ.
            switch (wr->opcode) {
            case 0: opcode=0x08; break; case 2: opcode=0x0a; break;
            case 4: opcode=0x10; break; default: error=-EOPNOTSUPP; break;
            }
        }
        uint64_t remote=0; uint32_t rkey=0;
        if (!error && opcode!=0x0a) {
            const auto *rdma=reinterpret_cast<const AppleRDMAWR *>(wr);
            remote=rdma->remote; rkey=rdma->rkey;
            if (!rkey || remote>UINT64_MAX-wr->sge->length) error=-EINVAL;
        }
        if (!error && (!qp.sends.can_post(qp.producer) || qp.send_cq->outstanding>=31)) error=-ENOMEM;
        if (!error && !hca.post(qp,wr->id,opcode,wr->sge->address,wr->sge->lkey,
                              wr->sge->length,remote,rkey)) error=-EIO;
        if (error) { *bad=wr; return error; }
    }
    return 0;
}

int apple_post_recv(Hca &hca,HardwareQP &qp,const AppleRecvWR *wr,const AppleRecvWR **bad) {
    if (!bad) return -EINVAL;
    *bad=nullptr;
    for (;wr;wr=wr->next) {
        int error=0;
        if (!qp.client_context || !qp.object.live || qp.state<1 || !hca.transport.ready()) error=-EIO;
        else if (!usable_sge(wr->sge,wr->sge_count)) error=-EINVAL;
        else if (!qp.receives.can_post(qp.recv_producer) || qp.recv_cq->outstanding>=31) error=-ENOMEM;
        if (!error && !hca.receive(qp,wr->id,wr->sge->address,wr->sge->length,wr->sge->lkey)) error=-EIO;
        if (error) { *bad=wr; return error; }
    }
    return 0;
}

int apple_poll_cq(Hca &hca,HardwareCQ &cq,int maximum,AppleWC *output) {
    if (maximum<0 || (maximum && !output)) return -EINVAL;
    int count=0;
    while (count<maximum) {
        cx5::Completion hardware{}; cx5::WorkRecord work{};
        void *qp=nullptr; bool user=false;
        const auto result=hca.poll(cq,hardware,work,&qp,&user);
        if (result==cx5::CQResult::empty) break;
        if (result==cx5::CQResult::unsupported) return count ? count : -EIO;
        if (!qp) { hca.transport.quarantined=true; return count ? count : -EIO; }
        AppleWC completion{};
        completion.id=work.id; completion.qp=qp; completion.port=1;
        const bool receive=hardware.opcode!=0 && hardware.opcode!=13;
        // A user-posted QP has no kernel record: the WQE counter is the ID, the
        // send opcode is unknown here, and the byte count is the hardware's.
        completion.opcode=receive ? 128 : user ? 0 : work.opcode==0x08 ? 1 : work.opcode==0x10 ? 2 : 0;
        if (result==cx5::CQResult::error) {
            completion.status=completion_status(hardware.syndrome);
            completion.vendor=(uint32_t(hardware.vendor_syndrome)<<8)|hardware.syndrome;
        } else if (receive && hardware.opcode!=2) {
            // Immediate/invalidate receive formats are not supported yet.
            // Consume the real completion, but never report plain SEND success.
            completion.status=21; completion.vendor=hardware.opcode;
        } else if (receive && !user && hardware.bytes>work.length) {
            completion.status=1; // A receive cannot successfully exceed its posted buffer.
        } else if (receive) completion.bytes=hardware.bytes;
        else if (!user && work.opcode==0x10) completion.bytes=work.length;
        output[count++]=completion;
    }
    return count;
}
}
