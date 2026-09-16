#pragma once
#include "apple_provider.hpp"
#include "rdma_network.hpp"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>

class MCDMACX5Native : public IOService {
    OSDeclareDefaultStructors(MCDMACX5Native)
public:
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    bool willTerminate(IOService *,IOOptionBits) override;
    bool didTerminate(IOService *,IOOptionBits,bool *) override;
    IOWorkLoop *getWorkLoop() const override;
    // Root may change the lab knobs at run time through the registry
    // (IORegistryEntrySetCFProperties): MCDMAMaxReadRequestBytes applies to
    // the device at once, MCDMARelaxedOrdering to later registrations and
    // MCDMAAckRequestEveryPacket to later connections. No reinstall needed.
    IOReturn setProperties(OSObject *properties) override;
private:
    struct State;
    State *state_=nullptr;
    IOWorkLoop *workloop_=nullptr;
    IOTimerEventSource *timer_=nullptr;
    bool timer_attached_=false,retained_=false,stopping_=false;
    static void poll(OSObject *,IOTimerEventSource *);
    static IOReturn cancel(OSObject *,void *,void *,void *,void *);
    void cancel_poll();
    bool cleanup();
    void retain_failed_state();
};
