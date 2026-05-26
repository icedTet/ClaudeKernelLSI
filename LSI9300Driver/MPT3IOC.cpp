/**
 * MPT3IOC.cpp — IOC firmware initialization state machine
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MPT3IOC.h"
#include <DriverKit/IOLib.h>
#include <os/log.h>

#define IOC_LOG(fmt, ...) \
    os_log(OS_LOG_DEFAULT, "[LSI9300][IOC] " fmt, ##__VA_ARGS__)
#define IOC_ERR(fmt, ...) \
    os_log_error(OS_LOG_DEFAULT, "[LSI9300][IOC][ERR] " fmt, ##__VA_ARGS__)

/* =========================================================================
 * Sizing constants local to this file
 * ========================================================================= */

/** Polling interval while waiting for IOC state transitions (microseconds) */
static constexpr uint32_t kPollIntervalUS   = 10000U;   // 10 ms

/** Total wait for READY after reset */
static constexpr uint32_t kResetTimeoutMS   = 5000U;

/** Total wait for OPERATIONAL after IOCInit */
static constexpr uint32_t kOperTimeoutMS    = 3000U;

/** Timeout for each doorbell word exchange */
static constexpr uint32_t kDoorbellWordWaitUS = 100U;
static constexpr uint32_t kDoorbellWordRetries = 5000U; // 5000 × 100 µs = 500 ms

// ===========================================================================
// Attach
// ===========================================================================

void MPT3IOCManager::Attach(volatile uint8_t *bar1Base)
{
    fBarBase = bar1Base;
}

// ===========================================================================
// MMIO helpers
// ===========================================================================

uint32_t MPT3IOCManager::ReadReg32(uint32_t offset) const
{
    return OSReadLittleInt32(fBarBase, offset);
}

void MPT3IOCManager::WriteReg32(uint32_t offset, uint32_t value)
{
    OSWriteLittleInt32(fBarBase, offset, value);
    OSSynchronizeIO();
}

// ===========================================================================
// Interrupt mask
// ===========================================================================

void MPT3IOCManager::MaskAllInterrupts(void)
{
    WriteReg32(MPI3_SYSIF_HOST_INT_MASK_REG, MPI3_SYSIF_HOST_INT_MASK_ALL);
}

void MPT3IOCManager::UnmaskReplyInterrupt(void)
{
    uint32_t mask = MPI3_SYSIF_HOST_INT_MASK_ALL & ~MPI3_SYSIF_HOST_INT_MASK_REPLY;
    WriteReg32(MPI3_SYSIF_HOST_INT_MASK_REG, mask);
}

// ===========================================================================
// IOC state
// ===========================================================================

MPT3IOCState MPT3IOCManager::GetIOCState(void) const
{
    return mpt3_ioc_state(ReadReg32(MPI3_SYSIF_IOC_STATE_REG));
}

kern_return_t MPT3IOCManager::WaitForState(MPT3IOCState desired, uint32_t timeoutMS)
{
    uint32_t elapsed = 0;
    uint32_t intervalMS = kPollIntervalUS / 1000;
    if (intervalMS == 0) intervalMS = 1;

    while (elapsed < timeoutMS) {
        IOSleep(intervalMS);
        elapsed += intervalMS;
        MPT3IOCState st = GetIOCState();
        if (st == desired) {
            IOC_LOG("WaitForState: reached state %u after %u ms",
                    (unsigned)desired, elapsed);
            return kIOReturnSuccess;
        }
        if (st == MPI3_IOC_STATE_FAULT) {
            IOC_ERR("WaitForState: IOC in FAULT state while waiting for %u",
                    (unsigned)desired);
            return kIOReturnDeviceError;
        }
    }

    IOC_ERR("WaitForState: timed out after %u ms (current=%u, desired=%u)",
            timeoutMS, (unsigned)GetIOCState(), (unsigned)desired);
    return kIOReturnTimeout;
}

// ===========================================================================
// SoftReset — diagnostic-register unlock + HOLD_IOC_RESET sequence
// ===========================================================================

kern_return_t MPT3IOCManager::SoftReset(void)
{
    IOC_LOG("SoftReset: beginning diagnostic reset sequence");

    // Step 1: Write the unlock sequence to the write-sequence register
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_KEY_VALUE_FLUSH);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_1);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_2);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_3);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_4);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_5);
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_6);

    // Brief delay so the firmware processes the unlock
    IODelay(50);

    // Step 2: Verify the diagnostic write-enable bit is now set
    uint32_t diag = ReadReg32(MPI3_SYSIF_HOST_DIAG_REG);
    if (!(diag & MPI3_SYSIF_HOST_DIAG_DIAG_WRITE_ENABLE)) {
        IOC_ERR("SoftReset: diagnostic write-enable not set (diag=0x%08x)", diag);
        // Re-lock and return error
        WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_KEY_VALUE_FLUSH);
        return kIOReturnNotPermitted;
    }

    // Step 3: Assert HOLD_IOC_RESET (keeps firmware paused during reset)
    WriteReg32(MPI3_SYSIF_HOST_DIAG_REG, diag | MPI3_SYSIF_HOST_DIAG_HOLD_IOC_RESET);
    IODelay(200);   // 200 µs: give firmware time to pause

    // Step 4: Deassert HOLD_IOC_RESET to let the IOC begin its reset sequence
    diag = ReadReg32(MPI3_SYSIF_HOST_DIAG_REG);
    WriteReg32(MPI3_SYSIF_HOST_DIAG_REG, diag & ~MPI3_SYSIF_HOST_DIAG_HOLD_IOC_RESET);

    // Step 5: Re-lock the diagnostic register
    WriteReg32(MPI3_SYSIF_WRITE_SEQ_REG, MPI3_SYSIF_WRITE_SEQ_KEY_VALUE_FLUSH);

    // Step 6: Wait for the IOC to reach READY state
    kern_return_t ret = WaitForState(MPI3_IOC_STATE_READY, kResetTimeoutMS);
    if (ret != kIOReturnSuccess) {
        IOC_ERR("SoftReset: failed to reach READY state");
        return ret;
    }

    IOC_LOG("SoftReset: controller is READY");
    return kIOReturnSuccess;
}

// ===========================================================================
// DoorbellHandshake
// ===========================================================================

kern_return_t MPT3IOCManager::DoorbellHandshake(const uint32_t *req,
                                                 uint32_t        reqDWords,
                                                 uint32_t       *reply,
                                                 uint32_t       *replyDWords)
{
    auto waitForBit = [this](uint32_t bit, bool set, uint32_t retries) -> bool {
        for (uint32_t i = 0; i < retries; i++) {
            IODelay(kDoorbellWordWaitUS);
            uint32_t status = ReadReg32(MPI3_SYSIF_HOST_INT_STATUS_REG);
            bool hasBit = (status & bit) != 0;
            if (hasBit == set) return true;
        }
        return false;
    };

    // 1. Write the first DWORD (function + size) to the doorbell register.
    //    This initiates the handshake.
    WriteReg32(MPI3_SYSIF_DOORBELL_REG, req[0]);

    // 2. Wait for the IOC to acknowledge receipt of the function code
    //    (SYSTEM_TO_IOC_DB_STATUS bit clears when the IOC is ready for data).
    if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_SYSTEM_TO_IOC_DB_STATUS,
                    false, kDoorbellWordRetries)) {
        IOC_ERR("DoorbellHandshake: timeout waiting for initial DB ACK");
        return kIOReturnTimeout;
    }

    // 3. Send remaining DWORDs of the request (split into two 16-bit halfwords
    //    per the MPT3 spec, because the doorbell data path is 16 bits wide).
    for (uint32_t i = 1; i < reqDWords; i++) {
        // Low halfword
        WriteReg32(MPI3_SYSIF_DOORBELL_REG, req[i] & 0xFFFFU);

        if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_SYSTEM_TO_IOC_DB_STATUS,
                        false, kDoorbellWordRetries)) {
            IOC_ERR("DoorbellHandshake: timeout sending req word %u (low)", i);
            return kIOReturnTimeout;
        }

        // High halfword
        WriteReg32(MPI3_SYSIF_DOORBELL_REG, (req[i] >> 16) & 0xFFFFU);

        if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_SYSTEM_TO_IOC_DB_STATUS,
                        false, kDoorbellWordRetries)) {
            IOC_ERR("DoorbellHandshake: timeout sending req word %u (high)", i);
            return kIOReturnTimeout;
        }
    }

    // 4. Wait for the IOC to assert the doorbell status interrupt (reply ready)
    if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS,
                    true, kDoorbellWordRetries)) {
        IOC_ERR("DoorbellHandshake: timeout waiting for reply-ready interrupt");
        return kIOReturnTimeout;
    }

    // 5. Read the reply length (in DWORDs) from the doorbell register
    uint32_t actualReplyDW = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
    if (actualReplyDW == 0) {
        IOC_ERR("DoorbellHandshake: IOC reported zero-length reply");
        return kIOReturnBadArgument;
    }
    if (actualReplyDW > *replyDWords) {
        IOC_ERR("DoorbellHandshake: reply too large: %u DW (max %u)",
                actualReplyDW, *replyDWords);
        return kIOReturnNoSpace;
    }
    *replyDWords = actualReplyDW;

    // Acknowledge the "reply ready" doorbell interrupt
    WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
               MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

    // 6. Read reply data.
    //
    // The doorbell is 16 bits wide.  The IOC delivers ONE 16-bit half-word
    // per DOORBELL_STATUS interrupt, so reconstructing each 32-bit DWORD
    // requires TWO reads (low half-word first, then high half-word).
    //
    // Reference: Linux mpt3sas _base_handshake_req_reply_wait()
    for (uint32_t i = 0; i < actualReplyDW; i++) {
        // --- low half-word ---
        if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS,
                        true, kDoorbellWordRetries)) {
            IOC_ERR("DoorbellHandshake: timeout reading reply word %u (low)", i);
            return kIOReturnTimeout;
        }
        uint32_t lo = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
        WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
                   MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

        // --- high half-word ---
        if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS,
                        true, kDoorbellWordRetries)) {
            IOC_ERR("DoorbellHandshake: timeout reading reply word %u (high)", i);
            return kIOReturnTimeout;
        }
        uint32_t hi = ReadReg32(MPI3_SYSIF_DOORBELL_REG) & 0xFFFFU;
        WriteReg32(MPI3_SYSIF_HOST_INT_STATUS_REG,
                   MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS);

        // Reassemble full DWORD (little-endian: low half first)
        reply[i] = lo | (hi << 16);
    }

    // 7. Wait for the IOC to deassert the doorbell interrupt (handshake done)
    if (!waitForBit(MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS,
                    false, kDoorbellWordRetries)) {
        IOC_ERR("DoorbellHandshake: timeout waiting for handshake completion");
        return kIOReturnTimeout;
    }

    return kIOReturnSuccess;
}

// ===========================================================================
// RunInitSequence
// ===========================================================================

kern_return_t MPT3IOCManager::RunInitSequence(MPT3IOCFactsReply *outFacts)
{
    kern_return_t ret;

    IOC_LOG("RunInitSequence: starting");

    // Mask interrupts during init
    MaskAllInterrupts();

    // Ensure the IOC is in a known state
    MPT3IOCState state = GetIOCState();
    IOC_LOG("RunInitSequence: initial IOC state = %u", (unsigned)state);

    if (state == MPI3_IOC_STATE_FAULT || state == MPI3_IOC_STATE_COREDUMP) {
        IOC_LOG("RunInitSequence: IOC in fault/coredump, attempting reset");
        ret = SoftReset();
        if (ret != kIOReturnSuccess) {
            IOC_ERR("RunInitSequence: reset failed (0x%x)", ret);
            return ret;
        }
        state = GetIOCState();
    }

    if (state != MPI3_IOC_STATE_READY) {
        IOC_LOG("RunInitSequence: IOC not READY (state=%u), performing reset",
                (unsigned)state);
        ret = SoftReset();
        if (ret != kIOReturnSuccess) {
            return ret;
        }
    }

    // --- IOCFacts ---
    MPT3IOCFactsRequest req = {};
    req.Header.Function = MPI3_FUNCTION_IOC_FACTS;

    constexpr uint32_t kMaxReplyDW = sizeof(MPT3IOCFactsReply) / sizeof(uint32_t);
    uint32_t replyBuf[kMaxReplyDW] = {};
    uint32_t replyDW = kMaxReplyDW;

    ret = DoorbellHandshake(
              reinterpret_cast<const uint32_t *>(&req),
              sizeof(req) / sizeof(uint32_t),
              replyBuf,
              &replyDW);

    if (ret != kIOReturnSuccess) {
        IOC_ERR("RunInitSequence: IOCFacts handshake failed (0x%x)", ret);
        return ret;
    }

    __builtin_memcpy(outFacts, replyBuf, sizeof(*outFacts));

    if (outFacts->Header.IOCStatus != MPI3_IOCSTATUS_SUCCESS) {
        IOC_ERR("RunInitSequence: IOCFacts returned status 0x%04x",
                outFacts->Header.IOCStatus);
        return kIOReturnDeviceError;
    }

    IOC_LOG("RunInitSequence: IOCFacts OK — MaxDevices=%u, Credit=%u, FW=0x%08x",
            outFacts->IOCMaxDevices,
            outFacts->RequestCredit,
            outFacts->FWVersion);

    return kIOReturnSuccess;
}

// ===========================================================================
// SendIOCInit
// ===========================================================================

kern_return_t MPT3IOCManager::SendIOCInit(uint64_t requestFramePhys,
                                           uint16_t requestFrameSize,
                                           uint64_t replyPostPhys,
                                           uint16_t replyPostDepth,
                                           uint64_t replyFreeRingPhys,
                                           uint16_t replyFreeDepth,
                                           uint32_t sensePhysHigh,
                                           uint16_t msixVectors)
{
    MPT3IOCInitRequest req = {};

    // --- Function header ---
    req.Function    = MPI3_FUNCTION_IOC_INIT;
    req.WhoInit     = 0x04U;    // MPI2_WHOINIT_HOST_DRIVER (not 0x03)
    req.MsgVersion  = 0x0200U;  // MPI 2.0 base protocol
    req.HostMSIxVectors = msixVectors;

    // --- Request frame pool ---
    // IOC validates SMID-indexed request frames against this base address.
    req.SystemRequestFrameBaseAddress = requestFramePhys;
    req.SystemRequestFrameSize =
        static_cast<uint16_t>(requestFrameSize / sizeof(uint32_t));

    // --- Sense buffer pool ---
    req.SenseBufferAddressHigh = sensePhysHigh;

    // --- Reply post queue ring (IOC → host, 8-byte reply descriptors) ---
    req.ReplyDescriptorPostQueueAddress = replyPostPhys;
    req.ReplyDescriptorPostQueueDepth   = replyPostDepth;

    // --- Reply free queue ring (host → IOC, 4-byte reply frame PAs) ---
    // NOTE: replyFreeRingPhys is the PA of the uint32_t[] DMA ring, NOT
    //       the PA of the reply frame pool itself.
    req.ReplyFreeQueueAddress = replyFreeRingPhys;
    req.ReplyFreeQueueDepth   = replyFreeDepth;

    constexpr uint32_t kMaxReplyDW = sizeof(MPT3IOCInitReply) / sizeof(uint32_t);
    uint32_t replyBuf[kMaxReplyDW] = {};
    uint32_t replyDW = kMaxReplyDW;

    kern_return_t ret = DoorbellHandshake(
                            reinterpret_cast<const uint32_t *>(&req),
                            sizeof(req) / sizeof(uint32_t),
                            replyBuf,
                            &replyDW);
    if (ret != kIOReturnSuccess) {
        IOC_ERR("SendIOCInit: handshake failed (0x%x)", ret);
        return ret;
    }

    const MPT3IOCInitReply *reply =
        reinterpret_cast<const MPT3IOCInitReply *>(replyBuf);

    if (reply->IOCStatus != MPI3_IOCSTATUS_SUCCESS) {
        IOC_ERR("SendIOCInit: IOCStatus=0x%04x", reply->IOCStatus);
        return kIOReturnDeviceError;
    }

    // After IOCInit, the IOC transitions to OPERATIONAL
    ret = WaitForState(MPI3_IOC_STATE_OPERATIONAL, kOperTimeoutMS);
    if (ret != kIOReturnSuccess) {
        IOC_ERR("SendIOCInit: IOC did not reach OPERATIONAL state");
        return ret;
    }

    IOC_LOG("SendIOCInit: OPERATIONAL — reqFrames@0x%llx replyPost@0x%llx "
            "replyFreeRing@0x%llx",
            requestFramePhys, replyPostPhys, replyFreeRingPhys);
    return kIOReturnSuccess;
}

// ===========================================================================
// PostHighPriorityDescriptor
// ===========================================================================

void MPT3IOCManager::PostHighPriorityDescriptor(
    const MPT3HighPriorityRequestDescriptor &desc)
{
    const uint32_t *words = reinterpret_cast<const uint32_t *>(&desc);
    WriteReg32(MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_LOW_REG,  words[0]);
    WriteReg32(MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_HIGH_REG, words[1]);
}

// ===========================================================================
// SendPortEnable
// ===========================================================================

kern_return_t MPT3IOCManager::SendPortEnable(MPT3SCSIIORequest *reqFrameBase)
{
    // PortEnable uses a simple 8-byte message; re-use the management slot
    struct PortEnableRequest {
        MPT3RequestHeader   Header;
        uint8_t             PhysicalPort;   // 0xFF = all ports
        uint8_t             Reserved[3];
    };

    PortEnableRequest msg = {};
    msg.Header.Function = MPI3_FUNCTION_PORT_ENABLE;
    msg.PhysicalPort    = 0xFF;  // all ports

    // Copy into the management request frame (SMID 0 slot)
    __builtin_memcpy(reqFrameBase, &msg, sizeof(msg));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.MSIxIndex      = 0;
    desc.SMID           = 0;

    PostHighPriorityDescriptor(desc);

    IOC_LOG("SendPortEnable: port enable posted (async, awaiting event reply)");
    return kIOReturnSuccess;
}

// ===========================================================================
// SendEventNotificationEnable
// ===========================================================================

kern_return_t MPT3IOCManager::SendEventNotificationEnable(
    MPT3SCSIIORequest *reqFrameBase)
{
    MPT3EventNotificationRequest msg = {};
    msg.Header.Function = MPI3_FUNCTION_EVENT_NOTIFICATION;

    // Subscribe to the events we care about for IT-mode operation
    msg.EventSwitches[0] =
          (1U << (MPI3_EVENT_SAS_DISCOVERY          & 0x1F))
        | (1U << (MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST & 0x1F))
        | (1U << (MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE & 0x1F))
        | (1U << (MPI3_EVENT_DEVICE_ADDED            & 0x1F));

    __builtin_memcpy(reqFrameBase, &msg, sizeof(msg));

    MPT3HighPriorityRequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY;
    desc.MSIxIndex      = 0;
    desc.SMID           = 0;

    PostHighPriorityDescriptor(desc);

    IOC_LOG("SendEventNotificationEnable: event notification enabled");
    return kIOReturnSuccess;
}
