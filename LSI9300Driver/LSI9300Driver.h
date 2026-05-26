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
#include <DriverKit/OSDynamicCast.h>      // OSDeclareDefaultStructors, OSDynamicCast
#include <DriverKit/IOService.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSDictionary.h>
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
    /**
     * Completion OSAction retained from UserProcessParallelTask.
     * Released and NULLed after ParallelTaskCompletion() is called.
     */
    OSAction               *completion;

    /** The framework's unique identifier for this task (from SCSIUserParallelTask.fControllerTaskIdentifier) */
    uint64_t                controllerTaskID;

    /** Target device identifier (from SCSIUserParallelTask.fTargetID) */
    SCSITargetIdentifier    targetID;

    /** Physical address of the pre-allocated sense buffer */
    uint64_t                sensePhysAddr;

    /** Virtual pointer to the request frame in the pool */
    MPT3SCSIIORequest      *requestFrame;

    /** Whether this slot is in use */
    bool                    inUse;
    uint8_t                 _pad[7];
};

/* =========================================================================
 * Main driver class
 * ========================================================================= */

class LSI9300Driver final : public IOUserSCSIParallelInterfaceController {

    // DriverKit mandatory class-registration macro.
    // Generates the inner types (Start_Impl, etc.) required by the IMPL macro.
    OSDeclareDefaultStructors(LSI9300Driver);

    using super = IOUserSCSIParallelInterfaceController;

public:

    /* ------------------------------------------------------------------
     * IOService lifecycle  (DriverKit: defined with IMPL macro in .cpp)
     * ------------------------------------------------------------------ */

    /**
     * Called by the kernel when a matching PCI device is found.
     * Saves the PCI device reference, then calls super::Start which
     * causes the framework to invoke UserInitializeController and
     * UserStartController in sequence.
     */
    virtual kern_return_t Start(IOService *provider) override;

    /**
     * Called during device removal or system shutdown.
     * Issues an IOC reset, releases all DMA pools and interrupt sources,
     * then calls super::Stop.
     */
    virtual kern_return_t Stop(IOService *provider) override;

    /* ------------------------------------------------------------------
     * IOUserSCSIParallelInterfaceController — pure-virtual overrides
     *
     * Called by the framework after Start() completes, in this order:
     *   1. UserInitializeController  — hardware init, DMA pools, IOCInit
     *   2. UserStartController       — enable interrupts, port enable
     * ------------------------------------------------------------------ */

    /**
     * Open the PCI device, map BAR1, soft-reset the IOC, issue IOCFacts,
     * allocate DMA descriptor pools, and send IOCInit.
     * Called by the framework immediately after Start() returns success.
     */
    virtual kern_return_t UserInitializeController() override;

    /**
     * Set up the MSI-X interrupt source, unmask reply interrupts,
     * enable async event notifications, and send PortEnable.
     * Called by the framework after UserInitializeController() succeeds.
     */
    virtual kern_return_t UserStartController() override;

    /**
     * Called for each new SCSI task submitted by the storage stack.
     * Builds an MPT3SCSIIORequest frame and posts it to hardware.
     *
     * @param parallelRequest  All I/O metadata (CDB, DMA address, target, …).
     * @param response         Output: synchronous service response code.
     * @param completion       OSAction to invoke when the I/O completes.
     */
    virtual kern_return_t UserProcessParallelTask(
                              SCSIUserParallelTask  parallelRequest,
                              uint32_t             *response,
                              OSAction             *completion) override;

    /**
     * Called by the framework to allocate per-task HBA data.
     * We use the SMID free-list for task tracking, so just return success.
     */
    virtual kern_return_t UserMapHBAData(uint32_t *uniqueTaskID) override;

    /**
     * Report whether the hardware performs auto-sense.
     * The SAS3008 always writes autosense data into the pre-allocated
     * sense buffer, so we return true.
     */
    virtual kern_return_t UserDoesHBAPerformAutoSense(bool *result) override;

    /* ------------------------------------------------------------------
     * IOUserSCSIParallelInterfaceController — optional virtual overrides
     * ------------------------------------------------------------------ */

    virtual kern_return_t UserDoesHBAPerformDeviceManagement(
                              bool *result) override;

    virtual kern_return_t UserReportMaximumTaskCount(
                              uint32_t *count) override;

    virtual kern_return_t UserReportHighestSupportedDeviceID(
                              uint64_t *id) override;

    virtual kern_return_t UserReportInitiatorIdentifier(
                              uint64_t *id) override;

    virtual kern_return_t UserReportHBAHighestLogicalUnitNumber(
                              uint64_t *value) override;

    /**
     * Report HBA DMA constraints via key–value pairs in constraints dict.
     */
    virtual kern_return_t UserReportHBAConstraints(
                              OSDictionary *constraints) override;

    /**
     * Report DMA segment parameters so the framework can prepare DMA properly.
     */
    virtual kern_return_t UserGetDMASpecification(
                              uint64_t             *maxTransferSize,
                              uint32_t             *alignment,
                              uint8_t              *numAddressBits,
                              DMAOutputSegmentType *segmentType) override;

    /**
     * Abort a specific task (TMF ABORT_TASK).
     */
    virtual kern_return_t UserAbortTaskRequest(
                              uint64_t  theT,
                              uint64_t  theL,
                              uint64_t  theQ,
                              uint32_t *response) override;

    /**
     * Abort all tasks for a target/LUN (TMF ABORT_TASK_SET).
     */
    virtual kern_return_t UserAbortTaskSetRequest(
                              uint64_t  theT,
                              uint64_t  theL,
                              uint32_t *response) override;

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
     */
    kern_return_t   DoorbellHandshake(const uint32_t *req,
                                      uint32_t        reqWords,
                                      uint32_t       *reply,
                                      uint32_t        replyWords);

    /** Read IOCFacts and store results in fIOCFacts */
    kern_return_t   ReadIOCFacts(void);

    /**
     * Allocate DMA-coherent pools:
     *   - Request frames       (kNumRequestFrames × MPT3_REQUEST_FRAME_SIZE)
     *   - Reply frame pool     (kNumReplyFrames   × MPT3_REPLY_FRAME_SIZE)
     *   - Reply free queue ring (kNumReplyFrames  × sizeof(uint32_t))
     *   - Reply post queue ring (kReplyQueueDepth × MPT3_REPLY_DESCRIPTOR_SIZE)
     *   - Sense buffers        (kNumRequestFrames × kSenseBufferSize)
     */
    kern_return_t   AllocateDMAPools(void);

    /** Free all DMA pools allocated by AllocateDMAPools */
    void            FreeDMAPools(void);

    /**
     * Write the physical addresses of all pre-allocated reply frames into
     * the reply free queue DMA ring and update the host index register.
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
     */
    void            PostRequestDescriptor(uint32_t low, uint32_t high);

    /**
     * Return a consumed reply frame to the IOC's free pool.
     */
    void            ReturnReplyFrameToFreeQueue(uint64_t replyFramePhys);

    /* ------------------------------------------------------------------
     * Private helpers — I/O completion
     * ------------------------------------------------------------------ */

    /**
     * MSI-X interrupt handler.
     */
    void            HandleInterrupt(IOInterruptDispatchSource *source,
                                    uint64_t timestamp);

    /**
     * Process a single SCSI IO reply frame and invoke ParallelTaskCompletion.
     */
    void            CompleteScsiIO(const MPT3SCSIIOReply *reply, uint16_t smid);

    /**
     * Process an asynchronous event notification reply.
     */
    void            HandleEventNotification(const MPT3EventNotificationReply *event);

    /**
     * Build the single-segment SGL for the given I/O in the request frame.
     * The framework guarantees one contiguous DMA segment via fBufferIOVMAddr.
     */
    kern_return_t   BuildSGL(MPT3SCSIIORequest          *req,
                             const SCSIUserParallelTask &task);

    /* ------------------------------------------------------------------
     * MMIO accessor helpers
     * ------------------------------------------------------------------ */

    inline uint32_t ReadReg32(uint32_t offset);
    inline void     WriteReg32(uint32_t offset, uint32_t value);

    /* ------------------------------------------------------------------
     * Member variables
     * ------------------------------------------------------------------ */

    /** PCI device provider (retained in Start, released in Stop) */
    IOPCIDevice                    *fPCIDevice      = nullptr;

    /** MMIO map for BAR1 */
    IOMemoryMap                    *fBAR1Map        = nullptr;
    volatile uint8_t               *fBAR1Base       = nullptr;

    /** MSI-X interrupt dispatch source (vector 0) */
    IOInterruptDispatchSource      *fInterruptSource = nullptr;

    /* --- DMA pool descriptors --- */

    /** IOBufferMemoryDescriptor for the request frame pool */
    IOBufferMemoryDescriptor       *fRequestFramePool    = nullptr;
    uint64_t                        fRequestFramePhysBase = 0;
    MPT3SCSIIORequest              *fRequestFrameVirtBase = nullptr;

    /**
     * Reply frame pool — 128-byte frames the IOC writes replies into.
     * Physical addresses of these frames go into the reply free queue ring.
     */
    IOBufferMemoryDescriptor       *fReplyFramePool      = nullptr;
    uint64_t                        fReplyFramePhysBase  = 0;
    uint8_t                        *fReplyFrameVirtBase  = nullptr;

    /**
     * Reply free queue ring — DMA ring of uint32_t physical addresses.
     * The host fills this with reply frame PAs; the IOC reads from it.
     * ReplyFreeQueueAddress in IOCInit points here.
     */
    IOBufferMemoryDescriptor       *fReplyFreeQueueRing  = nullptr;
    uint64_t                        fReplyFreeRingPhys   = 0;
    uint32_t                       *fReplyFreeRingVirt   = nullptr;

    /** Reply post queue — IOC writes 8-byte reply descriptors here on IRQ */
    IOBufferMemoryDescriptor       *fReplyPostQueue      = nullptr;
    uint64_t                        fReplyPostPhysBase   = 0;
    MPT3ReplyDescriptor            *fReplyPostVirtBase   = nullptr;

    /** Sense buffer pool */
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
