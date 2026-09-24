#pragma once
#include "kernel_hca.hpp"
#include <future>
#include <map>
#include <set>
#include <vector>
namespace cx5_test {
struct Simulation {
    IOPCIDevice pci;
    uint16_t vendor_id=0x15b3, device_id=0x1019;
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
    // PCIe configuration probe and firmware refusal of relaxed-ordering keys.
    uint32_t mrrs_requested=0; unsigned pcie_configs=0;
    bool refuse_relaxed_ordering=false; unsigned relaxed_keys=0, strict_keys=0;
    std::map<uint64_t,std::vector<uint8_t>> user_pages;
    uint16_t max_frame=10000,admin_frame=1522,oper_frame=1522,vport_frame=1522;
    bool reject_jumbo_vport_once=false,ignore_jumbo_port_once=false;
    // MPCNT group 0: the 16 counters in register order, and firmware support.
    uint32_t mpcnt[16]{}; bool mpcnt_supported=true; unsigned mpcnt_queries=0;
    // PTYS port speed (legacy eth_proto masks) and every PAOS admin write in order.
    uint32_t ptys_capability=(1u<<12)|(1u<<27), ptys_admin=1u<<12, ptys_oper=1u<<12, ptys_partner=0;
    uint8_t ptys_an_status=0; bool ptys_an_disable_cap=true, ptys_an_disabled=false;
    unsigned ptys_writes=0; std::vector<uint8_t> paos_writes;
    // Injected refusals: the PTYS write whose running count equals
    // fail_ptys_at (0 for none), and the next N PAOS down/up writes. A refused
    // write changes nothing; port_admin is the applied state.
    unsigned fail_ptys_at=0, fail_paos_down=0, fail_paos_up=0; uint8_t port_admin=0;
    std::map<uint64_t,uint8_t *> maps;
    std::map<uint32_t,uint64_t> cq_dma, qp_dma;
    std::vector<uint8_t> command;
    std::vector<uint64_t> pages;
    std::set<uint32_t> objects;
};
extern Simulation sim;
}
