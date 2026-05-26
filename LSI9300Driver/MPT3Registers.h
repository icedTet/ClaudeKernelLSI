/**
 * MPT3Registers.h — Hardware register map for LSI SAS3008 (LSI 9300 series)
 *
 * Covers:
 *   - PCI configuration identifiers
 *   - BAR0/BAR1 system interface register offsets
 *   - Host interrupt / doorbell registers
 *   - Reply / request descriptor FIFO registers
 *   - IOC state machine values
 *   - Capability / feature flags
 *
 * Reference:
 *   Broadcom/LSI SAS3008 Fusion-MPT SAS-3 Technical Reference Manual,
 *   rev. 1.4 (document DB17-000XX).
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

/* =========================================================================
 * PCI identifiers
 * ========================================================================= */

/** Broadcom (formerly Avago / LSI Logic) vendor ID */
#define LSI_PCI_VENDOR_ID                   0x1000U

/** SAS3008 – eight internal SAS/SATA ports, used on LSI 9300-8i */
#define SAS3008_PCI_DEVICE_ID               0x0097U

/** SAS3004 – four internal SAS/SATA ports, used on LSI 9300-4i */
#define SAS3004_PCI_DEVICE_ID               0x0086U

/** SAS3108 – eight external SAS ports (9300-8e variant) */
#define SAS3108_PCI_DEVICE_ID               0x005DU

/** SAS3116 – sixteen SAS/SATA ports */
#define SAS3116_PCI_DEVICE_ID               0x00C3U

/* =========================================================================
 * BAR layout
 *   BAR0  : 32-bit I/O (legacy, avoid)
 *   BAR1  : 64-bit memory-mapped system interface registers  ← primary
 *   BAR2  : (absent on SAS3008)
 * ========================================================================= */

/** All MMIO offsets are relative to the start of BAR1. */

/* =========================================================================
 * System Interface Registers  (BAR1, offset 0x00 – 0x3FF)
 * ========================================================================= */

/** IOC state / doorbell register (host → IOC writes trigger state transitions) */
#define MPI3_SYSIF_IOC_STATE_REG            0x00000000U

/**
 * Write-only doorbell register.  The low 28 bits carry function-specific
 * data; the top 4 bits encode the function code.
 */
#define MPI3_SYSIF_DOORBELL_REG             0x00000000U   /* same address, different direction */

/** Host interrupt status register (read to check pending interrupts)  */
#define MPI3_SYSIF_HOST_INT_STATUS_REG      0x00000030U

/** Host interrupt mask register (write to mask/unmask interrupt sources) */
#define MPI3_SYSIF_HOST_INT_MASK_REG        0x00000034U

/** Host diagnostic register  */
#define MPI3_SYSIF_HOST_DIAG_REG            0x00000040U

/** Write-sequence register (used to unlock diagnostic access) */
#define MPI3_SYSIF_WRITE_SEQ_REG            0x00000054U

/* =========================================================================
 * Reply / Request Descriptor FIFO Registers  (BAR1 + 0xC0 – 0xFF)
 * ========================================================================= */

/** Write a request descriptor (64-bit, low word first) to submit a command */
#define MPI3_SYSIF_REPLY_FREE_HOST_INDEX_REG   0x000000C0U
#define MPI3_SYSIF_REPLY_POST_HOST_INDEX_REG   0x000000C4U
#define MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_LOW_REG   0x000000C8U
#define MPI3_SYSIF_REQUEST_DESCRIPTOR_POST_HIGH_REG  0x000000CCU

/* Convenience macro: write a 64-bit request descriptor */
#define MPI3_SYSIF_REPLY_FREE_INDEX_REG     0x000000C0U

/* =========================================================================
 * IOC State values  (read from bits 31:28 of MPI3_SYSIF_IOC_STATE_REG)
 * ========================================================================= */

#define MPI3_IOC_STATE_MASK                 0xF0000000U
#define MPI3_IOC_STATE_SHIFT                28U

typedef enum {
    MPI3_IOC_STATE_RESET            = 0x0U, /**< Hard-reset, firmware not running  */
    MPI3_IOC_STATE_READY            = 0x1U, /**< Firmware ready to accept IOCInit  */
    MPI3_IOC_STATE_OPERATIONAL      = 0x2U, /**< Fully operational, can process I/O*/
    MPI3_IOC_STATE_FAULT            = 0x4U, /**< Unrecoverable firmware fault      */
    MPI3_IOC_STATE_COREDUMP         = 0x5U, /**< Generating coredump               */
} MPT3IOCState;

/** Extract IOC state from the doorbell/state register value */
static inline MPT3IOCState mpt3_ioc_state(uint32_t reg_val)
{
    return (MPT3IOCState)((reg_val & MPI3_IOC_STATE_MASK) >> MPI3_IOC_STATE_SHIFT);
}

/* =========================================================================
 * Doorbell function codes  (top 4 bits of the 32-bit doorbell write)
 * ========================================================================= */

#define MPI3_DOORBELL_FUNCTION_SHIFT        28U
#define MPI3_DOORBELL_FUNCTION_MASK         0xF0000000U

#define MPI3_DOORBELL_FUNC_IOC_MESSAGE_UNIT_RESET   0x40U
#define MPI3_DOORBELL_FUNC_HANDSHAKE                0x42U

/* =========================================================================
 * Host Interrupt Status / Mask bit definitions
 * ========================================================================= */

#define MPI3_SYSIF_HOST_INT_STATUS_DOORBELL_STATUS  (1U << 31) /**< IOC wrote doorbell   */
#define MPI3_SYSIF_HOST_INT_STATUS_RESET_IRQ        (1U << 30) /**< Reset IRQ asserted    */
#define MPI3_SYSIF_HOST_INT_STATUS_REPLY_DESCRIPTOR_INT (1U << 3) /**< Reply available   */
#define MPI3_SYSIF_HOST_INT_STATUS_SYSTEM_TO_IOC_DB_STATUS (1U << 0) /**< DB ACK pending */

/* Mirror: mask register bit meanings identical to status */
#define MPI3_SYSIF_HOST_INT_MASK_DOORBELL   (1U << 31)
#define MPI3_SYSIF_HOST_INT_MASK_RESET      (1U << 30)
#define MPI3_SYSIF_HOST_INT_MASK_REPLY      (1U <<  3)

/* Set to mask ALL interrupts (written during init before MSI-X setup) */
#define MPI3_SYSIF_HOST_INT_MASK_ALL        0xFFFFFFFFU

/* =========================================================================
 * Host Diagnostic register bits
 * ========================================================================= */

#define MPI3_SYSIF_HOST_DIAG_HOLD_IOC_RESET        (1U <<  1)
#define MPI3_SYSIF_HOST_DIAG_DIAG_WRITE_ENABLE     (1U <<  7)
#define MPI3_SYSIF_HOST_DIAG_RESET_HISTORY         (1U << 10)

/* =========================================================================
 * Write-Sequence "magic" bytes  (unlock diagnostic register)
 * ========================================================================= */

#define MPI3_SYSIF_WRITE_SEQ_KEY_VALUE_FLUSH        0x00U
#define MPI3_SYSIF_WRITE_SEQ_1                      0xF6U
#define MPI3_SYSIF_WRITE_SEQ_2                      0x2EU
#define MPI3_SYSIF_WRITE_SEQ_3                      0x86U
#define MPI3_SYSIF_WRITE_SEQ_4                      0xCEU
#define MPI3_SYSIF_WRITE_SEQ_5                      0x34U
#define MPI3_SYSIF_WRITE_SEQ_6                      0x75U

/* =========================================================================
 * Request / Reply Descriptor type tags
 * ========================================================================= */

typedef enum : uint8_t {
    MPI3_REQUEST_DESCRTYPE_DEFAULT      = 0x00U,
    MPI3_REQUEST_DESCRTYPE_SCSI_IO      = 0x01U,
    MPI3_REQUEST_DESCRTYPE_SCSI_TARGET  = 0x02U,
    MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY= 0x06U,
    MPI3_REQUEST_DESCRTYPE_MPI3         = 0x08U,  /**< IOC management messages */
} MPT3RequestDescriptorType;

typedef enum : uint8_t {
    MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY  = 0x01U,  /**< Points to a reply frame   */
    MPI3_REPLY_DESCRTYPE_TARGET_ASSIST  = 0x02U,
    MPI3_REPLY_DESCRTYPE_TARGET_CMD     = 0x04U,
    MPI3_REPLY_DESCRTYPE_UNUSED         = 0x0FU,  /**< Empty slot sentinel       */
} MPT3ReplyDescriptorType;

/* =========================================================================
 * Maximum / sizing constants
 * ========================================================================= */

/** Maximum number of outstanding SCSI commands (request queue depth) */
#define MPT3_MAX_COMMANDS               4096U

/** Maximum number of outstanding reply descriptors */
#define MPT3_MAX_REPLY_DESCRIPTORS      4096U

/** Size of a single MPT3 request frame in bytes */
#define MPT3_REQUEST_FRAME_SIZE         128U

/** Size of a single MPT3 reply frame in bytes */
#define MPT3_REPLY_FRAME_SIZE           128U

/** Size of a single reply descriptor (8 bytes, read from post queue) */
#define MPT3_REPLY_DESCRIPTOR_SIZE      8U

/** Maximum SGL (Scatter-Gather List) elements per request frame */
#define MPT3_MAX_SGL_ENTRIES_IN_FRAME   4U

/** Maximum chain SGL buffer size in bytes */
#define MPT3_MAX_CHAIN_BUFFER_SIZE      4096U

/** Maximum SCSI CDB length supported */
#define MPT3_MAX_CDB_LENGTH             32U

/** I/O timeout in milliseconds for normal commands */
#define MPT3_IO_TIMEOUT_MS              30000U

/** Maximum number of MSI-X vectors we will claim */
#define MPT3_MAX_MSIX_VECTORS           16U

/** Number of reply queues (one per MSI-X vector) */
#define MPT3_MAX_REPLY_QUEUES           MPT3_MAX_MSIX_VECTORS

/* =========================================================================
 * Configuration page related
 * ========================================================================= */

/** Manufacturing page 0 – firmware version */
#define MPI3_CONFIG_PAGE_TYPE_MANUFACTURING     0x01U
#define MPI3_CONFIG_PAGE_TYPE_IO_UNIT           0x02U
#define MPI3_CONFIG_PAGE_TYPE_IOC               0x03U
#define MPI3_CONFIG_PAGE_TYPE_BIOS              0x04U
#define MPI3_CONFIG_PAGE_TYPE_SAS_DEVICE        0x12U
#define MPI3_CONFIG_PAGE_TYPE_SAS_IO_UNIT       0x10U

/* =========================================================================
 * SAS address / target helpers
 * ========================================================================= */

/** 64-bit SAS address type (big-endian on the wire) */
typedef uint64_t MPT3SASAddress;

/** Invalid / unset SAS address */
#define MPT3_SAS_ADDRESS_INVALID        0xFFFFFFFFFFFFFFFFULL

/* end of MPT3Registers.h */
