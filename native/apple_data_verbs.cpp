#include "apple_data_verbs.hpp"
#include <sys/errno.h>
#include <string.h>

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
bool usable_sge(const AppleSGE &sge) {
    return sge.length && sge.lkey && sge.address<=UINT64_MAX-sge.length;
}
// Public verbs send opcodes: WRITE 0, WRITE_WITH_IMM 1, SEND 2, SEND_WITH_IMM 3, READ 4.
uint8_t hardware_opcode(int32_t opcode) {
    switch (opcode) {
    case 0: return cx5::wqe_write; case 1: return cx5::wqe_write_imm;
    case 2: return cx5::wqe_send; case 3: return cx5::wqe_send_imm;
    case 4: return cx5::wqe_read; default: return 0;
    }
}
}
bool apple_send_request(const AppleSendWR *wr,bool signal_all,cx5::SendRequest &request,int &error) {
    request={};
    error=0;
    const uint8_t opcode=hardware_opcode(wr->opcode);
    if (!opcode) { error=-EOPNOTSUPP; return false; }
    // Every request completes signalled; inline bytes cannot be read here.
    if ((wr->flags&~unsigned(cx5::send_flag_fence|cx5::send_flag_signaled|cx5::send_flag_solicited)) ||
        (!signal_all && !(wr->flags&cx5::send_flag_signaled))) { error=-EOPNOTSUPP; return false; }
    const unsigned limit=cx5::rdma_opcode(opcode) ? cx5::max_rdma_sge : cx5::max_send_sge;
    if (!wr->sge || wr->sge_count<1 || unsigned(wr->sge_count)>limit) { error=-EINVAL; return false; }
    request.opcode=opcode; request.flags=uint8_t(wr->flags&(cx5::send_flag_fence|cx5::send_flag_solicited));
    request.immediate=wr->immediate;
    for (int32_t i=0;i<wr->sge_count;++i) {
        if (!usable_sge(wr->sge[i])) { error=-EINVAL; return false; }
        request.sge[i]={wr->sge[i].address,wr->sge[i].lkey,wr->sge[i].length};
    }
    request.sge_count=unsigned(wr->sge_count);
    if (cx5::rdma_opcode(opcode)) {
        const auto *rdma=reinterpret_cast<const AppleRDMAWR *>(wr);
        request.remote=rdma->remote; request.rkey=rdma->rkey;
    }
    if (!cx5::request_bytes(request)) { error=-EINVAL; return false; }
    return true;
}

int apple_post_send(Hca &hca,HardwareQP &qp,bool signal_all,const AppleSendWR *wr,const AppleSendWR **bad) {
    if (!bad) return -EINVAL;
    *bad=nullptr;
    for (;wr;wr=wr->next) {
        int error=0; cx5::SendRequest request;
        if (!qp.client_context || !qp.object.live || qp.state!=3 || !hca.transport.ready()) error=-EIO;
        else if (!apple_send_request(wr,signal_all,request,error)) {}
        else if (!qp.sends.can_post(qp.producer) || qp.send_cq->outstanding>=31) error=-ENOMEM;
        else if (!hca.post(qp,wr->id,request)) error=-EIO;
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
        else if (wr->sge_count!=1 || !wr->sge || !usable_sge(*wr->sge)) error=-EINVAL;
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
        // Hardware receive formats: 2 SEND, 3 SEND with immediate, 1 WRITE
        // with immediate (which consumes a receive entry and reports the
        // written length). Public completion opcodes: RECV 128,
        // RECV_RDMA_WITH_IMM 129; wc flag 2 marks an immediate.
        const bool with_immediate=receive && (hardware.opcode==1 || hardware.opcode==3);
        // A user-posted QP has no kernel record: the WQE counter is the ID, the
        // send opcode is unknown here, and the byte count is the hardware's.
        completion.opcode=receive ? (hardware.opcode==1 ? 129u : 128u)
                        : user ? 0 : (work.opcode==cx5::wqe_write || work.opcode==cx5::wqe_write_imm) ? 1
                        : work.opcode==cx5::wqe_read ? 2 : 0;
        if (result==cx5::CQResult::error) {
            completion.status=completion_status(hardware.syndrome);
            completion.vendor=(uint32_t(hardware.vendor_syndrome)<<8)|hardware.syndrome;
        } else if (receive && hardware.opcode!=2 && !with_immediate) {
            // Invalidate receive formats are not supported yet. Consume the
            // real completion, but never report plain SEND success.
            completion.status=21; completion.vendor=hardware.opcode;
        } else if (receive && !user && hardware.opcode!=1 && hardware.bytes>work.length) {
            completion.status=1; // A receive cannot successfully exceed its posted buffer.
        } else if (receive) {
            completion.bytes=hardware.bytes;
            if (with_immediate) { completion.immediate=hardware.immediate; completion.flags|=2; }
        }
        else if (!user && work.opcode==cx5::wqe_read) completion.bytes=work.length;
        output[count++]=completion;
    }
    return count;
}
}
