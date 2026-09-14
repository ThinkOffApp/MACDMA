#include "kernel_transport.hpp"

namespace cx5_native {
bool Transport::detached() const {
    // isInactive also covers logical IOService termination with the card still
    // present: it rejects new IO in ready(), but cannot authorize unpinning.
    return !pci_ || pci_->configRead16(0) == 0xffff;
}

bool Transport::write64(uint64_t offset, uint64_t value) {
    // Ordinary eight-byte doorbells use only the non-WC UAR mapping and the
    // canonical first register; unsupported BF sizes need no invented stride.
    if (!ready() || !bf_map_ || bf_buffer_ || offset!=bf_uar_+0x800 ||
        offset<bf_base_ || bf_map_->getLength()<8 || offset-bf_base_>bf_map_->getLength()-8)
        return false;
    auto address = reinterpret_cast<volatile uint64_t *>(bf_map_->getVirtualAddress()+offset-bf_base_);
    *address = value; // Doorbell caller supplies the exact WQE bytes.
    publish_dma(); return true;
}

bool Transport::configure_doorbell(uint64_t uar_offset) {
    if (!ready() || !bar_ || bf_map_ || uar_offset<16384 || (uar_offset&4095) ||
        bar_bytes_<4096 || uar_offset>bar_bytes_-4096) return false;
    if (!map_uar(uar_offset&~uint64_t(16383),false)) return false;
    bf_uar_=uar_offset; bf_buffer_=0; return true;
}
bool Transport::configure_blueflame(uint64_t uar_offset,uint32_t buffer_bytes) {
    // Single UAR, two banks; a 128-byte padded write fits either bank.
    if (!ready() || !bar_ || bf_map_ || uar_offset<16384 || (uar_offset&4095) ||
        buffer_bytes<128 || buffer_bytes>1024 || (buffer_bytes&(buffer_bytes-1)) ||
        bar_bytes_<4096 || uar_offset>bar_bytes_-4096) return false;
    if (!map_uar(uar_offset&~uint64_t(16383),true)) return false;
    bf_uar_=uar_offset; bf_buffer_=buffer_bytes; return true;
}
bool Transport::write_blueflame(uint64_t offset,const uint8_t *wqe) {
    if (!ready() || !bf_map_ || !wqe || !bf_buffer_ ||
        (offset!=bf_uar_+0x800 && offset!=bf_uar_+0x800+bf_buffer_) ||
        offset<bf_base_ || bf_map_->getLength()<128 ||
        offset-bf_base_>bf_map_->getLength()-128) return false;
    auto *destination=reinterpret_cast<volatile uint64_t *>(
        bf_map_->getVirtualAddress()+offset-bf_base_);
    // The complete 64-byte WQEBB is followed by zero padding up to one
    // Apple-Silicon cache line. DS still describes the original WQE.
    // Volatile scalar stores preserve width; this kernel uses no SIMD.
    for (unsigned i=0;i<16;++i) {
        uint64_t word=0;
        if (i<8) __builtin_memcpy(&word,wqe+i*8,8);
        destination[i]=word;
    }
    // Flush the WC stores before the provider lock can be released or another
    // QP uses this shared UAR. Existing WQE/DBR publication barriers remain.
    publish_dma(); return true;
}

}
