/* Original lab tool: sets one of the native driver's run-time knobs through
 * the registry (root). No reinstall, no reboot. Usage:
 *   mcdma-set [-i mcrdmaN] MCDMAMaxReadRequestBytes 0|128|256|512|1024|2048|4096
 *   mcdma-set [-i mcrdmaN] MCDMARelaxedOrdering 0|1
 *   mcdma-set [-i mcrdmaN] MCDMAAckRequestEveryPacket 0|1
 *   mcdma-set [-i mcrdmaN] MCDMAPortSpeedForce 0|1
 *   mcdma-set [-i mcrdmaN] MCDMAPortSpeedAdmin MASK
 *   mcdma-set [-i mcrdmaN] MCDMAPortSpeedQuery 1
 * MASK is a PTYS eth_proto mask (decimal or 0x hex; 10GBASE-CR 0x1000,
 * 25GBASE-CR 0x8000000). Setting it cycles the port and is refused while any
 * QP exists. -i picks the driver instance by address interface; without it the
 * first instance is used. Prints the driver's resulting registry values. */
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
static io_service_t find_service(const char *interface) {
    io_iterator_t iterator=0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("MCDMACX5Native"),&iterator)) return 0;
    io_service_t service, found=0;
    while (!found && (service=IOIteratorNext(iterator))) {
        int match=!interface;
        if (interface) {
            CFTypeRef value=IORegistryEntryCreateCFProperty(service,CFSTR("MCDMAAddressInterface"),kCFAllocatorDefault,0);
            char name[64]="";
            if (value && CFGetTypeID(value)==CFStringGetTypeID()) CFStringGetCString(value,name,sizeof(name),kCFStringEncodingUTF8);
            if (value) CFRelease(value);
            match=!strcmp(name,interface);
        }
        if (match) found=service; else IOObjectRelease(service);
    }
    IOObjectRelease(iterator); return found;
}
int main(int argc,char **argv) {
    enum { mrrs, relaxed, ack, force, admin, query, count };
    const char *keys[count]={"MCDMAMaxReadRequestBytes","MCDMARelaxedOrdering","MCDMAAckRequestEveryPacket",
                             "MCDMAPortSpeedForce","MCDMAPortSpeedAdmin","MCDMAPortSpeedQuery"};
    const char *interface=NULL;
    if (argc>=3 && !strcmp(argv[1],"-i")) { interface=argv[2]; argv+=2; argc-=2; }
    if (argc!=3 && argc!=1) { fputs("Usage: mcdma-set [-i mcrdmaN] KEY VALUE (or no KEY to show)\n",stderr); return 2; }
    io_service_t service=find_service(interface);
    if (!service) { fputs(interface?"No native driver with that interface\n":"Native driver not found\n",stderr); return 1; }
    if (argc==3) {
        int index=-1; for (int i=0;i<count;++i) if (!strcmp(argv[1],keys[i])) index=i;
        if (index<0) { fputs("Unknown key\n",stderr); IOObjectRelease(service); return 2; }
        char *end=NULL; unsigned long long value=strtoull(argv[2],&end,index==admin?0:10);
        const int flag=index==relaxed||index==ack||index==force;
        if (!*argv[2] || *argv[2]=='-' || *end || (flag && value>1) || (index==mrrs && value>4096) ||
            (index==admin && (!value || value>0xffffffffull)) || (index==query && value!=1)) {
            fputs("Bad value\n",stderr); IOObjectRelease(service); return 2;
        }
        CFStringRef key=CFStringCreateWithCString(kCFAllocatorDefault,keys[index],kCFStringEncodingUTF8);
        long long number=(long long)value;
        CFTypeRef cf=flag||index==query ? (CFTypeRef)(value?kCFBooleanTrue:kCFBooleanFalse)
                                        : (CFTypeRef)CFNumberCreate(kCFAllocatorDefault,kCFNumberLongLongType,&number);
        CFDictionaryRef dictionary=CFDictionaryCreate(kCFAllocatorDefault,(const void **)&key,(const void **)&cf,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        kern_return_t result=IORegistryEntrySetCFProperties(service,dictionary);
        printf("MCDMA_SET result=0x%x\n",result);
        CFRelease(dictionary); CFRelease(key); if (!flag && index!=query) CFRelease(cf);
        if (result) { IOObjectRelease(service); return 1; }
    }
    show(service,"MCDMAAddressInterface");
    show(service,"MCDMANativeVersion"); show(service,"MCDMAPCIePath"); show(service,"MCDMAMaxReadRequestRequested"); show(service,"MCDMAMaxReadRequestApplied");
    show(service,"MCDMARelaxedOrdering"); show(service,"MCDMARelaxedOrderingRefused"); show(service,"MCDMARelaxedOrderingKeys"); show(service,"MCDMAAckRequestEveryPacket");
    show(service,"MCDMAPortSpeed"); show(service,"MCDMAPortSpeedOper"); show(service,"MCDMAPortSpeedCapability");
    show(service,"MCDMAPortSpeedAdvertised"); show(service,"MCDMAPortSpeedPartner"); show(service,"MCDMAPortAutonegStatus");
    show(service,"MCDMAPortAutonegDisabled"); show(service,"MCDMAPortSpeedForce"); show(service,"MCDMAPortSpeedResult");
    IOObjectRelease(service); return 0;
}
