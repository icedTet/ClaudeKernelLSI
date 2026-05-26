/**
 * LSI9300Driver.cpp — macOS DriverKit extension for LSI 9300-series HBAs
 *
 * Full implementation of IOUserSCSIParallelInterfaceController for the
 * Broadcom SAS3008 chip (LSI 9300-8i / 9300-4i) operating in IT mode.
 *
 * DriverKit lifecycle:
 *   Start()                  → saves PCI device, calls super (triggers below)
 *   UserInitializeController → PCI open, BAR map, IOC reset, DMA alloc, IOCInit
 *   UserStartController      → MSI-X, interrupt unmask, PortEnable
 *   Stop()                   → full hardware teardown
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LSI9300Driver.h"
#include <DriverKit/OSCollections.h>
#include <DriverKit/IOLib.h>
#include <os/log.h>

// ---------------------------------------------------------------------------
// Logging macros
// ---------------------------------------------------------------------------

#define LSI_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "[LSI9300] " fmt, ##__VA_ARGS__)
#define LSI_ERR(fmt, ...) \
    os_log_error(OS_LOG_DEFAULT, "[LSI9300][ERROR] " fmt, ##__VA_ARGS__)
#define LSI_DEBUG(fmt, ...) \
    os_log_debug(OS_LOG_DEFAULT, "[LSI9300][DBG] " fmt, ##__VA_ARGS__)

// ---------------------------------------------------------------------------
// MMIO accessor helpers
//
// OSReadLittleInt32 / OSWriteLittleInt32 are kernel-only; in DriverKit we
// use direct volatile pointer reads/writes.  On ARM64 (Apple Silicon) the
// CPU is natively little-endian so no byte-swap is needed.  OSSynchronizeIO()
// issues a "dmb oshst" to ensure MMIO store ordering.
// ---------------------------------------------------------------------------

inline uint32_t LSI9300Driver::ReadReg32(uint32_t offset)
{
    return *(volatile uint32_t *)(fBAR1Base + offset);
}

inline void LSI9300Driver::WriteReg32(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(fBAR1Base + offset) = value;
    OSSynchronizeIO();   // dmb oshst — ensures store is visible to device
}

// ===========================================================================
// Start — saves the PCI device reference, then calls super.
//
// The framework calls UserInitializeController and UserStartController
// in sequence after super::Start returns success.
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, Start)(IOService *provider)
{
    LSI_LOG("Start: probing LSI 9300 (SAS3008) in IT mode");

    // Save the PCI device before calling super — UserInitializeController
    // does not receive the provider argument and reads fPCIDevice directly.
    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        LSI_ERR("Start: provider is not an IOPCIDevice");
        return kIOReturnNoDevice;
    }
    fPCIDevice->retain();

    // Call the base class — this registers with the framework and triggers
    // the UserInitializeController → UserStartController call chain.
    kern_return_t ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: super::Start failed: 0x%x", ret);
        OSSafeReleaseNULL(fPCIDevice);
        return ret;
    }

    return kIOReturnSuccess;
}

// ===========================================================================
// Stop — full hardware teardown
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, Stop)(IOService *provider)
{
    LSI_LOG("Stop: taking LSI 9300 offline");

    fControllerOnline = false;

    // Cancel and release the interrupt source before touching hardware
    if (fInterruptSource) {
        fInterruptSource->Cancel();
        OSSafeReleaseNULL(fInterruptSource);
    }

    if (fBAR1Base) {
        MaskInterrupts();
        (void)SoftResetIOC();    // best-effort reset
    }

    FreeDMAPools();

    if (fBAR1Map) {
        OSSafeReleaseNULL(fBAR1Map);
        fBAR1Base = nullptr;
    }

    if (fPCIDevice) {
        fPCIDevice->Close(this, 0);
        OSSafeReleaseNULL(fPCIDevice);
    }

    // Release any pending completion OSAction objects
    for (uint32_t i = 0; i < kNumRequestFrames; i++) {
        if (fCmdCtx[i].completion) {
            OSSafeReleaseNULL(fCmdCtx[i].completion);
            fCmdCtx[i].inUse = false;
        }
    }

    return Stop(provider, SUPERDISPATCH);
}

// ===========================================================================
// UserInitializeController
//
// Called by the framework after Start() returns success.
// Responsible for: PCI open, BAR map, IOC reset, IOCFacts, DMA pool alloc,
// IOCInit, and interrupt source setup.
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, UserInitializeController)()
{
    kern_return_t ret;

    LSI_LOG("UserInitializeController: opening PCI device");

    // Open the PCI device (enables config-space and MMIO access)
    ret = fPCIDevice->Open(this, 0);
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserInitializeController: failed to open PCI device: 0x%x", ret);
        return ret;
    }

    // Enable PCI bus mastering (required for DMA) and memory space decode
    {
        uint16_t cmd = 0;
        fPCIDevice->ConfigurationRead16(kIOPCIConfigurationOffsetCommand, &cmd);
        cmd |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace);
        fPCIDevice->ConfigurationWrite16(kIOPCIConfigurationOffsetCommand, cmd);
    }

    // Map BAR1 (64-bit MMIO, system interface registers)
    ret = fPCIDevice->MapMemory(kIOPCIMemoryRangeBAR1, &fBAR1Map);
    if (ret != kIOReturnSuccess || !fBAR1Map) {
        LSI_ERR("UserInitializeController: failed to map BAR1: 0x%x", ret);
        goto fail_map;
    }
    fBAR1Base = reinterpret_cast<volatile uint8_t *>(fBAR1Map->GetAddress());
    LSI_LOG("UserInitializeController: BAR1 at %p (len %llu)",
            fBAR1Base, fBAR1Map->GetLength());

    // Mask all interrupts while initialising
    MaskInterrupts();

    // Soft-reset the IOC and wait for READY state
    ret = SoftResetIOC();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserInitializeController: IOC reset failed: 0x%x", ret);
        goto fail_reset;
    }

    // Retrieve IOC capabilities
    ret = ReadIOCFacts();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserInitializeController: IOCFacts failed: 0x%x", ret);
        goto fail_facts;
    }

    // Allocate DMA descriptor pools
    ret = AllocateDMAPools();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserInitializeController: DMA pool alloc failed: 0x%x", ret);
        goto fail_dma;
    }

    // Populate the reply-free queue with all reply frame physical addresses
    FillReplyFreeQueue();

    // Send IOCInit to give the IOC all four queue addresses/depths
    ret = SendIOCInit();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserInitializeController: IOCInit failed: 0x%x", ret);
        goto fail_init;
    }

    // Set up the MSI-X interrupt dispatch source (vector 0)
    {
        ret = IOInterruptDispatchSource::Create(
                  fPCIDevice,
                  0,                   // first MSI-X vector
                  GetDispatchQueue(),
                  &fInterruptSource);
        if (ret != kIOReturnSuccess || !fInterruptSource) {
            LSI_ERR("UserInitializeController: interrupt source create failed: 0x%x",
                    ret);
            goto fail_irq;
        }

        ret = fInterruptSource->SetHandler(
                  this,
                  OSMemberFunctionCast(
                      IOInterruptDispatchSource::ActionBlock,
                      this,
                      &LSI9300Driver::HandleInterrupt));
        if (ret != kIOReturnSuccess) {
            LSI_ERR("UserInitializeController: SetHandler failed: 0x%x", ret);
            goto fail_irq_handler;
        }

        ret = fInterruptSource->Activate();
        if (ret != kIOReturnSuccess) {
            LSI_ERR("UserInitializeController: Activate failed: 0x%x", ret);
            goto fail_irq_handler;
        }
    }

    LSI_LOG("UserInitializeController: hardware ready — %u credits, FW 0x%08x",
            fIOCFacts.RequestCredit, fIOCFacts.FWVersion);
    return kIOReturnSuccess;

fail_irq_handler:
    if (fInterruptSource) {
        fInterruptSource->Cancel();
        OSSafeReleaseNULL(fInterruptSource);
    }
fail_irq:
fail_init:
    FreeDMAPools();
fail_dma:
fail_facts:
fail_reset:
    OSSafeReleaseNULL(fBAR1Map);
    fBAR1Base = nullptr;
fail_map:
    fPCIDevice->Close(this, 0);
    return ret;
}

// ===========================================================================
// UserStartController
//
// Called by the framework after UserInitializeController() succeeds.
// Unmasks reply interrupts, enables event notifications, and triggers
// SAS/SATA topology discovery via PortEnable.
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, UserStartController)()
{
    LSI_LOG("UserStartController: enabling I/O");

    // Unmask the reply-available interrupt now that MSI-X is active
    UnmaskInterrupts();

    // Enable async event notifications (SAS discovery, device add/remove)
    kern_return_t ret = EnableEventNotification();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserStartController: EventNotification failed: 0x%x", ret);
        // Non-fatal
    }

    // Trigger SAS/SATA topology discovery (async; devices appear via events)
    ret = SendPortEnable();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("UserStartController: PortEnable failed: 0x%x", ret);
        // Non-fatal
    }

    fControllerOnline = true;
    LSI_LOG("UserStartController: controller online");
    return kIOReturnSuccess;
}

// ===========================================================================
// Interrupt / mask helpers
// ===========================================================================

void LSI9300Driver::MaskInterrupts(void)
{
    WriteReg32(MPI3_SYSIF_HOST_INT_MASK_REG, MPI3_SYSIF_HOST_INT_MASK_ALL);
}

void LSI9300Driver::UnmaskInterrupts(void)
{
    // Unmask only the reply-available interrupt; keep doorbell masked
    uint32_t mask = MPI3_SYSIF_HOST_INT_MASK_ALL
                  & ~MPI3_SYSIF_HOST_INT_MASK_REPLY;
    WriteReg32(MPI3_SYSIF_HOST_INT_MASK_REG, mask);
}

// ===========================================================================
// IOC state
// ===========================================================================

MPT3IOCState LSI9300Driver::ReadIOCState(void)
{
    uint32_t reg = ReadReg32(MPI3_SYSIF_IOC_STATE_REG);
    return mpt3_ioc_state(reg);
}

// ===========================================================================
// SoftResetIOC — diagnostic-register reset sequence
// ===========================================================================

kern_return_t LSI9300Driver::SoftResetIOC(void)
{
    constexpr uint32_t kPollIntervalMS = 10U;
    constexpr uint32_t kMaxPollMS      = 5000U;

    LSI_LOG("SoftResetIOC: resetting controller");

    // Step 1: Unlock the diagnostic register
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_KEY_VALUE_FLUSH);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_1);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_2);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_3);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_4);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_5);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_6);

    // Step 2: Assert HOLD_IOC_RESET
    uint32_t diag = ReadReg32(MPI3_SYSIF_HOST_DIAG_REG);
    if (!(diag & MPI3_SYSIF_HOST_DIAG_DIAG_WRITE_ENABLE)) {
        LSI_ERR("SoftResetIOC: diagnostic write not enabled (diag=0x%08x)", diag);
        return kIOReturnNotPermitted;
    }
    WriteReg32(MPI3_SYSIF_HOST_DIAG_REG, diag | MPI3_SYSIF_HOST_DIAG_HOLD_IOC_RESET);
    IODelay(100);

    // Step 3: Release the reset
    diag = ReadReg32(MPI3_SYSIF_HOST_DIAG_REG);
    WriteReg32(MPI3_SYSIF_HOST_DIAG_REG, diag & ~MPI3_SYSIF_HOST_DIAG_HOLD_IOC_RESET);

    // Step 4: Lock the diagnostic register again
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_KEY_VALUE_FLUSH);

    // Step 5: Wait for the IOC to reach READY state
    uint32_t elapsed = 0;
    while (elapsed < kMaxPollMS) {
        IOSleep(kPollIntervalMS);
        elapsed += kPollIntervalMS;
        MPT3IOCState st = ReadIOCState();
        if (st == MPI3_IOC_STATE_READY) {
            LSI_LOG("SoftResetIOC: IOC in READY state after %u ms", elapsed);
            return kIOReturnSuccess;
        }
        if (st == MPI3_IOC_STATE_FAULT) {
            LSI_ERR("SoftResetIOC: IOC in FAULT state");
            return kIOReturnDeviceError;
        }
    }

    LSI_ERR("SoftResetIOC: timed out waiting for READY (state=%u)",
            (unsigned)ReadIOCState());
    return kIOReturnTimeout;
}

// ===========================================================================
// DoorbellHandshake — send/receive via the legacy doorbell register
// ===========================================================================

kern_return_t LSI9300Driver::DoorbellHandshake(const uint32_t *req,
                                               uint32_t        reqWords,
                                               uint32_t       *reply,
                                               uint32_t        replyWords)
{
    // 1. Verify IOC is in READY state
    if (ReadIOCState() != MPI3_IOC_STATE_READY) {
        LSI_ERR("DoorbellHandshake: IOC not ready");
        return kIOReturnNotReady;
    }

    // 2. Write function code + size word to doorbell (triggers the handshake)
    WriteReg32(MPI3_SYSIF_DOORBELL_REG, req[0]);

    // 3. Wait for IOC to acknowledge (DOORBELL_STATUS bit clears)
    uint32_t timeout = kDoorbellTimeoutMS;
    while (timeout--) {
        IOSleep(1);
        if (!(ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG) &
              MPI3_SYSIF_HOST_INT_STATUS_SYSTEM_TO_IOC_DB_STATUS)) {
            break;
        }
    }
    if (timeout == 0) {
        LSI_ERR("DoorbellHandshake: timeout waiting for DB ACK");
        return kIOReturnTimeout;
    }

    // 4. Send remaining request words (two 16-bit half-words per DWORD)
    for (uint32_t i = 1; i < reqWords; i++) {
        WriteReg32(MPI3_SYSIF_DOORBELL_REG, req[i] & 0xFFFF);
        IODelay(2);
        WriteReg32(MPI3_SYSIF_DOORBELL_REG, (req[i] >> 16) & 0xFFFF);
        IODelay(2);
    }

    // 5. Wait for DOORBELL_STATUS interrupt (reply ready)
    timeout = kDoorbellTimeoutMS;
    while (timeout--) {
        IOSleep(1);
        if (ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG) &
            MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS) {
            break;
        }
    }
    if (timeout == 0) {
        LSI_ERR("DoorbellHandshake: timeout waiting for reply");
        return kIOReturnTimeout;
    }

    // 6. Read reply length (in DWORDs) from the doorbell register.
    uint32_t replyLenDW = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
    if (replyLenDW == 0 || replyLenDW > replyWords) {
        LSI_ERR("DoorbellHandshake: bad reply length %u DW (max %u)",
                replyLenDW, replyWords);
        return kIOReturnBadArgument;
    }

    WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
               MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

    // 7. Read reply data.  Doorbell is 16-bit wide: two reads per DWORD.
    for (uint32_t i = 0; i < replyLenDW; i++) {
        // low half-word
        timeout = 200;
        while (timeout--) {
            IODelay(10);
            if (ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG) &
                MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS) {
                break;
            }
        }
        if (timeout == 0) {
            LSI_ERR("DoorbellHandshake: timeout reading reply word %u (low)", i);
            return kIOReturnTimeout;
        }
        uint32_t lo = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
        WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
                   MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

        // high half-word
        timeout = 200;
        while (timeout--) {
            IODelay(10);
            if (ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG) &
                MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS) {
                break;
            }
        }
        if (timeout == 0) {
            LSI_ERR("DoorbellHandshake: timeout reading reply word %u (high)", i);
            return kIOReturnTimeout;
        }
        uint32_t hi = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
        WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
                   MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

        reply[i] = lo | (hi << 16);
    }

    return kIOReturnSuccess;
}

// ===========================================================================
// ReadIOCFacts
// ===========================================================================

kern_return_t LSI9300Driver::ReadIOCFacts(void)
{
    MPT3IOCFactsRequest req = {};
    req.Header.Function  = MPI3_FUNCTION_IOC_FACTS;
    req.Header.MsgFlags  = 0;

    static_assert(sizeof(req) % 4 == 0, "request must be DWORD aligned");

    uint32_t reply[sizeof(MPT3IOCFactsReply) / sizeof(uint32_t)] = {};
    kern_return_t ret = DoorbellHandshake(
                            reinterpret_cast<const uint32_t *>(&req),
                            sizeof(req) / sizeof(uint32_t),
                            reply,
                            sizeof(reply) / sizeof(uint32_t));
    if (ret != kIOReturnSuccess) return ret;

    __builtin_memcpy(&fIOCFacts, reply, sizeof(fIOCFacts));

    if (fIOCFacts.Header.IOCStatus != MPI3_IOCSTATUS_SUCCESS) {
        LSI_ERR("ReadIOCFacts: IOCStatus=0x%04x", fIOCFacts.Header.IOCStatus);
        return kIOReturnDeviceError;
    }

    LSI_LOG("ReadIOCFacts: MaxDevices=%u MaxCredit=%u FW=0x%08x",
            fIOCFacts.IOCMaxDevices,
            fIOCFacts.RequestCredit,
            fIOCFacts.FWVersion);
    return kIOReturnSuccess;
}

// ===========================================================================
// AllocateDMAPools
// ===========================================================================

kern_return_t LSI9300Driver::AllocateDMAPools(void)
{
    kern_return_t ret;

    auto allocPool = [&](IOBufferMemoryDescriptor **desc,
                         uint64_t *physBase,
                         void    **virtBase,
                         size_t    size,
                         const char *name) -> kern_return_t
    {
        ret = IOBufferMemoryDescriptor::Create(
                  kIOMemoryDirectionInOut,
                  size,
                  0,
                  desc);
        if (ret != kIOReturnSuccess || !*desc) {
            LSI_ERR("AllocateDMAPools: failed to create %s pool: 0x%x", name, ret);
            return ret;
        }
        ret = (*desc)->Map(0, 0, 0, 0, reinterpret_cast<uint64_t *>(virtBase));
        if (ret != kIOReturnSuccess) {
            LSI_ERR("AllocateDMAPools: failed to map %s pool: 0x%x", name, ret);
            return ret;
        }
        IODMACommandSpecification spec = {
            .options        = kIODMACommandSpecificationNoOptions,
            .maxAddressBits = 64,
        };
        IODMACommand *dmaCmd = nullptr;
        ret = IODMACommand::Create(fPCIDevice, 0, &spec, &dmaCmd);
        if (ret != kIOReturnSuccess) {
            LSI_ERR("AllocateDMAPools: DMA command create failed for %s: 0x%x",
                    name, ret);
            return ret;
        }
        ret = dmaCmd->Prepare(*desc, 0, size, true, physBase, nullptr);
        dmaCmd->release();
        if (ret != kIOReturnSuccess) {
            LSI_ERR("AllocateDMAPools: DMA prepare failed for %s: 0x%x", name, ret);
            return ret;
        }
        LSI_DEBUG("AllocateDMAPools: %s virt=%p phys=0x%llx size=%zu",
                  name, *virtBase, *physBase, size);
        return kIOReturnSuccess;
    };

    // 1. Request frame pool
    ret = allocPool(&fRequestFramePool,
                    &fRequestFramePhysBase,
                    reinterpret_cast<void **>(&fRequestFrameVirtBase),
                    kNumRequestFrames * MPT3_REQUEST_FRAME_SIZE,
                    "RequestFrames");
    if (ret != kIOReturnSuccess) return ret;

    // 2a. Reply frame pool (128-byte frames)
    ret = allocPool(&fReplyFramePool,
                    &fReplyFramePhysBase,
                    reinterpret_cast<void **>(&fReplyFrameVirtBase),
                    kNumReplyFrames * MPT3_REPLY_FRAME_SIZE,
                    "ReplyFrames");
    if (ret != kIOReturnSuccess) return ret;

    // Verify reply frames are in the first 4 GiB (MPI2 spec requirement)
    if ((fReplyFramePhysBase >> 32) != 0) {
        LSI_ERR("AllocateDMAPools: reply frame pool above 4 GiB (0x%llx)",
                fReplyFramePhysBase);
        return kIOReturnNoResources;
    }

    // 2b. Reply free queue ring (uint32_t array of reply frame PAs)
    ret = allocPool(&fReplyFreeQueueRing,
                    &fReplyFreeRingPhys,
                    reinterpret_cast<void **>(&fReplyFreeRingVirt),
                    kNumReplyFrames * sizeof(uint32_t),
                    "ReplyFreeQueueRing");
    if (ret != kIOReturnSuccess) return ret;

    // 3. Reply post queue (8-byte descriptor ring, IOC → host)
    ret = allocPool(&fReplyPostQueue,
                    &fReplyPostPhysBase,
                    reinterpret_cast<void **>(&fReplyPostVirtBase),
                    kReplyQueueDepth * MPT3_REPLY_DESCRIPTOR_SIZE,
                    "ReplyPostQueue");
    if (ret != kIOReturnSuccess) return ret;

    // Pre-fill reply post queue with "unused" sentinels
    for (uint32_t i = 0; i < kReplyQueueDepth; i++) {
        fReplyPostVirtBase[i].Words = 0xFFFFFFFFFFFFFFFFULL;
    }

    // 4. Sense buffer pool
    ret = allocPool(&fSenseBufferPool,
                    &fSensePhysBase,
                    reinterpret_cast<void **>(&fSenseVirtBase),
                    kNumRequestFrames * kSenseBufferSize,
                    "SenseBuffers");
    if (ret != kIOReturnSuccess) return ret;

    // 5. Initialise the SMID free-list (SMIDs are 1-based)
    for (uint32_t i = 0; i < kNumRequestFrames; i++) {
        fSMIDFreeList[i] = static_cast<uint16_t>(i + 1);
        fCmdCtx[i].requestFrame  = &fRequestFrameVirtBase[i];
        fCmdCtx[i].sensePhysAddr = fSensePhysBase + i * kSenseBufferSize;
        fCmdCtx[i].completion    = nullptr;
        fCmdCtx[i].inUse         = false;
    }
    fSMIDFreeHead = 0;
    fSMIDFreeTail = kNumRequestFrames;

    return kIOReturnSuccess;
}

void LSI9300Driver::FreeDMAPools(void)
{
    OSSafeReleaseNULL(fSenseBufferPool);
    OSSafeReleaseNULL(fReplyPostQueue);
    OSSafeReleaseNULL(fReplyFreeQueueRing);
    OSSafeReleaseNULL(fReplyFramePool);
    OSSafeReleaseNULL(fRequestFramePool);
    fRequestFrameVirtBase = nullptr;
    fReplyFrameVirtBase   = nullptr;
    fReplyFreeRingVirt    = nullptr;
    fReplyPostVirtBase    = nullptr;
    fSenseVirtBase        = nullptr;
}

// ===========================================================================
// FillReplyFreeQueue
// ===========================================================================

void LSI9300Driver::FillReplyFreeQueue(void)
{
    for (uint32_t i = 0; i < kNumReplyFrames; i++) {
        uint64_t phys = fReplyFramePhysBase + i * MPT3_REPLY_FRAME_SIZE;
        fReplyFreeRingVirt[i] = static_cast<uint32_t>(phys & 0xFFFFFFFFULL);
    }
    OSSynchronizeIO();
    fReplyFreeIndex = kNumReplyFrames;
    WriteReg32(MPI3_SYSIF_REPLY_FREE_HOST_INDEX_REG, fReplyFreeIndex);
}

// ===========================================================================
// SendIOCInit
// ===========================================================================

kern_return_t LSI9300Driver::SendIOCInit(void)
{
    MPT3IOCInitRequest req = {};

    req.Function    = MPI3_FUNCTION_IOC_INIT;
    req.WhoInit     = 0x04U;    // MPI2_WHOINIT_HOST_DRIVER
    req.MsgVersion  = 0x0200U;
    req.HostMSIxVectors = 1;

    req.SystemRequestFrameBaseAddress =
        fRequestFramePhysBase;
    req.SystemRequestFrameSize =
        static_cast<uint16_t>(MPT3_REQUEST_FRAME_SIZE / sizeof(uint32_t));

    req.SenseBufferAddressHigh =
        static_cast<uint32_t>(fSensePhysBase >> 32);

    req.ReplyDescriptorPostQueueAddress = fReplyPostPhysBase;
    req.ReplyDescriptorPostQueueDepth   =
        static_cast<uint16_t>(kReplyQueueDepth);

    req.ReplyFreeQueueAddress = fReplyFreeRingPhys;
    req.ReplyFreeQueueDepth   =
        static_cast<uint16_t>(kNumReplyFrames);

    static_assert(sizeof(req) % 4 == 0, "IOCInit must be DWORD-aligned");

    MPT3IOCInitReply reply = {};
    uint32_t replyWords = sizeof(reply) / sizeof(uint32_t);
    kern_return_t ret = DoorbellHandshake(
                            reinterpret_cast<const uint32_t *>(&req),
                            sizeof(req) / sizeof(uint32_t),
                            reinterpret_cast<uint32_t *>(&reply),
                            replyWords);
    if (ret != kIOReturnSuccess) return ret;

    if (reply.IOCStatus != MPI3_IOCSTATUS_SUCCESS) {
        LSI_ERR("SendIOCInit: IOCStatus=0x%04x", reply.IOCStatus);
        return kIOReturnDeviceError;
    }

    LSI_LOG("SendIOCInit: IOC operational — reqPool 0x%llx replyPost 0x%llx",
            fRequestFramePhysBase, fReplyPostPhysBase);
    return kIOReturnSuccess;
}

// ===========================================================================
// EnableEventNotification
// ===========================================================================

kern_return_t LSI9300Driver::EnableEventNotification(void)
{
    MPT3EventNotificationRequest req = {};
    req.Header.Function = MPI3_FUNCTION_EVENT_NOTIFICATION;
    req.EventSwitches[0] =
          (1U << MPI3_EVENT_SAS_DISCOVERY)
        | (1U << MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST)
        | (1U << MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE)
        | (1U << MPI3_EVENT_DEVICE_ADDED);

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0;

    __builtin_memcpy(fRequestFrameVirtBase, &req, sizeof(req));
    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    return kIOReturnSuccess;
}

// ===========================================================================
// SendPortEnable
// ===========================================================================

kern_return_t LSI9300Driver::SendPortEnable(void)
{
    struct {
        MPT3RequestHeader   Header;
        uint8_t             PhysicalPort;
        uint8_t             Reserved[3];
    } req = {};
    req.Header.Function = MPI3_FUNCTION_PORT_ENABLE;
    req.PhysicalPort    = 0xFF;

    __builtin_memcpy(fRequestFrameVirtBase, &req, sizeof(req));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    return kIOReturnSuccess;
}

// ===========================================================================
// SMID allocation
// ===========================================================================

uint16_t LSI9300Driver::AllocateSMID(void)
{
    if (fSMIDFreeHead == fSMIDFreeTail) return 0;
    uint16_t smid = fSMIDFreeList[fSMIDFreeHead % kNumRequestFrames];
    fSMIDFreeHead++;
    return smid;
}

void LSI9300Driver::FreeSMID(uint16_t smid)
{
    if (smid == 0 || smid > kNumRequestFrames) return;
    fSMIDFreeList[fSMIDFreeTail % kNumRequestFrames] = smid;
    fSMIDFreeTail++;
}

// ===========================================================================
// PostRequestDescriptor
// ===========================================================================

void LSI9300Driver::PostRequestDescriptor(uint32_t low, uint32_t high)
{
    WriteReg32(MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_LOW_REG,  low);
    WriteReg32(MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_HIGH_REG, high);
}

void LSI9300Driver::ReturnReplyFrameToFreeQueue(uint64_t replyFramePhys)
{
    fReplyFreeRingVirt[fReplyFreeIndex] =
        static_cast<uint32_t>(replyFramePhys & 0xFFFFFFFFULL);
    OSSynchronizeIO();
    fReplyFreeIndex = (fReplyFreeIndex + 1) % kNumReplyFrames;
    WriteReg32(MPI3_SYSIF_REPLY_FREE_HOST_INDEX_REG, fReplyFreeIndex);
}

// ===========================================================================
// BuildSGL — single-segment SGL using the contiguous DMA address from the
//            framework (SCSIUserParallelTask.fBufferIOVMAddr)
// ===========================================================================

kern_return_t LSI9300Driver::BuildSGL(MPT3SCSIIORequest          *req,
                                      const SCSIUserParallelTask &task)
{
    uint64_t dataLen  = task.fRequestedTransferCount;
    uint64_t dataPhys = task.fBufferIOVMAddr;

    if (dataLen == 0 || dataPhys == 0) {
        req->DataLength = 0;
        return kIOReturnSuccess;
    }

    req->DataLength = static_cast<uint32_t>(dataLen);

    // Build a single simple SGL entry.
    // The DriverKit framework guarantees one contiguous physical segment.
    uint32_t flags = MPI3_SGE_FLAGS_SIMPLE_ELEMENT
                   | MPI3_SGE_FLAGS_64_BIT_ADDRESSING
                   | MPI3_SGE_FLAGS_LAST_ELEMENT
                   | MPI3_SGE_FLAGS_END_OF_BUFFER
                   | MPI3_SGE_FLAGS_END_OF_LIST;

    // kSCSIDataTransfer_FromInitiatorToTarget == write (host → device)
    if (task.fTransferDirection == kSCSIDataTransfer_FromInitiatorToTarget) {
        flags |= MPI3_SGE_FLAGS_HOST_TO_IOC;
    }

    req->SGL[0].FlagsLength   = flags
                              | (static_cast<uint32_t>(dataLen) & MPI3_SGE_LENGTH_MASK);
    req->SGL[0].DataBufferLow  = static_cast<uint32_t>(dataPhys & 0xFFFFFFFFULL);
    req->SGL[0].DataBufferHigh = static_cast<uint32_t>(dataPhys >> 32);

    return kIOReturnSuccess;
}

// ===========================================================================
// UserProcessParallelTask — main I/O submission path
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, UserProcessParallelTask)(
    SCSIUserParallelTask  parallelRequest,
    uint32_t             *response,
    OSAction             *completion)
{
    *response = kSCSIServiceResponse_Request_In_Process;

    if (!fControllerOnline) {
        *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return kIOReturnOffline;
    }

    uint16_t smid = AllocateSMID();
    if (smid == 0) {
        LSI_ERR("UserProcessParallelTask: no free SMID slots");
        *response = kSCSIServiceResponse_TASK_SET_FULL;
        return kIOReturnBusy;
    }

    MPT3CommandContext &ctx = fCmdCtx[smid - 1];
    ctx.controllerTaskID = parallelRequest.fControllerTaskIdentifier;
    ctx.targetID         = static_cast<SCSITargetIdentifier>(parallelRequest.fTargetID);
    ctx.completion       = completion;
    ctx.completion->retain();   // retained until CompleteScsiIO releases it
    ctx.inUse = true;

    MPT3SCSIIORequest *req = ctx.requestFrame;
    __builtin_memset(req, 0, sizeof(*req));

    // Fill request header
    req->Header.Function = MPI3_FUNCTION_SCSI_IO;
    req->DevHandle       = static_cast<uint16_t>(parallelRequest.fTargetID);

    // CDB (fCommandDescriptorBlock is SCSICommandDescriptorBlock = uint8_t[16])
    static_assert(sizeof(parallelRequest.fCommandDescriptorBlock) <= sizeof(req->CDB),
                  "CDB field size mismatch");
    __builtin_memcpy(req->CDB,
                     parallelRequest.fCommandDescriptorBlock,
                     parallelRequest.fCommandSize);

    // Sense buffer (pre-allocated per SMID)
    req->SenseBufferLowAddress = static_cast<uint32_t>(ctx.sensePhysAddr & 0xFFFFFFFFULL);
    req->SenseBufferLength     = kSenseBufferSize;

    // Task attribute (simple, ordered, ACA, HoQ)
    req->TaskAttributes = static_cast<uint8_t>(parallelRequest.fTaskAttribute);

    // SGL — single contiguous segment from the framework-prepared DMA buffer
    req->SGLOffset0 = offsetof(MPT3SCSIIORequest, SGL) / sizeof(uint32_t);
    kern_return_t sglRet = BuildSGL(req, parallelRequest);
    if (sglRet != kIOReturnSuccess) {
        OSSafeReleaseNULL(ctx.completion);
        ctx.inUse = false;
        FreeSMID(smid);
        *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        return sglRet;
    }

    // DMA barrier: ensure request frame data is written to memory before
    // the descriptor is posted to the controller via MMIO.
    OSSynchronizeIO();

    // Build and post the SCSI IO request descriptor
    MPT3SCSIIORequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_SCSI_IO;
    desc.MSIxIndex      = 0;
    desc.SMID           = smid;
    desc.DevHandle      = req->DevHandle;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    return kIOReturnSuccess;
}

// ===========================================================================
// Interrupt handler
// ===========================================================================

void LSI9300Driver::HandleInterrupt(IOInterruptDispatchSource * /*source*/,
                                    uint64_t /*timestamp*/)
{
    uint32_t status = ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG);

    if (!(status & MPI3_SYSIF_HOST_INT_STATUS_REPLY_DESCRIPTOR_INT)) {
        return;  // spurious interrupt
    }

    WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
               MPI3_SYSIF_HOST_INT_STATUS_REPLY_DESCRIPTOR_INT);

    for (;;) {
        MPT3ReplyDescriptor &desc = fReplyPostVirtBase[fReplyPostIndex];

        if (desc.Words == 0xFFFFFFFFFFFFFFFFULL) break;

        uint8_t descType = desc.AddressReply.DescriptorType;

        if (descType == MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY) {
            uint64_t replyPhys =
                static_cast<uint64_t>(desc.AddressReply.ReplyFrameAddress) << 4;
            uint64_t offset = replyPhys - fReplyFramePhysBase;
            if (offset < kNumReplyFrames * MPT3_REPLY_FRAME_SIZE) {
                const MPT3ReplyHeader *hdr =
                    reinterpret_cast<MPT3ReplyHeader *>(fReplyFrameVirtBase + offset);

                switch (hdr->Function) {
                    case MPI3_FUNCTION_SCSI_IO: {
                        const MPT3SCSIIOReply *ioReply =
                            reinterpret_cast<const MPT3SCSIIOReply *>(hdr);
                        CompleteScsiIO(ioReply, desc.AddressReply.SMID);
                        break;
                    }
                    case MPI3_FUNCTION_EVENT_NOTIFICATION: {
                        const MPT3EventNotificationReply *ev =
                            reinterpret_cast<const MPT3EventNotificationReply *>(hdr);
                        HandleEventNotification(ev);
                        break;
                    }
                    default:
                        LSI_DEBUG("HandleInterrupt: unhandled function 0x%02x",
                                  hdr->Function);
                        break;
                }
                ReturnReplyFrameToFreeQueue(replyPhys);
            }
        }

        desc.Words = 0xFFFFFFFFFFFFFFFFULL;
        fReplyPostIndex = (fReplyPostIndex + 1) % kReplyQueueDepth;
        WriteReg32(MPI3_SYSIF_REPLY_POST_HOST_INDEX_REG, fReplyPostIndex);
    }
}

// ===========================================================================
// CompleteScsiIO
// ===========================================================================

void LSI9300Driver::CompleteScsiIO(const MPT3SCSIIOReply *reply, uint16_t smid)
{
    if (smid == 0 || smid > kNumRequestFrames) {
        LSI_ERR("CompleteScsiIO: invalid SMID %u", smid);
        return;
    }

    MPT3CommandContext &ctx = fCmdCtx[smid - 1];
    if (!ctx.inUse || !ctx.completion) {
        LSI_ERR("CompleteScsiIO: SMID %u not in use", smid);
        return;
    }

    // Build the DriverKit parallel response struct
    SCSIUserParallelResponse resp = {};
    resp.version                    = kScsiUserParallelTaskCurrentVersion1;
    resp.fControllerTaskIdentifier  = ctx.controllerTaskID;
    resp.fTargetID                  = static_cast<uint64_t>(ctx.targetID);

    if (reply->Header.IOCStatus != MPI3_IOCSTATUS_SUCCESS &&
        reply->Header.IOCStatus != MPI3_IOCSTATUS_SCSI_DATA_UNDERRUN) {
        // Hardware-level failure
        resp.fServiceResponse  = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        resp.fCompletionStatus = kSCSITaskStatus_No_Status;
        LSI_ERR("CompleteScsiIO: SMID %u IOCStatus=0x%04x",
                smid, reply->Header.IOCStatus);
    } else {
        resp.fServiceResponse  = kSCSIServiceResponse_TASK_COMPLETE;
        resp.fCompletionStatus = static_cast<SCSITaskStatus>(reply->SCSIStatus);
        resp.fBytesTransferred = reply->TransferCount;

        // Copy autosense data if valid
        if (reply->SCSIState & MPI3_SCSI_STATE_AUTOSENSE_VALID) {
            uint8_t *senseData = fSenseVirtBase + (smid - 1) * kSenseBufferSize;
            uint8_t  senseLen  = static_cast<uint8_t>(
                                    reply->SenseCount < sizeof(resp.fSenseBuffer)
                                    ? reply->SenseCount
                                    : sizeof(resp.fSenseBuffer));
            __builtin_memcpy(resp.fSenseBuffer, senseData, senseLen);
            resp.fSenseLength = senseLen;
        }

        if (reply->Header.IOCStatus == MPI3_IOCSTATUS_SCSI_DATA_UNDERRUN) {
            LSI_DEBUG("CompleteScsiIO: underrun SMID=%u actual=%u",
                      smid, reply->TransferCount);
        }
    }

    // Release the SMID before the completion callback (it may re-queue)
    OSAction *completion = ctx.completion;
    ctx.completion = nullptr;
    ctx.inUse      = false;
    FreeSMID(smid);

    // Invoke the framework completion callback
    ParallelTaskCompletion(completion, resp);
    completion->release();
}

// ===========================================================================
// HandleEventNotification
// ===========================================================================

void LSI9300Driver::HandleEventNotification(const MPT3EventNotificationReply *event)
{
    switch (event->Event) {
        case MPI3_EVENT_SAS_DISCOVERY:
            LSI_LOG("Event: SAS discovery in progress");
            break;
        case MPI3_EVENT_DEVICE_ADDED:
            LSI_LOG("Event: SAS/SATA device added");
            break;
        case MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE:
            LSI_LOG("Event: SAS device status changed");
            break;
        case MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST:
            LSI_LOG("Event: SAS topology change");
            break;
        default:
            LSI_DEBUG("HandleEventNotification: unknown event 0x%08x", event->Event);
            break;
    }

    if (event->AckRequired) {
        struct {
            MPT3RequestHeader Header;
            uint32_t          Event;
            uint32_t          EventContext;
        } ack = {};
        ack.Header.Function = MPI3_FUNCTION_EVENT_ACK;
        ack.Event           = event->Event;
        ack.EventContext    = event->EventContext;

        __builtin_memcpy(fRequestFrameVirtBase, &ack, sizeof(ack));
        MPT3HighPriorityRequestDescriptor desc = {};
        desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
        PostRequestDescriptor(
            *reinterpret_cast<uint32_t *>(&desc),
            *(reinterpret_cast<uint32_t *>(&desc) + 1));
    }
}

// ===========================================================================
// Task management
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, UserAbortTaskRequest)(
    uint64_t  theT,
    uint64_t  theL,
    uint64_t  theQ,
    uint32_t *response)
{
    LSI_LOG("UserAbortTaskRequest: target=%llu lun=%llu tag=%llu",
            theT, theL, theQ);

    MPT3SCTMRequest req = {};
    req.Header.Function = MPI3_FUNCTION_SCSI_TASK_MGMT;
    req.DevHandle       = static_cast<uint16_t>(theT);
    req.TaskType        = MPI3_SCSITASKMGMT_TASKTYPE_ABORT_TASK;
    req.TaskMID         = static_cast<uint16_t>(theQ);

    __builtin_memcpy(fRequestFrameVirtBase, &req, sizeof(req));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    *response = kSCSIServiceResponse_Request_In_Process;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserAbortTaskSetRequest)(
    uint64_t  theT,
    uint64_t  theL,
    uint32_t *response)
{
    LSI_LOG("UserAbortTaskSetRequest / TargetReset: target=%llu", theT);

    MPT3SCTMRequest req = {};
    req.Header.Function = MPI3_FUNCTION_SCSI_TASK_MGMT;
    req.DevHandle       = static_cast<uint16_t>(theT);
    req.TaskType        = MPI3_SCSITASKMGMT_TASKTYPE_TARGET_RESET;

    __builtin_memcpy(fRequestFrameVirtBase, &req, sizeof(req));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    *response = kSCSIServiceResponse_Request_In_Process;
    return kIOReturnSuccess;
}

// ===========================================================================
// Capability report overrides
// ===========================================================================

kern_return_t IMPL(LSI9300Driver, UserMapHBAData)(uint32_t *uniqueTaskID)
{
    // SMID allocation is handled inside UserProcessParallelTask.
    // Report 0 here; the framework uses the returned uniqueTaskID for its
    // own per-task data tracking, which we do not need to override.
    *uniqueTaskID = 0;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserDoesHBAPerformAutoSense)(bool *result)
{
    // The SAS3008 always writes autosense data to the pre-allocated sense
    // buffer pointed to by SenseBufferLowAddress in the request frame.
    *result = true;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserDoesHBAPerformDeviceManagement)(bool *result)
{
    // We do not perform device management; the storage stack does it.
    *result = false;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserReportMaximumTaskCount)(uint32_t *count)
{
    *count = fIOCFacts.RequestCredit ? fIOCFacts.RequestCredit : kNumRequestFrames;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserReportHighestSupportedDeviceID)(uint64_t *id)
{
    *id = fIOCFacts.IOCMaxDevices ? fIOCFacts.IOCMaxDevices - 1 : 255;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserReportInitiatorIdentifier)(uint64_t *id)
{
    *id = 7;   // default SAS initiator ID
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserReportHBAHighestLogicalUnitNumber)(uint64_t *value)
{
    *value = 255;
    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserReportHBAConstraints)(OSDictionary *constraints)
{
    // Set DMA constraints via key/value pairs in the provided dictionary.
    // kIOMaximumSegmentCountReadKey etc. are defined in DriverKit/IODMACommand.h
    auto setNum = [&](const char *key, uint64_t val) {
        OSNumber *n = OSNumber::withNumber(val, 64);
        if (n) {
            constraints->setObject(key, n);
            n->release();
        }
    };

    setNum(kIOMaximumSegmentAddressableBitCountKey, 64);  // full 64-bit DMA
    setNum(kIOMaximumSegmentCountReadKey,  kMaxSGSegments);
    setNum(kIOMaximumSegmentCountWriteKey, kMaxSGSegments);
    setNum(kIOMaximumByteCountReadKey,  1ULL * 1024 * 1024);  // 1 MiB max
    setNum(kIOMaximumByteCountWriteKey, 1ULL * 1024 * 1024);

    return kIOReturnSuccess;
}

kern_return_t IMPL(LSI9300Driver, UserGetDMASpecification)(
    uint64_t             *maxTransferSize,
    uint32_t             *alignment,
    uint8_t              *numAddressBits,
    DMAOutputSegmentType *segmentType)
{
    *maxTransferSize = 1ULL * 1024 * 1024;  // 1 MiB — hardware limit
    *alignment       = 4;                    // 4-byte alignment
    *numAddressBits  = 64;
    *segmentType     = kIODMACommandOutputSegments64;
    return kIOReturnSuccess;
}
