#pragma once
#include "kernel_hca.hpp"
namespace cx5_native {
// Separate lifetime from the provider: retired, mapped CQ storage stays
// charged until its last descriptor is released, including map failures.
struct CQMappingQuota {
    static constexpr unsigned limit=64;
    unsigned references=1, mappings=0;
    const unsigned maximum;
    explicit CQMappingQuota(unsigned max=limit):maximum(max) {}
    bool acquire();
    void release_mapping();
    void release();
};
// A context-owned firmware UAR. The provider list owns the lease and issues
// DEALLOC_UAR only once no mapping descriptor references it; descriptors
// never run firmware commands.
struct UarLease {
    HardwareObject uar{};
    unsigned references=1;
    bool orphan=false;
    // Set by the mapping descriptor when it forwarded the write-combined
    // cache attribute for this UAR page (userspace BlueFlame contexts).
    bool write_combined=false;
    UarLease *next=nullptr;
    void retain() { __atomic_fetch_add(&references,1,__ATOMIC_RELAXED); }
    void release() { __atomic_fetch_sub(&references,1,__ATOMIC_ACQ_REL); }
    unsigned count() const { return __atomic_load_n(&references,__ATOMIC_ACQUIRE); }
};
IOMemoryDescriptor *cq_read_mapping(IOBufferMemoryDescriptor *,CQMappingQuota &,CQMappingQuota *context=nullptr);
// Writable mapping of one user-posted QP's own work-queue page: receive
// queue, send queue and doorbell record, nothing else.
IOMemoryDescriptor *queue_write_mapping(IOBufferMemoryDescriptor *,CQMappingQuota &,CQMappingQuota *context);
// Writable mapping of a context's UAR page (device memory, one 16 KiB page).
// With write_combine the descriptor maps it with the same write-combined
// attribute as the kernel's own BlueFlame page, so a userspace BlueFlame
// push can leave the core as one burst instead of eight separate writes.
IOMemoryDescriptor *uar_mapping(IOMemoryDescriptor *bar_page,UarLease &,CQMappingQuota &,CQMappingQuota *context,
                                bool write_combine=false);
}
