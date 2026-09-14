#include "apple_umem.hpp"
#include "apple_build.hpp"
#include <string.h>

extern "C" IOReturn ib_umem_get(ib_ucontext *, uint64_t, uint64_t, int, ib_umem **);
extern "C" void ib_umem_release(ib_umem *);

namespace cx5_native {
IOReturn UserMemory::pin(ib_ucontext *context,uint64_t start,uint64_t length,uint32_t access) {
    if (!supported_build() || !context || umem_ || !length ||
        length>2*1024*1024 || start>UINT64_MAX-length || (access&~7u) ||
        ((access&2) && !(access&1))) return kIOReturnBadArgument;
    // Core registration also requires the HCA VA and process VA to have equal
    // native-page offsets; the provider enforces that before calling us.
    const auto result=ib_umem_get(context,start,length,int(access),&umem_);
    if (result) { umem_=nullptr; return result; }
    if (!umem_) return kIOReturnNoMemory;
    start_=start; length_=length; return kIOReturnSuccess;
}
IOReturn UserMemory::pages_4k(uint64_t *pages,size_t capacity,size_t &count) const {
    count=0;
    if (!umem_ || !pages) return kIOReturnBadArgument;
    const size_t needed=size_t(((start_&4095)+length_+4095)/4096);
    if (needed>capacity || needed>512) return kIOReturnNoSpace;
    // ib_umem +8 embeds memory_mapping; its +8 is the mapped IODMACommand.
    // This is verified in ib_umem_get and memory_mapping_map_to_device, and
    // only accessed on the exact OS build accepted by pin().
    IODMACommand *mapping=nullptr;
    memcpy(&mapping,reinterpret_cast<const uint8_t *>(umem_)+0x10,sizeof(mapping));
    if (!mapping) return kIOReturnNotReady;
    for (size_t i=0;i<needed;++i) {
        UInt64 offset=(start_&0x3fff&~uint64_t(4095))+i*4096;
        IODMACommand::Segment64 segment{};
        UInt32 segments=1;
        auto result=mapping->gen64IOVMSegments(&offset,&segment,&segments);
        if (result) return result;
        if (segments!=1 || segment.fLength<4096 || !segment.fIOVMAddr || (segment.fIOVMAddr&4095))
            return kIOReturnUnsupported;
        pages[i]=segment.fIOVMAddr;
    }
    count=needed; return kIOReturnSuccess;
}
void UserMemory::release() {
    if (umem_) ib_umem_release(umem_);
    umem_=nullptr; start_=length_=0;
}
}
