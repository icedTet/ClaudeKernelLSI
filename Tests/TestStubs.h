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
#include <stdio.h>

// ---------------------------------------------------------------------------
// Basic DriverKit types
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
#define kIOReturnOffline        (-11)

// ---------------------------------------------------------------------------
// SCSI types
// ---------------------------------------------------------------------------

typedef uint64_t SCSITargetIdentifier;
typedef uint64_t SCSILogicalUnitNumber;
typedef uint64_t SCSITaggedTaskIdentifier;
typedef uint64_t SCSIDeviceIdentifier;
typedef uint8_t  SCSICommandDescriptorBlock[16];
typedef uint8_t  SCSITaskStatus;
typedef uint8_t  SCSIServiceResponse;
typedef uint8_t  SCSITaskAttribute;

#define kSCSIServiceResponse_Request_In_Process         0
#define kSCSIServiceResponse_TASK_COMPLETE              1
#define kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE 2
#define kSCSIServiceResponse_TASK_SET_FULL              3

#define kSCSITaskStatus_GOOD                            0
#define kSCSITaskStatus_No_Status                       1
#define kSCSITaskStatus_DeliveryFailure                 2
#define kSCSITaskStatus_CHECK_CONDITION                 2

#define kSCSIDataTransfer_NoDataTransfer                0
#define kSCSIDataTransfer_FromTargetToInitiator         1
#define kSCSIDataTransfer_FromInitiatorToTarget         2

#define kSCSITask_SIMPLE        0
#define kSCSITask_ORDERED       1
#define kSCSITask_HEAD_OF_QUEUE 2
#define kSCSITask_ACA           3

// SCSIUserParallelTaskVersion enum
typedef uint64_t SCSIUserParallelTaskVersion;
#define kScsiUserParallelTaskCurrentVersion1  ((SCSIUserParallelTaskVersion)1)

// ---------------------------------------------------------------------------
// SCSIUserParallelTask — the struct the framework passes to UserProcessParallelTask
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t                    version;
    uint64_t                    fControllerTaskIdentifier;
    uint64_t                    fTargetID;
    uint8_t                     fLogicalUnitBytes[8];
    SCSICommandDescriptorBlock  fCommandDescriptorBlock;
    uint8_t                     fCommandSize;
    uint8_t                     fTransferDirection;
    uint8_t                     fTaskAttribute;
    uint8_t                     _pad1;
    uint32_t                    _pad2;
    uint64_t                    fRequestedTransferCount;
    uint64_t                    fBufferIOVMAddr;
    uint64_t                    fTaskTagIdentifier;
    uint32_t                    fTimeoutInMilliSec;
    uint8_t                     reserved[20];
} SCSIUserParallelTask;

// ---------------------------------------------------------------------------
// SCSIUserParallelResponse — passed back via ParallelTaskCompletion
// ---------------------------------------------------------------------------

typedef struct {
    uint64_t            version;
    uint64_t            fControllerTaskIdentifier;
    uint64_t            fTargetID;
    SCSIServiceResponse fServiceResponse;
    SCSITaskStatus      fCompletionStatus;
    uint8_t             fSenseLength;
    uint8_t             _pad1;
    uint32_t            _pad2;
    uint64_t            fBytesTransferred;
    uint8_t             fSenseBuffer[256];
    uint8_t             reserved[16];
} SCSIUserParallelResponse;

// ---------------------------------------------------------------------------
// DMA output segment type
// ---------------------------------------------------------------------------

typedef uint32_t DMAOutputSegmentType;
#define kIODMACommandOutputSegments64   2

// ---------------------------------------------------------------------------
// DriverKit object stubs
// ---------------------------------------------------------------------------

class OSObject {
public:
    virtual void retain()  {}
    virtual void release() {}
};

class OSAction : public OSObject {};

class OSDictionary : public OSObject {
public:
    static OSDictionary *withCapacity(uint32_t) { return nullptr; }
    bool setObject(const char *, OSObject *) { return true; }
};

class OSNumber : public OSObject {
public:
    static OSNumber *withNumber(uint64_t, uint32_t) { return new OSNumber(); }
};

// ---------------------------------------------------------------------------
// IOService stubs
// ---------------------------------------------------------------------------

class IOService : public OSObject {
public:
    virtual kern_return_t Start(IOService *, ...) { return kIOReturnSuccess; }
    virtual kern_return_t Stop(IOService *, ...)  { return kIOReturnSuccess; }
};

// ---------------------------------------------------------------------------
// DriverKit hardware / DMA stubs
// ---------------------------------------------------------------------------

class IOMemoryDescriptor : public OSObject {
public:
    virtual uint64_t GetLength() { return 0; }
};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static kern_return_t Create(int, size_t, int, IOBufferMemoryDescriptor **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t Map(int, int, int, int, uint64_t *) { return kIOReturnSuccess; }
};

class IOMemoryMap : public OSObject {
public:
    uint64_t GetAddress() { return 0; }
    uint64_t GetLength()  { return 0; }
};

class IOInterruptDispatchSource : public OSObject {
public:
    typedef void *ActionBlock;
    static kern_return_t Create(void *, uint32_t, void *, IOInterruptDispatchSource **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t SetHandler(void *, void *) { return kIOReturnSuccess; }
    kern_return_t Activate()                 { return kIOReturnSuccess; }
    kern_return_t Cancel()                   { return kIOReturnSuccess; }
};

struct IODMACommandSpecification {
    int options;
    int maxAddressBits;
};
#define kIODMACommandSpecificationNoOptions 0

class IODMACommand : public OSObject {
public:
    static kern_return_t Create(void *, int, IODMACommandSpecification *, IODMACommand **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t Prepare(void *, uint64_t, uint64_t, bool, uint64_t *, void *) {
        return kIOReturnSuccess;
    }
};

class IOPCIDevice : public IOService {
public:
    kern_return_t Open(void *, int)  { return kIOReturnSuccess; }
    kern_return_t Close(void *, int) { return kIOReturnSuccess; }
    kern_return_t MapMemory(int, IOMemoryMap **out) {
        *out = nullptr; return kIOReturnSuccess;
    }
    kern_return_t ConfigurationRead16(int, uint16_t *out) {
        if (out) *out = 0; return kIOReturnSuccess;
    }
    kern_return_t ConfigurationWrite16(int, uint16_t) { return kIOReturnSuccess; }
};

// ---------------------------------------------------------------------------
// IOUserSCSIParallelInterfaceController stub
// Mirrors the actual DriverKit API with User-prefixed methods.
// ---------------------------------------------------------------------------

class IOUserSCSIParallelInterfaceController : public IOService {
public:
    virtual kern_return_t Start(IOService *p, ...)  { return kIOReturnSuccess; }
    virtual kern_return_t Stop(IOService *p, ...)   { return kIOReturnSuccess; }

    // Pure virtual overrides the subclass must implement
    virtual kern_return_t UserInitializeController() { return kIOReturnSuccess; }
    virtual kern_return_t UserStartController()      { return kIOReturnSuccess; }
    virtual kern_return_t UserProcessParallelTask(
        SCSIUserParallelTask, uint32_t *, OSAction *) { return kIOReturnSuccess; }
    virtual kern_return_t UserMapHBAData(uint32_t *id) {
        if (id) *id = 0; return kIOReturnSuccess;
    }
    virtual kern_return_t UserDoesHBAPerformAutoSense(bool *r) {
        if (r) *r = false; return kIOReturnSuccess;
    }

    // Optional overrides
    virtual kern_return_t UserDoesHBAPerformDeviceManagement(bool *r) {
        if (r) *r = false; return kIOReturnSuccess;
    }
    virtual kern_return_t UserReportMaximumTaskCount(uint32_t *c) {
        if (c) *c = 512; return kIOReturnSuccess;
    }
    virtual kern_return_t UserReportHighestSupportedDeviceID(uint64_t *id) {
        if (id) *id = 255; return kIOReturnSuccess;
    }
    virtual kern_return_t UserReportInitiatorIdentifier(uint64_t *id) {
        if (id) *id = 7; return kIOReturnSuccess;
    }
    virtual kern_return_t UserReportHBAHighestLogicalUnitNumber(uint64_t *v) {
        if (v) *v = 255; return kIOReturnSuccess;
    }
    virtual kern_return_t UserReportHBAConstraints(OSDictionary *) {
        return kIOReturnSuccess;
    }
    virtual kern_return_t UserGetDMASpecification(
        uint64_t *sz, uint32_t *align, uint8_t *bits, DMAOutputSegmentType *seg) {
        if (sz)    *sz    = 1024*1024;
        if (align) *align = 4;
        if (bits)  *bits  = 64;
        if (seg)   *seg   = kIODMACommandOutputSegments64;
        return kIOReturnSuccess;
    }
    virtual kern_return_t UserAbortTaskRequest(
        uint64_t, uint64_t, uint64_t, uint32_t *r) {
        if (r) *r = kSCSIServiceResponse_Request_In_Process; return kIOReturnSuccess;
    }
    virtual kern_return_t UserAbortTaskSetRequest(
        uint64_t, uint64_t, uint32_t *r) {
        if (r) *r = kSCSIServiceResponse_Request_In_Process; return kIOReturnSuccess;
    }

    // Framework callback — driver calls this when I/O completes
    void ParallelTaskCompletion(OSAction *, SCSIUserParallelResponse) {}

    // Framework helper
    void *GetDispatchQueue() { return nullptr; }
};

// ---------------------------------------------------------------------------
// DriverKit compiler macros
// ---------------------------------------------------------------------------

// IMPL: in DriverKit the method body is written as IMPL(Class, Method)(args).
// For unit tests we just define IMPL as the standard method definition.
#define IMPL(cls, method)   cls::method

// SUPERDISPATCH: sentinel for calling base-class implementation.
// In tests we define it as an extra ignored argument.
#define SUPERDISPATCH       0

#define OSDeclareDefaultStructors(cls)   /* no-op in unit tests */

#define OSDynamicCast(T, obj)            (static_cast<T *>(obj))
#define OSSafeReleaseNULL(p)             do { \
    if (p) { (p)->release(); (p) = nullptr; } } while (0)
#define OSMemberFunctionCast(T, obj, fn) ((T)nullptr)

// ---------------------------------------------------------------------------
// Platform / timing stubs
// ---------------------------------------------------------------------------

#define OSSynchronizeIO()   do {} while (0)
#define IODelay(us)         do {} while (0)
#define IOSleep(ms)         do {} while (0)

// ---------------------------------------------------------------------------
// PCI constants
// ---------------------------------------------------------------------------

#define kIOPCIConfigurationOffsetCommand    4
#define kIOPCIMemoryRangeBAR1               1
#define kIOPCICommandBusMaster              (1 << 2)
#define kIOPCICommandMemorySpace            (1 << 1)
#define kIOMemoryDirectionInOut             0

// ---------------------------------------------------------------------------
// DMA constraint dictionary keys
// ---------------------------------------------------------------------------

#define kIOMaximumSegmentAddressableBitCountKey  "IOMaximumSegmentAddressableBitCount"
#define kIOMaximumSegmentCountReadKey            "IOMaximumSegmentCountRead"
#define kIOMaximumSegmentCountWriteKey           "IOMaximumSegmentCountWrite"
#define kIOMaximumByteCountReadKey               "IOMaximumByteCountRead"
#define kIOMaximumByteCountWriteKey              "IOMaximumByteCountWrite"

// ---------------------------------------------------------------------------
// Logging stubs
// ---------------------------------------------------------------------------

#define os_log(log, fmt, ...)       printf(fmt "\n", ##__VA_ARGS__)
#define os_log_error(log, fmt, ...) printf("[ERR] " fmt "\n", ##__VA_ARGS__)
#define os_log_debug(log, fmt, ...) do {} while (0)
#define OS_LOG_DEFAULT              ((void*)0)

#endif /* UNIT_TEST */
