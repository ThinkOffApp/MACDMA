#include "apple_registration.hpp"
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>

// Calling the exported nonvirtual methods avoids inventing a private C++
// class layout or copying somebody else's reconstructed IORDMAInterface.h.
// ARM64 register arguments and bool/void returns are checked in our ABI report.
extern "C" bool apple_register_ib(IOService *, ib_device *)
    __asm__("__ZN15IORDMAInterface19registerIBInterfaceEP9ib_device");
extern "C" void apple_unregister_ib(IOService *)
    __asm__("__ZN15IORDMAInterface21unregisterIBInterfaceEv");
extern "C" int apple_quiesce_ib(IOService *)
    __asm__("__ZN15IORDMAInterface7quiesceEv");

namespace cx5_native {
IOService *publish(IOService *parent, ib_device *device, const char *name,bool advertise) {
    if (!parent || !device || !name || !*name) return nullptr;
    // Apple allocates its own object, including private expansion storage.
    OSObject *object = OSMetaClass::allocClassWithName("IORDMAInterface");
    if (!object) return nullptr;
    IOService *service = OSDynamicCast(IOService, object);
    if (!service) { object->release(); return nullptr; }
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *client_class = OSString::withCString("IORDMAFamilyUC");
    bool configured = properties && client_class &&
        properties->setObject("IOUserClientClass", client_class);
    if (client_class) client_class->release();
    bool initialized = configured && service->init(properties);
    if (properties) properties->release();
    if (!initialized) { service->release(); return nullptr; }
    if (!service->attach(parent)) { service->release(); return nullptr; }
    if (!service->start(parent)) {
        service->detach(parent); service->release(); return nullptr;
    }
    // start() assigns a default name, so set our requested name afterward.
    service->setName(name);
    if (!apple_register_ib(service, device)) {
        service->stop(parent); service->detach(parent);
        service->release(); return nullptr;
    }
    if (advertise) service->registerService();
    return service;
}

bool unpublish(IOService *service) {
    if (!service) return true;
    if (apple_quiesce_ib(service)) return false;
    apple_unregister_ib(service);
    // IOService owns the asynchronous termination/stop/detach sequence.
    service->terminate();
    service->release();
    return true;
}
}
