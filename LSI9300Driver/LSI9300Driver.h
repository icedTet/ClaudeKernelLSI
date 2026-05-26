/**
 * LSI9300Driver.h — macOS DriverKit extension for LSI 9300-series HBAs
 *
 * Implements IOUserSCSIParallelInterfaceController to expose SAS/SATA
 * drives attached to an LSI SAS3008 (9300-8i) in IT (passthrough) mode
 * to macOS on Apple Silicon (ARM64) systems.
 *
 * Build requirements:
 *   macOS 12.0+, Xcode 14+, DriverKit SDK 21+
 *   Entitlement: com.apple.developer.driverkit.family.scsi-controller
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOService.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <SCSIControllerDriverKit/IOUserSCSIParallelInterfaceController.h>

#include "MPT3Registers.h"
#include "MPT3Types.h"

/* =========================================================================
 * Sizing / configuration constants
 * ========================================================================= */

/** Number of request frames to allocate.  Must be ≤ IOCFacts.RequestCredit */
static constexpr uint32_t kNumRequestFrames     = 512U;

/** Number of reply frames in the free queue */
static constexpr uint32_t kNumReplyFrames       = 512U;

/** Depth of the reply post queue (ring buffer read by the interrupt handler).
 *  Must be a power of 2 and ≥ kNumReplyFrames + 1 */
static constexpr uint32_t kReplyQueueDepth      = 512U;

/** Maximum physical scatter-gather segments per I/O */
static constexpr uint32_t kMaxSGSegments        = 128U;

/** Per-command sense buffer size (minimum 252 per SAM-4) */
static constexpr uint32_t kSenseBufferSize      = 252U;

/** Timeout for synchronous doorbell messages (IOCFacts/IOCInit) in ms */
static constexpr uint32_t kDoorbellTimeoutMS    = 5000U;

/* =========================================================================
 * Per-command context  (one per SMID slot)
 * ========================================================================= */

struct MPT3CommandContext {
    /** Physical address of the DMA-able data buffer for this command */
    uint64_t            dataPhysAddr;
    /** Physical address of the pre-allocated sense buffer */
    uint64_t            sensePhysAddr;
    /** Virtual pointer to the request frame in the pool */
    MPT3SCSIIORequest  *requestFrame;
    /** The SCSIParallelTask token from DriverKit */
    SCSIParallelTaskIdentifier  task;
    /** Whether this slot is in use */
    bool                inUse;
    uint8_t             _pad[7];
};

/* =========================================================================
 * Main driver class
 * ========================================================================= */

class LSI9300Driver final : public IOUserSCSIParallelInterfaceController {

    // DriverKit macro – declares the class in the dext namespace
    using super = IOUserSCSIParallelInterfaceController;

public:

    /* ------------------------------------------------------------------
     * IOService lifecycle
     * ------------------------------------------------------------------ */

    /**
     * Called by the kernel when a matching PCI device is found.
     * Performs:
     *   1. PCI resource enumeration (BARs, MSI-X)
     *   2. MPT3 controller reset and IOCFacts handshake
     *   3. Descriptor pool allocation (DMA)
     *   4. IOCInit to bring the controller online
     *   5. Event notification enable
     *   6. Port enable (triggers SAS/SATA discovery)
     */
    virtual kern_return_t Start(IOService *provider) override;

    /**
     * Called during device removal or system shutdown.
     * Issues an IOC reset then releases all DMA pools and interrupt sources.
     */
    virtual kern_return_t Stop(IOService *provider) override;

    /* ------------------------------------------------------------------
     * IOUserSCSIParallelInterfaceController overrides
     * ------------------------------------------------------------------ */

    /**
     * Return the maximum number of logical units per target.
     * SAS drives expose a single LUN 0; expanders can have more.
     */
    virtual uint32_t      ReportHBASpecificDeviceData(void) override;

    /**
     * Return the maximum number of outstanding tasks per target.
     */
    virtual uint32_t      ReportMaximumTaskCount(void) override;

    /**
     * Return the maximum supported CDB length.
     */
    virtual uint32_t      ReportMaxSupportedTaskCount(void) override;

    /**
     * Called for each new SCSI task submitted by the storage stack.
     * Builds an MPT3SCSIIORequest frame, posts the descriptor, returns
     * kSCSIServiceResponse_Request_In_Process on success.
     */
    virtual SCSIServiceResponse ProcessParallelTask(
                                    SCSIParallelTaskIdentifier parallelRequest) override;

    /**
     * Called to abort a specific task (TMF ABORT_TASK).
     */
    virtual SCSIServiceResponse AbortTask(
                                    SCSITargetIdentifier targetID,
                                    SCSILogicalUnitNumber logicalUnitNumber,
                                    SCSITaggedTaskIdentifier taggedTaskID) override;

    /**
     * Called to reset a target (TMF TARGET_RESET).
     */
    virtual SCSIServiceResponse AbortTaskSet(
                                    SCSITargetIdentifier targetID,
                                    SCSILogicalUnitNumber logicalUnitNumber) override;

    /**
     * Called to reset the entire HBA.
     */
    virtual bool              InitializeController(void) override;
    virtual void              TerminateController(void) override;

    /**
     * Report HBA constraints to the storage layer.
     */
    virtual void              ReportHBAConstraints(
                                    IOMapper *mapper,
                                    SCSIDeviceIdentifier *maxDeviceID,
                                    SCSILogicalUnitNumber *maxLUN,
                                    uint32_t *maxIOSize,
                                    uint32_t *maxSegmentCount,
                                    uint64_t *maxSegmentSize,
                                    uint64_t *maxAddressableSegment,
                                    uint32_t *alignmentMask) override;

private:

    /* ------------------------------------------------------------------
     * Private helpers — hardware control
     * ------------------------------------------------------------------ */

    /** Mask all host interrupts */
    void    MaskInterrupts(void);

    /** Unmask reply-available interrupt (used after MSI-X setup) */
    void    UnmaskInterrupts(void);

    /** Read IOC state from the doorbell register */
    MPT3IOCState    ReadIOCState(void);

    /**
     * Perform a full IOC soft-reset via the diagnostic register.
     * Blocks until the IOC reaches READY state (up to kDoorbellTimeoutMS).
     */
    kern_return_t   SoftResetIOC(void);

    /**
     * Doorbell-based handshake: send a request frame word-by-word and read
     * back the reply.  Used only for IOCFacts and IOCInit before the DMA
     * pools are live.
     *
     * @param req      Pointer to the request frame
     * @param reqWords Size of the request in 32-bit words
     * @param reply    Buffer to receive the reply (up to replyWords words)
     * @param replyWords Maximum reply size in 32-bit words
     */
    kern_return_t   DoorbellHandshake(const uint32_t *req,
                                      uint32_t        reqWords,
                                      uint32_t       *reply,
                                      uint32_t        replyWords);

    /** Read IOCFacts and store results in fIOCFacts */
    kern_return_t   ReadIOCFacts(void);

    /**
     * Allocate DMA-coherent pools:
     *   - Request frames  (kNumRequestFrames × MPT3_REQUEST_FRAME_SIZE)
     *   - Reply free queue (kNumReplyFrames × MPT3_REPLY_FRAME_SIZE)
     *   - Reply post queue (kReplyQueueDepth × MPT3_REPLY_DESCRIPTOR_SIZE)
     *   - Sense buffers   (kNumRequestFrames × kSenseBufferSize)
     */
    kern_return_t   AllocateDMAPools(void);

    /** Free all DMA pools allocated by AllocateDMAPools */
    void            FreeDMAPools(void);

    /**
     * Populate the reply-free queue with the physical addresses of all
     * pre-allocated reply frames, so the IOC can write replies to them.
     */
    void            FillReplyFreeQueue(void);

    /**
     * Send IOCInit to configure the descriptor queue addresses and depths
     * in the controller.
     */
    kern_return_t   SendIOCInit(void);

    /**
     * Enable event notifications (SAS discovery, device add/remove, …).
     */
    kern_return_t   EnableEventNotification(void);

    /**
     * Send PortEnable to trigger SAS/SATA topology discovery.
     * This is asynchronous; device-added events arrive via the event
     * notification path.
     */
    kern_return_t   SendPortEnable(void);

    /* ------------------------------------------------------------------
     * Private helpers — descriptor ring management
     * ------------------------------------------------------------------ */

    /**
     * Allocate an unused SMID slot.
     * @return SMID (1-based) or 0 on exhaustion.
     */
    uint16_t        AllocateSMID(void);

    /** Return a SMID slot to the free pool */
    void            FreeSMID(uint16_t smid);

    /**
     * Post a 64-bit request descriptor to the controller via MMIO.
     * Both 32-bit halves are written with a barrier between them to
     * prevent reordering.
     */
    void            PostRequestDescriptor(uint32_t low, uint32_t high);

    /**
     * Advance the reply free host index register after consuming a reply.
     */
    void            AdvanceReplyFreeIndex(void);

    /* ------------------------------------------------------------------
     * Private helpers — I/O completion
     * ------------------------------------------------------------------ */

    /**
     * MSI-X interrupt handler.  Called by IOInterruptDispatchSource.
     * Drains the reply post queue and dispatches completions.
     *
     * @param source   The interrupt source that fired
     * @param timestamp Time at which the interrupt arrived
     */
    void            HandleInterrupt(IOInterruptDispatchSource *source,
                                    uint64_t timestamp);

    /**
     * Process a single SCSI IO reply frame.
     */
    void            CompleteScsiIO(const MPT3SCSIIOReply *reply, uint16_t smid);

    /**
     * Process an asynchronous event notification reply.
     */
    void            HandleEventNotification(const MPT3EventNotificationReply *event);

    /**
     * Build the SGL chain for the given I/O in the request frame.
     * Uses only the inline SGL entries for small transfers; allocates
     * chain buffers for larger ones.
     */
    kern_return_t   BuildSGL(MPT3SCSIIORequest          *req,
                             SCSIParallelTaskIdentifier  task);

    /* ------------------------------------------------------------------
     * MMIO accessor helpers (barriers enforced by OSWriteLittleInt32 etc.)
     * ------------------------------------------------------------------ */

    inline uint32_t ReadReg32(uint32_t offset);
    inline void     WriteReg32(uint32_t offset, uint32_t value);

    /* ------------------------------------------------------------------
     * Member variables
     * ------------------------------------------------------------------ */

    /** PCI device provider */
    IOPCIDevice                    *fPCIDevice      = nullptr;

    /** MMIO map for BAR1 */
    IOMemoryMap                    *fBAR1Map        = nullptr;
    volatile uint8_t               *fBAR1Base       = nullptr;

    /** MSI-X interrupt dispatch source (one for queue 0) */
    IOInterruptDispatchSource      *fInterruptSource = nullptr;

    /* --- DMA pool descriptors --- */

    /** IOBufferMemoryDescriptor for the request frame pool */
    IOBufferMemoryDescriptor       *fRequestFramePool    = nullptr;
    uint64_t                        fRequestFramePhysBase = 0;
    MPT3SCSIIORequest              *fRequestFrameVirtBase = nullptr;

    /** IOBufferMemoryDescriptor for the reply frame (free) pool */
    IOBufferMemoryDescriptor       *fReplyFramePool      = nullptr;
    uint64_t                        fReplyFramePhysBase  = 0;
    uint8_t                        *fReplyFrameVirtBase  = nullptr;

    /** IOBufferMemoryDescriptor for the reply post queue (descriptor ring) */
    IOBufferMemoryDescriptor       *fReplyPostQueue      = nullptr;
    uint64_t                        fReplyPostPhysBase   = 0;
    MPT3ReplyDescriptor            *fReplyPostVirtBase   = nullptr;

    /** IOBufferMemoryDescriptor for SCSI sense buffers */
    IOBufferMemoryDescriptor       *fSenseBufferPool     = nullptr;
    uint64_t                        fSensePhysBase       = 0;
    uint8_t                        *fSenseVirtBase       = nullptr;

    /* --- IOCFacts result --- */
    MPT3IOCFactsReply               fIOCFacts            = {};

    /* --- SMID free-list --- */
    uint16_t                        fSMIDFreeList[kNumRequestFrames] = {};
    uint32_t                        fSMIDFreeHead    = 0;
    uint32_t                        fSMIDFreeTail    = 0;

    /* --- Per-command context array (index = SMID - 1) --- */
    MPT3CommandContext              fCmdCtx[kNumRequestFrames] = {};

    /* --- Reply post queue consumer index --- */
    uint32_t                        fReplyPostIndex  = 0;

    /* --- Reply free queue producer index --- */
    uint32_t                        fReplyFreeIndex  = 0;

    /* --- Driver state --- */
    bool                            fControllerOnline = false;
};
