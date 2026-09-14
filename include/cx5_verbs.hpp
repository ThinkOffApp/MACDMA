#pragma once
#include "cx5_protocol.hpp"
namespace cx5 {
bool set_bits(uint8_t *wire,size_t bytes,size_t offset,unsigned width,uint64_t value);
uint64_t get_bits(const uint8_t *wire,size_t bytes,size_t offset,unsigned width);
bool create_mkey(uint8_t *wire,size_t capacity,uint32_t pd,uint8_t key,
                 uint64_t virtual_base,uint64_t length,const uint64_t *pages,size_t count);
// Access bits use the public verbs convention: local write=1, remote write=2,
// remote read=4; unsupported capabilities are rejected, never widened.
size_t create_user_mkey(uint8_t *wire,size_t capacity,uint32_t pd,uint8_t key,
                       uint64_t virtual_base,uint64_t length,const uint64_t *pages,
                       size_t count,uint32_t access);
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
};
bool encode_rc_transition(uint8_t *wire,size_t capacity,uint16_t opcode,const RCConnection &connection);
struct Completion {
    uint32_t qpn,bytes;
    uint16_t wqe_counter;
    uint8_t opcode,syndrome,vendor_syndrome;
};
enum class CQResult { empty, success, error, unsupported };
CQResult decode_cqe(const uint8_t *wire,uint32_t consumer,unsigned log_entries,Completion &out);
}
