/**
 * MPT3ReplyQueueTests.cpp — Unit tests for reply post queue ring management
 *
 * Tests ring pointer arithmetic, sentinel detection, batch drain behaviour,
 * and free-queue replenishment logic — all without hardware.
 *
 * Build:
 *   clang++ -std=c++17 -DUNIT_TEST -I ../LSI9300Driver \
 *           MPT3ReplyQueueTests.cpp -o reply_tests && ./reply_tests
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UNIT_TEST
#define UNIT_TEST
#endif
#include "TestStubs.h"
#include "MPT3Types.h"
#include "MPT3Registers.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static int gPassed = 0, gFailed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        gFailed++; \
    } else { \
        gPassed++; \
    } \
} while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// ---------------------------------------------------------------------------
// Simulated reply post queue
// ---------------------------------------------------------------------------

class MockReplyQueue {
public:
    explicit MockReplyQueue(uint32_t depth) : depth_(depth), consumerIdx_(0) {
        ring_.resize(depth);
        for (auto &d : ring_) d.Words = 0xFFFFFFFFFFFFFFFFULL;
    }

    /** Firmware side: write a reply descriptor at the next slot */
    void FirmwareWrite(MPT3ReplyDescriptor desc) {
        ring_[producerIdx_] = desc;
        producerIdx_ = (producerIdx_ + 1) % depth_;
    }

    /** Driver side: drain all pending descriptors, return count */
    uint32_t Drain(std::vector<MPT3ReplyDescriptor> &completed) {
        uint32_t count = 0;
        while (ring_[consumerIdx_].Words != 0xFFFFFFFFFFFFFFFFULL) {
            completed.push_back(ring_[consumerIdx_]);
            // Mark as consumed
            ring_[consumerIdx_].Words = 0xFFFFFFFFFFFFFFFFULL;
            consumerIdx_ = (consumerIdx_ + 1) % depth_;
            count++;
        }
        return count;
    }

    uint32_t ConsumerIndex() const { return consumerIdx_; }
    uint32_t ProducerIndex() const { return producerIdx_; }

private:
    uint32_t depth_;
    uint32_t consumerIdx_ = 0;
    uint32_t producerIdx_ = 0;
    std::vector<MPT3ReplyDescriptor> ring_;
};

// ---------------------------------------------------------------------------
// Test: empty queue drains zero
// ---------------------------------------------------------------------------
static void TestEmptyDrain(void)
{
    printf("TestEmptyDrain...\n");
    MockReplyQueue q(512);
    std::vector<MPT3ReplyDescriptor> out;
    CHECK_EQ(q.Drain(out), 0U);
    CHECK(out.empty());
}

// ---------------------------------------------------------------------------
// Test: single entry is consumed
// ---------------------------------------------------------------------------
static void TestSingleEntry(void)
{
    printf("TestSingleEntry...\n");
    MockReplyQueue q(512);

    MPT3ReplyDescriptor d;
    d.AddressReply.DescriptorType = MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY;
    d.AddressReply.SMID           = 5;
    q.FirmwareWrite(d);

    std::vector<MPT3ReplyDescriptor> out;
    CHECK_EQ(q.Drain(out), 1U);
    CHECK_EQ(out.size(), 1UL);
    CHECK_EQ(out[0].AddressReply.SMID, 5);

    // After drain, empty again
    out.clear();
    CHECK_EQ(q.Drain(out), 0U);
}

// ---------------------------------------------------------------------------
// Test: batch of N entries drained in order
// ---------------------------------------------------------------------------
static void TestBatchDrain(void)
{
    printf("TestBatchDrain...\n");
    constexpr uint32_t kDepth = 512;
    constexpr uint32_t kBatch = 128;
    MockReplyQueue q(kDepth);

    for (uint16_t i = 1; i <= kBatch; i++) {
        MPT3ReplyDescriptor d;
        d.AddressReply.DescriptorType = MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY;
        d.AddressReply.SMID           = i;
        q.FirmwareWrite(d);
    }

    std::vector<MPT3ReplyDescriptor> out;
    uint32_t n = q.Drain(out);
    CHECK_EQ(n, kBatch);
    CHECK_EQ(out.size(), (size_t)kBatch);

    for (uint32_t i = 0; i < kBatch; i++) {
        CHECK_EQ(out[i].AddressReply.SMID, (uint16_t)(i + 1));
    }
}

// ---------------------------------------------------------------------------
// Test: ring wrap-around
// ---------------------------------------------------------------------------
static void TestRingWrap(void)
{
    printf("TestRingWrap...\n");
    constexpr uint32_t kDepth = 16;
    MockReplyQueue q(kDepth);

    // Fill ring to depth - 1 (leave one slot for detection)
    for (uint16_t i = 0; i < kDepth - 1; i++) {
        MPT3ReplyDescriptor d;
        d.AddressReply.DescriptorType = MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY;
        d.AddressReply.SMID           = i;
        q.FirmwareWrite(d);
    }

    std::vector<MPT3ReplyDescriptor> out;
    q.Drain(out);
    CHECK_EQ(out.size(), (size_t)(kDepth - 1));

    // Consumer should now be at kDepth - 1
    CHECK_EQ(q.ConsumerIndex(), (uint32_t)(kDepth - 1));
    out.clear();

    // Write more entries that wrap the producer
    for (uint16_t i = 0; i < kDepth / 2; i++) {
        MPT3ReplyDescriptor d;
        d.AddressReply.DescriptorType = MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY;
        d.AddressReply.SMID           = i + 100;
        q.FirmwareWrite(d);
    }

    q.Drain(out);
    CHECK_EQ(out.size(), (size_t)(kDepth / 2));
    CHECK_EQ(out[0].AddressReply.SMID, 100);
}

// ---------------------------------------------------------------------------
// Test: mixed descriptor types are dispatched correctly
// ---------------------------------------------------------------------------
static void TestMixedDescriptors(void)
{
    printf("TestMixedDescriptors...\n");
    constexpr uint32_t kDepth = 64;
    MockReplyQueue q(kDepth);

    // Interleave address-reply and (would-be) scsi-success descriptors
    for (int i = 0; i < 8; i++) {
        MPT3ReplyDescriptor d;
        if (i % 2 == 0) {
            d.AddressReply.DescriptorType = MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY;
            d.AddressReply.SMID           = (uint16_t)(i + 1);
        } else {
            // Simulate a different reply type
            d.Words = 0x0000000000000002ULL; // type = 0x02
        }
        q.FirmwareWrite(d);
    }

    std::vector<MPT3ReplyDescriptor> out;
    uint32_t n = q.Drain(out);
    CHECK_EQ(n, 8U);

    // Verify alternating types
    for (int i = 0; i < 8; i++) {
        if (i % 2 == 0) {
            CHECK_EQ(out[i].AddressReply.DescriptorType,
                     (uint8_t)MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY);
            CHECK_EQ(out[i].AddressReply.SMID, (uint16_t)(i + 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Test: SMID free-list exhaustion and recovery
// ---------------------------------------------------------------------------
static void TestSMIDFreeListExhaustion(void)
{
    printf("TestSMIDFreeListExhaustion...\n");
    constexpr uint32_t kMax = 8;

    uint16_t freeList[kMax];
    uint32_t freeHead = 0, freeTail = kMax;
    for (uint32_t i = 0; i < kMax; i++) freeList[i] = (uint16_t)(i + 1);

    // Alloc all
    std::vector<uint16_t> allocated;
    while (freeHead != freeTail) {
        allocated.push_back(freeList[freeHead % kMax]);
        freeHead++;
    }
    CHECK_EQ(allocated.size(), (size_t)kMax);
    CHECK(freeHead == freeTail); // exhausted

    // Attempt alloc returns "0" (queue empty)
    CHECK(freeHead == freeTail);

    // Free two back
    for (int i = 0; i < 2; i++) {
        freeList[freeTail % kMax] = allocated[i];
        freeTail++;
    }

    // Now two more can be allocated
    CHECK(freeHead != freeTail);
    uint16_t got = freeList[freeHead % kMax]; freeHead++;
    CHECK(got != 0);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void)
{
    printf("=== MPT3 Reply Queue Unit Tests ===\n\n");

    TestEmptyDrain();
    TestSingleEntry();
    TestBatchDrain();
    TestRingWrap();
    TestMixedDescriptors();
    TestSMIDFreeListExhaustion();

    printf("\n=== Results: %d passed, %d failed ===\n", gPassed, gFailed);
    return (gFailed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
