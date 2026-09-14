#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <mutex>
#include <functional>
#include <vector>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0;
constexpr IOReturn kIOReturnError=1;
constexpr IOReturn kIOReturnBusy=2;
constexpr IOReturn kIOReturnBadArgument=3;
constexpr IOReturn kIOReturnNoMemory=4;
constexpr IOReturn kIOReturnIOError=5;
using IOLock=std::mutex;
inline IOLock *IOLockAlloc() { return new IOLock; }
inline void IOLockFree(IOLock *lock) { delete lock; }
inline thread_local std::vector<IOLock *> test_held_locks;
inline std::function<void(IOLock *)> test_lock_attempt;
inline void IOLockLock(IOLock *lock) { if(test_lock_attempt) test_lock_attempt(lock); lock->lock(); test_held_locks.push_back(lock); }
inline void IOLockUnlock(IOLock *lock) { test_held_locks.pop_back(); lock->unlock(); }
inline void *IOMallocData(size_t size) { return malloc(size); }
inline void *IOMallocZeroData(size_t size) { return calloc(1,size); }
inline void IOFreeData(void *data,size_t) { free(data); }
class IOMemoryDescriptor {
public:
    uint8_t *bytes=nullptr;
    unsigned references=1;
    virtual ~IOMemoryDescriptor()=default;
    void retain() { ++references; }
    void release() { if (!--references) free(); }
    virtual void free() { delete this; }
};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    unsigned *live_buffers=nullptr;
    void free() override { ::free(bytes); --*live_buffers; IOMemoryDescriptor::free(); }
};
using IODirection=unsigned;
constexpr unsigned kIODirectionOut=1, kIODirectionInOut=3;
// Mapping option bits as in IOKit/IOTypes.h (cache mode in bits 8..11).
using task_t=void *; using IOVirtualAddress=uint64_t; using IOOptionBits=uint32_t; using IOByteCount=uint64_t;
constexpr IOOptionBits kIOMapAnywhere=1, kIOMapCacheMask=0xf00, kIOMapInhibitCache=0x100,
                       kIOMapWriteCombineCache=0x400, kIOMapReadOnly=0x1000;
class IOMemoryMap;
class IOSubMemoryDescriptor : public IOMemoryDescriptor {
    IOMemoryDescriptor *parent_=nullptr;
public:
    static inline bool fail_init=false;
    // The base class records what a subclass forwards for the task mapping;
    // the real one hands these options to the parent descriptor's doMap.
    static inline IOOptionBits last_map_options=0;
    static inline unsigned map_calls=0;
    bool writable=false;
    virtual IOMemoryMap *makeMapping(IOMemoryDescriptor *,task_t,IOVirtualAddress,IOOptionBits options,IOByteCount,IOByteCount) {
        last_map_options=options; ++map_calls; return nullptr;
    }
    bool initSubRange(IOMemoryDescriptor *parent,uint64_t offset,uint64_t length,unsigned direction) {
        if (fail_init) return false;
        if (!parent || offset || length!=16384 || (direction!=kIODirectionOut && direction!=kIODirectionInOut)) return false;
        writable=direction==kIODirectionInOut;
        parent_=parent; parent->retain(); bytes=parent->bytes; return true;
    }
    void free() override { if (parent_) parent_->release(); IOMemoryDescriptor::free(); }
};
class IODMACommand;
// Only the access methods used by the real transport fast path are simulated.
class IOPCIDevice {
public:
    bool *inactive=nullptr;
    uint16_t vendor=0x15b3;
    bool logically_inactive=false;
    unsigned config_reads=0;
    bool isInactive() const { return logically_inactive || (inactive && *inactive); }
    uint16_t configRead16(unsigned) { ++config_reads; return inactive && *inactive ? 0xffff : vendor; }
};
class IOService;
class IOMemoryMap {
public:
    std::vector<uint64_t> words=std::vector<uint64_t>(262144);
    unsigned *writes=nullptr;
    uint64_t length=words.size()*sizeof(uint64_t);
    uint64_t getLength() const { return length; }
    uintptr_t getVirtualAddress() { if (writes) ++*writes; return reinterpret_cast<uintptr_t>(words.data()); }
};
class IOMapper;
