/**
 * MPT3ProtocolTests.cpp — Unit tests for MPT3 wire-format structures
 *
 * Tests run on the host (Linux CI or macOS userspace) without hardware.
 * They verify struct sizes, alignment, descriptor encoding, and ring
 * pointer arithmetic.
 *
 * Build (no DriverKit SDK needed):
 *   clang++ -std=c++17 -DUNIT_TEST -I ../LSI9300Driver \
 *           MPT3ProtocolTests.cpp -o mpt3_tests && ./mpt3_tests
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Stub out DriverKit / PCIDriverKit / SCSIControllerDriverKit so the
// header files compile in a plain C++ toolchain (Linux CI).
#define DRIVERKIT_STUB
#include "TestStubs.h"

#include "MPT3Types.h"
#include "MPT3Registers.h"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int  gTestsPassed = 0;
static int  gTestsFailed = 0;

#define EXPECT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAIL  %s:%d  expected %lld got %lld\n", \
               __FILE__, __LINE__, (long long)(b), (long long)(a)); \
        gTestsFailed++; \
    } else { \
        gTestsPassed++; \
    } \
} while (0)

#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("FAIL  %s:%d  condition false: %s\n", \
               __FILE__, __LINE__, #cond); \
        gTestsFailed++; \
    } else { \
        gTestsPassed++; \
    } \
} while (0)

// ---------------------------------------------------------------------------
// Test: on-wire struct sizes (verified against Broadcom MPI 2.5 spec)
// ---------------------------------------------------------------------------

static void TestStructSizes(void)
{
    printf("TestStructSizes...\n");

    // Reply descriptor types are 8 bytes (one cache line per two entries)
    EXPECT_EQ(sizeof(MPT3SCSIIORequestDescriptor), 8UL);
    EXPECT_EQ(sizeof(MPT3HighPriorityRequestDescriptor), 8UL);
    EXPECT_EQ(sizeof(MPT3AddressReplyDescriptor), 8UL);
    EXPECT_EQ(sizeof(MPT3ReplyDescriptor), 8UL);

    // Request frame headers
    EXPECT_EQ(sizeof(MPT3RequestHeader), 12UL);
    EXPECT_EQ(sizeof(MPT3ReplyHeader), 18UL);  // 2+1+1+2+1+1+1+1+2+2+4

    // IOC handshake frames
    EXPECT_EQ(sizeof(MPT3IOCFactsRequest), 32UL);
    EXPECT_EQ(sizeof(MPT3IOCFactsReply), 68UL);
    EXPECT_EQ(sizeof(MPT3IOCInitRequest), 68UL);  // MPI 2.5 spec: Mpi2IOCInitRequest_t = 0x44

    // SCSI IO frame — must be exactly MPT3_REQUEST_FRAME_SIZE
    EXPECT_EQ(sizeof(MPT3SCSIIORequest), (size_t)MPT3_REQUEST_FRAME_SIZE);

    // SCSI IO reply (header 18 + 28 payload)
    EXPECT_EQ(sizeof(MPT3SCSIIOReply), 46UL);

    // Task management (header 12 + 28 fields)
    EXPECT_EQ(sizeof(MPT3SCTMRequest), 40UL);
    EXPECT_EQ(sizeof(MPT3SCTMReply), 26UL);

    // SGL elements
    EXPECT_EQ(sizeof(MPT3SGESimple64), 12UL);
    EXPECT_EQ(sizeof(MPT3SGEChain64), 12UL);
}

// ---------------------------------------------------------------------------
// Test: SGL flags encode correctly
// ---------------------------------------------------------------------------

static void TestSGLFlags(void)
{
    printf("TestSGLFlags...\n");

    MPT3SGESimple64 sge = {};
    uint32_t len = 4096;

    // Last element of a simple host-to-device transfer
    sge.FlagsLength = MPI3_SGE_FLAGS_SIMPLE_ELEMENT
                    | MPI3_SGE_FLAGS_64_BIT_ADDRESSING
                    | MPI3_SGE_FLAGS_HOST_TO_IOC
                    | MPI3_SGE_FLAGS_LAST_ELEMENT
                    | MPI3_SGE_FLAGS_END_OF_BUFFER
                    | MPI3_SGE_FLAGS_END_OF_LIST
                    | len;

    // Length field should survive the OR
    EXPECT_EQ(sge.FlagsLength & 0x00FFFFFFU, len);

    // END_OF_LIST flag should be set
    EXPECT_TRUE(sge.FlagsLength & MPI3_SGE_FLAGS_END_OF_LIST);
}

// ---------------------------------------------------------------------------
// Test: SCSI IO request descriptor fields
// ---------------------------------------------------------------------------

static void TestRequestDescriptorEncoding(void)
{
    printf("TestRequestDescriptorEncoding...\n");

    MPT3SCSIIORequestDescriptor desc = {};
    desc.DescriptorType = MPI3_REQUEST_DESCRTYPE_SCSI_IO;
    desc.MSIxIndex      = 0;
    desc.SMID           = 42;
    desc.DevHandle      = 0x0010;

    EXPECT_EQ(desc.DescriptorType, (uint8_t)MPI3_REQUEST_DESCRTYPE_SCSI_IO);
    EXPECT_EQ(desc.SMID, 42);
    EXPECT_EQ(desc.DevHandle, 0x0010);

    // Verify the all-ones "unused" sentinel for reply descriptors
    MPT3ReplyDescriptor reply = {};
    reply.Words = 0xFFFFFFFFFFFFFFFFULL;
    EXPECT_EQ(reply.Words, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(reply.AddressReply.DescriptorType, 0xFFU);
}

// ---------------------------------------------------------------------------
// Test: Reply post queue ring-pointer arithmetic
// ---------------------------------------------------------------------------

static void TestReplyRingArithmetic(void)
{
    printf("TestReplyRingArithmetic...\n");

    constexpr uint32_t kDepth = 512U;
    uint32_t consumerIdx = 0;

    // Simulate draining kDepth - 1 entries without wrapping
    for (uint32_t i = 0; i < kDepth - 1; i++) {
        consumerIdx = (consumerIdx + 1) % kDepth;
    }
    EXPECT_EQ(consumerIdx, kDepth - 1);

    // One more should wrap to 0
    consumerIdx = (consumerIdx + 1) % kDepth;
    EXPECT_EQ(consumerIdx, 0U);

    // Verify that the "unused" sentinel is detectable before any wrapping
    MPT3ReplyDescriptor sentinels[kDepth];
    for (uint32_t i = 0; i < kDepth; i++) {
        sentinels[i].Words = 0xFFFFFFFFFFFFFFFFULL;
    }

    // Simulate firmware writing one real descriptor at index 0
    sentinels[0].AddressReply.DescriptorType = MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY;
    sentinels[0].AddressReply.SMID           = 1;

    // Driver should detect it as valid (not the all-ones sentinel)
    EXPECT_TRUE(sentinels[0].Words != 0xFFFFFFFFFFFFFFFFULL);
    // Index 1 is still a sentinel
    EXPECT_EQ(sentinels[1].Words, 0xFFFFFFFFFFFFFFFFULL);
}

// ---------------------------------------------------------------------------
// Test: IOC state extraction from doorbell register
// ---------------------------------------------------------------------------

static void TestIOCStateExtraction(void)
{
    printf("TestIOCStateExtraction...\n");

    // Construct a doorbell register value with READY state
    uint32_t reg = (static_cast<uint32_t>(MPI3_IOC_STATE_READY) << MPI3_IOC_STATE_SHIFT);
    EXPECT_EQ(mpt3_ioc_state(reg), MPI3_IOC_STATE_READY);

    reg = (static_cast<uint32_t>(MPI3_IOC_STATE_OPERATIONAL) << MPI3_IOC_STATE_SHIFT);
    EXPECT_EQ(mpt3_ioc_state(reg), MPI3_IOC_STATE_OPERATIONAL);

    reg = (static_cast<uint32_t>(MPI3_IOC_STATE_FAULT) << MPI3_IOC_STATE_SHIFT);
    EXPECT_EQ(mpt3_ioc_state(reg), MPI3_IOC_STATE_FAULT);

    // Lower bits should be ignored
    reg = (static_cast<uint32_t>(MPI3_IOC_STATE_RESET) << MPI3_IOC_STATE_SHIFT)
        | 0x0FFFFFFFU;
    EXPECT_EQ(mpt3_ioc_state(reg), MPI3_IOC_STATE_RESET);
}

// ---------------------------------------------------------------------------
// Test: SMID allocation / free cycle  (pure logic, no hardware)
// ---------------------------------------------------------------------------

static void TestSMIDAllocator(void)
{
    printf("TestSMIDAllocator...\n");

    // Replicate the driver's SMID free-list logic
    constexpr uint32_t kMaxSMIDs = 16U;

    uint16_t freeList[kMaxSMIDs];
    uint32_t freeHead = 0;
    uint32_t freeTail = kMaxSMIDs;

    for (uint32_t i = 0; i < kMaxSMIDs; i++) {
        freeList[i] = static_cast<uint16_t>(i + 1);
    }

    // Alloc all SMIDs
    for (uint16_t expected = 1; expected <= kMaxSMIDs; expected++) {
        EXPECT_TRUE(freeHead != freeTail);
        uint16_t smid = freeList[freeHead % kMaxSMIDs];
        freeHead++;
        EXPECT_EQ(smid, expected);
    }

    // Queue is now empty
    EXPECT_TRUE(freeHead == freeTail);

    // Free one SMID
    uint16_t smid = 7;
    freeList[freeTail % kMaxSMIDs] = smid;
    freeTail++;

    // Re-alloc returns the just-freed SMID
    EXPECT_TRUE(freeHead != freeTail);
    uint16_t got = freeList[freeHead % kMaxSMIDs];
    freeHead++;
    EXPECT_EQ(got, smid);
}

// ---------------------------------------------------------------------------
// Test: IOCFacts reply CDB-length field
// ---------------------------------------------------------------------------

static void TestCDBSizeLimit(void)
{
    printf("TestCDBSizeLimit...\n");

    // The SCSI IO request frame must accommodate a 32-byte CDB
    MPT3SCSIIORequest frame = {};
    EXPECT_EQ(sizeof(frame.CDB), 32UL);

    // Maximum CDB constant must not exceed the field size
    EXPECT_TRUE(MPT3_MAX_CDB_LENGTH <= sizeof(frame.CDB));
}

// ---------------------------------------------------------------------------
// Test: Task management types
// ---------------------------------------------------------------------------

static void TestTaskManagementTypes(void)
{
    printf("TestTaskManagementTypes...\n");

    MPT3SCTMRequest tmReq = {};
    tmReq.Header.Function = MPI3_FUNCTION_SCSI_TASK_MGMT;
    tmReq.TaskType        = MPI3_SCSITASKMGMT_TASKTYPE_ABORT_TASK;
    tmReq.TaskMID         = 0x0042;

    EXPECT_EQ(tmReq.Header.Function, (uint8_t)MPI3_FUNCTION_SCSI_TASK_MGMT);
    EXPECT_EQ(tmReq.TaskType, (uint8_t)MPI3_SCSITASKMGMT_TASKTYPE_ABORT_TASK);
    EXPECT_EQ(tmReq.TaskMID, 0x0042);
}

// ---------------------------------------------------------------------------
// Test: SAS address constants
// ---------------------------------------------------------------------------

static void TestSASAddress(void)
{
    printf("TestSASAddress...\n");

    MPT3SASAddress invalid = MPT3_SAS_ADDRESS_INVALID;
    EXPECT_EQ(invalid, 0xFFFFFFFFFFFFFFFFULL);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    printf("=== MPT3 Protocol Unit Tests ===\n\n");

    TestStructSizes();
    TestSGLFlags();
    TestRequestDescriptorEncoding();
    TestReplyRingArithmetic();
    TestIOCStateExtraction();
    TestSMIDAllocator();
    TestCDBSizeLimit();
    TestTaskManagementTypes();
    TestSASAddress();

    printf("\n=== Results: %d passed, %d failed ===\n",
           gTestsPassed, gTestsFailed);

    return (gTestsFailed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
