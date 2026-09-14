#pragma once
#include <IOKit/IOService.h>
#include <net/kpi_interface.h>

namespace cx5_native {
// A BSD address interface for an RDMA-only port. It has the physical HCA MAC
// and follows the HCA link, but deliberately rejects ordinary IP transmission.
// RoCE payloads use the real RC queues; initial peers need static neighbours
// installed out of band. This is not a general Ethernet driver.
class RdmaNetwork {
public:
    bool attach(IOService *owner,const uint8_t mac[6],uint16_t mtu);
    bool detach();
    bool link(bool active);
    const char *name() const { return name_; }
private:
    ifnet_t interface_=nullptr;
    bool attached_=false;
    char name_[16]{};
    static errno_t output(ifnet_t,mbuf_t);
    static errno_t ioctl(ifnet_t,unsigned long,void *);
    static void detached(ifnet_t);
};
}
