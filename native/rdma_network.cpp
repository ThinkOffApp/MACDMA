#include "rdma_network.hpp"
#include <IOKit/IOLib.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_ether.h>
#include <net/if_types.h>
#include <sys/errno.h>
#include <sys/kpi_mbuf.h>
#include <sys/sockio.h>
#include <string.h>

namespace cx5_native {
errno_t RdmaNetwork::output(ifnet_t,mbuf_t packet) {
    // Never silently claim delivery or tunnel the payload through TCP.
    mbuf_freem_list(packet); return EOPNOTSUPP;
}
errno_t RdmaNetwork::ioctl(ifnet_t interface,unsigned long command,void *data) {
    switch (command) {
    case SIOCSIFFLAGS: return 0; // Administrative state is owned by the stack.
    case SIOCSIFMTU: {
        auto *request=static_cast<struct ifreq *>(data);
        if (!request) return EINVAL;
        // Hardware MTU is configured and read back before registration.
        // Refuse a software-only change that would misreport the RoCE path.
        return request->ifr_mtu==static_cast<int>(ifnet_mtu(interface)) ? 0 : EOPNOTSUPP;
    }
    default: return ether_ioctl(interface,static_cast<uint32_t>(command),data);
    }
}
void RdmaNetwork::detached(ifnet_t interface) {
    // The owner's reference prevents unloading this callback before the
    // asynchronous BSD detach has finished, including on a failed start.
    auto *owner=static_cast<IOService *>(ifnet_softc(interface));
    ifnet_release(interface);
    owner->release();
}
bool RdmaNetwork::attach(IOService *owner,const uint8_t mac[6],uint16_t mtu) {
    if (interface_ || !owner || !mac || (mac[0]&1) || mtu<1280 || mtu>9000) return false;
    static const uint8_t broadcast[6]={255,255,255,255,255,255};
    ifnet_init_params parameters{};
    parameters.uniqueid=mac; parameters.uniqueid_len=6;
    parameters.name="mcrdma";
    parameters.family=IFNET_FAMILY_ETHERNET; parameters.type=IFT_ETHER;
    parameters.output=output; parameters.demux=ether_demux;
    parameters.add_proto=ether_add_proto; parameters.del_proto=ether_del_proto;
    parameters.check_multi=ether_check_multi; parameters.framer=ether_frameout;
    parameters.softc=owner; parameters.ioctl=ioctl; parameters.detach=detached;
    parameters.broadcast_addr=broadcast; parameters.broadcast_len=6;
    // Reserve a free name without changing any existing interface or route.
    for (unsigned unit=0;unit<256;++unit) {
        snprintf(name_,sizeof(name_),"mcrdma%u",unit);
        ifnet_t existing=nullptr;
        if (!ifnet_find_by_name(name_,&existing)) { ifnet_release(existing); continue; }
        parameters.unit=unit;
        if (!ifnet_allocate(&parameters,&interface_)) break;
    }
    if (!interface_) return false;
    ifnet_set_mtu(interface_,mtu);
    ifnet_set_flags(interface_,IFF_BROADCAST|IFF_MULTICAST,IFF_BROADCAST|IFF_MULTICAST);
    sockaddr_dl address{};
    address.sdl_len=sizeof(address); address.sdl_family=AF_LINK;
    address.sdl_type=IFT_ETHER; address.sdl_alen=6;
    memcpy(address.sdl_data,mac,6);
    owner->retain();
    if (ifnet_attach(interface_,&address)) {
        // No detach callback runs for an interface that never attached.
        ifnet_release(interface_); interface_=nullptr; owner->release(); return false;
    }
    attached_=true;
    if (ifnet_set_lladdr(interface_,mac,6)) { detach(); return false; }
    return true;
}
bool RdmaNetwork::link(bool active) {
    return interface_ && attached_ &&
        !ifnet_set_flags(interface_,active?IFF_RUNNING:0,IFF_RUNNING);
}
bool RdmaNetwork::detach() {
    if (!interface_) return true;
    link(false);
    if (attached_) {
        if (ifnet_detach(interface_)) return false;
        // detached() releases the allocation reference, possibly synchronously.
    } else ifnet_release(interface_);
    interface_=nullptr; attached_=false; return true;
}
}
