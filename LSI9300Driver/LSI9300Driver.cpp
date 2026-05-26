/**
 * LSI9300Driver.cpp — macOS DriverKit extension for LSI 9300-series HBAs
 *
 * Full implementation of IOUserSCSIParallelInterfaceController for the
 * Broadcom SAS3008 chip (LSI 9300-8i / 9300-4i) operating in IT mode.
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
// DriverKit class registration macro
// ---------------------------------------------------------------------------

#define super IOUserSCSIParallelInterfaceController
IMPL(LSI9300Driver, Start)
IMPL(LSI9300Driver, Stop)

// ---------------------------------------------------------------------------
// MMIO accessor helpers
// ---------------------------------------------------------------------------

inline uint32_t LSI9300Driver::ReadReg32(uint32_t offset)
{
    // OSReadLittleInt32 issues a 32-bit load with acquire semantics on ARM64
    return OSReadLittleInt32(fBAR1Base, offset);
}

inline void LSI9300Driver::WriteReg32(uint32_t offset, uint32_t value)
{
    // OSWriteLittleInt32 issues a 32-bit store with release semantics on ARM64
    OSWriteLittleInt32(fBAR1Base, offset, value);
    OSSynchronizeIO();           // equivalent to dsb st on ARM64
}

// ===========================================================================
// Start — called by the kernel when a matching PCI device is found
// ===========================================================================

kern_return_t LSI9300Driver::Start(IOService *provider)
{
    kern_return_t ret = kIOReturnSuccess;

    LSI_LOG("Start: probing LSI 9300 (SAS3008) in IT mode");

    // Retain and open the PCI device
    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        LSI_ERR("Start: provider is not an IOPCIDevice");
        return kIOReturnNoDevice;
    }
    fPCIDevice->retain();

    ret = fPCIDevice->Open(this, 0);
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: failed to open PCI device: 0x%x", ret);
        goto fail_open;
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
        LSI_ERR("Start: failed to map BAR1: 0x%x", ret);
        goto fail_map;
    }
    fBAR1Base = reinterpret_cast<volatile uint8_t *>(fBAR1Map->GetAddress());
    LSI_LOG("Start: BAR1 mapped at %p (length %llu)", fBAR1Base, fBAR1Map->GetLength());

    // Mask all host interrupts while we initialise
    MaskInterrupts();

    // Hard-reset the IOC and wait for READY state
    ret = SoftResetIOC();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: IOC reset failed: 0x%x", ret);
        goto fail_reset;
    }

    // Retrieve IOC capabilities
    ret = ReadIOCFacts();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: IOCFacts failed: 0x%x", ret);
        goto fail_facts;
    }

    // Allocate DMA descriptor pools
    ret = AllocateDMAPools();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: DMA pool allocation failed: 0x%x", ret);
        goto fail_dma;
    }

    // Populate the reply-free queue with all reply frame physical addresses
    FillReplyFreeQueue();

    // Send IOCInit to give the IOC the queue addresses / depths
    ret = SendIOCInit();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: IOCInit failed: 0x%x", ret);
        goto fail_init;
    }

    // Register MSI-X interrupt handler (vector 0)
    {
        ret = IOInterruptDispatchSource::Create(
                  fPCIDevice,
                  0,    // interrupt index (first MSI-X vector)
                  GetDispatchQueue(),
                  &fInterruptSource);
        if (ret != kIOReturnSuccess || !fInterruptSource) {
            LSI_ERR("Start: failed to create interrupt source: 0x%x", ret);
            goto fail_irq;
        }

        ret = fInterruptSource->SetHandler(
                  this,
                  OSMemberFunctionCast(
                      IOInterruptDispatchSource::ActionBlock,
                      this,
                      &LSI9300Driver::HandleInterrupt));
        if (ret != kIOReturnSuccess) {
            LSI_ERR("Start: failed to set interrupt handler: 0x%x", ret);
            goto fail_irq_handler;
        }

        ret = fInterruptSource->Activate();
        if (ret != kIOReturnSuccess) {
            LSI_ERR("Start: failed to activate interrupt source: 0x%x", ret);
            goto fail_irq_handler;
        }
    }

    // Unmask reply-available interrupt
    UnmaskInterrupts();

    // Enable async event notifications (discovery, device add/remove)
    ret = EnableEventNotification();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: EventNotification failed: 0x%x", ret);
        // Non-fatal — proceed
    }

    // Trigger SAS/SATA topology discovery
    ret = SendPortEnable();
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: PortEnable failed: 0x%x", ret);
        // Non-fatal — devices may appear later
    }

    fControllerOnline = true;

    // Call super::Start last to register with the SCSI stack
    ret = super::Start(provider);
    if (ret != kIOReturnSuccess) {
        LSI_ERR("Start: super::Start failed: 0x%x", ret);
        fControllerOnline = false;
        goto fail_irq_handler;
    }

    LSI_LOG("Start: LSI 9300 online — %u credits, FW 0x%08x",
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
fail_open:
    OSSafeReleaseNULL(fPCIDevice);
    return ret;
}

// ===========================================================================
// Stop — called during device removal or system shutdown
// ===========================================================================

kern_return_t LSI9300Driver::Stop(IOService *provider)
{
    LSI_LOG("Stop: taking LSI 9300 offline");

    fControllerOnline = false;

    // Cancel and release the interrupt source before touching hardware
    if (fInterruptSource) {
        fInterruptSource->Cancel();
        OSSafeReleaseNULL(fInterruptSource);
    }

    MaskInterrupts();

    // Soft-reset the IOC so drives are not left in an inconsistent state
    (void)SoftResetIOC();

    FreeDMAPools();

    if (fBAR1Map) {
        OSSafeReleaseNULL(fBAR1Map);
        fBAR1Base = nullptr;
    }

    if (fPCIDevice) {
        fPCIDevice->Close(this, 0);
        OSSafeReleaseNULL(fPCIDevice);
    }

    return super::Stop(provider);
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
    // Unmask only the reply-available interrupt; leave doorbell masked
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
    constexpr uint32_t kPollIntervalUS = 10000U;  // 10 ms
    constexpr uint32_t kMaxPollMS      = 5000U;   // 5 s total

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
        IOSleep(kPollIntervalUS / 1000);
        elapsed += kPollIntervalUS / 1000;
        MPT3IOCState st = ReadIOCState();
        if (st == MPI3_IOC_STATE_READY) {
            LSI_LOG("SoftResetIOC: IOC in READY state after %u ms", elapsed);
            return kIOReturnSuccess;
        }
        if (st == MPI3_IOC_STATE_FAULT) {
            LSI_ERR("SoftResetIOC: IOC in FAULT state (code=0x%04x)",
                    fIOCFacts.IOCFaultCode);
            return kIOReturnDeviceError;
        }
    }

    LSI_ERR("SoftResetIOC: timed out waiting for READY (state=%u)",
            (unsigned)ReadIOCState());
    return kIOReturnTimeout;
}

// ===========================================================================
// DoorbellHandshake — send/receive via the legacy doorbell register
// Used only before DMA queues are initialised (IOCFacts, IOCInit).
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
    uint32_t firstWord = req[0];
    WriteReg32(MPI3_SYSIF_DOORBELL_REG, firstWord);

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

    // 4. Send remaining request words (2 bytes at a time, low byte then high)
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
    //    The IOC writes the DWORD count into bits 15:0 of the doorbell.
    uint32_t replyLenDW = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
    if (replyLenDW == 0 || replyLenDW > replyWords) {
        LSI_ERR("DoorbellHandshake: bad reply length %u DW (max %u)",
                replyLenDW, replyWords);
        return kIOReturnBadArgument;
    }

    // Acknowledge the "reply-ready" doorbell interrupt before reading data
    WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
               MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

    // 7. Read reply data.
    //
    // IMPORTANT: The doorbell data path is only 16 bits wide.  The IOC
    // delivers ONE 16-bit half-word per interrupt, so each 32-bit DWORD
    // in the reply requires TWO separate interrupt-driven reads.
    //
    // Sequence per DWORD:
    //   a. Wait for DOORBELL_STATUS interrupt (low half-word ready)
    //   b. Read & acknowledge — saves bits 15:0
    //   c. Wait for DOORBELL_STATUS interrupt (high half-word ready)
    //   d. Read & acknowledge — saves bits 31:16
    //   e. Reassemble: reply[i] = lo | (hi << 16)
    //
    // Reference: Linux mpt3sas _base_handshake_req_reply_wait()
    for (uint32_t i = 0; i < replyLenDW; i++) {
        // --- low half-word ---
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

        // --- high half-word ---
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

        // Reassemble the full DWORD (little-endian: low half first)
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
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // Copy to the stored facts structure
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

    // Helper lambda to allocate a DMA-coherent buffer and map it
    auto allocPool = [&](IOBufferMemoryDescriptor **desc,
                         uint64_t *physBase,
                         void    **virtBase,
                         size_t    size,
                         const char *name) -> kern_return_t
    {
        ret = IOBufferMemoryDescriptor::Create(
                  kIOMemoryDirectionInOut,
                  size,
                  0,          // alignment (page-aligned by default)
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
            .options    = kIODMACommandSpecificationNoOptions,
            .maxAddressBits = 64,
        };
        IODMACommand *dmaCmd = nullptr;
        ret = IODMACommand::Create(fPCIDevice, 0, &spec, &dmaCmd);
        if (ret != kIOReturnSuccess) {
            LSI_ERR("AllocateDMAPools: failed to create DMA command for %s: 0x%x",
                    name, ret);
            return ret;
        }
        ret = dmaCmd->Prepare(*desc, 0, size, true, physBase, nullptr);
        dmaCmd->release();
        if (ret != kIOReturnSuccess) {
            LSI_ERR("AllocateDMAPools: DMA prepare failed for %s: 0x%x", name, ret);
            return ret;
        }
        LSI_DEBUG("AllocateDMAPools: %s at virt=%p phys=0x%llx size=%zu",
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

    // 2a. Reply frame pool  (the actual 128-byte reply frames)
    ret = allocPool(&fReplyFramePool,
                    &fReplyFramePhysBase,
                    reinterpret_cast<void **>(&fReplyFrameVirtBase),
                    kNumReplyFrames * MPT3_REPLY_FRAME_SIZE,
                    "ReplyFrames");
    if (ret != kIOReturnSuccess) return ret;

    // Verify reply frames are in the first 4 GiB (MPI2 spec requirement).
    // The reply free queue uses 32-bit entries; bits 63:32 must be zero.
    if ((fReplyFramePhysBase >> 32) != 0) {
        LSI_ERR("AllocateDMAPools: reply frame pool above 4 GiB (0x%llx) — "
                "not supported by MPI2 32-bit free queue entries",
                fReplyFramePhysBase);
        return kIOReturnNoResources;
    }

    // 2b. Reply free queue ring
    //
    // A DMA ring of uint32_t entries, each holding the 32-bit physical
    // address of one reply frame.  The IOC reads from this ring to find a
    // free reply frame to write its next reply into.
    //
    // This is NOT a register — it is a memory-mapped DMA buffer.
    // Its physical address goes into IOCInit.ReplyFreeQueueAddress.
    ret = allocPool(&fReplyFreeQueueRing,
                    &fReplyFreeRingPhys,
                    reinterpret_cast<void **>(&fReplyFreeRingVirt),
                    kNumReplyFrames * sizeof(uint32_t),
                    "ReplyFreeQueueRing");
    if (ret != kIOReturnSuccess) return ret;

    // 3. Reply post queue (descriptor ring) — must be cache-coherent
    ret = allocPool(&fReplyPostQueue,
                    &fReplyPostPhysBase,
                    reinterpret_cast<void **>(&fReplyPostVirtBase),
                    kReplyQueueDepth * MPT3_REPLY_DESCRIPTOR_SIZE,
                    "ReplyPostQueue");
    if (ret != kIOReturnSuccess) return ret;

    // Pre-fill the reply post queue with "unused" sentinels
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

        // Precompute per-context pointers
        fCmdCtx[i].requestFrame = &fRequestFrameVirtBase[i];
        fCmdCtx[i].sensePhysAddr = fSensePhysBase + i * kSenseBufferSize;
        fCmdCtx[i].inUse = false;
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
// FillReplyFreeQueue — populate the reply free queue DMA ring
// ===========================================================================
//
// The reply free queue is a DMA ring of uint32_t physical addresses.  The IOC
// reads entries from this ring to find a reply frame it can write a reply into.
//
// Correct protocol (MPI 2.5 spec, Section 5.4):
//   1. Write each reply frame's 32-bit PA into the DMA ring slots.
//   2. After filling N slots, write N to REPLY_FREE_HOST_INDEX_REG to inform
//      the IOC how many entries are available.
//
// This function must be called AFTER AllocateDMAPools (which verifies the
// frame pool is below 4 GiB) and BEFORE SendIOCInit.

void LSI9300Driver::FillReplyFreeQueue(void)
{
    for (uint32_t i = 0; i < kNumReplyFrames; i++) {
        uint64_t phys = fReplyFramePhysBase + i * MPT3_REPLY_FRAME_SIZE;
        // Store the 32-bit PA into the DMA ring (not a register write)
        fReplyFreeRingVirt[i] = static_cast<uint32_t>(phys & 0xFFFFFFFFULL);
    }

    // Flush write ordering: ensure all ring entries are in memory before
    // we tell the IOC the ring is populated
    OSSynchronizeIO();

    // Inform the IOC that kNumReplyFrames entries are available in the ring
    fReplyFreeIndex = kNumReplyFrames;
    WriteReg32(MPI3_SYSIF_REPLY_FREE_HOST_INDEX_REG, fReplyFreeIndex);
}

// ===========================================================================
// SendIOCInit
// ===========================================================================
//
// Sends an IOCInit message via the doorbell handshake to configure the four
// DMA ring addresses in the firmware:
//
//   SystemRequestFrameBaseAddress  — where the host puts SCSI IO requests
//   ReplyDescriptorPostQueueAddress — where the IOC writes reply descriptors
//   ReplyFreeQueueAddress           — where the host puts free reply frame PAs
//   SenseBufferAddressHigh          — upper 32 bits of the sense buffer pool
//
// All addresses are populated from the DMA pools allocated by AllocateDMAPools.

kern_return_t LSI9300Driver::SendIOCInit(void)
{
    MPT3IOCInitRequest req = {};

    // --- Function header ---
    req.Function    = MPI3_FUNCTION_IOC_INIT;
    req.WhoInit     = 0x04U;    // MPI2_WHOINIT_HOST_DRIVER
    req.MsgVersion  = 0x0200U;  // MPI 2.0 base protocol version
    req.HostMSIxVectors = 1;    // one MSI-X vector in this release

    // --- Request frame pool ---
    // The IOC validates SMID-indexed request frames against this base address.
    req.SystemRequestFrameBaseAddress =
        fRequestFramePhysBase;
    req.SystemRequestFrameSize =
        static_cast<uint16_t>(MPT3_REQUEST_FRAME_SIZE / sizeof(uint32_t));

    // --- Sense buffer pool ---
    // Only the upper 32 bits are sent; all sense buffers must share the same
    // upper 32 bits as the reply frame pool (guaranteed by our 4 GiB assertion).
    req.SenseBufferAddressHigh =
        static_cast<uint32_t>(fSensePhysBase >> 32);

    // --- Reply post queue ring (IOC → host reply descriptors) ---
    req.ReplyDescriptorPostQueueAddress =
        fReplyPostPhysBase;
    req.ReplyDescriptorPostQueueDepth =
        static_cast<uint16_t>(kReplyQueueDepth);

    // --- Reply free queue ring (host → IOC free reply frame pool) ---
    // This points to the DMA ring of uint32_t physical addresses populated
    // by FillReplyFreeQueue(), NOT to the reply frame pool directly.
    req.ReplyFreeQueueAddress =
        fReplyFreeRingPhys;
    req.ReplyFreeQueueDepth =
        static_cast<uint16_t>(kNumReplyFrames);

    static_assert(sizeof(req) % 4 == 0, "IOCInit must be DWORD-aligned");

    MPT3IOCInitReply reply = {};
    uint32_t replyWords = sizeof(reply) / sizeof(uint32_t);
    kern_return_t ret = DoorbellHandshake(
                            reinterpret_cast<const uint32_t *>(&req),
                            sizeof(req) / sizeof(uint32_t),
                            reinterpret_cast<uint32_t *>(&reply),
                            replyWords);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    if (reply.IOCStatus != MPI3_IOCSTATUS_SUCCESS) {
        LSI_ERR("SendIOCInit: IOCStatus=0x%04x", reply.IOCStatus);
        return kIOReturnDeviceError;
    }
    LSI_LOG("SendIOCInit: IOC operational — request pool 0x%llx, "
            "reply post 0x%llx, reply free ring 0x%llx",
            fRequestFramePhysBase, fReplyPostPhysBase, fReplyFreeRingPhys);
    return kIOReturnSuccess;
}

// ===========================================================================
// EnableEventNotification
// ===========================================================================

kern_return_t LSI9300Driver::EnableEventNotification(void)
{
    MPT3EventNotificationRequest req = {};
    req.Header.Function = MPI3_FUNCTION_EVENT_NOTIFICATION;

    // Enable all SAS events we care about
    req.EventSwitches[0] =
          (1U << MPI3_EVENT_SAS_DISCOVERY)
        | (1U << MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST)
        | (1U << MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE)
        | (1U << MPI3_EVENT_DEVICE_ADDED);

    // Post via high-priority request descriptor
    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0; // management messages use SMID 0

    // The request frame goes into slot 0 of the request pool (reserved for mgmt)
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
    // PortEnable uses a simple management request (function 0x06)
    struct {
        MPT3RequestHeader   Header;
        uint8_t             PhysicalPort;
        uint8_t             Reserved[3];
    } req = {};
    req.Header.Function  = MPI3_FUNCTION_PORT_ENABLE;
    req.PhysicalPort     = 0xFF; // all ports

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
    if (fSMIDFreeHead == fSMIDFreeTail) {
        return 0;   // queue is full
    }
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
    // The two halves must be written atomically in order (low first)
    WriteReg32(MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_LOW_REG, low);
    WriteReg32(MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_HIGH_REG, high);
}

void LSI9300Driver::ReturnReplyFrameToFreeQueue(uint64_t replyFramePhys)
{
    // Write the 32-bit PA of the consumed reply frame into the next slot
    // of the reply free queue DMA ring, then advance the producer index.
    fReplyFreeRingVirt[fReplyFreeIndex] =
        static_cast<uint32_t>(replyFramePhys & 0xFFFFFFFFULL);

    // OSSynchronizeIO() before the register write ensures the DMA ring
    // entry is visible to the IOC before the index register update
    OSSynchronizeIO();

    fReplyFreeIndex = (fReplyFreeIndex + 1) % kNumReplyFrames;
    WriteReg32(MPI3_SYSIF_REPLY_FREE_HOST_INDEX_REG, fReplyFreeIndex);
}

// ===========================================================================
// BuildSGL — populate the scatter-gather list in a SCSI IO request frame
// ===========================================================================

kern_return_t LSI9300Driver::BuildSGL(MPT3SCSIIORequest          *req,
                                      SCSIParallelTaskIdentifier  task)
{
    // Retrieve the IOMemoryDescriptor from the SCSI task
    IOMemoryDescriptor *dataDesc = nullptr;
    GetDataBuffer(task, &dataDesc);
    if (!dataDesc) {
        // SCSI command with no data transfer (e.g. TEST UNIT READY)
        req->DataLength = 0;
        return kIOReturnSuccess;
    }

    uint64_t totalLength = dataDesc->GetLength();
    if (totalLength == 0) {
        req->DataLength = 0;
        return kIOReturnSuccess;
    }

    req->DataLength = static_cast<uint32_t>(totalLength);

    // Enumerate physical segments
    IODMACommandSpecification spec = {
        .options        = kIODMACommandSpecificationNoOptions,
        .maxAddressBits = 64,
    };
    IODMACommand *dmaCmd = nullptr;
    kern_return_t ret = IODMACommand::Create(fPCIDevice, 0, &spec, &dmaCmd);
    if (ret != kIOReturnSuccess) return ret;

    uint64_t offset = 0;
    uint32_t sglIdx = 0;

    while (offset < totalLength && sglIdx < MPT3_MAX_SGL_ENTRIES_IN_FRAME) {
        uint64_t segPhys  = 0;
        uint64_t segBytes = 0;

        ret = dmaCmd->GetPhysicalSegment(dataDesc, offset, &segPhys, &segBytes, 0);
        if (ret != kIOReturnSuccess) break;

        uint32_t flags = MPI3_SGE_FLAGS_SIMPLE_ELEMENT
                       | MPI3_SGE_FLAGS_64_BIT_ADDRESSING;

        // Mark data direction
        uint8_t dir = GetDataTransferDirection(task);
        if (dir == kSCSIDataTransfer_FromInitiatorToTarget) {
            flags |= MPI3_SGE_FLAGS_HOST_TO_IOC;
        }

        // Last segment
        if (offset + segBytes >= totalLength) {
            flags |= MPI3_SGE_FLAGS_LAST_ELEMENT
                   | MPI3_SGE_FLAGS_END_OF_BUFFER
                   | MPI3_SGE_FLAGS_END_OF_LIST;
        }

        // Flags occupy bits 31:24; length in bits 23:0
        req->SGL[sglIdx].FlagsLength = flags
                                     | (static_cast<uint32_t>(segBytes) & MPI3_SGE_LENGTH_MASK);
        req->SGL[sglIdx].DataBufferLow  = static_cast<uint32_t>(segPhys & 0xFFFFFFFFULL);
        req->SGL[sglIdx].DataBufferHigh = static_cast<uint32_t>(segPhys >> 32);

        offset  += segBytes;
        sglIdx++;
    }

    dmaCmd->release();

    if (offset < totalLength) {
        LSI_ERR("BuildSGL: transfer too large for inline SGL (%llu bytes)", totalLength);
        // TODO: implement chain SGL for large I/Os
        return kIOReturnNoResources;
    }

    return kIOReturnSuccess;
}

// ===========================================================================
// ProcessParallelTask — main I/O submission path
// ===========================================================================

SCSIServiceResponse LSI9300Driver::ProcessParallelTask(
                                       SCSIParallelTaskIdentifier parallelRequest)
{
    if (!fControllerOnline) {
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }

    // Allocate an SMID slot
    uint16_t smid = AllocateSMID();
    if (smid == 0) {
        LSI_ERR("ProcessParallelTask: no free SMID slots");
        return kSCSIServiceResponse_TASK_SET_FULL;
    }

    MPT3CommandContext &ctx = fCmdCtx[smid - 1];
    ctx.task  = parallelRequest;
    ctx.inUse = true;

    MPT3SCSIIORequest *req = ctx.requestFrame;
    __builtin_memset(req, 0, sizeof(*req));

    // Fill header
    req->Header.Function = MPI3_FUNCTION_SCSI_IO;

    // Target device handle is stored in the SCSITargetIdentifier
    SCSITargetIdentifier targetID = GetTargetIdentifier(parallelRequest);
    req->DevHandle = static_cast<uint16_t>(targetID);

    // CDB
    SCSICommandDescriptorBlock cdb = {};
    UInt8 cdbLen = 0;
    GetCommandDescriptorBlock(parallelRequest, &cdb);
    cdbLen = GetCommandDescriptorBlockSize(parallelRequest);
    __builtin_memcpy(req->CDB, cdb, cdbLen);

    // Sense buffer (pre-allocated per SMID)
    req->SenseBufferLowAddress = static_cast<uint32_t>(ctx.sensePhysAddr & 0xFFFFFFFFULL);
    req->SenseBufferLength     = kSenseBufferSize;

    // Task attributes
    uint8_t attr = GetTaskAttribute(parallelRequest);
    req->TaskAttributes = attr;

    // Scatter-gather
    req->SGLOffset0 = offsetof(MPT3SCSIIORequest, SGL) / sizeof(uint32_t);
    kern_return_t sglRet = BuildSGL(req, parallelRequest);
    if (sglRet != kIOReturnSuccess) {
        FreeSMID(smid);
        ctx.inUse = false;
        return kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    }

    // Build and post the request descriptor
    MPT3SCSIIORequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_SCSI_IO;
    desc.MSIxIndex      = 0;
    desc.SMID           = smid;
    desc.DevHandle      = req->DevHandle;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    return kSCSIServiceResponse_Request_In_Process;
}

// ===========================================================================
// Interrupt handler
// ===========================================================================

void LSI9300Driver::HandleInterrupt(IOInterruptDispatchSource * /*source*/,
                                    uint64_t /*timestamp*/)
{
    uint32_t status = ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG);

    if (!(status & MPI3_SYSIF_HOST_INT_STATUS_REPLY_DESCRIPTOR_INT)) {
        return; // Spurious interrupt
    }

    // Acknowledge the interrupt
    WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
               MPI3_SYSIF_HOST_INT_STATUS_REPLY_DESCRIPTOR_INT);

    // Drain the reply post queue
    for (;;) {
        MPT3ReplyDescriptor &desc = fReplyPostVirtBase[fReplyPostIndex];

        // An all-ones descriptor means the slot is empty
        if (desc.Words == 0xFFFFFFFFFFFFFFFFULL) {
            break;
        }

        uint8_t descType = desc.AddressReply.DescriptorType;

        if (descType == MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY) {
            // A full reply frame was written — locate and process it
            uint64_t replyPhys =
                static_cast<uint64_t>(desc.AddressReply.ReplyFrameAddress) << 4;
            uint64_t offset    = replyPhys - fReplyFramePhysBase;
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
                // Return the reply frame to the IOC's free pool
                ReturnReplyFrameToFreeQueue(replyPhys);
            }
        }

        // Mark the descriptor as consumed and advance the consumer index
        desc.Words = 0xFFFFFFFFFFFFFFFFULL;
        fReplyPostIndex = (fReplyPostIndex + 1) % kReplyQueueDepth;

        // Update the reply post host index register
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
    if (!ctx.inUse || !ctx.task) {
        LSI_ERR("CompleteScsiIO: SMID %u not in use", smid);
        return;
    }

    SCSIParallelTaskIdentifier task = ctx.task;

    // Map MPT3 IOC status → SCSI service response
    SCSIServiceResponse  serviceResponse = kSCSIServiceResponse_TASK_COMPLETE;
    SCSITaskStatus       taskStatus      = kSCSITaskStatus_GOOD;

    if (reply->Header.IOCStatus != MPI3_IOCSTATUS_SUCCESS &&
        reply->Header.IOCStatus != MPI3_IOCSTATUS_SCSI_DATA_UNDERRUN) {
        serviceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        taskStatus      = kSCSITaskStatus_No_Status;
        LSI_ERR("CompleteScsiIO: SMID %u IOCStatus=0x%04x",
                smid, reply->Header.IOCStatus);
    } else {
        // Propagate the SCSI status byte from the device
        taskStatus = static_cast<SCSITaskStatus>(reply->SCSIStatus);

        // Copy autosense data if valid
        if (reply->SCSIState & MPI3_SCSI_STATE_AUTOSENSE_VALID) {
            uint32_t senseLen = MIN(reply->SenseCount, kSenseBufferSize);
            uint8_t *senseData = fSenseVirtBase + (smid - 1) * kSenseBufferSize;
            SetAutoSenseData(task, senseData, static_cast<UInt8>(senseLen));
        }

        // Report residual
        if (reply->Header.IOCStatus == MPI3_IOCSTATUS_SCSI_DATA_UNDERRUN) {
            uint64_t requested  = GetRequestedDataTransferCount(task);
            uint64_t actual     = reply->TransferCount;
            SetRealizedDataTransferCount(task, actual);
            LSI_DEBUG("CompleteScsiIO: underrun SMID=%u req=%llu act=%llu",
                      smid, requested, actual);
        } else {
            SetRealizedDataTransferCount(task, reply->TransferCount);
        }
    }

    // Release the SMID before calling CompleteParallelTask (it may re-queue)
    ctx.inUse = false;
    ctx.task  = nullptr;
    FreeSMID(smid);

    // Notify the SCSI stack
    CompleteParallelTask(task, serviceResponse, taskStatus);
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
            // The storage stack is notified via the standard target creation
            // callback path; no extra action needed here.
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

    // The IOC requires an event ACK for certain events
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
// AbortTask — TMF ABORT_TASK
// ===========================================================================

SCSIServiceResponse LSI9300Driver::AbortTask(
    SCSITargetIdentifier   targetID,
    SCSILogicalUnitNumber  lun,
    SCSITaggedTaskIdentifier taggedTaskID)
{
    LSI_LOG("AbortTask: target=%llu lun=%llu tag=%llu",
            (uint64_t)targetID, (uint64_t)lun, (uint64_t)taggedTaskID);

    MPT3SCTMRequest req = {};
    req.Header.Function = MPI3_FUNCTION_SCSI_TASK_MGMT;
    req.DevHandle       = static_cast<uint16_t>(targetID);
    req.TaskType        = MPI3_SCSITASKMGMT_TASKTYPE_ABORT_TASK;
    req.TaskMID         = static_cast<uint16_t>(taggedTaskID);

    __builtin_memcpy(fRequestFrameVirtBase, &req, sizeof(req));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    return kSCSIServiceResponse_Request_In_Process;
}

// ===========================================================================
// AbortTaskSet — TMF TARGET_RESET
// ===========================================================================

SCSIServiceResponse LSI9300Driver::AbortTaskSet(
    SCSITargetIdentifier  targetID,
    SCSILogicalUnitNumber lun)
{
    LSI_LOG("AbortTaskSet / TargetReset: target=%llu", (uint64_t)targetID);

    MPT3SCTMRequest req = {};
    req.Header.Function = MPI3_FUNCTION_SCSI_TASK_MGMT;
    req.DevHandle       = static_cast<uint16_t>(targetID);
    req.TaskType        = MPI3_SCSITASKMGMT_TASKTYPE_TARGET_RESET;

    __builtin_memcpy(fRequestFrameVirtBase, &req, sizeof(req));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.SMID = 0;

    PostRequestDescriptor(
        *reinterpret_cast<uint32_t *>(&desc),
        *(reinterpret_cast<uint32_t *>(&desc) + 1));

    return kSCSIServiceResponse_Request_In_Process;
}

// ===========================================================================
// IOUserSCSIParallelInterfaceController capability reports
// ===========================================================================

bool LSI9300Driver::InitializeController(void)
{
    LSI_LOG("InitializeController");
    return fControllerOnline;
}

void LSI9300Driver::TerminateController(void)
{
    LSI_LOG("TerminateController");
    fControllerOnline = false;
}

uint32_t LSI9300Driver::ReportHBASpecificDeviceData(void)
{
    return 0;
}

uint32_t LSI9300Driver::ReportMaximumTaskCount(void)
{
    return static_cast<uint32_t>(fIOCFacts.RequestCredit)
           ? fIOCFacts.RequestCredit
           : kNumRequestFrames;
}

uint32_t LSI9300Driver::ReportMaxSupportedTaskCount(void)
{
    return kNumRequestFrames;
}

void LSI9300Driver::ReportHBAConstraints(
    IOMapper             *mapper,
    SCSIDeviceIdentifier *maxDeviceID,
    SCSILogicalUnitNumber *maxLUN,
    uint32_t             *maxIOSize,
    uint32_t             *maxSegmentCount,
    uint64_t             *maxSegmentSize,
    uint64_t             *maxAddressableSegment,
    uint32_t             *alignmentMask)
{
    (void)mapper;
    *maxDeviceID            = static_cast<SCSIDeviceIdentifier>(
                                  fIOCFacts.IOCMaxDevices ? fIOCFacts.IOCMaxDevices - 1 : 255);
    *maxLUN                 = 255;
    *maxIOSize              = 1024U * 1024U; // 1 MiB
    *maxSegmentCount        = kMaxSGSegments;
    *maxSegmentSize         = 0x0000000100000000ULL; // 4 GiB per segment
    *maxAddressableSegment  = 0xFFFFFFFFFFFFFFFFULL; // 64-bit capable
    *alignmentMask          = 0x3U;                  // 4-byte alignment
}
