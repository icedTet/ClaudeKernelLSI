# SAS3008 BAR1 Register Map

All offsets are relative to the start of BAR1 (the 64-bit MMIO region).
All registers are 32 bits wide. Multi-byte fields are little-endian.

> **Source:** Broadcom SAS3008 Fusion-MPT SAS-3 Technical Reference Manual.

---

## System Interface Registers (offset 0x000–0x0FF)

### 0x0000 — IOC State / Doorbell Register

**Read:** Returns the current IOC state machine value.

| Bits | Field | Description |
|------|-------|-------------|
| 31:28 | `IOC_STATE` | 0x0=RESET, 0x1=READY, 0x2=OPERATIONAL, 0x4=FAULT, 0x5=COREDUMP |
| 27:0  | `DOORBELL_DATA` | State-dependent data (reply length, fault code, etc.) |

**Write:** Initiates a doorbell function.

| Bits | Field | Description |
|------|-------|-------------|
| 31:28 | `FUNCTION` | Doorbell function code (e.g. 0x4=IOC_MSG_UNIT_RESET) |
| 27:24 | `ADD_DWORDS` | Number of additional DWORDs to send |
| 23:0  | `DATA` | First DWORD payload (e.g. function code for IOCFacts) |

---

### 0x0004 — Write Sequence Register

Write the following bytes in sequence to unlock the diagnostic register.
Writing any other value or the flush value (0x00) re-locks.

| Write order | Value |
|-------------|-------|
| Flush (lock) | `0x00` |
| 1st key | `0xF6` |
| 2nd key | `0x2E` |
| 3rd key | `0x86` |
| 4th key | `0xCE` |
| 5th key | `0x34` |
| 6th key | `0x75` |

---

### 0x0030 — Host Interrupt Status Register

Read to check pending interrupt sources. Write a bit to acknowledge/clear it.

| Bit | Name | Description |
|-----|------|-------------|
| 31 | `IOC2SYS_DB_STATUS` | IOC wrote a doorbell reply; read doorbell to get data |
| 30 | `RESET_IRQ_STATUS` | IOC-initiated reset (FW fault recovery) |
| 3  | `REPLY_DESCRIPTOR_INT` | One or more reply descriptors are pending in the post queue |
| 0  | `SYS2IOC_DB_STATUS` | Host wrote doorbell; IOC has read it (cleared by IOC) |

---

### 0x0034 — Host Interrupt Mask Register

Set a bit to **mask** (suppress) the corresponding interrupt source.
All bits set = all interrupts masked.

| Bit | Name |
|-----|------|
| 31 | Mask doorbell interrupt |
| 30 | Mask reset IRQ |
| 3  | Mask reply descriptor interrupt |

---

### 0x0040 — Host Diagnostic Register

Requires the write-sequence register to be unlocked first.

| Bit | Name | Description |
|-----|------|-------------|
| 10 | `RESET_HISTORY` | Set by firmware on reset; clear to acknowledge |
| 7  | `DIAG_WRITE_ENABLE` | Read: 1 = diagnostic write lock is open |
| 1  | `HOLD_IOC_RESET` | Write 1 = assert reset; write 0 = release reset |

---

### 0x0054 — Write Sequence Register (alias)

Duplicate of offset 0x0004 — some revisions of the register map use this
address; writing to either address has the same effect.

---

## Reply / Request FIFO Registers (offset 0x0C0–0x0CF)

### 0x00C0 — Reply Free Host Index Register

**Write:** Advance the reply free queue producer index.

The driver writes this register after returning a reply frame to the free queue,
telling the IOC where the new last-valid entry is.

| Bits | Description |
|------|-------------|
| 15:0 | New producer index into the reply free queue ring |

---

### 0x00C4 — Reply Post Host Index Register

**Write:** Advance the reply post queue consumer index.

The driver writes this register after draining reply descriptors, telling the
IOC that the consumed slots can be overwritten.

| Bits | Description |
|------|-------------|
| 15:0 | New consumer index into the reply post queue ring |

---

### 0x00C8 — Request Descriptor Post Low Register

**Write (low 32 bits of an 8-byte request descriptor).**

Always write this register before the high register.

| Bits | Field | Description |
|------|-------|-------------|
| 31:16 | `SMID` | System Message ID (1-based slot into request frame pool) |
| 15:8  | `MSIxIndex` | Reply queue / MSI-X vector to use for the completion |
| 7:0   | `DescriptorType` | 0x01=SCSI_IO, 0x06=HIGH_PRIORITY, 0x08=MPI3 |

---

### 0x00CC — Request Descriptor Post High Register

**Write (high 32 bits of an 8-byte request descriptor).**

Must be written after the low register. Writing this register triggers the IOC
to fetch and process the new request.

| Bits | Field | Description |
|------|-------|-------------|
| 31:16 | `DevHandle` | Target device handle (from SAS topology discovery) |
| 15:0  | (function-dependent) | Additional descriptor fields |

---

## PCI Configuration Space (standard)

| Offset | Register | Driver use |
|--------|----------|------------|
| 0x00 | Vendor ID = 0x1000 | Device matching |
| 0x02 | Device ID (0x0097, 0x0086, 0x005D) | Device matching |
| 0x04 | Command register | Enable Bus Master (bit 2) + Memory Space (bit 1) |
| 0x10–0x17 | BAR0 (32-bit I/O, unused) | — |
| 0x14–0x1B | BAR1 (64-bit MMIO) | System interface registers |
| 0x2C | Subsystem Vendor ID | Logging only |
| 0x2E | Subsystem ID | Logging only |

---

## IOC State Transition Diagram

```
         ┌──────────────────────────────────────────┐
         │                                          │
  Power-on /                                   Fault / FW crash
  PCI reset                                         │
         │                                          ▼
         ▼                              ┌────────────────────┐
    ┌─────────┐   Diagnostic reset      │       FAULT        │
    │  RESET  │──────────────────────► │  (read fault code) │
    └────┬────┘                         └────────────────────┘
         │ FW loads                              │ Soft reset
         ▼                                       │
    ┌─────────┐ ◄─────────────────────────────────┘
    │  READY  │ (accepts IOCFacts + IOCInit)
    └────┬────┘
         │ IOCInit ACK
         ▼
  ┌──────────────┐
  │ OPERATIONAL  │ (accepts SCSI IO requests)
  └──────────────┘
```
