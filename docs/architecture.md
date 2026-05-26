# LSI9300Driver — Architecture Reference

## 1. Motivation and Scope

Apple Silicon Macs (M1/M2/M3/M4) use an ARM64 SoC with Thunderbolt 3/4 as the
sole external PCIe bus. Users wishing to attach SAS/SATA drives via an LSI 9300
HBA must do so through a Thunderbolt-to-PCIe enclosure. macOS ships no driver for
the SAS3008 chip; this project fills that gap.

The driver targets **IT mode** only (no RAID firmware), because:
- IT mode exposes each physical drive directly to the OS.
- macOS's native software RAID and third-party stacks (ZFS via OpenZFS-for-macOS)
  then manage data protection.
- IR firmware uses a different reply-message schema that would require a separate
  implementation path.

---

## 2. macOS Driver Framework Selection

### Why DriverKit, not a KEXT?

macOS 11 Big Sur deprecated third-party KernelExtensions (kexts) in favour of
**DriverKit** (dext) — user-space drivers that communicate with the kernel via a
restricted IPC layer. Key differences:

| Aspect | KEXT | DriverKit (dext) |
|--------|------|-----------------|
| Execution space | Kernel | User space (sandboxed process) |
| Crash impact | Kernel panic | Driver restarts; kernel survives |
| Memory access | Direct physical | Via IOMemoryMap / IODMACommand |
| Interrupt delivery | True ISR | Dispatch source on a queue |
| Build signing | No special cert | Requires Apple Developer + entitlements |
| macOS 12+ support | Legacy only | Fully supported / required for new drivers |

### SCSI family choice

`IOUserSCSIParallelInterfaceController` (from `SCSIControllerDriverKit.framework`)
is the correct base class for an HBA driver. It exposes the HBA as a SCSI parallel
bus; individual target drivers (`IOSCSIDiskDriver`, etc.) attach to each discovered
device handle and handle protocol-layer tasks (READ, WRITE, INQUIRY, etc.).

---

## 3. Hardware Protocol — MPT3SAS Overview

The SAS3008 implements Broadcom's **MPI 2.5 ("MPT3SAS")** protocol over BAR1 MMIO.

### 3.1 BAR Layout

| BAR | Width | Purpose |
|-----|-------|---------|
| BAR0 | 32-bit I/O | Legacy; unused |
| BAR1 | 64-bit MMIO | System interface registers (this driver uses this) |

### 3.2 Initialisation Sequence

```
Host                                    SAS3008
─────────────────────────────────────────────────────────
1. Mask all host interrupts             (write MASK_ALL to INT_MASK_REG)
2. Read IOC state                       ← RESET / READY / FAULT
3. [If not READY] Soft reset           (unlock WRITE_SEQ, assert HOLD_IOC_RESET)
4. Wait for IOC state = READY
5. IOCFacts (doorbell handshake)       → doorbell write (function=IOC_FACTS)
                                        ← doorbell read  (MaxDevices, Credits, etc.)
6. Allocate DMA pools
   • Request frame pool (128 B × N)
   • Reply post queue  (8 B × depth)
   • Reply free queue  (8 B × depth) ← physical addresses of reply frames
   • Sense buffer pool (252 B × N)
7. Populate reply free queue
8. IOCInit (doorbell handshake)        → doorbell write (queue addresses + depths)
                                        ← doorbell ACK
9. IOC state = OPERATIONAL
10. Setup MSI-X interrupt handler
11. Unmask reply interrupt
12. EventNotification                  → request descriptor post
13. PortEnable                         → request descriptor post
                                        ← async event: devices discovered
```

### 3.3 Request / Reply Descriptor FIFOs

**Submitting a command:**

```
CPU                                    SAS3008
─────────────────────────────────────────────────────────
1. Allocate SMID from free-list
2. Write MPI3SCSIIORequest into        (request frame pool, SMID-indexed)
   DMA-coherent frame
3. Build SGL chain in frame            (physical segments from IODMACommand)
4. OSSynchronizeIO()                   (ARM64 DMB: ensure frame visible before post)
5. Write 64-bit descriptor             → BAR1 + REQUEST_DESCRIPTOR_POST_LOW/HIGH
   to MMIO FIFO register               (DevHandle, SMID, MSIxIndex, type=SCSI_IO)
```

**Completing a command (MSI-X interrupt path):**

```
SAS3008                                CPU
─────────────────────────────────────────────────────────
                                       1. MSI-X fires → IOInterruptDispatchSource
                                          handler on dispatch queue
1. IOC writes 8-byte reply descriptor  2. Read reply descriptor from post ring[consumerIdx]
   into reply post queue ring
                                       3. If DescriptorType = SCSI_IO_SUCCESS:
                                          • Extract SMID
                                          • Call CompleteParallelTask(task, GOOD)
                                          • FreeSMID(smid)
                                       4. If DescriptorType = ADDRESS_REPLY:
                                          • Look up reply frame by physical address
                                          • Read SCSIStatus, SenseCount, TransferCount
                                          • Copy sense bytes from sense pool
                                          • Call CompleteParallelTask with status
                                          • Return reply frame to free queue
                                       5. Mark slot as 0xFFFFFFFFFFFFFFFF (consumed)
                                       6. consumerIdx = (consumerIdx + 1) % depth
                                       7. Write consumerIdx to REPLY_POST_HOST_INDEX_REG
```

---

## 4. ARM64 Memory Ordering Considerations

Apple Silicon uses the **AArch64 weakly-ordered memory model**. Device MMIO is
mapped as `Device-nGnRnE` (strongly ordered device memory), but DMA buffers in
system RAM are `Normal` memory (weakly ordered).

### Rules applied in this driver

| Operation | Barrier used | Reason |
|-----------|-------------|--------|
| Write request frame, then post descriptor | `OSSynchronizeIO()` (= `dmb oshst`) before posting | Ensure all frame writes are visible to the device before the descriptor is posted |
| Read reply descriptor from ring | `volatile` load (no explicit barrier needed) | Ring is in Normal memory; the MSI-X interrupt itself acts as a synchronisation point on ARM64 |
| Write `REPLY_POST_HOST_INDEX_REG` | `OSSynchronizeIO()` after write | Ensure the register write completes before the CPU touches the recycled ring slot |

### Why `volatile` is not sufficient on its own

`volatile` prevents the **compiler** from reordering or eliminating loads/stores.
It does NOT prevent the **CPU** (or interconnect) from reordering stores to Normal
cacheable memory relative to MMIO stores. The `OSSynchronizeIO()` macro generates
a `dmb oshst` (Data Memory Barrier, Outer Shareable, Stores) that orders all prior
stores against subsequent MMIO stores visible to the device.

---

## 5. DMA Pool Layout

All DMA-coherent memory is allocated via `IOBufferMemoryDescriptor::Create()`.
Physical addresses are obtained once via `IODMACommand::PrepareForDMA()` and
cached — no per-I/O address translation occurs on the hot path.

```
┌──────────────────────────────────────────────────────────────┐
│  Request Frame Pool  (kNumRequestFrames × 128 bytes)         │
│  Frame[0]  Frame[1]  Frame[2]  ...  Frame[N-1]               │
│  SMID=1    SMID=2    SMID=3         SMID=N                   │
├──────────────────────────────────────────────────────────────┤
│  Sense Buffer Pool  (kNumRequestFrames × 252 bytes)          │
│  Sense[0]  Sense[1]  ...  Sense[N-1]                         │
│  SMID=1    SMID=2         SMID=N                             │
├──────────────────────────────────────────────────────────────┤
│  Reply Free Queue  (kNumReplyFrames × 8 bytes)               │
│  [PA of ReplyFrame[0]]  [PA of ReplyFrame[1]]  ...           │
├──────────────────────────────────────────────────────────────┤
│  Reply Frame Pool  (kNumReplyFrames × 128 bytes)             │
│  [Reply header space for address-reply messages]             │
├──────────────────────────────────────────────────────────────┤
│  Reply Post Queue  (kReplyQueueDepth × 8 bytes)              │
│  [Descriptor 0]  [Descriptor 1]  ...  [0xFFFF... sentinels]  │
└──────────────────────────────────────────────────────────────┘
```

Pool sizes are chosen conservatively:

| Pool | Count | Per-entry size | Total |
|------|-------|---------------|-------|
| Request frames | 512 | 128 B | 64 KiB |
| Sense buffers | 512 | 252 B | 126 KiB |
| Reply frames | 512 | 128 B | 64 KiB |
| Reply free queue | 512 | 8 B | 4 KiB |
| Reply post queue | 512 | 8 B | 4 KiB |
| **Total** | | | **~262 KiB** |

This is well within the DriverKit user-space process's memory limits.

---

## 6. SMID Allocation

The driver uses a **circular queue (ring buffer)** free-list for O(1) SMID
allocation and deallocation with no per-allocation memory overhead:

```cpp
uint16_t fSMIDFreeList[kNumRequestFrames];   // indices 0..N-1
uint32_t fSMIDFreeHead;                      // consumer (alloc) pointer
uint32_t fSMIDFreeTail;                      // producer (free)  pointer

// Allocate: returns SMID (1-based) or 0 if empty
uint16_t smid = (fSMIDFreeHead == fSMIDFreeTail)
              ? 0
              : fSMIDFreeList[fSMIDFreeHead++ % kNumRequestFrames];

// Free:
fSMIDFreeList[fSMIDFreeTail++ % kNumRequestFrames] = smid;
```

SMIDs are 1-based (per the MPT3 spec); SMID 0 is reserved for management messages
(IOCInit, PortEnable, EventAck, etc.) which use slot 0 of the frame pool.

---

## 7. Scatter-Gather List Construction

### Inline SGL (≤4 segments)

For small I/Os with ≤ 4 physical segments, the SGL entries are placed directly
inside the 128-byte request frame (after the fixed header fields):

```
MPT3SCSIIORequest frame (128 bytes)
  ├── Header + control fields  (72 bytes)
  └── SGL entries [0..3]       (4 × 12 bytes = 48 bytes)
       └── [flags|length] [addrLow] [addrHigh]
            Last entry: FLAGS |= END_OF_LIST | END_OF_BUFFER | LAST_ELEMENT
```

### Chain SGL (> 4 segments)

For larger I/Os, the 4th inline entry is replaced by a chain element pointing to
a separate DMA buffer containing the overflow SGL entries. This buffer is
pre-allocated per-SMID to avoid runtime allocation.

---

## 8. Error Handling and Recovery

### Per-command errors

| IOC status | Driver response |
|------------|-----------------|
| `SCSI_DATA_UNDERRUN` | Report residual; task completes as GOOD (device under-ran) |
| `SCSI_TASK_TERMINATED` | Return `kSCSITaskStatus_TaskTimeoutOccurred` |
| `SCSI_PROTOCOL_ERROR` | Return `SERVICE_DELIVERY_OR_TARGET_FAILURE` |
| `SUCCESS` with CHECK CONDITION | Copy autosense; return `kSCSITaskStatus_CHECK_CONDITION` |

### Controller fault recovery

When the IOC transitions to `FAULT` or `COREDUMP` state (detected via the
doorbell register or a fault-notification event):

1. Complete all in-flight commands with `DeliveryFailure`.
2. Issue `SoftReset()`.
3. Re-run the full initialisation sequence.
4. Re-enable MSI-X and event notification.
5. Post a `PortEnable` to re-discover devices.

---

## 9. Thread-Safety Model

DriverKit delivers interrupts on a **dispatch queue** (not a real ISR), so the
interrupt handler and the command submission path can run concurrently. The
following invariants are maintained:

- `AllocateSMID()` / `FreeSMID()` operate on the ring-buffer free-list.
  Since the free-list head is only advanced from the submission path and the
  tail only from the completion path, and because dispatch queues are
  serialised within each queue, no explicit lock is needed *as long as
  submission and completion run on the same queue*.

  If multiple dispatch queues are used (multi-queue MSI-X), an `os_unfair_lock`
  guards the SMID free-list.

- The reply post queue is owned exclusively by the interrupt handler queue.
  No lock is needed for the ring-pointer `fReplyPostIndex`.

- `fReplyFreeIndex` is advanced only from the interrupt handler (via
  `AdvanceReplyFreeIndex()`), so no lock is needed there either.

---

## 10. File Index

```
LSI9300Driver/
  LSI9300Driver.h        Main driver class declaration
  LSI9300Driver.cpp      Main driver implementation
  MPT3Registers.h        MMIO register offsets and bitmasks
  MPT3Types.h            Wire-format protocol structures (packed, with static_asserts)
  MPT3IOC.h              IOC initialization state machine declaration
  MPT3IOC.cpp            IOC initialization state machine implementation
  Info.plist             DriverKit bundle descriptor + IOKit personalities
  LSI9300Driver.entitlements  Code-signing entitlements

Tests/
  TestStubs.h            DriverKit API stubs for unit test compilation
  MPT3ProtocolTests.cpp  Struct-size, encoding, and ring-arithmetic tests
  MPT3ReplyQueueTests.cpp Reply ring simulation tests

docs/
  architecture.md        This document
  register_map.md        Detailed BAR1 register reference

Makefile                 Build rules (dext + unit tests)
README.md                Project overview, build instructions, usage guide
```
