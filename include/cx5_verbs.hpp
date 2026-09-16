#pragma once
#include "cx5_protocol.hpp"
namespace cx5 {
bool set_bits(uint8_t *wire,size_t bytes,size_t offset,unsigned width,uint64_t value);
uint64_t get_bits(const uint8_t *wire,size_t bytes,size_t offset,unsigned width);
bool create_mkey(uint8_t *wire,size_t capacity,uint32_t pd,uint8_t key,
                 uint64_t virtual_base,uint64_t length,const uint64_t *pages,size_t count);
// Access bits use the public verbs convention: local write=1, remote write=2,
// remote read=4; unsupported capabilities are rejected, never widened.
// log_page selects the MKey page size (12..30); every page address must be
// aligned to it and the list must cover the range exactly. relaxed_ordering
// sets the MKey's PCIe relaxed-ordering read and write bits (public NVIDIA
// MKC fields); the firmware refuses them when unsupported.
size_t create_user_mkey(uint8_t *wire,size_t capacity,uint32_t pd,uint8_t key,
                       uint64_t virtual_base,uint64_t length,const uint64_t *pages,
                       size_t count,uint32_t access,unsigned log_page=12,bool relaxed_ordering=false);
// One CREATE_MKEY carries at most this many translation entries.
constexpr size_t max_mkey_pages=(max_command_bytes-0x110)/8;
// A pinned range as the IOMMU maps it: `va` is the process address where the
// segment starts, `iova` its device address; consecutive segments abut in
// process space and each is a whole number of 4 KiB pages.
struct Segment { uint64_t va, iova, length; };
// The largest page size (log2, 12..30) with which the range [start,start+length)
// can be described by at most `capacity` whole-page entries: segment starts
// after the first must be page aligned, each segment's device address must
// share the process address's offset within a page, and `alias` (the HCA
// virtual address the key is programmed with) must share `start`'s offset.
// Returns 0 when no page size fits.
unsigned choose_log_page(const Segment *segments,size_t count,uint64_t start,uint64_t length,
                         uint64_t alias,size_t capacity);
// Fills the entries for that page size; returns their number, or 0 if the
// segments cannot describe the range with it.
size_t page_list(const Segment *segments,size_t count,uint64_t start,uint64_t length,
                 unsigned log_page,uint64_t *pages,size_t capacity);
// One send request as the hardware sees it. Hardware opcodes: 0x08 WRITE,
// 0x09 WRITE with immediate, 0x0a SEND, 0x0b SEND with immediate, 0x10 READ.
// flags carry the public verbs bits fence (1) and solicited (4); every
// request completes signalled. Inline data replaces the scatter list and is
// only for userspace posting, where the bytes are in the posting process.
// Everything must fit one 64-byte WQEBB: ctrl (16) + rdma (16) + data.
struct SendSge { uint64_t address; uint32_t lkey, length; };
struct SendRequest {
    uint8_t opcode=0, flags=0;
    uint32_t immediate=0;              // Raw network-order bits, copied unchanged.
    uint64_t remote=0; uint32_t rkey=0;
    SendSge sge[3]{}; unsigned sge_count=0;
    const uint8_t *inline_data=nullptr; unsigned inline_bytes=0;
};
constexpr uint8_t wqe_write=0x08, wqe_write_imm=0x09, wqe_send=0x0a, wqe_send_imm=0x0b, wqe_read=0x10;
constexpr unsigned max_send_sge=3, max_rdma_sge=2, max_inline_send=44, max_inline_rdma=28;
constexpr uint8_t send_flag_fence=1, send_flag_signaled=2, send_flag_solicited=4, send_flag_inline=8;
inline bool rdma_opcode(uint8_t opcode) { return opcode==wqe_write || opcode==wqe_write_imm || opcode==wqe_read; }
inline bool immediate_opcode(uint8_t opcode) { return opcode==wqe_write_imm || opcode==wqe_send_imm; }
// Total payload bytes of an encodable request, else 0.
uint32_t request_bytes(const SendRequest &request);
bool encode_send_request(uint8_t *wire,size_t capacity,uint32_t qpn,uint16_t producer,const SendRequest &request);
// One-SGE convenience used by the original tests and the kernel wrapper.
bool encode_wqe(uint8_t *wire,size_t capacity,uint32_t qpn,uint16_t producer,
                uint8_t opcode,uint64_t local_address,uint32_t lkey,uint32_t length,
                uint64_t remote_address,uint32_t rkey);
struct RCConnection {
    uint32_t qpn,pd,cq,remote_qpn,send_psn,receive_psn;
    uint64_t doorbell;
    uint8_t remote_gid[16],remote_mac[6];
    // Defaults preserve the original, measured DriverKit connection profile.
    uint8_t access=6, path_mtu=3, min_rnr_timer=12;
    uint8_t retry_count=7, rnr_retry=7, timeout=14, hop_limit=64;
    // Requester acknowledgement-request frequency (log2 packets); 8 is the
    // vendor default, 0 asks the responder to acknowledge every packet.
    uint8_t log_ack_req_freq=8;
};
bool encode_rc_transition(uint8_t *wire,size_t capacity,uint16_t opcode,const RCConnection &connection);
struct Completion {
    uint32_t qpn,bytes;
    uint16_t wqe_counter;
    uint8_t opcode,syndrome,vendor_syndrome;
    uint32_t immediate;   // Raw network-order bits of a received immediate.
};
enum class CQResult { empty, success, error, unsupported };
CQResult decode_cqe(const uint8_t *wire,uint32_t consumer,unsigned log_entries,Completion &out);
}
