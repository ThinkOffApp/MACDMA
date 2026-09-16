#pragma once
#ifdef CX5_NATIVE_TEST
#include "kernel_test_types.hpp"
#else
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>
#endif
#include <stdint.h>
#include <stddef.h>

struct ib_ucontext;
struct ib_umem;
namespace cx5_native {
// Largest registration accepted; the page list, not this bound, decides what
// a given mapping can describe.
constexpr uint64_t max_mr_bytes=uint64_t(1)<<40;
// Observed Apple ABI for 26A5425a; unlike Linux, ib_umem_get returns an
// IOReturn and fills an out-parameter. The native ucontext identifies the task.
class UserMemory {
public:
    IOReturn pin(ib_ucontext *context, uint64_t start, uint64_t length, uint32_t access);
    // Fills the MKey page list for the pinned range with the largest page size
    // the IOMMU mapping allows, given the HCA address `alias` the key will
    // carry. kIOReturnNoSpace when no page size fits `capacity` entries.
    IOReturn translate(uint64_t alias, uint64_t *pages, size_t capacity, size_t &count, unsigned &log_page) const;
    void release(); // Caller MUST destroy the hardware MKey before this call.
    bool pinned() const { return umem_ != nullptr; }
private:
    ib_umem *umem_ = nullptr;
    uint64_t start_ = 0, length_ = 0;
};
}
