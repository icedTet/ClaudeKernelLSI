# LSI9300Driver

A macOS **DriverKit** extension (dext) for **LSI 9300-series SAS/SATA host bus adapters** (HBAs) in **IT (passthrough) mode**, targeting **Apple Silicon** (M1 / M2 / M3 / M4) Macs running **macOS 12 Monterey or later**.

---

## Overview

The LSI 9300 family (SAS3008, SAS3004, SAS3108) is a popular line of Broadcom/LSI SAS3 12 Gb/s HBAs widely used in NAS builds, JBOD enclosures, and direct-attach storage. When flashed to **IT mode** firmware the card acts as a pure HBA — presenting every connected SAS/SATA drive directly to the operating system without RAID abstraction.

Apple Silicon Macs do not have internal PCIe slots, but the LSI 9300 can be connected via a **Thunderbolt-to-PCIe enclosure** (e.g., OWC Mercury Helios, Sonnet Echo, Akitio Node). This driver binds to the card in that enclosure and exposes the attached drives to macOS as standard SCSI block devices.

---

## Supported Hardware

| Card model | PCI Device ID | Notes |
|------------|---------------|-------|
| LSI 9300-8i | `0x0097` (SAS3008) | 8 internal SAS/SATA ports |
| LSI 9300-4i | `0x0086` (SAS3004) | 4 internal SAS/SATA ports |
| LSI 9300-8e | `0x005D` (SAS3108) | 8 external SAS ports |

> **IT mode firmware is required.** IR (Integrated RAID) firmware is not supported and the driver will refuse to bind. Reflash with the `HBA_9300_8i_IT` firmware image from Broadcom's website before loading this driver.

---

## Architecture

```
macOS Storage Stack
      │
      │  IOSCSIParallelCommand
      ▼
┌─────────────────────────────────────────────────┐
│  IOUserSCSIParallelInterfaceController (kernel) │
│        (bridge proxy — part of macOS)           │
└──────────────────┬──────────────────────────────┘
                   │ DriverKit IPC
                   ▼
┌─────────────────────────────────────────────────┐
│         LSI9300Driver  (this dext)              │
│                                                 │
│  ┌─────────────┐   ┌────────────────────────┐  │
│  │  MPT3IOC    │   │  Reply Post Queue Ring │  │
│  │ (init / SM) │   │  (interrupt-driven)    │  │
│  └──────┬──────┘   └────────────┬───────────┘  │
│         │ Doorbell               │ MSI-X         │
│         │ handshake              │               │
│         ▼                        ▼               │
│  ┌──────────────────────────────────────────┐   │
│  │          BAR1 MMIO  (IOPCIDevice)        │   │
│  └──────────────────────┬───────────────────┘   │
└─────────────────────────┼───────────────────────┘
                          │ PCIe (Thunderbolt enclosure)
                          ▼
              ┌───────────────────────┐
              │  LSI SAS3008 HBA      │
              │  (IT mode firmware)   │
              └─────┬────────────────┘
                    │ SAS / SATA cables
              ┌─────▼────────────────┐
              │  SAS/SATA drives     │
              └──────────────────────┘
```

### Key components

| File | Purpose |
|------|---------|
| `LSI9300Driver.h/cpp` | Main driver class — `IOUserSCSIParallelInterfaceController` subclass |
| `MPT3Registers.h` | BAR1 MMIO register map and constants for SAS3008 |
| `MPT3Types.h` | Wire-format structures for the MPT3SAS (MPI 2.x) protocol |
| `MPT3IOC.h/cpp` | IOC initialization state machine (reset → READY → OPERATIONAL) |

---

## Build Requirements

| Requirement | Minimum version |
|-------------|-----------------|
| macOS | 12.0 (Monterey) |
| Xcode | 14.0 |
| DriverKit SDK | 21.0 (bundled with Xcode 14) |
| Apple Developer Program | Active membership required |
| Entitlements | `com.apple.developer.driverkit` + `com.apple.developer.driverkit.transport.pci` + `com.apple.developer.driverkit.family.scsi-controller` |

> Unit tests can be compiled and run on **Linux** (e.g., CI) without any Apple SDK.

---

## Building

### Unit tests (no macOS SDK required)

```bash
make tests
```

This compiles and runs `Tests/MPT3ProtocolTests.cpp` and `Tests/MPT3ReplyQueueTests.cpp` using only standard C++17 and the stub headers in `Tests/TestStubs.h`.

### Driver extension (macOS + Xcode required)

```bash
make            # assemble the .dext bundle (unsigned)
make sign TEAM=A1B2C3D4E5   # code-sign with your Developer ID
```

Replace `A1B2C3D4E5` with your 10-character Apple Developer Team ID.

### Loading the driver

1. Ensure your Mac boots with **Reduced Security** (Apple Silicon Security Policy).
   Open System Settings → Privacy & Security → Security → "Reduced Security".
2. Install the system extension:
   ```bash
   make install   # runs: systemextensionsctl install build/LSI9300Driver.dext
   ```
3. Approve the extension in **System Settings → Privacy & Security** when prompted.
4. Plug in your Thunderbolt enclosure containing the LSI 9300. The driver loads
   automatically and attached drives appear as `disk*` devices.

---

## Debugging

### Stream driver logs in real time

```bash
log stream \
  --level debug \
  --predicate 'subsystem == "com.claudekernellsi.driver.LSI9300Driver"'
```

### Check system extension status

```bash
systemextensionsctl list
```

### Verify PCI device visibility

```bash
system_profiler SPPCIDataType | grep -A10 "SAS3008"
```

### IORegistry dump

```bash
ioreg -l -w0 | grep -A30 "LSI9300Driver"
```

---

## Protocol Notes

The SAS3008 implements **MPT3SAS (MPI 2.x)** — Broadcom/LSI's "Message Passing Technology" bus protocol. Key protocol concepts used by this driver:

| Concept | Description |
|---------|-------------|
| **SMID** | System Message ID — a 1-based index into the request frame pool. Each in-flight command has a unique SMID |
| **Request frame pool** | DMA-coherent buffer of 128-byte frames, one per SMID slot |
| **Reply post queue** | Ring of 8-byte reply descriptors written by the IOC; driver drains on MSI-X interrupt |
| **Reply free queue** | Ring of 8-byte physical addresses pointing to pre-allocated reply frames; driver replenishes after consuming |
| **Doorbell register** | Legacy synchronous message path; used only during initialisation (IOCFacts, IOCInit) |
| **IOCFacts** | First command issued; IOC reports max credits, queue depths, FW version |
| **IOCInit** | Driver configures queue DMA addresses; IOC transitions to OPERATIONAL |
| **PortEnable** | Triggers SAS/SATA topology discovery; device-added events arrive asynchronously |

---

## IT Mode vs. IR Mode

| Feature | IT mode | IR mode |
|---------|---------|---------|
| Drive visibility | Each physical drive visible directly | OS sees virtual drives / arrays |
| RAID | None (done in software) | Hardware RAID (0, 1, 10) |
| Flexibility | Full (ZFS, mdraid, macOS RAID) | Limited to firmware RAID levels |
| This driver | ✅ Supported | ❌ Not supported |

---

## Known Limitations

- **SATA port multipliers** are not supported (this is a SAS3008 hardware limitation in IT mode).
- **SMP passthrough** (for expander management) is not yet implemented.
- **Secure Boot / Full Security** prevents third-party DriverKit extensions from loading. Reduced Security policy is required.
- Only **Thunderbolt 3/4** PCIe enclosures have been tested. Older Thunderbolt 2 enclosures may work via a TB2→TB3 adapter.
- The driver currently uses a **single MSI-X vector** (reply queue 0). Multi-queue support for improved parallelism with NVMe-over-SAS targets is on the roadmap.

---

## Roadmap

- [ ] Multi-queue MSI-X (one reply queue per CPU)
- [ ] SMP passthrough for SAS expander management
- [ ] SATA power management (DIPM/SLUMBER)
- [ ] SMART data forwarding via IOKit property tables
- [ ] IOUserClient for userspace management tools
- [ ] `lsiutil`-compatible diagnostic interface

---

## References

- [Broadcom SAS3008 Product Page](https://www.broadcom.com/products/storage/host-bus-adapters/sas3008)
- [Broadcom MPI 2.5 Specification (request access from Broadcom support)](https://www.broadcom.com)
- [Apple DriverKit Documentation](https://developer.apple.com/documentation/driverkit)
- [Apple SCSIControllerDriverKit Framework](https://developer.apple.com/documentation/scsiconfigurationdriverkit)
- Linux kernel reference driver: `drivers/scsi/mpt3sas/` (GPL-2.0)

---

## License

BSD 2-Clause License. See [LICENSE](LICENSE) for details.

This project is not affiliated with or endorsed by Broadcom Inc., LSI Corporation, or Apple Inc.
