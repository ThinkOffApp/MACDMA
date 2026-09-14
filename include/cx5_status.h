#pragma once
#include <stdint.h>
#define CX5_STATUS_ABI 1u
#define CX5_GET_STATUS 0u
#define CX5_RUN_FW_PROBE 1u
#define CX5_RUN_RESOURCE_PROBE 2u
#define CX5_NETWORK 3u
#define CX5_NETWORK_ABI 2u
#define CX5_FW_PROBE_ABI 1u
#define CX5_DRIVER_ID "org.mcdma.cx5.driver"
#define CX5_DRIVER_CLASS "MCDMACX5"
// Stable POD ABI; no process pointers, DMA addresses or firmware write commands.
struct cx5_status {
    uint32_t abi, bytes, identity, inspection_error;
    uint32_t firmware_major, firmware_minor, firmware_patch, command_revision;
    uint32_t pci_revision, log_slots, log_stride, pci_command;
    uint64_t bar_size;
    uint32_t dma_prepare_status, dma_segments, dma_complete_status, hardware_rdma_ready;
};
struct cx5_command_result {
    uint32_t opcode, transport_error, delivery_status, firmware_status;
    uint32_t syndrome, completed, polls, reserved;
};
struct cx5_firmware_probe {
    uint32_t abi, bytes, setup_status, release_status;
    uint32_t pci_command_before, pci_command_active, pci_command_after, quarantined;
    uint32_t commands_completed, dma_segments, current_issi, boot_pages;
    struct cx5_command_result commands[4];
    uint8_t issi_output[112];
};
struct cx5_resource_probe {
    uint32_t abi, bytes, resource_status, checks_failed;
    uint32_t command_count, resources_passed, pages_given, pages_returned;
    uint32_t quarantined, reserved[3];
    struct cx5_firmware_probe queue;
    struct cx5_command_result commands[96];
};
struct cx5_endpoint {
    uint32_t function, qpn, psn, rkey;
    uint64_t address;
    uint32_t length, reserved;
    uint8_t gid[16], mac[6], padding[2];
};
struct cx5_network_request { uint32_t abi, operation; struct cx5_endpoint peer; };
struct cx5_network_diagnostic {
    uint32_t complete,qp_state,pm_state,mtu,access,pd,send_cq,receive_cq;
    uint32_t doorbell_matches,source_gid_index,grh,log_message_size,uar_page_shift;
    uint32_t roce_enabled,roce_version,roce_l3_type,gid_matches,port_admin,port_oper,vport_state;
};
struct cx5_network_result {
    uint32_t abi, bytes, status, operation;
    struct cx5_endpoint local;
    struct cx5_command_result last_command;
    uint32_t cq_opcode, cq_syndrome, cq_vendor, cq_counter;
    uint32_t writes_completed, reads_completed, bytes_verified, link_state;
    struct cx5_network_diagnostic diagnostic;
};
