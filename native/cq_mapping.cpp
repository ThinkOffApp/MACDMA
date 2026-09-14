#include "cq_mapping.hpp"
#include "cx5_cq_observer.h"
#ifndef CX5_NATIVE_TEST
#include <IOKit/IOSubMemoryDescriptor.h>
#endif

namespace cx5_native {
bool CQMappingQuota::acquire() {
    auto count=__atomic_load_n(&mappings,__ATOMIC_RELAXED);
    do { if (count>=maximum) return false; }
    while (!__atomic_compare_exchange_n(&mappings,&count,count+1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED));
    __atomic_fetch_add(&references,1,__ATOMIC_RELAXED); return true;
}
void CQMappingQuota::release_mapping() {
    __atomic_fetch_sub(&mappings,1,__ATOMIC_RELEASE); release();
}
void CQMappingQuota::release() {
    if (__atomic_fetch_sub(&references,1,__ATOMIC_ACQ_REL)==1) delete this;
}
}
// Public IOKit subclass, with no Apple-private layout or VMA close hook.
// A live descriptor instance also retains the kernel module's class code.
class MCDMACQMapping final : public IOSubMemoryDescriptor {
#ifndef CX5_NATIVE_TEST
    OSDeclareDefaultStructors(MCDMACQMapping)
#endif
public:
    cx5_native::CQMappingQuota *quota_=nullptr, *context_quota_=nullptr;
    cx5_native::UarLease *lease_=nullptr;
    bool write_combine_=false;
    // Apple's MemoryMap derives its mapping options from the VMA protection
    // alone, which leaves a BAR page as ordinary device memory. The cache
    // attribute of a mapping travels in these options: through the
    // IOSubMemoryDescriptor to the physical parent's doMap, which applies it
    // to the named entry before the task mapping is entered (public XNU,
    // IOMemoryDescriptor.cpp memoryReferenceMap). The kernel's own BlueFlame
    // page is mapped by the same path with the same attribute. Only the
    // cache bits change; protection and placement stay the core's.
    IOMemoryMap *makeMapping(IOMemoryDescriptor *owner,task_t task,IOVirtualAddress address,
                             IOOptionBits options,IOByteCount offset,IOByteCount length) override {
        if (write_combine_) {
            options=(options&~IOOptionBits(kIOMapCacheMask))|IOOptionBits(kIOMapWriteCombineCache);
            if (lease_) lease_->write_combined=true;
        }
        return IOSubMemoryDescriptor::makeMapping(owner,task,address,options,offset,length);
    }
    void free() override {
        auto *quota=quota_; quota_=nullptr;
        auto *context=context_quota_; context_quota_=nullptr;
        auto *lease=lease_; lease_=nullptr;
        if (lease) lease->release();
        if (context) context->release_mapping();
        if (quota) quota->release_mapping();
        // Superclass free may delete the last instance retaining our module;
        // it must be the final operation, with no driver code after it.
        IOSubMemoryDescriptor::free();
    }
};
#ifndef CX5_NATIVE_TEST
OSDefineMetaClassAndStructors(MCDMACQMapping,IOSubMemoryDescriptor)
#endif
namespace cx5_native {
namespace {
IOMemoryDescriptor *charged_mapping(IOMemoryDescriptor *parent,CQMappingQuota &quota,CQMappingQuota *context,
                                    IODirection direction,UarLease *lease,bool write_combine=false) {
    if (!parent || !quota.acquire()) return nullptr;
    if (context && !context->acquire()) { quota.release_mapping(); return nullptr; }
    auto *mapping=new MCDMACQMapping;
    if (!mapping) { if(context) context->release_mapping(); quota.release_mapping(); return nullptr; }
    mapping->quota_=&quota; mapping->context_quota_=context;
    mapping->write_combine_=write_combine && lease!=nullptr; // UAR pages only, never DMA storage.
    if (lease) { lease->retain(); mapping->lease_=lease; }
    if (!mapping->initSubRange(parent,0,MCDMA_CQ_MAP_BYTES,direction)) {
        mapping->release(); return nullptr;
    }
    return mapping;
}
}
IOMemoryDescriptor *cq_read_mapping(IOBufferMemoryDescriptor *parent,CQMappingQuota &quota,CQMappingQuota *context) {
    return charged_mapping(parent,quota,context,kIODirectionOut,nullptr);
}
IOMemoryDescriptor *queue_write_mapping(IOBufferMemoryDescriptor *parent,CQMappingQuota &quota,CQMappingQuota *context) {
    return charged_mapping(parent,quota,context,kIODirectionInOut,nullptr);
}
IOMemoryDescriptor *uar_mapping(IOMemoryDescriptor *bar_page,UarLease &lease,CQMappingQuota &quota,CQMappingQuota *context,
                                bool write_combine) {
    return charged_mapping(bar_page,quota,context,kIODirectionInOut,&lease,write_combine);
}
}
