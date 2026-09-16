#include "apple_umem.hpp"
#include "apple_build.hpp"
#include "cx5_verbs.hpp"
#include <string.h>

extern "C" IOReturn ib_umem_get(ib_ucontext *, uint64_t, uint64_t, int, ib_umem **);
extern "C" void ib_umem_release(ib_umem *);

namespace cx5_native {
IOReturn UserMemory::pin(ib_ucontext *context,uint64_t start,uint64_t length,uint32_t access) {
    if (!supported_build() || !context || umem_ || !length ||
        length>max_mr_bytes || start>UINT64_MAX-length || (access&~7u) ||
        ((access&2) && !(access&1))) return kIOReturnBadArgument;
    // Core registration also requires the HCA VA and process VA to have equal
    // native-page offsets; the provider enforces that before calling us.
    const auto result=ib_umem_get(context,start,length,int(access),&umem_);
    if (result) { umem_=nullptr; return result; }
    if (!umem_) return kIOReturnNoMemory;
    start_=start; length_=length; return kIOReturnSuccess;
}
IOReturn UserMemory::translate(uint64_t alias,uint64_t *pages,size_t capacity,size_t &count,unsigned &log_page) const {
    count=0; log_page=0;
    if (!umem_ || !pages || !capacity) return kIOReturnBadArgument;
    // ib_umem +8 embeds memory_mapping; its +8 is the mapped IODMACommand.
    // This is verified in ib_umem_get and memory_mapping_map_to_device, and
    // only accessed on the exact OS build accepted by pin().
    IODMACommand *mapping=nullptr;
    memcpy(&mapping,reinterpret_cast<const uint8_t *>(umem_)+0x10,sizeof(mapping));
    if (!mapping) return kIOReturnNotReady;
    // The mapping begins at the 16 KiB page holding start_. Walk it from the
    // first 4 KiB page of the range as the IOMMU reports it, merging device
    // contiguous pieces, and let the portable page-size selection decide.
    constexpr size_t max_segments=256;
    auto *segments=static_cast<cx5::Segment *>(IOMallocData(max_segments*sizeof(cx5::Segment)));
    if (!segments) return kIOReturnNoMemory;
    const uint64_t base=start_&~uint64_t(0x3fff);
    const uint64_t first=(start_&0x3fff)&~uint64_t(4095);
    const uint64_t end=first+(((start_&4095)+length_+4095)&~uint64_t(4095));
    UInt64 offset=first; size_t n=0; IOReturn result=kIOReturnSuccess;
    while (offset<end && !result) {
        IODMACommand::Segment64 raw[8]{}; UInt32 got=8;
        UInt64 cursor=offset;
        result=mapping->gen64IOVMSegments(&offset,raw,&got);
        if (result) break;
        if (!got || offset<=cursor) { result=kIOReturnUnsupported; break; }
        for (UInt32 i=0;i<got;++i) {
            if (!raw[i].fIOVMAddr || (raw[i].fIOVMAddr&4095) || !raw[i].fLength ||
                raw[i].fIOVMAddr>UINT64_MAX-raw[i].fLength) { result=kIOReturnUnsupported; break; }
            if (n && segments[n-1].iova+segments[n-1].length==raw[i].fIOVMAddr) segments[n-1].length+=raw[i].fLength;
            else if (n==max_segments) { result=kIOReturnNoSpace; break; }
            else segments[n++]={base+cursor,raw[i].fIOVMAddr,raw[i].fLength};
            cursor+=raw[i].fLength;
        }
    }
    if (!result) {
        // A final piece may run past the range; trim it to whole pages.
        if (n && segments[n-1].va+segments[n-1].length>base+end)
            segments[n-1].length=base+end-segments[n-1].va;
        log_page=cx5::choose_log_page(segments,n,start_,length_,alias,capacity);
        if (!log_page) result=kIOReturnNoSpace;
        else {
            count=cx5::page_list(segments,n,start_,length_,log_page,pages,capacity);
            if (!count) { log_page=0; result=kIOReturnUnsupported; }
        }
    }
    IOFreeData(segments,max_segments*sizeof(cx5::Segment));
    return result;
}
void UserMemory::release() {
    if (umem_) ib_umem_release(umem_);
    umem_=nullptr; start_=length_=0;
}
}
