/**
 * MPT3IOC.h — IOC firmware initialization state machine
 *
 * Implements the multi-step sequence required to bring a SAS3008 controller
 * from hard-reset through to OPERATIONAL state, ready to accept SCSI I/O:
 *
 *   RESET → (diagnostic reset) → READY → (IOCFacts) → (IOCInit) → OPERATIONAL
 *              → (PortEnable) → (EventNotification) → online
 *
 * This module is deliberately separate from the main driver class so the
 * initialization logic can be unit-tested without a live PCI device.
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <DriverKit/DriverKit.h>
#include "MPT3Types.h"
#include "MPT3Registers.h"

/* =========================================================================
 * Forward declarations
 * ========================================================================= */
class IOPCIDevice;
class IOMemoryMap;
class IOBufferMemoryDescriptor;

/* =========================================================================
 * IOC initialization result
 * ========================================================================= */

struct MPT3IOCInitResult {
    /** Capabilities reported by the IOC */
    MPT3IOCFactsReply   facts;

    /** True if the controller came up cleanly */
    bool                success;

    /** Number of retry resets performed */
    uint32_t            resetCount;
};

/* =========================================================================
 * MPT3IOCManager — Owns the full init / reset / recovery lifecycle
 * ========================================================================= */

class MPT3IOCManager {

public:

    /**
     * Attach to the controller's MMIO region.
     *
     * @param bar1Base  Virtual base address of BAR1 (system interface regs)
     *                  already mapped by the caller via IOPCIDevice::MapMemory.
     */
    void Attach(volatile uint8_t *bar1Base);

    /* ------------------------------------------------------------------
     * Initialisation sequence
     * ------------------------------------------------------------------ */

    /**
     * Run the full IOC initialisation sequence.
     *
     * Sequence:
     *   1. Mask all host interrupts.
     *   2. Read current state; if not READY, perform a soft reset.
     *   3. Issue the IOCFacts doorbell handshake.
     *   4. Receive IOCFacts reply; fill *outFacts on success.
     *   5. Receive IOCInit command (caller must have pre-allocated DMA pools
     *      and populated *initReq before calling this step via SendIOCInit).
     *   6. Wait for the IOC to reach OPERATIONAL state.
     *
     * @param outFacts  Populated on success with the IOC capabilities.
     * @return kIOReturnSuccess on success.
     */
    kern_return_t RunInitSequence(MPT3IOCFactsReply *outFacts);

    /**
     * Send IOCInit to configure all four DMA ring addresses in the firmware.
     * Must be called after RunInitSequence and after DMA pools are allocated
     * and the reply free queue ring has been filled (FillReplyFreeQueue).
     *
     * @param requestFramePhys   PA of the request frame DMA pool.
     * @param requestFrameSize   Size of each request frame in bytes
     *                           (used to compute SystemRequestFrameSize in DWORDs).
     * @param replyPostPhys      PA of the reply post queue DMA ring.
     * @param replyPostDepth     Number of slots in the reply post ring.
     * @param replyFreeRingPhys  PA of the reply free queue DMA ring
     *                           (ring of uint32_t reply frame PAs — NOT the frame pool).
     * @param replyFreeDepth     Number of slots in the reply free ring.
     * @param sensePhysHigh      Upper 32 bits of the sense buffer pool PA.
     * @param msixVectors        Number of MSI-X vectors the host will use.
     */
    kern_return_t SendIOCInit(uint64_t requestFramePhys,
                              uint16_t requestFrameSize,
                              uint64_t replyPostPhys,
                              uint16_t replyPostDepth,
                              uint64_t replyFreeRingPhys,
                              uint16_t replyFreeDepth,
                              uint32_t sensePhysHigh,
                              uint16_t msixVectors);

    /**
     * Issue PortEnable (async — completion arrives as an event).
     * Call after MSI-X is active so the completion can be received.
     *
     * @param reqFrameBase  Virtual base of the management request frame slot.
     */
    kern_return_t SendPortEnable(MPT3SCSIIORequest *reqFrameBase);

    /**
     * Enable all relevant async event notifications.
     */
    kern_return_t SendEventNotificationEnable(MPT3SCSIIORequest *reqFrameBase);

    /* ------------------------------------------------------------------
     * Reset / recovery
     * ------------------------------------------------------------------ */

    /**
     * Perform a diagnostic-register soft reset.
     * Blocks until the IOC reaches READY state (up to 5 seconds).
     */
    kern_return_t SoftReset(void);

    /**
     * Read the current IOC state from the doorbell register.
     */
    MPT3IOCState  GetIOCState(void) const;

    /**
     * Wait up to @p timeoutMS milliseconds for the IOC to enter @p desired.
     * Returns kIOReturnTimeout on failure.
     */
    kern_return_t WaitForState(MPT3IOCState desired, uint32_t timeoutMS);

    /* ------------------------------------------------------------------
     * Interrupt mask helpers
     * ------------------------------------------------------------------ */

    /** Mask all host interrupts (call before touching hardware) */
    void MaskAllInterrupts(void);

    /** Unmask the reply-available interrupt (call after MSI-X is live) */
    void UnmaskReplyInterrupt(void);

    /* ------------------------------------------------------------------
     * Low-level MMIO helpers (exposed for testing)
     * ------------------------------------------------------------------ */

    uint32_t ReadReg32(uint32_t offset) const;
    void     WriteReg32(uint32_t offset, uint32_t value);

private:

    /**
     * Doorbell-based request/reply handshake.
     * Used for IOCFacts and IOCInit before the DMA queues are live.
     *
     * @param req       Request buffer (DWORD-aligned)
     * @param reqDWords Size of request in 32-bit words
     * @param reply     Output buffer for reply
     * @param replyDWords Maximum reply size in 32-bit words; updated on return
     * @return kIOReturnSuccess, kIOReturnTimeout, or kIOReturnDeviceError
     */
    kern_return_t DoorbellHandshake(const uint32_t *req,
                                    uint32_t        reqDWords,
                                    uint32_t       *reply,
                                    uint32_t       *replyDWords);

    /**
     * Post a 64-bit request descriptor to the hardware FIFO registers.
     * Writes low word first, then high word, with an MMIO barrier between.
     */
    void PostHighPriorityDescriptor(const MPT3HighPriorityRequestDescriptor &desc);

    volatile uint8_t *fBarBase = nullptr;
};
