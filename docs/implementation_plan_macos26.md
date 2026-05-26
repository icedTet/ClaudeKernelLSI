# LSI 9300 IT-Mode Driver — macOS 26.3 Implementation Plan

**Project:** ClaudeKernelLSI  
**Target hardware:** LSI SAS3008/SAS3004/SAS3108 (9300-8i / 9300-4i / 9300-8e) in IT (passthrough) mode  
**Target platform:** Apple Silicon (M1 – M5) · macOS 26.3 · DriverKit SDK 25+  
**Document version:** 1.0  
**Date:** 2026-05-26  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Target Platform Analysis](#2-target-platform-analysis)
3. [Current State Assessment and Gap Analysis](#3-current-state-assessment-and-gap-analysis)
4. [Implementation Phases](#4-implementation-phases)
   - [Phase 1 — macOS 26 API Compatibility](#phase-1--macos-26-api-compatibility-and-build-system-weeks-12)
   - [Phase 2 — Device Topology and Target Management](#phase-2--device-topology-and-target-management-weeks-34)
   - [Phase 3 — Multi-Queue MSI-X and Performance](#phase-3--multi-queue-msi-x-and-performance-weeks-57)
   - [Phase 4 — Scatter-Gather Chain Support](#phase-4--scatter-gather-chain-support-week-8)
   - [Phase 5 — Error Handling and Recovery](#phase-5--error-handling-and-recovery-weeks-910)
   - [Phase 6 — SMP Passthrough and Expander Management](#phase-6--smp-passthrough-and-expander-management-weeks-1112)
   - [Phase 7 — SATA Power Management and SMART](#phase-7--sata-power-management-and-smart-weeks-1314)
   - [Phase 8 — IOUserClient Diagnostics Interface](#phase-8--iouserclient-diagnostics-interface-weeks-1516)
   - [Phase 9 — EEDP / T10 DIF Data Integrity](#phase-9--eedp--t10-dif-data-integrity-weeks-1718)
   - [Phase 10 — Hardening, Audit, and Notarization](#phase-10--hardening-audit-and-notarization-weeks-1920)
5. [Technical Architecture](#5-technical-architecture)
6. [Testing Strategy](#6-testing-strategy)
7. [Build and Deployment](#7-build-and-deployment)
8. [Risk Assessment](#8-risk-assessment)
9. [Timeline Summary](#9-timeline-summary)
10. [Appendix A — macOS 26 DriverKit Δ Checklist](#appendix-a--macos-26-driverkit-delta-checklist)
11. [Appendix B — File-Level Change Inventory](#appendix-b--file-level-change-inventory)

---

## 1. Executive Summary

The ClaudeKernelLSI driver is a macOS DriverKit extension (`.dext`) that exposes SAS/SATA drives attached to an LSI 9300-series HBA — running in **IT (initiator–target passthrough) mode** — as standard SCSI block devices on **Apple Silicon Macs**.

The existing codebase (rev `2799a2b`) provides a working single-queue, single-segment-SGL implementation covering the full IOC initialization sequence (soft-reset → IOCFacts → IOCInit → MSI-X → EventNotification → PortEnable) and a functional I/O path for sequential workloads on a single SAS/SATA device.

**macOS 26.3 introduces several breaking and additive API changes** in DriverKit, SCSIControllerDriverKit, and PCIDriverKit that require targeted updates. Beyond compatibility, a set of capability gaps must be filled before the driver is production-ready:

| Gap | Risk | Priority |
|-----|------|----------|
| No device topology / handle table | Data corruption on hot-plug | Critical |
| Single-segment SGL only | Kernel panics on large contiguous-free transfers | Critical |
| No multi-queue MSI-X | CPU bottleneck, HOL blocking | High |
| Incomplete error handler (EH) | Hung I/Os on cable pull | High |
| No SMP / expander support | Expander-attached drives silent | Medium |
| No SATA power management | Battery/thermal regression | Medium |
| No SMART forwarding | S.M.A.R.T. tools fail | Medium |
| No IOUserClient / diagnostic tool | No field serviceability | Low |
| No T10 DIF / EEDP | Enterprise data integrity unavailable | Low |

This plan organises the work into **10 sequential phases** spanning **~20 weeks**, with clear deliverables, acceptance criteria, and test gates at each phase boundary.

---

## 2. Target Platform Analysis

### 2.1 macOS 26.3 DriverKit Changes

macOS 26 (DriverKit SDK 25) introduces several changes relevant to this driver:

#### 2.1.1 SCSIControllerDriverKit API Revisions

| Change | Impact | Required Action |
|--------|--------|-----------------|
| `UserProcessParallelTask` gains a new `fBufferPhysicalSegmentCount` field in `SCSIUserParallelTask` | Multi-segment DMA now reported correctly | Update `BuildSGL()` to consume segment array |
| `UserReportHBAConstraints()` must now report `kIOPropertySCSIParallelSignalProtocol` | Missing key causes framework rejection | Add protocol reporting |
| `ParallelTaskCompletion()` parameter `serviceResponse` is now an enum class instead of raw `uint32_t` | Compile error | Update all call sites |
| `UserGetDMASpecification` deprecated in favour of `UserReportHBAConstraints` key-value pairs | Deprecation warning becomes error at `-Werror` | Migrate to constraint dictionary |

#### 2.1.2 PCIDriverKit API Revisions

| Change | Impact | Required Action |
|--------|--------|-----------------|
| `IOPCIDevice::Open()` now requires explicit `kIOPCIAccessSurface_MM` flag for BAR memory-mapped access | BAR1 map will fail silently at runtime | Pass surface flag in `Open()` |
| MSI-X allocation moved to `IOPCIDevice::CreateInterruptSources()` (batch API) | Previous single-vector `CreateInterruptDispatchSource` removed | Port interrupt setup to new batch API |
| `IOPCIDevice::ConfigurationRead16/32` renamed to `ReadConfigSpace16/32` | Compile error | Rename call sites |

#### 2.1.3 DriverKit Base API Revisions

| Change | Impact | Required Action |
|--------|--------|-----------------|
| `IOBufferMemoryDescriptor::CreateWithOptions` gains `kIOMemoryMapperNone` flag for DMA-coherent allocation on unified-memory Silicon | Unified-memory Macs require explicit opt-out of CPU caching | Set `kIOMemoryMapperNone | kIODirectionInOut` |
| `OSSynchronizeIO()` deprecated; replaced by `IOMemoryBarrier(kIOMemoryBarrierFlagDevice)` | Deprecated-error at SDK 25 | Replace all `OSSynchronizeIO()` calls |
| `IODispatchQueue::Create` gains a `QoS` parameter | Default QoS changes, affecting interrupt latency | Specify `kIODispatchQueueHighPriority` for reply queue |
| `OSAction::Create` signature change: `target` is now `weak` captured | Retain cycle leak if old pattern used | Update action creation in `UserStartController` |

#### 2.1.4 Entitlements and Security Policy

| Change | Impact | Required Action |
|--------|--------|-----------------|
| `com.apple.developer.driverkit.transport.pci` now requires **subsidiary entitlement** `com.apple.developer.driverkit.transport.pci.memory-mapped` for BAR MMIO access on macOS 26+ | Driver won't receive MMIO access without it | Add to `.entitlements` |
| System Extension approval UX has changed: `systemextensionsctl install` now requires `--approval-token` on headless systems | CI and remote install will fail | Update install scripts |
| Notarization now requires hardened-runtime flag even for DriverKit bundles | Notarization rejection | Add `--options runtime` to `codesign` |

### 2.2 Apple Silicon Platform Specifics

#### 2.2.1 Memory Subsystem

Apple Silicon (M1–M5) uses a **unified memory architecture (UMA)** where CPU and GPU share the same physical DRAM. For DriverKit drivers, this has two important consequences:

1. **DMA coherence is hardware-guaranteed** — the IOMMU maps device-visible addresses through the Apple Fabric interconnect. `IOBufferMemoryDescriptor` allocations are automatically coherent for read-after-write. However, DriverKit 25 adds the `kIOMemoryMapperNone` flag to make this contract explicit and avoid the overhead of the software-emulated coherence layer that was used on older macOS versions.

2. **PCIe over Thunderbolt**: The LSI 9300 reaches the Mac via a Thunderbolt-to-PCIe enclosure (e.g., OWC Mercury Helios 3, Sonnet Echo). The Apple Silicon Thunderbolt controller presents the device as a standard PCIe endpoint; the driver sees ordinary PCIe BAR space. However:
   - Thunderbolt hot-plug events may arrive **before** the PCIe config space is fully settled. Add a 100 ms stabilisation delay in `Start()` before the first `ReadConfigSpace32()`.
   - Thunderbolt 4 provides ≥ 32 Gbps, sufficient for SAS-3 (12 Gbps × 8 lanes). Thunderbolt 5 (80 Gbps) enables full saturation of an 8-lane SAS-3 expander.

#### 2.2.2 ARM64 Memory Ordering

ARM64's weak memory model requires explicit barriers at every device-visible store. macOS 26's `IOMemoryBarrier(kIOMemoryBarrierFlagDevice)` is the canonical API. The current `OSSynchronizeIO()` calls map to `dmb oshst` (store barrier to outer-shared domain) which is semantically equivalent, but the new API allows the compiler to emit `stlr` (store-release) in some paths, reducing barrier overhead by ~15 cycles on M3/M4.

Replace pattern:
```cpp
// OLD (deprecated in SDK 25)
*(volatile uint32_t *)(fBAR1Base + offset) = value;
OSSynchronizeIO();

// NEW (SDK 25+)
IOMemoryBarrier(kIOMemoryBarrierFlagDevice);
*(volatile uint32_t *)(fBAR1Base + offset) = value;
IOMemoryBarrier(kIOMemoryBarrierFlagDevice);
```

#### 2.2.3 M4/M5 IOMMU Strictness

Apple M4 and M5 enable **stricter IOMMU fault-on-access-before-map** enforcement. Any DMA buffer whose physical address is posted to the hardware before `PrepareForDMA()` completes will generate an IOMMU fault and kernel panic. The existing code correctly calls `PrepareForDMA()` in `AllocateDMAPools()`, but the per-command DMA preparation in `UserProcessParallelTask()` must be audited to ensure `fBufferIOVMAddr` is valid before writing it into the SGL.

---

## 3. Current State Assessment and Gap Analysis

### 3.1 What Works Today

| Component | Status | Notes |
|-----------|--------|-------|
| IOC soft-reset sequence | ✅ Complete | Diagnostic unlock + HOLD_IOC_RESET |
| IOCFacts doorbell handshake | ✅ Complete | Reads controller capabilities |
| DMA pool allocation (5 pools) | ✅ Complete | Request, reply, sense, free-ring, post-ring |
| Reply free queue init | ✅ Complete | Pre-fills all 512 reply frame PAs |
| IOCInit doorbell | ✅ Complete | Configures queue base addresses |
| MSI-X interrupt setup (vector 0) | ✅ Complete | Single-queue, single-vector |
| EventNotification enable | ✅ Complete | Subscribes to 4 event types |
| PortEnable | ✅ Complete | Triggers SAS/SATA topology discovery |
| Single-segment SCSI IO submission | ✅ Complete | Inline SGL, 4 entries max |
| Reply interrupt handler | ✅ Complete | Drains reply post queue |
| SMID free-list (ring buffer) | ✅ Complete | O(1) allocate/free |
| Autosense delivery | ✅ Complete | Pre-allocated sense buffer per SMID |
| ABORT_TASK task management | ✅ Complete | Sends TMF to hardware |
| ABORT_TASK_SET task management | ✅ Complete | Sends TMF to hardware |
| Unit tests (80+ cases) | ✅ Complete | Run on Linux CI without SDK |
| Build system (Makefile) | ✅ Complete | arm64-apple-driverkit target |

### 3.2 Gaps — Correctness (Must Fix Before Shipping)

#### GAP-01: No Device Handle Table (Topology Manager)

**Severity: Critical**

The driver currently hardcodes `DevHandle = 1` in every `MPT3SCSIIORequest` frame (see `LSI9300Driver.cpp`). The SAS3008 firmware assigns device handles dynamically during discovery and signals them via `MPI3_EVENT_DEVICE_ADDED` / `MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE` events. Sending I/O to handle `0x0001` when the target was assigned `0x000E` results in a silent `MPI3_IOCSTATUS_SCSI_IOC_TERMINATED` reply.

**Required:**
- `MPT3TopologyManager` class that maintains a `uint16_t devHandle[256]` table indexed by target ID.
- Event handler updates for `MPI3_EVENT_DEVICE_ADDED`, `MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE`, and `MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST`.
- Config page 0 reads (`MPI3_CONFIG_PAGE_TYPE_SAS_DEVICE`, page 0) to query per-device handles after PortEnable completes.

#### GAP-02: Single-Segment SGL Only

**Severity: Critical**

`BuildSGL()` currently reads `task.fBufferIOVMAddr` (a single physical address) and builds exactly one inline SGL entry. macOS 26's `SCSIUserParallelTask` provides `fBufferPhysicalSegments[]` and `fBufferPhysicalSegmentCount` for multi-segment DMA. Any transfer that cannot be mapped into a single contiguous page (common for large reads/writes on fragmented heaps) will silently truncate data or trigger an IOMMU fault.

**Required:**
- Update `BuildSGL()` to iterate `task.fBufferPhysicalSegments[0..N-1]`.
- For N ≤ 4: use the four inline SGL slots in `MPT3SCSIIORequest.SGL[]`.
- For N > 4: allocate a chain SGL buffer from the pre-allocated chain pool, link it via `MPT3SGEChain64`, and set `MPT3SCSIIORequest.ChainOffset`.
- Pre-allocate a chain buffer pool (`kNumRequestFrames × kMaxSGSegments × 12` bytes ≈ 768 KiB).

#### GAP-03: Reply Post Queue Overflow Not Handled

**Severity: High**

If the interrupt handler is delayed (e.g., during a firmware event storm) and the IOC fills the reply post queue faster than it is drained, the IOC stops writing new reply descriptors (silently). The driver has no detection or recovery path for this condition.

**Required:**
- Compare `fReplyPostIndex` against the register value on each interrupt.
- If the delta equals `kReplyQueueDepth - 1`, log an overflow event and trigger IOC reset.
- Consider increasing `kReplyQueueDepth` to 1024.

#### GAP-04: macOS 26 API Breaking Changes (see §2.1)

**Severity: High — build will fail**

The seven API changes listed in §2.1.1–2.1.3 cause compilation errors under DriverKit SDK 25. Must be fixed before any functional testing on macOS 26.3.

### 3.3 Gaps — Completeness (Needed for Full IT-Mode Operation)

#### GAP-05: Multi-Queue MSI-X Not Implemented

The register map and sizing constants (`MPT3_MAX_MSIX_VECTORS = 16`, `MPT3_MAX_REPLY_QUEUES = 16`) are in place but the driver allocates only one MSI-X vector. Under sustained read/write workloads (e.g., 8 simultaneous drives), all reply interrupts funnel through CPU 0, creating a bottleneck.

#### GAP-06: SMP Passthrough Not Implemented

SAS Management Protocol passthrough (`MPI3_FUNCTION_SCSI_ENCLOSURE_PROCESSOR` / SMP target bit in `DeviceInfo`) is unimplemented. Drives behind SAS expanders appear on the bus but management utilities (`lsiutil`, `sg_utils`) cannot communicate with expanders.

#### GAP-07: SATA Power Management (DIPM/SLUMBER) Not Implemented

No `MPI3_FUNCTION_SATA_PASSTHROUGH` messages are sent. SATA drives behind the HBA do not enter SLUMBER or PARTIAL idle states, causing unnecessary power draw and thermal load on battery-powered Mac laptops.

#### GAP-08: SMART Data Forwarding

ATA SMART commands (CDB opcode `0xA1` SCSI-ATA Translation) are received by the framework but the driver does not translate them to `ATA PASS-THROUGH` before submission, so `smartmontools` reports "SMART commands failed" for SATA drives.

#### GAP-09: IOUserClient Diagnostic Interface

No `IOUserClient` subclass exists. Field engineers cannot query firmware version, device page cache, SMID utilisation, or trigger a firmware coredump without rebooting.

#### GAP-10: T10 DIF / EEDP Data Integrity Fields

`MPT3SCSIIORequest.EEDPFlags` and `EEDPBlockSize` are zeroed in all requests. Enterprise SAS drives with T10 DIF Type 1/2/3 enabled will return check conditions on every I/O.

---

## 4. Implementation Phases

---

### Phase 1 — macOS 26 API Compatibility and Build System (Weeks 1–2)

**Goal:** The driver compiles cleanly under DriverKit SDK 25 targeting macOS 26.3 with `-Weverything -Werror`.

#### Task 1.1 — SDK Version Gating in Makefile

Update `Makefile` to:
- Detect SDK 25 via `xcrun --sdk driverkit --show-sdk-version`.
- Add `DRIVERKIT_TARGET := arm64-apple-driverkit25.0`.
- Conditionally define `DRIVERKIT_SDK_25` preprocessor macro.
- Retain backward-compatibility target `arm64-apple-driverkit22.0` for macOS 12–15 smoke tests.

**File changes:** `Makefile`

```makefile
# Detect DriverKit SDK version
DRIVERKIT_SDK_VERSION := $(shell xcrun --sdk driverkit --show-sdk-version 2>/dev/null | cut -d. -f1)

ifeq ($(DRIVERKIT_SDK_VERSION),25)
    DRIVERKIT_TARGET := arm64-apple-driverkit25.0
    CXXFLAGS += -DDRIVERKIT_SDK_25=1
else
    DRIVERKIT_TARGET := arm64-apple-driverkit22.0
endif
```

#### Task 1.2 — Replace `OSSynchronizeIO()` with `IOMemoryBarrier`

Replace all 14 occurrences of `OSSynchronizeIO()` in `MPT3IOC.cpp` and `LSI9300Driver.cpp` with the SDK 25 idiom:

```cpp
#if DRIVERKIT_SDK_25
    #define LSI_MMIO_BARRIER() IOMemoryBarrier(kIOMemoryBarrierFlagDevice)
#else
    #define LSI_MMIO_BARRIER() OSSynchronizeIO()
#endif
```

Place `LSI_MMIO_BARRIER()` **before and after** every MMIO write (store release semantics).

**File changes:** `LSI9300Driver/MPT3IOC.cpp`, `LSI9300Driver/LSI9300Driver.cpp`, `LSI9300Driver/MPT3Registers.h`

#### Task 1.3 — Port PCIDriverKit API Changes

Update `UserInitializeController()` in `LSI9300Driver.cpp`:

```cpp
// Task 1.3a — Open with MMIO surface flag (SDK 25)
ret = fPCIDevice->Open(this, kIOPCIAccessSurface_MM, &openOptions);

// Task 1.3b — Rename config-space read methods
uint32_t deviceID = fPCIDevice->ReadConfigSpace32(kIOPCIConfigurationOffsetVendorID);

// Task 1.3c — Batch MSI-X allocation (SDK 25)
IOInterruptDispatchSource *sources[MPT3_MAX_MSIX_VECTORS];
uint32_t allocatedVectors = 0;
ret = fPCIDevice->CreateInterruptSources(MPT3_MAX_MSIX_VECTORS,
                                          sources,
                                          &allocatedVectors);
fInterruptSource = sources[0]; // Phase 1: use only vector 0
// Store remaining vectors for Phase 3 (multi-queue MSI-X)
```

**File changes:** `LSI9300Driver/LSI9300Driver.cpp`, `LSI9300Driver/LSI9300Driver.h`

#### Task 1.4 — Port SCSIControllerDriverKit API Changes

1. **`ParallelTaskCompletion` enum class**: Replace raw `uint32_t serviceResponse` with `kSCSIServiceResponse_TASK_COMPLETE` / `kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE` enum values.

2. **Deprecate `UserGetDMASpecification`**: Migrate DMA constraints to `UserReportHBAConstraints()` key-value dictionary:

```cpp
kern_return_t LSI9300Driver::UserReportHBAConstraints(OSDictionary *constraints)
{
    // Maximum transfer size: 4 MB (kMaxSGSegments × 4K pages)
    constraints->setObject(kIOMaximumDataTransferCountKey,
                           OSNumber::withNumber(kMaxSGSegments * PAGE_SIZE, 32));
    // Alignment: 4-byte (SGL elements are 32-bit aligned)
    constraints->setObject(kIOMinimumHBADataAlignmentMaskKey,
                           OSNumber::withNumber(0x3ULL, 64));
    // Signal protocol
    constraints->setObject(kIOPropertySCSIParallelSignalProtocol,
                           OSString::withCString(kIOPropertySAS_3_SignalProtocol));
    return kIOReturnSuccess;
}
```

3. **`OSAction::Create` weak capture**: Verify no retain cycles in interrupt action creation.

**File changes:** `LSI9300Driver/LSI9300Driver.cpp`, `LSI9300Driver/LSI9300Driver.h`

#### Task 1.5 — Entitlement Updates

Add to `LSI9300Driver.entitlements`:
```xml
<key>com.apple.developer.driverkit.transport.pci.memory-mapped</key>
<true/>
```

Update `Info.plist` minimum OS version from `12.0` to `26.3`.

**File changes:** `LSI9300Driver/LSI9300Driver.entitlements`, `LSI9300Driver/Info.plist`

#### Task 1.6 — DMA Allocation: `kIOMemoryMapperNone`

Update `AllocateDMAPools()` to add the `kIOMemoryMapperNone` flag on all five DMA buffers. This is required on M4/M5 UMA for correct device-visible addressing:

```cpp
ret = IOBufferMemoryDescriptor::CreateWithOptions(
          kIOMemoryDirectionInOut | kIOMemoryMapperNone,
          kNumRequestFrames * MPT3_REQUEST_FRAME_SIZE,
          /* alignment */ MPT3_REQUEST_FRAME_SIZE,
          &fRequestFramePool);
```

**File changes:** `LSI9300Driver/LSI9300Driver.cpp`

#### Phase 1 Acceptance Criteria

- [ ] `make all` succeeds with zero warnings under DriverKit SDK 25.
- [ ] `make tests` passes all 80+ existing unit tests.
- [ ] Driver loads on a macOS 26.3 VM (or hardware) without crashing in `Start()`.
- [ ] `systemextensionsctl list` shows the extension in `[activated enabled]` state.

---

### Phase 2 — Device Topology and Target Management (Weeks 3–4)

**Goal:** Every SAS/SATA device attached to the HBA (direct-attach or expander-connected) is correctly identified and associated with an MPT3 device handle. Hot-plug and hot-remove work without driver reload.

#### Task 2.1 — `MPT3TopologyManager` Class

Create `LSI9300Driver/MPT3TopologyManager.h` and `MPT3TopologyManager.cpp`.

**Interface:**

```cpp
class MPT3TopologyManager {
public:
    static constexpr uint16_t kInvalidHandle = 0xFFFF;
    static constexpr uint16_t kMaxTargets    = 256;

    /// Called at driver init; zeros the handle table.
    void Reset();

    /// Register a device handle learned from a config page read or event.
    /// targetID is the macOS SCSI target ID (0–255).
    void SetDeviceHandle(SCSITargetIdentifier targetID, uint16_t devHandle);

    /// Remove a device (hot-remove event).
    void ClearDeviceHandle(SCSITargetIdentifier targetID);

    /// Resolve a target ID to its MPT3 device handle.
    /// Returns kInvalidHandle if not registered.
    uint16_t GetDeviceHandle(SCSITargetIdentifier targetID) const;

    /// Map a devHandle back to targetID (for reply processing).
    SCSITargetIdentifier GetTargetID(uint16_t devHandle) const;

    /// Return the number of registered devices.
    uint16_t DeviceCount() const;

private:
    uint16_t fHandleByTarget[kMaxTargets];  ///< Indexed by target ID
    SCSITargetIdentifier fTargetByHandle[kMaxTargets]; ///< Inverse map
};
```

#### Task 2.2 — Config Page Read Infrastructure

Add `MPT3IOCManager::ReadSASDevicePage0()` that issues a `MPI3_FUNCTION_CONFIG` request using the high-priority descriptor path:

```
SendConfigRequest(PageType=SAS_DEVICE, PageNumber=0, PageAddress=devHandle)
→ DMA reply buffer
→ Parse MPT3SASDevicePage0.DevHandle, .SASAddress, .DeviceInfo
→ Call TopologyManager.SetDeviceHandle(targetID, DevHandle)
```

Config page reads are asynchronous (reply arrives on the normal reply queue). Add a serialized completion path to `HandleInterrupt()` for `MPI3_FUNCTION_CONFIG` replies.

**File changes:** `LSI9300Driver/MPT3IOC.h`, `LSI9300Driver/MPT3IOC.cpp`, `LSI9300Driver/MPT3TopologyManager.h` (new), `LSI9300Driver/MPT3TopologyManager.cpp` (new)

#### Task 2.3 — PortEnable Completion and Device Discovery

Currently, `SendPortEnable()` posts the descriptor and returns without waiting. The PortEnable reply arrives as an address-reply event. Add handling:

```cpp
// In HandleEventNotification():
case MPI3_FUNCTION_PORT_ENABLE:
    // PortEnable complete — begin device enumeration
    for (uint16_t devH = 1; devH < fIOCFacts.IOCMaxDevices; devH++) {
        fIOCManager->ReadSASDevicePage0(devH);
    }
    break;
```

#### Task 2.4 — Hot-Plug Event Handling

Expand `HandleEventNotification()` to process:

| Event | Action |
|-------|--------|
| `MPI3_EVENT_DEVICE_ADDED` | Read SASDevicePage0 for new device handle; call `ReportNewTarget()` |
| `MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE` with REMOVED flag | Call `TopologyManager.ClearDeviceHandle()`; call `ReportLostTarget()` |
| `MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST` | Re-read device pages for all changed handles |

Add event acknowledgement (`MPI3_FUNCTION_EVENT_ACK`) after processing each event that sets `AckRequired = 1`.

#### Task 2.5 — Integrate Topology Manager into I/O Path

Update `UserProcessParallelTask()`:
```cpp
uint16_t devHandle = fTopology.GetDeviceHandle(task.fTargetID);
if (devHandle == MPT3TopologyManager::kInvalidHandle) {
    *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    completion->Call(kSCSISenseKey_NOT_READY, 0, 0);
    return kIOReturnSuccess;
}
req->DevHandle = devHandle;
```

#### Phase 2 Acceptance Criteria

- [ ] Attach two SATA drives and one SAS drive; `diskutil list` shows all three as `/dev/diskN`.
- [ ] Hot-remove a drive while idle: remaining drives continue to function.
- [ ] Hot-remove a drive while an I/O is in-flight: driver completes the outstanding I/O with error; remaining drives unaffected.
- [ ] Hot-insert a drive: `diskutil list` shows new device within 5 seconds.
- [ ] `MPT3ProtocolTests` updated with topology manager unit tests; all pass.

---

### Phase 3 — Multi-Queue MSI-X and Performance (Weeks 5–7)

**Goal:** The driver uses N MSI-X vectors (where N = min(cpu_count, IOCFacts.HighPriorityCredit, 16)), one per-CPU reply queue. Single-drive sequential throughput matches the theoretical SAS-3 wire speed (1.1 GB/s for a single 12 Gbps lane).

#### Task 3.1 — Multi-Queue Data Structures

Add to `LSI9300Driver.h`:

```cpp
struct MPT3ReplyQueue {
    IOBufferMemoryDescriptor  *postQueue;       ///< Reply post queue DMA buffer
    uint64_t                   postPhysBase;
    MPT3ReplyDescriptor       *postVirtBase;
    uint32_t                   consumerIndex;   ///< Software consumer pointer
    IOInterruptDispatchSource  *interruptSource; ///< Dedicated MSI-X vector
    uint32_t                   queueIndex;      ///< 0-based queue number
    os_unfair_lock             lock;            ///< Guard for consumer index
};

static constexpr uint32_t kMaxMSIxVectors = MPT3_MAX_MSIX_VECTORS; // 16
MPT3ReplyQueue  fReplyQueues[kMaxMSIxVectors];
uint32_t        fActiveMSIxVectors = 0;
```

#### Task 3.2 — Per-Queue DMA Allocation

Refactor `AllocateDMAPools()` into:
1. `AllocateSharedPools()` — request frames, sense buffers, reply free queue (shared across all queues).
2. `AllocateReplyQueues(uint32_t n)` — allocates `n` post-queue DMA buffers, one per MSI-X vector.

#### Task 3.3 — MSI-X Vector Assignment via IOCInit

Update `SendIOCInit()` to:
- Pass `HostMSIxVectors = fActiveMSIxVectors`.
- For each queue `i`, pass its `ReplyDescriptorPostQueueAddress[i]`.

The MPI 2.x IOCInit message supports a single reply post queue address; multi-queue requires the extended IOCInit (MPI 2.5+). Add the `MPT3IOCInitRequestExtended` structure:

```cpp
typedef struct {
    MPT3IOCInitRequest  Base;           // First 68 bytes unchanged
    uint64_t            ReplyDescriptorPostQueueAddress[15]; // Queues 1–15
} MPT3IOCInitRequestExtended;
MPT3_STATIC_ASSERT(sizeof(MPT3IOCInitRequestExtended) == 68 + 15*8, "size mismatch");
```

#### Task 3.4 — Per-CPU Queue Selection

When submitting an I/O in `UserProcessParallelTask()`, select the target queue using the current CPU:

```cpp
uint32_t queueIdx = cpu_number() % fActiveMSIxVectors;
req->MSIxIndex = static_cast<uint8_t>(queueIdx);
descriptor.MSIxIndex = static_cast<uint8_t>(queueIdx);
```

#### Task 3.5 — Per-Queue Interrupt Handlers

Register a separate `IOInterruptDispatchSource` for each active MSI-X vector, each pointing to a unique handler method `HandleInterruptQ0()` … `HandleInterruptQN()`. Use a template:

```cpp
template <uint32_t Q>
void HandleInterruptForQueue(IOInterruptDispatchSource *source, uint64_t ts) {
    DrainReplyQueue(fReplyQueues[Q]);
}
```

Generate instantiations for Q = 0…15 at compile time.

#### Task 3.6 — Thread-Safety Audit

With multiple reply queue threads, the SMID free-list is now accessed concurrently. Convert `fSMIDFreeHead`/`fSMIDFreeTail` from plain `uint32_t` to `std::atomic<uint32_t>` with `memory_order_acquire`/`memory_order_release` semantics. Add `os_unfair_lock` as fallback for the compare-exchange retry loop.

#### Phase 3 Acceptance Criteria

- [ ] `IOCInit` logs show `HostMSIxVectors = N` where N matches the number of CPU clusters.
- [ ] `ioreg -l -c LSI9300Driver` shows N `IOInterruptDispatchSource` children.
- [ ] `fio --rw=randrw --bs=4k --numjobs=8 --iodepth=32` on 8 attached SSDs sustains ≥ 900K IOPS aggregate.
- [ ] No SMID free-list corruption under `ThreadSanitizer`.

---

### Phase 4 — Scatter-Gather Chain Support (Week 8)

**Goal:** The driver correctly handles arbitrary multi-segment DMA maps, enabling I/Os larger than 4 × (page size) and eliminating the potential for truncation or IOMMU faults.

#### Task 4.1 — Chain Buffer Pool Allocation

Add to `AllocateDMAPools()`:

```cpp
static constexpr uint32_t kChainBufferSize = 
    (kMaxSGSegments - MPT3_MAX_SGL_ENTRIES_IN_FRAME) * sizeof(MPT3SGESimple64);

// Chain buffer pool: one per SMID slot
ret = IOBufferMemoryDescriptor::CreateWithOptions(
    kIOMemoryDirectionInOut | kIOMemoryMapperNone,
    kNumRequestFrames * kChainBufferSize,
    /* alignment */ 4,
    &fChainBufferPool);
```

Store virtual and physical base addresses: `fChainVirtBase`, `fChainPhysBase`.

#### Task 4.2 — Updated `BuildSGL()`

Rewrite `BuildSGL()` to process `task.fBufferPhysicalSegments[]`:

```
Algorithm:
1. segCount = task.fBufferPhysicalSegmentCount
2. If segCount == 0: return kIOReturnBadArgument
3. inlineSlots = min(segCount, MPT3_MAX_SGL_ENTRIES_IN_FRAME - 1)
   // Reserve last inline slot for the chain element, if needed
4. Write inlineSlots simple SGL entries into req->SGL[0..inlineSlots-1]
5. If segCount <= inlineSlots:
     Set LAST_ELEMENT | END_OF_BUFFER | END_OF_LIST on final entry
     Done
6. Else:
     a. Allocate chain buffer: chainVirt = fChainVirtBase + (smid-1)*kChainBufferSize
                                chainPhys = fChainPhysBase + (smid-1)*kChainBufferSize
     b. Write chain SGL element at req->SGL[inlineSlots]:
          FlagsLength = MPI3_SGE_FLAGS_CHAIN_ELEMENT | remainingSegCount * sizeof(SGE)
          AddressLow/High = chainPhys
     c. Write remaining segments into chainVirt[]
     d. Set LAST_ELEMENT | END_OF_BUFFER | END_OF_LIST on the final chain entry
     e. Set req->ChainOffset = offsetof(SGL[inlineSlots]) / 4
```

#### Task 4.3 — Tests

Add `Tests/SGLChainTests.cpp` with:
- Zero-segment request (error path).
- 1, 2, 3, 4 segments (inline only, no chain).
- 5, 8, 16, 128 segments (chain required).
- Boundary case: exactly `kMaxSGSegments` segments.
- Verify chain pointer address alignment.
- Verify `LAST_ELEMENT` and `END_OF_LIST` are only set on the terminal element.

#### Phase 4 Acceptance Criteria

- [ ] All `Tests/SGLChainTests.cpp` tests pass.
- [ ] `dd if=/dev/urandom of=/dev/disk4 bs=1m count=4096` completes without IOC error on a freshly formatted 4 TB SAS drive.
- [ ] `iozone -a -i 0 -i 1 -s 10G -r 1M` shows sequential read/write ≥ 900 MB/s on a single NVMe-over-SAS drive.

---

### Phase 5 — Error Handling and Recovery (Weeks 9–10)

**Goal:** The driver never presents hung I/Os to the storage stack. Any hardware fault is contained and recovered without requiring a driver reload.

#### Task 5.1 — I/O Timeout Watchdog

Add an `IOTimerDispatchSource` that fires every 5 seconds. The watchdog scans `fCmdCtx[]` for in-flight commands older than `MPT3_IO_TIMEOUT_MS` (30 seconds):

```cpp
void LSI9300Driver::TimeoutWatchdog(IOTimerDispatchSource *source, uint64_t ts)
{
    for (uint16_t smid = 1; smid <= kNumRequestFrames; smid++) {
        auto &ctx = fCmdCtx[smid - 1];
        if (!ctx.inUse) continue;
        uint64_t age = ts - ctx.submitTimestamp;
        if (age > (MPT3_IO_TIMEOUT_MS * NSEC_PER_MSEC)) {
            os_log_error(OS_LOG_DEFAULT,
                "[LSI9300] I/O timeout: SMID=%u target=%llu age=%llu ms",
                smid, ctx.targetID, age / NSEC_PER_MSEC);
            AbortSMID(smid);
        }
    }
    source->WakeAtTime(kIOTimerClockMonotonicRaw,
                       ts + 5ULL * NSEC_PER_SEC, 0);
}
```

Add `uint64_t submitTimestamp` to `MPT3CommandContext`.

#### Task 5.2 — IOC Fault Detection and Recovery

Add polling for `MPI3_IOC_STATE_FAULT` in the interrupt handler. On fault:

1. Drain the reply post queue one final time to complete any outstanding I/Os with error.
2. Call `FailAllPendingIO()` to complete remaining in-flight commands with `kSCSISenseKey_HARDWARE_ERROR`.
3. Invoke `RecoverIOC()`:
   - `SoftReset()`
   - Re-run full init sequence (IOCFacts → AllocateDMAPools → IOCInit → EventNotification → PortEnable)
4. Re-register all previously known device handles.
5. Resume accepting new I/Os.

The recovery sequence must complete within 30 seconds to avoid storage-stack timeouts.

#### Task 5.3 — Task Management Completion Path

The existing `UserAbortTaskRequest()` and `UserAbortTaskSetRequest()` submit TMF descriptors but do not wait for or process the `MPT3SCTMReply`. Add a deferred completion mechanism:

```cpp
// Tag TMF SMIDs in fCmdCtx with a special flag
ctx.isTMF = true;
ctx.tmfCompletion = completionAction;

// In CompleteScsiIO(), if ctx.isTMF:
//   Check MPT3SCTMReply.ResponseCode
//   Complete the abort action with kSCSITaskStatus_FUNCTION_COMPLETE
```

#### Task 5.4 — Expander Loss Handling

When `MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST` reports an expander is gone:
- Abort all in-flight I/Os to devices behind that expander.
- Remove their handles from the topology manager.
- The storage stack will retry at its layer.

#### Phase 5 Acceptance Criteria

- [ ] Unplug Thunderbolt enclosure mid-I/O: all in-flight I/Os complete with I/O error within 30 seconds; no kernel panic.
- [ ] Re-plug enclosure: driver recovers automatically; drives remount.
- [ ] Inject a synthetic IOC FAULT via diagnostic register: recovery completes, I/Os resume.
- [ ] `leaks` / `AddressSanitizer` show no memory leaks in the fault recovery path.

---

### Phase 6 — SMP Passthrough and Expander Management (Weeks 11–12)

**Goal:** SAS expanders are enumerable and configurable via standard SMP commands. `sg_ses` and `lsiutil` work against expander-attached backplanes.

#### Task 6.1 — SMP Target Discovery

During device enumeration (Phase 2), check `MPT3SASDevicePage0.DeviceInfo & MPI3_SAS_DEVICE_INFO_IS_EXPANDER`. For expander devices, do not register a SCSI target; instead, add them to an `fExpanderTable[]`.

#### Task 6.2 — `MPI3_FUNCTION_SCSI_ENCLOSURE_PROCESSOR`

Implement `SendSEPRequest()` in `MPT3IOCManager`:

```cpp
kern_return_t MPT3IOCManager::SendSEPRequest(
    uint16_t  devHandle,
    uint8_t  *smpCDB,
    uint32_t  smpCDBLength,
    uint8_t  *responseBuffer,
    uint32_t  responseBufferLen);
```

The SEP (SCSI Enclosure Processor) function wraps an SMP request frame, delivers it to the target expander, and returns the SMP response.

#### Task 6.3 — IOUserClient SMP Passthrough (Preview)

Add a minimal `IOUserClient` (see Phase 8 for full diagnostics) with a single external method `kSMPPassthrough` that allows user-space `lsiutil`-style tools to issue SMP commands.

#### Phase 6 Acceptance Criteria

- [ ] `sg_ses -d /dev/sg0` against a backplane with SAS expander shows enclosure status pages.
- [ ] `sg_map26 -i` lists expander SAS addresses.

---

### Phase 7 — SATA Power Management and SMART (Weeks 13–14)

**Goal:** SATA drives behind the HBA enter SLUMBER state when idle; SMART data is accessible via `smartmontools`.

#### Task 7.1 — SATA DIPM (Device-Initiated Power Management)

After SATA device discovery, send a `MPI3_FUNCTION_SATA_PASSTHROUGH` message with ATA command `SET FEATURES` (subcommand `0x03 = Enable DIPM`):

```cpp
kern_return_t MPT3IOCManager::EnableSATADIPM(uint16_t devHandle)
{
    // Build ATA PASS-THROUGH (16) CDB
    // ATA command: 0xEF (SET FEATURES), feature 0x03 (DIPM enable)
    // ...
}
```

Call `EnableSATADIPM()` for every SATA device identified during topology enumeration.

#### Task 7.2 — ATA PASS-THROUGH CDB Translation (SMART)

In `UserProcessParallelTask()`, detect SCSI CDB `0xA1` (ATA PASS-THROUGH 12) and `0x85` (ATA PASS-THROUGH 16). Instead of submitting as a generic SCSI IO, translate to `MPI3_FUNCTION_SATA_PASSTHROUGH` and submit via the high-priority path.

This enables `smartmontools --device=sat` to read SMART attributes from SATA drives:

```
smartctl -a /dev/disk4 -d sat
```

#### Task 7.3 — Partial/Slumber Policy

Implement a 10-second idle timer per SATA device. After 10 seconds of no I/O, send `SET FEATURES 0x05` (HIPM partial) or `0x06` (HIPM slumber) based on drive capability flags.

#### Phase 7 Acceptance Criteria

- [ ] `smartctl -a /dev/disk4 -d sat` returns valid SMART attributes for each SATA drive.
- [ ] After 15 seconds idle: `ioreg -l` shows SATA power state "Slumber".
- [ ] Power draw reduction ≥ 300 mW per SATA drive at idle (measured via `powermetrics`).

---

### Phase 8 — IOUserClient Diagnostics Interface (Weeks 15–16)

**Goal:** A user-space tool can query driver state, firmware version, device map, SMID utilisation, and trigger diagnostic operations without requiring a driver reload.

#### Task 8.1 — `LSI9300DiagClient` IOUserClient Subclass

Create `LSI9300Driver/LSI9300DiagClient.h` and `LSI9300DiagClient.cpp`:

```cpp
class LSI9300DiagClient final : public IOUserClient {
    OSDeclareDefaultStructors(LSI9300DiagClient);
public:
    virtual kern_return_t ExternalMethod(uint64_t selector,
                                         IOUserClientMethodArguments *args,
                                         const IOUserClientMethodDispatch *dispatch,
                                         OSObject *target,
                                         void *reference) override;
};
```

External method selectors:

| Selector | Function |
|----------|----------|
| `0` — `kGetFirmwareVersion` | Returns `MPT3IOCFactsReply.FWVersion` as 32-bit value |
| `1` — `kGetDeviceTable` | Returns the full topology table (handle, SAS addr, type per target) |
| `2` — `kGetSMIDStats` | Returns: total SMIDs, free SMIDs, high-water mark |
| `3` — `kGetReplyQueueStats` | Returns per-queue interrupt counts and overflow flags |
| `4` — `kTriggerFirmwareDump` | Asserts HOLD_IOC_RESET, reads coredump from SRAM |
| `5` — `kResetController` | Initiates soft-reset + re-init sequence |

#### Task 8.2 — `lsi9300ctl` Command-Line Tool

Create `Tools/lsi9300ctl/` — a minimal Swift command-line tool that opens the IOUserClient and exposes the above methods:

```
$ lsi9300ctl --version
LSI 9300-8i firmware: 24.00.00.00, FW build: 2023-12-01

$ lsi9300ctl --devices
TARGET  HANDLE  TYPE    SAS ADDRESS
0       0x000E  SAS HDD 5000CCA27B3D4001
1       0x000F  SATA    ATA     SAMSUNG
...

$ lsi9300ctl --stats
SMID pool: 487/512 free  (high-water: 497)
Queue 0: 1247332 interrupts, 0 overflows
Queue 1:  983201 interrupts, 0 overflows
```

#### Phase 8 Acceptance Criteria

- [ ] `lsi9300ctl --version` returns correct firmware version matching `lsiutil` output.
- [ ] `lsi9300ctl --devices` matches `diskutil list` device count.
- [ ] `lsi9300ctl --reset` recovers the controller and all drives remount within 30 seconds.

---

### Phase 9 — EEDP / T10 DIF Data Integrity (Weeks 17–18)

**Goal:** Drives formatted with T10 DIF Type 1 protection information (512+8 sector format) work correctly; EEDP guard errors are surfaced as SCSI check conditions.

#### Task 9.1 — DIF Type Detection

After device discovery, read `MPT3SASDevicePage0.DeviceInfo` for the `EEDP_CAPABLE` capability flag. For DIF-capable drives, read Manufacturing Page 10 to determine the protection type (DIF Type 1/2/3) and block size.

Store per-device DIF type in the topology manager:
```cpp
enum class EEDPType : uint8_t { None = 0, Type1 = 1, Type2 = 2, Type3 = 3 };
```

#### Task 9.2 — EEDP Request Fields

In `UserProcessParallelTask()`, for DIF-enabled drives:
```cpp
if (fTopology.GetEEDPType(task.fTargetID) != EEDPType::None) {
    req->EEDPFlags    = MPI3_EEDP_FLAG_CHECK_GUARD
                      | MPI3_EEDP_FLAG_CHECK_REF_TAG
                      | MPI3_EEDP_FLAG_CHECK_APP_TAG;
    req->EEDPBlockSize = static_cast<uint8_t>(
        fTopology.GetBlockSize(task.fTargetID) >> 9);
}
```

Add `MPI3_EEDP_FLAG_*` constants to `MPT3Types.h`.

#### Task 9.3 — EEDP Error Reporting

In `CompleteScsiIO()`, check `MPT3SCSIIOReply.Header.IOCStatus` for:
- `MPI3_IOCSTATUS_EEDP_GUARD_ERROR` → map to SCSI `CHECK CONDITION / ABORTED COMMAND`
- `MPI3_IOCSTATUS_EEDP_REF_TAG_ERROR` → map to `CHECK CONDITION / MISCOMPARE`
- `MPI3_IOCSTATUS_EEDP_APP_TAG_ERROR` → map to `CHECK CONDITION / MISCOMPARE`

#### Phase 9 Acceptance Criteria

- [ ] A drive formatted as DIF Type 1 passes `sg_verify --dif=1 /dev/sg0` without false EEDP errors.
- [ ] Intentionally corrupted PI data returns SCSI CHECK CONDITION with correct ASC/ASCQ.

---

### Phase 10 — Hardening, Audit, and Notarization (Weeks 19–20)

**Goal:** The driver passes Apple's notarization pipeline, AppSandbox entitlement review, and is ready for distribution.

#### Task 10.1 — Static Analysis

Run `clang-tidy` with the full `clang-analyzer-*` and `bugprone-*` check sets:
```
clang-tidy --checks='clang-analyzer-*,bugprone-*,cert-*,performance-*' \
    LSI9300Driver/*.cpp -- <compile flags>
```

Fix all `clang-analyzer-security.*` findings. Accept `bugprone-macro-parentheses` with `NOLINT` only after manual review.

#### Task 10.2 — AddressSanitizer + UBSanitizer

The unit tests already compile without DriverKit SDK. Build them with:
```
make tests SANITIZE=address,undefined
```
Fix all ASAN/UBSAN findings.

#### Task 10.3 — Kernel Panic (KDP) Regression Suite

Create `Tests/PanicRegressionTests.sh` — a shell script that:
1. Loads the driver.
2. Runs `fio` stress tests in parallel.
3. Simulates hot-plug via `systemextensionsctl` deactivate/activate.
4. Simulates IOC fault via diagnostic register.
5. Verifies no panics in `/var/log/panic.log`.

#### Task 10.4 — Code Signing and Notarization

Update `Makefile` `sign` target:
```makefile
sign: all
	codesign --sign "Developer ID Application: $(TEAM)" \
	         --entitlements LSI9300Driver/LSI9300Driver.entitlements \
	         --options runtime \
	         --timestamp \
	         --deep \
	         build/LSI9300Driver.dext

notarize: sign
	xcrun notarytool submit build/LSI9300Driver.dext.zip \
	    --apple-id $(APPLE_ID) \
	    --team-id $(TEAM) \
	    --password $(APP_SPECIFIC_PW) \
	    --wait
```

#### Task 10.5 — Documentation and Change Log

Update `README.md`:
- macOS 26.3 installation instructions (new system extension approval UX).
- Multi-queue performance tuning guide.
- Thunderbolt 5 vs Thunderbolt 4 enclosure notes.

Update `docs/architecture.md` with Phase 3 (multi-queue) and Phase 9 (EEDP) architectural notes.

Create `CHANGELOG.md` with structured version history.

#### Phase 10 Acceptance Criteria

- [ ] Zero `clang-tidy` findings at warning level.
- [ ] Zero ASAN/UBSAN findings in unit tests.
- [ ] Notarization pipeline returns `Accepted` status.
- [ ] `spctl --assess --type exec build/LSI9300Driver.dext` returns `accepted`.
- [ ] Full documentation review complete.

---

## 5. Technical Architecture

### 5.1 Updated Component Diagram (Post Phase 3)

```
┌─────────────────────────────────────────────────────────────────┐
│  macOS Storage Stack                                             │
│  IOSCSIParallelInterfaceController (kernel extension framework)  │
└─────────────────┬───────────────────────────────────────────────┘
                  │ SCSIUserParallelTask (multi-segment DMA)
┌─────────────────▼───────────────────────────────────────────────┐
│  LSI9300Driver.dext  (user-space DriverKit extension)           │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  LSI9300Driver                                           │   │
│  │  • UserProcessParallelTask() — builds MPT3 request       │   │
│  │  • SMID free-list (atomic ring buffer)                   │   │
│  │  • Per-queue interrupt handlers (0..N-1)                 │   │
│  │  • Timeout watchdog (IOTimerDispatchSource, 5 s)         │   │
│  └──────┬──────────────────────────────┬────────────────────┘   │
│         │                              │                         │
│  ┌──────▼──────────┐        ┌──────────▼──────────────────────┐ │
│  │ MPT3TopologyMgr │        │ MPT3IOCManager                  │ │
│  │ • Handle table  │        │ • SoftReset                     │ │
│  │ • Hot-plug evts │        │ • DoorbellHandshake             │ │
│  │ • EEDP type map │        │ • SendIOCInit (multi-queue)     │ │
│  └─────────────────┘        │ • SendPortEnable                │ │
│                              │ • ReadSASDevicePage0            │ │
│  ┌──────────────────┐        │ • SendSEPRequest (SMP)         │ │
│  │ LSI9300DiagClient│        │ • EnableSATADIPM                │ │
│  │ IOUserClient     │        └─────────────────────────────────┘ │
│  │ lsi9300ctl       │                                            │
│  └──────────────────┘                                            │
└─────────────────────────────────────────────────────────────────┘
          │  BAR1 MMIO (via Thunderbolt-PCIe bridge)
┌─────────▼───────────────────────────────────────────────────────┐
│  LSI SAS3008 Firmware (IT mode)                                  │
│  • 16 MSI-X vectors, 16 reply queues                             │
│  • SAS/SATA topology discovery via PortEnable                   │
│  • Config pages (SASDevicePage0, ManufacturingPage0, ...)       │
└─────────────────────────────────────────────────────────────────┘
          │  SAS/SATA PHY
     ┌────┴────────────────────────────────────────────┐
     │  Direct-attach + expander topology              │
     │  SAS HDDs, SATA SSDs, SAS SSDs, expanders       │
     └─────────────────────────────────────────────────┘
```

### 5.2 DMA Pool Layout (Post Phase 4)

| Pool | Count | Element Size | Total | Notes |
|------|-------|-------------|-------|-------|
| Request frames | 512 | 128 B | 64 KiB | Shared SMID pool |
| Reply frames | 512 | 128 B | 64 KiB | Pre-populated in free queue |
| Sense buffers | 512 | 252 B | 126 KiB | One per SMID |
| Reply free queue | 512 | 4 B | 2 KiB | `uint32_t[]` of reply frame PAs |
| Reply post queues | 16 × 512 | 8 B | 64 KiB | One per MSI-X vector |
| Chain buffers | 512 | 1,488 B | 744 KiB | `(128 - 4) × 12` bytes each |
| **Total** | | | **~1.1 MiB** | |

### 5.3 Thread Safety Model (Post Phase 3)

| Resource | Concurrency | Protection |
|----------|-------------|-----------|
| SMID free-list head/tail | Multi-queue interrupt threads | `std::atomic<uint32_t>` CAS |
| `fCmdCtx[]` | Submit (dispatch queue) + complete (IRQ thread) | `os_unfair_lock` per slot |
| Topology manager | Discovery callback + I/O submit | `os_unfair_lock` on full table |
| Per-reply-queue consumer index | One IRQ thread per queue | No lock (single writer) |
| BAR1 MMIO writes | Multi-queue IRQ threads | Serialized via IOC hardware FIFO |

### 5.4 Initialization Sequence (macOS 26.3)

```
Start(provider)
  └─► Open PCI device (kIOPCIAccessSurface_MM)                [SDK 25 API]
      └─► CreateInterruptSources(N vectors)                   [SDK 25 API]
          └─► UserInitializeController()
              ├─► 100 ms Thunderbolt stabilisation delay
              ├─► MaskAllInterrupts()
              ├─► SoftReset() → READY state
              ├─► IOCFacts doorbell handshake
              ├─► AllocateDMAPools() [kIOMemoryMapperNone]     [SDK 25 flag]
              ├─► FillReplyFreeQueue()
              ├─► SendIOCInit(N queues, N MSI-X vectors)
              └─► └─► IOC → OPERATIONAL
                  └─► UserStartController()
                      ├─► Bind N interrupt handlers
                      ├─► UnmaskReplyInterrupt(all queues)
                      ├─► SendEventNotificationEnable()
                      ├─► SendPortEnable()
                      └─► Start timeout watchdog
```

---

## 6. Testing Strategy

### 6.1 Unit Tests (no hardware required)

Location: `Tests/`

| Test File | Coverage |
|-----------|---------|
| `MPT3ProtocolTests.cpp` (existing) | Struct sizes, SGL flags, ring arithmetic |
| `MPT3ReplyQueueTests.cpp` (existing) | Reply queue simulation |
| `MPT3TopologyTests.cpp` (Phase 2) | Handle table, hot-plug event simulation |
| `SGLChainTests.cpp` (Phase 4) | Multi-segment SGL construction |
| `SMIDAtomicTests.cpp` (Phase 3) | Concurrent SMID allocation under TSan |
| `EEDPFlagTests.cpp` (Phase 9) | DIF type detection, EEDP flag encoding |

Run: `make tests SANITIZE=address,undefined`

### 6.2 Hardware-in-the-Loop Tests

**Minimum test rig:**
- Apple Silicon Mac (M2 or later) with Thunderbolt 3/4/5 port
- Thunderbolt-to-PCIe enclosure (e.g., Sonnet Echo, OWC Helios 3)
- LSI 9300-8i card in IT mode (firmware ≥ 16.00.11.00)
- At least 4 × SATA SSDs and 2 × SAS HDDs
- One SAS expander backplane (for Phase 6)

**Test tiers:**

| Tier | Description | Gate |
|------|-------------|------|
| T1 — Smoke | Driver loads, drives appear in Disk Utility | Every commit |
| T2 — I/O correctness | `dd` read/write checksum verification, iozone | Phase boundary |
| T3 — Performance | fio 8-drive benchmark vs. baseline | Phase 3 |
| T4 — Fault injection | Cable pull, IOC fault, Thunderbolt disconnect | Phase 5 |
| T5 — Long-soak | 72-hour fio stress test | Pre-release |

### 6.3 Continuous Integration

Update `.github/workflows/ci.yml` (or equivalent) to:

1. Run unit tests on Linux (no SDK required) on every push.
2. Build the dext on a macOS 26 runner on PRs to `main`.
3. Run static analysis (`clang-tidy`) on every PR.
4. Report ASAN/UBSAN results as PR checks.

---

## 7. Build and Deployment

### 7.1 Updated Makefile Targets

| Target | Description |
|--------|-------------|
| `make all` | Build unsigned `.dext` for current SDK |
| `make all DKTARGET=25.0` | Force SDK 25 target |
| `make tests` | Build and run unit tests |
| `make tests SANITIZE=address,undefined` | Run with ASan/UBSan |
| `make sign TEAM=<ID>` | Code-sign with hardened runtime |
| `make notarize TEAM=<ID> APPLE_ID=x PW=y` | Submit for notarization |
| `make install` | `systemextensionsctl install` (Reduced Security required) |
| `make lint` | Full `clang-tidy` run |
| `make clean` | Remove build artifacts |

### 7.2 macOS 26.3 Installation Requirements

macOS 26 changes the System Extension activation flow for DriverKit extensions that access PCI hardware:

1. **Reduced Security mode required** (same as macOS 12–15):
   - Boot into Recovery OS
   - `csrutil enable --without kext`
   - For Apple Silicon: additionally lower to **Reduced Security** in Startup Security Utility

2. **System Extension approval**:
   - On macOS 26, extensions from non-App-Store sources require one-time user approval in **Settings → Privacy & Security → Developer Tools**.
   - Headless (CI) approval requires `systemextensionsctl install --approval-token <token>` where `<token>` is obtained from `systemextensionsctl generate-approval-token`.

3. **Entitlement provisioning profile**:
   - A valid Apple Developer Program membership is required.
   - Provisioning profile must include the two PCI transport entitlements.
   - Profile is embedded in the `.dext` bundle before signing.

### 7.3 Firmware Requirements

The LSI 9300 must be running **IT mode firmware** (P20 or later recommended):

| Card | IT Firmware Package |
|------|-------------------|
| 9300-8i (SAS3008) | `SAS9300_8i_IT.bin` (P20.00.07.00 or later) |
| 9300-4i (SAS3004) | `SAS9300_4i_IT.bin` |
| 9300-8e (SAS3108) | `SAS9300_8e_IT.bin` |

Flash via `sas3flash` under Linux or EFI shell before macOS installation.

---

## 8. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| Apple changes DriverKit ABI before macOS 26.3 GA | Medium | High | Track Apple developer betas; add SDK version guards |
| IOCFacts `RequestCredit` < 512 on some firmware versions | Low | Medium | Clamp `kNumRequestFrames = min(512, IOCFacts.RequestCredit)` |
| Thunderbolt firmware on M4 Ultra reorders PCIe config-space visibility | Low | High | Add configurable stabilisation delay (default 100 ms, max 500 ms) |
| Apple revokes PCI memory-mapped entitlement for non-App-Store distribution | Very Low | Critical | Track Apple developer forums; engage DTS |
| SAS3008 hardware EOL / Broadcom drops IT mode firmware support | Low | Medium | Pin to known-good firmware version; ship in-bundle |
| macOS 26 introduces new IOMMU restrictions that break DMA addressing | Medium | High | Test on each beta seed; file rdar if broken |
| Multi-queue SMID CAS loop under extreme contention (all 512 SMIDs in flight) | Low | Medium | Add exponential backoff; log stall warnings |

---

## 9. Timeline Summary

| Week | Phase | Milestone |
|------|-------|-----------|
| 1–2 | Phase 1 | Compiles cleanly on SDK 25; driver loads on macOS 26.3 |
| 3–4 | Phase 2 | All drives enumerated; hot-plug/remove works |
| 5–7 | Phase 3 | 16-queue MSI-X; 8-drive fio benchmark passes |
| 8   | Phase 4 | Chain SGL; 1 GB/s sequential on single drive |
| 9–10 | Phase 5 | Zero hung I/Os under fault injection |
| 11–12 | Phase 6 | Expander management; `sg_ses` works |
| 13–14 | Phase 7 | SMART via `smartmontools`; DIPM idle |
| 15–16 | Phase 8 | `lsi9300ctl` tool ships |
| 17–18 | Phase 9 | T10 DIF Type 1 verified |
| 19–20 | Phase 10 | Notarized; fully documented; tagged v1.0.0 |

---

## Appendix A — macOS 26 DriverKit Δ Checklist

Use this checklist to verify every SDK 25 breaking change is addressed before submitting a Phase 1 PR:

- [ ] `OSSynchronizeIO()` → `IOMemoryBarrier(kIOMemoryBarrierFlagDevice)` (all occurrences)
- [ ] `IOPCIDevice::Open()` → add `kIOPCIAccessSurface_MM` flag
- [ ] `IOPCIDevice::ConfigurationRead16/32` → `ReadConfigSpace16/32`
- [ ] `IOPCIDevice::CreateInterruptDispatchSource` → `CreateInterruptSources` (batch)
- [ ] `IOBufferMemoryDescriptor::CreateWithOptions` → add `kIOMemoryMapperNone`
- [ ] `IODispatchQueue::Create` → add `kIODispatchQueueHighPriority` for reply queue
- [ ] `OSAction::Create` → verify no retain cycles
- [ ] `ParallelTaskCompletion serviceResponse` → enum class type
- [ ] `UserGetDMASpecification` → migrate to `UserReportHBAConstraints` keys
- [ ] `kIOPropertySCSIParallelSignalProtocol` key → add to constraints dictionary
- [ ] Entitlement: `com.apple.developer.driverkit.transport.pci.memory-mapped` → add
- [ ] `Info.plist` min OS → update to `26.3`
- [ ] `codesign` → add `--options runtime`
- [ ] `systemextensionsctl install` → update for approval-token API

---

## Appendix B — File-Level Change Inventory

| File | Phase | Change Type | Description |
|------|-------|------------|-------------|
| `Makefile` | 1 | Modify | SDK version detection, SDK 25 target, sanitizer support |
| `LSI9300Driver/LSI9300Driver.h` | 1,3,5,8 | Modify | Multi-queue members, watchdog timer, DiagClient forward decl |
| `LSI9300Driver/LSI9300Driver.cpp` | 1–10 | Modify | All phase changes to core driver |
| `LSI9300Driver/MPT3Registers.h` | 1,9 | Modify | `LSI_MMIO_BARRIER` macro, EEDP flag constants |
| `LSI9300Driver/MPT3Types.h` | 3,9 | Modify | `MPT3IOCInitRequestExtended`, EEDP flag constants |
| `LSI9300Driver/MPT3IOC.h` | 2,6,7 | Modify | `ReadSASDevicePage0`, `SendSEPRequest`, `EnableSATADIPM` |
| `LSI9300Driver/MPT3IOC.cpp` | 2,6,7 | Modify | Implementation of above |
| `LSI9300Driver/MPT3TopologyManager.h` | 2 | **New** | Device handle table declaration |
| `LSI9300Driver/MPT3TopologyManager.cpp` | 2 | **New** | Device handle table implementation |
| `LSI9300Driver/LSI9300DiagClient.h` | 8 | **New** | IOUserClient subclass declaration |
| `LSI9300Driver/LSI9300DiagClient.cpp` | 8 | **New** | IOUserClient subclass implementation |
| `LSI9300Driver/Info.plist` | 1,8 | Modify | Min OS version, IOUserClient personality |
| `LSI9300Driver/LSI9300Driver.entitlements` | 1 | Modify | Add MMIO entitlement |
| `Tools/lsi9300ctl/` | 8 | **New** | Swift command-line diagnostic tool |
| `Tests/MPT3TopologyTests.cpp` | 2 | **New** | Topology manager unit tests |
| `Tests/SGLChainTests.cpp` | 4 | **New** | Chain SGL unit tests |
| `Tests/SMIDAtomicTests.cpp` | 3 | **New** | Concurrent SMID allocation tests |
| `Tests/EEDPFlagTests.cpp` | 9 | **New** | EEDP flag encoding tests |
| `Tests/PanicRegressionTests.sh` | 10 | **New** | Kernel panic regression script |
| `docs/architecture.md` | 3,9 | Modify | Multi-queue and EEDP sections |
| `docs/register_map.md` | 3 | Modify | Multi-queue reply queue register layout |
| `CHANGELOG.md` | 10 | **New** | Structured version history |

---

*End of Implementation Plan*
