/**
 * TestStubs.h — Minimal stubs for compiling driver headers in a plain C++
 *               environment (Linux CI / macOS userspace without DriverKit SDK).
 *
 * Include this BEFORE any LSI9300Driver headers when UNIT_TEST is defined.
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef UNIT_TEST

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Stub out DriverKit types / macros used in the headers
// ---------------------------------------------------------------------------

typedef int kern_return_t;
#define kIOReturnSuccess        0
#define kIOReturnError          (-1)
#define kIOReturnTimeout        (-2)
#define kIOReturnNoDevice       (-3)
#define kIOReturnNotPermitted   (-4)
#define kIOReturnBadArgument    (-5)
#define kIOReturnNoSpace        (-6)
#define kIOReturnNoResources    (-7)
#define kIOReturnNotReady       (-8)
#define kIOReturnDeviceError    (-9)
#define kIOReturnBusy           (-10)

// SCSIServiceResponse stubs
typedef int SCSIServiceResponse;
#define kSCSIServiceResponse_Request_In_Process         0
#define kSCSIServiceResponse_TASK_COMPLETE              1
#define kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE 2
#define kSCSIServiceResponse_TASK_SET_FULL              3

typedef int SCSITaskStatus;
#define kSCSITaskStatus_GOOD                            0
#define kSCSITaskStatus_No_Status                       1
#define kSCSITaskStatus_DeliveryFailure                 2

typedef uint64_t SCSITargetIdentifier;
typedef uint64_t SCSILogicalUnitNumber;
typedef uint64_t SCSITaggedTaskIdentifier;
typedef uint64_t SCSIDeviceIdentifier;
typedef uint64_t SCSIParallelTaskIdentifier;
typedef uint8_t  SCSICommandDescriptorBlock[16];

typedef int SCSIParallelFeature;

// Stub DriverKit IOService
class IOService {
public:
    virtual kern_return_t Start(IOService *) { return kIOReturnSuccess; }
    virtual kern_return_t Stop(IOService *)  { return kIOReturnSuccess; }
    virtual void retain()  {}
    virtual void release() {}
};

// Stub IOMapper
class IOMapper {};

// Stub IOMemoryDescriptor
class IOMemoryDescriptor {
public:
    virtual uint64_t GetLength() { return 0; }
    virtual void     release()   {}
};

// Stub IOBufferMemoryDescriptor
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static kern_return_t Create(int, size_t, int, IOBufferMemoryDescriptor **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t Map(int, int, int, int, uint64_t *) { return kIOReturnSuccess; }
};

// Stub IOMemoryMap
class IOMemoryMap {
public:
    uint64_t GetAddress() { return 0; }
    uint64_t GetLength()  { return 0; }
    void     release()    {}
};

// Stub IOInterruptDispatchSource
class IOInterruptDispatchSource {
public:
    typedef void *ActionBlock;
    static kern_return_t Create(void *, uint32_t, void *, IOInterruptDispatchSource **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t SetHandler(void *, void *) { return kIOReturnSuccess; }
    kern_return_t Activate()                 { return kIOReturnSuccess; }
    kern_return_t Cancel()                   { return kIOReturnSuccess; }
    void release() {}
};

// Stub IODMACommand
struct IODMACommandSpecification {
    int options;
    int maxAddressBits;
};
#define kIODMACommandSpecificationNoOptions 0

class IODMACommand {
public:
    static kern_return_t Create(void *, int, IODMACommandSpecification *, IODMACommand **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t Prepare(void *, uint64_t, uint64_t, bool, uint64_t *, void *) {
        return kIOReturnSuccess;
    }
    kern_return_t GetPhysicalSegment(void *, uint64_t, uint64_t *, uint64_t *, int) {
        return kIOReturnError;
    }
    void release() {}
};

// Stub IOPCIDevice
class IOPCIDevice : public IOService {
public:
    kern_return_t Open(void *, int)  { return kIOReturnSuccess; }
    kern_return_t Close(void *, int) { return kIOReturnSuccess; }
    kern_return_t MapMemory(int, IOMemoryMap **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t ConfigurationRead16(int, uint16_t *out) {
        *out = 0; return kIOReturnSuccess;
    }
    kern_return_t ConfigurationWrite16(int, uint16_t) { return kIOReturnSuccess; }
};

// Stub IOUserSCSIParallelInterfaceController
class IOUserSCSIParallelInterfaceController : public IOService {
public:
    virtual kern_return_t Start(IOService *p)  { return IOService::Start(p); }
    virtual kern_return_t Stop(IOService *p)   { return IOService::Stop(p); }

    virtual bool           InitializeController() { return true; }
    virtual void           TerminateController()  {}
    virtual uint32_t       ReportHBASpecificDeviceData()  { return 0; }
    virtual uint32_t       ReportMaximumTaskCount()       { return 512; }
    virtual uint32_t       ReportMaxSupportedTaskCount()  { return 512; }

    virtual SCSIServiceResponse ProcessParallelTask(SCSIParallelTaskIdentifier)
        { return kSCSIServiceResponse_Request_In_Process; }

    virtual SCSIServiceResponse AbortTask(SCSITargetIdentifier, SCSILogicalUnitNumber,
                                          SCSITaggedTaskIdentifier)
        { return kSCSIServiceResponse_Request_In_Process; }

    virtual SCSIServiceResponse AbortTaskSet(SCSITargetIdentifier, SCSILogicalUnitNumber)
        { return kSCSIServiceResponse_Request_In_Process; }

    virtual void ReportHBAConstraints(IOMapper *, SCSIDeviceIdentifier *, SCSILogicalUnitNumber *,
                                      uint32_t *, uint32_t *, uint64_t *, uint64_t *, uint32_t *) {}

    // Helpers the driver calls
    SCSITargetIdentifier   GetTargetIdentifier(SCSIParallelTaskIdentifier) { return 0; }
    void                   GetCommandDescriptorBlock(SCSIParallelTaskIdentifier,
                                                     SCSICommandDescriptorBlock *) {}
    uint8_t                GetCommandDescriptorBlockSize(SCSIParallelTaskIdentifier) { return 6; }
    uint8_t                GetTaskAttribute(SCSIParallelTaskIdentifier) { return 0; }
    uint8_t                GetDataTransferDirection(SCSIParallelTaskIdentifier) { return 0; }
    void                   GetDataBuffer(SCSIParallelTaskIdentifier, IOMemoryDescriptor **out) {
        if (out) *out = nullptr;
    }
    uint64_t               GetRequestedDataTransferCount(SCSIParallelTaskIdentifier) { return 0; }
    void                   SetRealizedDataTransferCount(SCSIParallelTaskIdentifier, uint64_t) {}
    void                   SetAutoSenseData(SCSIParallelTaskIdentifier, uint8_t *, uint8_t) {}
    void                   CompleteParallelTask(SCSIParallelTaskIdentifier,
                                               SCSIServiceResponse, SCSITaskStatus) {}
    void                  *GetDispatchQueue() { return nullptr; }
};

// Stub DriverKit helper macros
#define IMPL(cls, method)
#define OSDynamicCast(T, obj)   (static_cast<T *>(obj))
#define OSSafeReleaseNULL(p)    do { if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define OSMemberFunctionCast(T, obj, fn)  ((T)nullptr)
#define MIN(a, b)               ((a) < (b) ? (a) : (b))

// Stub memory ordering (no-ops in userspace tests)
#define OSSynchronizeIO()       do {} while (0)
#define IODelay(us)             do {} while (0)
#define IOSleep(ms)             do {} while (0)

// Stub PCI config offsets
#define kIOPCIConfigurationOffsetCommand    4
#define kIOPCIMemoryRangeBAR1               1
#define kIOPCICommandBusMaster              (1 << 2)
#define kIOPCICommandMemorySpace            (1 << 1)
#define kIOMemoryDirectionInOut             0

// Stub MMIO read/write (return 0 for reads, ignore writes)
static inline uint32_t OSReadLittleInt32(const volatile void *base, uint32_t offset) {
    (void)base; (void)offset; return 0;
}
static inline void OSWriteLittleInt32(volatile void *base, uint32_t offset, uint32_t value) {
    (void)base; (void)offset; (void)value;
}

// Stub os_log
#define os_log(log, fmt, ...)     printf(fmt "\n", ##__VA_ARGS__)
#define os_log_error(log, fmt, ...) printf("[ERR] " fmt "\n", ##__VA_ARGS__)
#define os_log_debug(log, fmt, ...) do {} while (0)
#define OS_LOG_DEFAULT            0

#endif /* UNIT_TEST */
