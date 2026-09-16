/* Original lab tool: sets one of the native driver's run-time knobs through
 * the registry (root). No device I/O, no reinstall, no reboot. Usage:
 *   mcdma-set MCDMAMaxReadRequestBytes 0|128|256|512|1024|2048|4096
 *   mcdma-set MCDMARelaxedOrdering 0|1
 *   mcdma-set MCDMAAckRequestEveryPacket 0|1
 * Prints the driver's resulting registry values for the campaign log. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void show(io_service_t service,const char *key) {
    CFStringRef name=CFStringCreateWithCString(kCFAllocatorDefault,key,kCFStringEncodingUTF8);
    CFTypeRef value=IORegistryEntryCreateCFProperty(service,name,kCFAllocatorDefault,0);
    CFRelease(name);
    if (!value) { printf("MCDMA_SET %s=<absent>\n",key); return; }
    if (CFGetTypeID(value)==CFBooleanGetTypeID()) printf("MCDMA_SET %s=%d\n",key,CFBooleanGetValue(value)?1:0);
    else if (CFGetTypeID(value)==CFNumberGetTypeID()) { long long n=0; CFNumberGetValue(value,kCFNumberLongLongType,&n); printf("MCDMA_SET %s=%lld\n",key,n); }
    else if (CFGetTypeID(value)==CFStringGetTypeID()) { char buffer[600]; if (CFStringGetCString(value,buffer,sizeof(buffer),kCFStringEncodingUTF8)) printf("MCDMA_SET %s=%s\n",key,buffer); }
    CFRelease(value);
}
int main(int argc,char **argv) {
    const char *keys[]={"MCDMAMaxReadRequestBytes","MCDMARelaxedOrdering","MCDMAAckRequestEveryPacket"};
    if (argc!=3 && argc!=1) { fputs("Usage: mcdma-set KEY VALUE (or no arguments to show)\n",stderr); return 2; }
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("MCDMACX5Native"));
    if (!service) { fputs("Native driver not found\n",stderr); return 1; }
    if (argc==3) {
        int index=-1; for (int i=0;i<3;++i) if (!strcmp(argv[1],keys[i])) index=i;
        if (index<0) { fputs("Unknown key\n",stderr); return 2; }
        char *end=NULL; long value=strtol(argv[2],&end,10);
        if (!*argv[2] || *end || value<0 || (index && value>1) || (!index && value>4096)) { fputs("Bad value\n",stderr); return 2; }
        CFStringRef key=CFStringCreateWithCString(kCFAllocatorDefault,keys[index],kCFStringEncodingUTF8);
        CFTypeRef cf=index ? (CFTypeRef)(value?kCFBooleanTrue:kCFBooleanFalse) : (CFTypeRef)CFNumberCreate(kCFAllocatorDefault,kCFNumberLongType,&value);
        CFDictionaryRef dictionary=CFDictionaryCreate(kCFAllocatorDefault,(const void **)&key,(const void **)&cf,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        kern_return_t result=IORegistryEntrySetCFProperties(service,dictionary);
        printf("MCDMA_SET result=0x%x\n",result);
        CFRelease(dictionary); CFRelease(key); if (!index) CFRelease(cf);
        if (result) { IOObjectRelease(service); return 1; }
    }
    show(service,"MCDMANativeVersion"); show(service,"MCDMAPCIePath"); show(service,"MCDMAMaxReadRequestRequested"); show(service,"MCDMAMaxReadRequestApplied");
    show(service,"MCDMARelaxedOrdering"); show(service,"MCDMARelaxedOrderingRefused"); show(service,"MCDMARelaxedOrderingKeys"); show(service,"MCDMAAckRequestEveryPacket");
    IOObjectRelease(service); return 0;
}
