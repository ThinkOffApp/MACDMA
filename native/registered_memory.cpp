#include "registered_memory.hpp"

namespace cx5_native {
IOReturn RegisteredMemory::create(Hca &hca,ib_ucontext *context,uint32_t pd,uint64_t start,
                                  uint64_t length,uint64_t iova,uint32_t access) {
    if (needs_retention() || object_.live || !hca.transport.ready() || pd>0xffffff ||
        ((start^iova)&0x3fff) || !length || length>max_mr_bytes || start>UINT64_MAX-length ||
        iova>UINT64_MAX-length || (access&~7u) || ((access&2) && !(access&1)))
        return kIOReturnBadArgument;
    auto result=memory_.pin(context,start,length,access);
    if (result) return result;
    // A maximum-size page list is 8 KiB: keep it off the kernel stack.
    constexpr size_t capacity=cx5::max_mkey_pages;
    auto *pages=static_cast<uint64_t *>(IOMallocData(capacity*sizeof(uint64_t)));
    if (!pages) { memory_.release(); return kIOReturnNoMemory; }
    size_t count=0; unsigned log_page=0;
    result=memory_.translate(iova,pages,capacity,count,log_page);
    if (!result && !hca.register_mr(object_,pd,iova,length,pages,count,access,key_,log_page))
        result=kIOReturnIOError;
    IOFreeData(pages,capacity*sizeof(uint64_t));
    if (result && !hca.transport.quarantined) memory_.release();
    // An uncertain CREATE_MKEY completion may still have installed translation
    // state. The provider must retain this object until hardware is removed.
    return result;
}
IOReturn RegisteredMemory::destroy(Hca &hca) {
    if (!needs_retention()) return kIOReturnSuccess;
    if (!hca.transport.detached() && hca.transport.quarantined) return kIOReturnBusy;
    if (!hca.deregister_mr(object_,key_)) return kIOReturnBusy;
    memory_.release(); key_=0; return kIOReturnSuccess;
}
}
