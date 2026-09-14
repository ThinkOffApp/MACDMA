#pragma once
#include "apple_umem.hpp"
#include "kernel_hca.hpp"

namespace cx5_native {
// Connects Apple's task-owned pinned memory to the real CX5 MTT/MKey command.
// There is deliberately no automatic destructor: a timeout cannot authorize
// unpinning pages which the NIC may still own.
class RegisteredMemory {
public:
    IOReturn create(Hca &hca, ib_ucontext *context, uint32_t pd, uint64_t start,
                    uint64_t length, uint64_t iova, uint32_t access);
    IOReturn destroy(Hca &hca);
    bool needs_retention() const { return memory_.pinned(); }
    uint32_t key() const { return key_; }
private:
    UserMemory memory_{};
    HardwareObject object_{};
    uint32_t key_ = 0;
};
}
