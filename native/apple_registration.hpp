#pragma once

// Original interoperability declarations, verified against macOS 27 26A5425a.
// This is a kernel-side adapter, not a DriverKit class or a complete provider.
#include <IOKit/IOService.h>

struct ib_device;

namespace cx5_native {
// The caller must own a fully initialized ib_device and its real verbs backend.
// No service is published until Apple's registration function succeeds.
IOService *publish(IOService *parent, ib_device *device, const char *name,
                   bool advertise=true);
// Caller must prevent new requests and drain the backend before unpublishing.
bool unpublish(IOService *service);
}
