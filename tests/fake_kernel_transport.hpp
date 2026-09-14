#pragma once
#include "kernel_hca.hpp"
#include <future>
#include <map>
#include <set>
#include <vector>
namespace cx5_test {
struct Simulation {
    IOPCIDevice pci;
    IOMemoryMap bar, uar;
    unsigned buffers=0, doorbells=0, calls=0;
    uint32_t next_id=1;
    uint64_t next_dma=0x40000000;
    uint16_t fail_opcode=0;
    bool removed=false, timeout=false, bad_reclaim=false;
    // Lock-coupling regression gate: when set, the next port-status ACCESS_REG
    // reports arrival and blocks inside the fake firmware until the test
    // releases it, exactly like a stalled health query under the command lock.
    bool port_gate=false;
    IOLock *blocked_command_lock=nullptr;
    std::promise<void> port_arrived, port_continue;
    uint8_t vport=0x11, bf_log=9;
    bool bf_capable=true, bf_map_fail=false, uc_map_fail=false;
    uint32_t fail_uar_allocation=0, uar_allocations=0;
    // 16 KiB UAR page negotiation (SET_HCA_CAP) and user-mappable BAR pages.
    bool uar_pages_16k_supported=true; unsigned uar_page_log=0, set_caps=0;
    std::map<uint64_t,std::vector<uint8_t>> user_pages;
    uint16_t max_frame=10000,admin_frame=1522,oper_frame=1522,vport_frame=1522;
    bool reject_jumbo_vport_once=false,ignore_jumbo_port_once=false;
    std::map<uint64_t,uint8_t *> maps;
    std::map<uint32_t,uint64_t> cq_dma, qp_dma;
    std::vector<uint8_t> command;
    std::vector<uint64_t> pages;
    std::set<uint32_t> objects;
};
extern Simulation sim;
}
