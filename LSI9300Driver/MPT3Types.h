/**
 * MPT3Types.h — Wire-format structures for the MPT3SAS protocol
 *
 * All multi-byte fields are little-endian (x86/ARM native on macOS).
 * Struct sizes are fixed by the protocol; static_asserts guard against
 * accidental padding.
 *
 * References:
 *   - Broadcom MPI 3.0 Specification, rev 1.28
 *   - Linux kernel drivers/scsi/mpt3sas/mpi/mpi2*.h (BSD-compatible headers)
 *
 * Copyright (c) 2024 ClaudeKernelLSI Project.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <type_traits>
#define MPT3_STATIC_ASSERT(cond, msg)   static_assert(cond, msg)
#else
#define MPT3_STATIC_ASSERT(cond, msg)   _Static_assert(cond, msg)
#endif

/* Force no padding in any struct defined here */
#pragma pack(push, 1)

/* =========================================================================
 * Request / Reply descriptor pairs
 * ========================================================================= */

/** 8-byte SCSI-IO request descriptor posted to request FIFO */
typedef struct {
    uint8_t  DescriptorType;        /**< MPI3_REQUEST_DESCRTYPE_SCSI_IO     */
    uint8_t  MSIxIndex;             /**< Which reply queue to use           */
    uint16_t SMID;                  /**< System Message ID (command tag)    */
    uint16_t DevHandle;             /**< Target device handle               */
    uint16_t Reserved;
} MPT3SCSIIORequestDescriptor;
MPT3_STATIC_ASSERT(sizeof(MPT3SCSIIORequestDescriptor) == 8, "size mismatch");

/** 8-byte high-priority request descriptor (for management messages) */
typedef struct {
    uint8_t  DescriptorType;        /**< MPI3_REQUEST_DESCRTYPE_HIGH_PRIORITY */
    uint8_t  MSIxIndex;
    uint16_t SMID;
    uint32_t Reserved;
} MPT3HighPriorityRequestDescriptor;
MPT3_STATIC_ASSERT(sizeof(MPT3HighPriorityRequestDescriptor) == 8, "size mismatch");

/** 8-byte default request descriptor */
typedef struct {
    uint8_t  DescriptorType;
    uint8_t  MSIxIndex;
    uint16_t SMID;
    uint32_t Reserved;
} MPT3DefaultRequestDescriptor;
MPT3_STATIC_ASSERT(sizeof(MPT3DefaultRequestDescriptor) == 8, "size mismatch");

/** 8-byte address reply descriptor read back from reply post queue */
typedef struct {
    uint8_t  DescriptorType;        /**< MPI3_REPLY_DESCRTYPE_ADDRESS_REPLY */
    uint8_t  MSIxIndex;
    uint16_t SMID;
    uint32_t ReplyFrameAddress;     /**< Physical address >> 4 of reply frame */
} MPT3AddressReplyDescriptor;
MPT3_STATIC_ASSERT(sizeof(MPT3AddressReplyDescriptor) == 8, "size mismatch");

/** Union over all reply descriptor types */
typedef union {
    MPT3AddressReplyDescriptor  AddressReply;
    uint64_t                    Words;
} MPT3ReplyDescriptor;
MPT3_STATIC_ASSERT(sizeof(MPT3ReplyDescriptor) == 8, "size mismatch");

/* =========================================================================
 * MPI message header (common to every request and reply frame)
 * ========================================================================= */

typedef struct {
    uint16_t FunctionDependent1;
    uint8_t  ChainOffset;           /**< In 32-bit words; 0 = no chain      */
    uint8_t  Function;              /**< Message function code              */
    uint16_t FunctionDependent2;
    uint8_t  IOCIndex;
    uint8_t  MsgFlags;
    uint8_t  VP_ID;
    uint8_t  VF_ID;
    uint16_t Reserved1;
} MPT3RequestHeader;
MPT3_STATIC_ASSERT(sizeof(MPT3RequestHeader) == 12, "size mismatch");

typedef struct {
    uint16_t FunctionDependent1;
    uint8_t  MsgLength;
    uint8_t  Function;
    uint16_t FunctionDependent2;
    uint8_t  IOCLogInfo;
    uint8_t  MsgFlags;
    uint8_t  VP_ID;
    uint8_t  VF_ID;
    uint16_t Reserved1;
    uint16_t IOCStatus;             /**< MPI3_IOCSTATUS_* codes             */
    uint32_t IOCLogInfo2;
} MPT3ReplyHeader;
MPT3_STATIC_ASSERT(sizeof(MPT3ReplyHeader) == 18, "size mismatch");

/* =========================================================================
 * MPI function codes
 * ========================================================================= */

typedef enum : uint8_t {
    MPI3_FUNCTION_SCSI_IO                       = 0x00U,
    MPI3_FUNCTION_SCSI_TASK_MGMT                = 0x01U,
    MPI3_FUNCTION_IOC_INIT                      = 0x02U,
    MPI3_FUNCTION_IOC_FACTS                     = 0x03U,
    MPI3_FUNCTION_CONFIG                        = 0x04U,
    MPI3_FUNCTION_PORT_FACTS                    = 0x05U,
    MPI3_FUNCTION_PORT_ENABLE                   = 0x06U,
    MPI3_FUNCTION_EVENT_NOTIFICATION            = 0x07U,
    MPI3_FUNCTION_EVENT_ACK                     = 0x08U,
    MPI3_FUNCTION_FW_DOWNLOAD                   = 0x09U,
    MPI3_FUNCTION_SCSI_ENCLOSURE_PROCESSOR      = 0x18U,
    MPI3_FUNCTION_SAS_IO_UNIT_CONTROL           = 0x1BU,
    MPI3_FUNCTION_SATA_PASSTHROUGH              = 0x1DU,
    MPI3_FUNCTION_DIAG_BUFFER_POST              = 0x1DU,
} MPT3Function;

/* =========================================================================
 * IOC status codes  (MPT3ReplyHeader.IOCStatus)
 * ========================================================================= */

typedef enum : uint16_t {
    MPI3_IOCSTATUS_SUCCESS                  = 0x0000U,
    MPI3_IOCSTATUS_INVALID_FUNCTION         = 0x0001U,
    MPI3_IOCSTATUS_BUSY                     = 0x0002U,
    MPI3_IOCSTATUS_INVALID_SGL             = 0x0003U,
    MPI3_IOCSTATUS_INTERNAL_ERROR           = 0x0004U,
    MPI3_IOCSTATUS_INSUFFICIENT_RESOURCES   = 0x0006U,
    MPI3_IOCSTATUS_INVALID_FIELD            = 0x0007U,
    MPI3_IOCSTATUS_INVALID_STATE            = 0x0008U,
    MPI3_IOCSTATUS_OP_STATE_NOT_SUPPORTED   = 0x0009U,
    MPI3_IOCSTATUS_SCSI_DATA_OVERRUN        = 0x0044U,
    MPI3_IOCSTATUS_SCSI_DATA_UNDERRUN       = 0x0045U,
    MPI3_IOCSTATUS_SCSI_IO_DATA_ERROR       = 0x0046U,
    MPI3_IOCSTATUS_SCSI_PROTOCOL_ERROR      = 0x0047U,
    MPI3_IOCSTATUS_SCSI_TASK_TERMINATED     = 0x0048U,
    MPI3_IOCSTATUS_SCSI_RESIDUAL_MISMATCH   = 0x0049U,
    MPI3_IOCSTATUS_SCSI_TASK_MGMT_FAILED    = 0x004AU,
    MPI3_IOCSTATUS_SCSI_IOC_TERMINATED      = 0x004BU,
    MPI3_IOCSTATUS_SCSI_EXT_TERMINATED      = 0x004CU,
    MPI3_IOCSTATUS_EEDP_GUARD_ERROR         = 0x004DU,
    MPI3_IOCSTATUS_EEDP_REF_TAG_ERROR       = 0x004EU,
    MPI3_IOCSTATUS_EEDP_APP_TAG_ERROR       = 0x004FU,
} MPT3IOCStatus;

/* =========================================================================
 * IOCFacts request / reply
 * ========================================================================= */

/** Host → IOC: request basic IOC capabilities */
typedef struct {
    MPT3RequestHeader   Header;
    uint8_t             Reserved[20];
} MPT3IOCFactsRequest;
MPT3_STATIC_ASSERT(sizeof(MPT3IOCFactsRequest) == 32, "size mismatch");

/** IOC → host: capabilities reply */
typedef struct {
    MPT3ReplyHeader     Header;
    uint16_t            IOCExceptions;
    uint16_t            IOCCapabilities;
    uint8_t             WhoInit;
    uint8_t             NumberOfPorts;
    uint16_t            RequestCredit;      /**< Max outstanding requests      */
    uint16_t            ProductID;
    uint16_t            IOCRequestFrameSize;/**< In 32-bit words               */
    uint16_t            PortNumber;
    uint8_t             HardwareRev;
    uint8_t             PciRevID;
    uint16_t            IOCFaultCode;
    uint16_t            IOCMaxDevices;      /**< Max device handles            */
    uint16_t            IOCMaxVolumes;
    uint16_t            IOCChainBufferSize; /**< In 32-bit words               */
    uint16_t            MaxTargets;
    uint16_t            MaxSasExpanders;
    uint16_t            MaxEnclosures;
    uint8_t             ProtocolFlags;
    uint8_t             HighPriorityCredit;
    uint16_t            MaxReplyDescriptorPostQueueDepth;
    uint8_t             ReplyFrameSize;     /**< In 32-bit words               */
    uint8_t             MaxVolumesPerDG;
    uint8_t             MaxDeviceCreditThreshold;
    uint8_t             Reserved1;
    uint32_t            FWVersion;
    uint32_t            IOCCapabilities2;
    uint8_t             Reserved2[4];
} MPT3IOCFactsReply;
MPT3_STATIC_ASSERT(sizeof(MPT3IOCFactsReply) == 68, "size mismatch");

/* =========================================================================
 * IOCInit request  (sent once after IOCFacts to configure descriptor pools)
 * ========================================================================= */

typedef struct {
    MPT3RequestHeader   Header;
    uint8_t             WhoInit;
    uint8_t             Reserved1;
    uint16_t            MsgVersion;         /**< 0x0200 = MPI 2.0              */
    uint32_t            Reserved2;
    uint32_t            ReplyDescriptorPostQueueAddress_Low;
    uint32_t            ReplyDescriptorPostQueueAddress_High;
    uint32_t            FreeReplyDescriptorPostQueueAddress_Low;
    uint32_t            FreeReplyDescriptorPostQueueAddress_High;
    uint32_t            SenseBufferAddressHigh;
    uint16_t            ReplyDescriptorPostQueueDepth;
    uint16_t            ReplyFreeQueueDepth;
    uint32_t            Flags;
    uint64_t            DriverTimestamp;    /**< Host timestamp for logging    */
    uint8_t             Reserved3[24];
} MPT3IOCInitRequest;
MPT3_STATIC_ASSERT(sizeof(MPT3IOCInitRequest) == 80, "size mismatch");

/** IOC reply to IOCInit (uses generic reply header; success = IOCStatus 0) */
typedef MPT3ReplyHeader MPT3IOCInitReply;

/* =========================================================================
 * SCSI IO Request frame  (128 bytes)
 * ========================================================================= */

/** Simple 64-bit SGL element */
typedef struct {
    uint32_t FlagsLength;           /**< Flags in top 8 bits, length in low 24*/
    uint32_t DataBufferLow;
    uint32_t DataBufferHigh;
} MPT3SGESimple64;
MPT3_STATIC_ASSERT(sizeof(MPT3SGESimple64) == 12, "size mismatch");

/** Chain SGL element (points to the next SGL buffer) */
typedef struct {
    uint32_t FlagsLength;
    uint32_t AddressLow;
    uint32_t AddressHigh;
} MPT3SGEChain64;
MPT3_STATIC_ASSERT(sizeof(MPT3SGEChain64) == 12, "size mismatch");

/*
 * SGL flags — all encoded in the upper byte (bits 31:24) of FlagsLength.
 * The lower 24 bits (bits 23:0) carry the transfer length.
 * Layout of the flags byte:
 *   Bit 7 (DWORD bit 31): LAST_ELEMENT — last SGL entry in the chain
 *   Bit 6 (DWORD bit 30): END_OF_BUFFER — last segment for this data buffer
 *   Bits 5:4 (DWORD bits 29:28): Element type (SIMPLE=01, CHAIN=11)
 *   Bit 2 (DWORD bit 26): HOST_TO_IOC direction (0 = IOC→host)
 *   Bit 1 (DWORD bit 25): 64-bit addressing (vs. 32-bit)
 *   Bit 0 (DWORD bit 24): END_OF_LIST — terminates the entire SGL
 */
#define MPI3_SGE_FLAGS_LAST_ELEMENT             (0x80U << 24)  /**< 0x80000000 */
#define MPI3_SGE_FLAGS_END_OF_BUFFER            (0x40U << 24)  /**< 0x40000000 */
#define MPI3_SGE_FLAGS_SIMPLE_ELEMENT           (0x10U << 24)  /**< 0x10000000 */
#define MPI3_SGE_FLAGS_CHAIN_ELEMENT            (0x30U << 24)  /**< 0x30000000 */
#define MPI3_SGE_FLAGS_HOST_TO_IOC              (0x04U << 24)  /**< 0x04000000 */
#define MPI3_SGE_FLAGS_64_BIT_ADDRESSING        (0x02U << 24)  /**< 0x02000000 */
#define MPI3_SGE_FLAGS_END_OF_LIST              (0x01U << 24)  /**< 0x01000000 */

/** Length mask — bits 23:0 */
#define MPI3_SGE_LENGTH_MASK                    0x00FFFFFFU

/** Full SCSI IO request frame  (must be exactly MPT3_REQUEST_FRAME_SIZE bytes) */
typedef struct {
    MPT3RequestHeader   Header;         /*  0 – 11 */
    uint16_t            DevHandle;      /* 12 – 13 */
    uint8_t             ChainOffset;    /* 14 */
    uint8_t             SGLOffset0;     /* 15 – offset in DWORDs to SGL list */
    uint16_t            SkipCount;      /* 16 – 17 */
    uint8_t             SGLFlags;       /* 18 */
    uint8_t             SenseBufferLength; /* 19 */
    uint8_t             Reserved1;      /* 20 */
    uint8_t             IOFlags;        /* 21 */
    uint8_t             EEDPBlockSize;  /* 22 */
    uint8_t             EEDPFlags;      /* 23 */
    uint32_t            DataLength;     /* 24 – 27 */
    uint16_t            IoFlags2;       /* 28 – 29 */
    uint16_t            TaskAttributes; /* 30 – 31 */
    uint32_t            SenseBufferLowAddress; /* 32 – 35 */
    uint16_t            SGLFlags2;      /* 36 – 37 */
    uint8_t             ChainOffsetNext; /* 38 */
    uint8_t             SGLOffset1;     /* 39 */
    uint8_t             CDB[32];        /* 40 – 71: SCSI CDB up to 32 bytes   */
    MPT3SGESimple64     SGL[4];         /* 72 – 119: four inline SGL entries  */
    uint8_t             Reserved2[8];   /* 120 – 127 */
} MPT3SCSIIORequest;
MPT3_STATIC_ASSERT(sizeof(MPT3SCSIIORequest) == 128, "size mismatch");

/** SCSI IO Reply frame */
typedef struct {
    MPT3ReplyHeader     Header;         /*  0 – 15 */
    uint16_t            DevHandle;      /* 16 – 17 */
    uint16_t            TaskTag;        /* 18 – 19 */
    uint8_t             SCSIStatus;     /* 20 – SAM-4 status byte             */
    uint8_t             SCSIState;      /* 21 */
    uint16_t            Reserved1;      /* 22 – 23 */
    uint32_t            IOCLogInfo;     /* 24 – 27 */
    uint32_t            TransferCount;  /* 28 – 31: bytes actually transferred */
    uint32_t            SenseCount;     /* 32 – 35: valid bytes in sense buffer*/
    uint32_t            ResponseInfo;   /* 36 – 39 */
    uint16_t            TaskTag2;       /* 40 – 41 */
    uint16_t            Reserved2;      /* 42 – 43 */
} MPT3SCSIIOReply;
MPT3_STATIC_ASSERT(sizeof(MPT3SCSIIOReply) == 46, "size mismatch");

/* SCSI IO Reply SCSIState flags */
#define MPI3_SCSI_STATE_AUTOSENSE_VALID         (1U << 0)
#define MPI3_SCSI_STATE_AUTOSENSE_FAILED        (1U << 3)
#define MPI3_SCSI_STATE_NO_SCSI_STATUS          (1U << 4)
#define MPI3_SCSI_STATE_RESPONSE_INFO_VALID     (1U << 5)
#define MPI3_SCSI_STATE_TERMINATED              (1U << 6)

/* =========================================================================
 * Task Management request / reply
 * ========================================================================= */

typedef struct {
    MPT3RequestHeader   Header;
    uint16_t            DevHandle;
    uint8_t             Reserved1[2];
    uint8_t             TaskType;       /**< MPI3_SCSITASKMGMT_TASKTYPE_*    */
    uint8_t             Reserved2;
    uint16_t            MsgFlags2;
    uint32_t            QueueTag;
    uint8_t             Reserved3[4];
    uint64_t            LUID;
    uint16_t            TaskMID;        /**< SMID of the task to abort       */
    uint8_t             Reserved4[2];
} MPT3SCTMRequest;
MPT3_STATIC_ASSERT(sizeof(MPT3SCTMRequest) == 40, "size mismatch");

typedef enum : uint8_t {
    MPI3_SCSITASKMGMT_TASKTYPE_ABORT_TASK           = 0x01U,
    MPI3_SCSITASKMGMT_TASKTYPE_ABORT_TASK_SET       = 0x02U,
    MPI3_SCSITASKMGMT_TASKTYPE_TARGET_RESET         = 0x03U,
    MPI3_SCSITASKMGMT_TASKTYPE_LUN_RESET            = 0x08U,
    MPI3_SCSITASKMGMT_TASKTYPE_QUERY_TASK           = 0x09U,
    MPI3_SCSITASKMGMT_TASKTYPE_CLEAR_TASK_SET       = 0x0AU,
    MPI3_SCSITASKMGMT_TASKTYPE_QAS_REQUEST          = 0x0BU,
    MPI3_SCSITASKMGMT_TASKTYPE_QUERY_UNIT_ATTENTION = 0x0CU,
    MPI3_SCSITASKMGMT_TASKTYPE_QUERY_ASYNC_EVENT    = 0x0DU,
} MPT3TaskManagementType;

typedef struct {
    MPT3ReplyHeader     Header;
    uint8_t             ResponseCode;
    uint8_t             Reserved[3];
    uint32_t            TerminationCount;
} MPT3SCTMReply;
MPT3_STATIC_ASSERT(sizeof(MPT3SCTMReply) == 26, "size mismatch");

/* =========================================================================
 * Event Notification  (async events the IOC sends to the host)
 * ========================================================================= */

typedef struct {
    MPT3RequestHeader   Header;
    uint32_t            EventSwitches[4]; /**< Bitmask of events to enable   */
} MPT3EventNotificationRequest;
MPT3_STATIC_ASSERT(sizeof(MPT3EventNotificationRequest) == 28, "size mismatch");

typedef struct {
    MPT3ReplyHeader     Header;
    uint16_t            EventDataLength;/**< In 32-bit words                 */
    uint8_t             AckRequired;
    uint8_t             Reserved1;
    uint32_t            Event;          /**< MPI3_EVENT_* code               */
    uint32_t            EventContext;
    uint32_t            EventData[1];   /**< Variable-length event payload   */
} MPT3EventNotificationReply;

/* Event codes */
#define MPI3_EVENT_LOG_DATA                     0x01U
#define MPI3_EVENT_STATE_CHANGE                 0x02U
#define MPI3_EVENT_SAS_DISCOVERY                0x16U
#define MPI3_EVENT_SAS_TOPOLOGY_CHANGE_LIST     0x1BU
#define MPI3_EVENT_SAS_ENCL_DEVICE_STATUS_CHANGE 0x25U
#define MPI3_EVENT_SAS_PHY_COUNTER              0x22U
#define MPI3_EVENT_SAS_DEVICE_STATUS_CHANGE     0x0FU
#define MPI3_EVENT_DEVICE_ADDED                 0x13U

/* =========================================================================
 * Config page header + request
 * ========================================================================= */

typedef struct {
    uint8_t  PageVersion;
    uint8_t  PageLength;            /**< In 32-bit words (0 for extended pgs)*/
    uint8_t  PageNumber;
    uint8_t  PageType;
} MPT3ConfigPageHeader;
MPT3_STATIC_ASSERT(sizeof(MPT3ConfigPageHeader) == 4, "size mismatch");

typedef struct {
    MPT3RequestHeader       Header;
    uint8_t                 Action;         /**< MPI3_CONFIG_ACTION_*        */
    uint8_t                 SGLFlags;
    uint8_t                 ChainOffset;
    uint8_t                 Function2;
    uint16_t                ExtPageLength;
    uint8_t                 ExtPageType;
    uint8_t                 MsgFlags2;
    uint32_t                Reserved1;
    uint32_t                Reserved2;
    MPT3ConfigPageHeader    Header2;
    uint32_t                PageAddress;
    MPT3SGESimple64         PageBufferSGE;
} MPT3ConfigRequest;

typedef enum : uint8_t {
    MPI3_CONFIG_ACTION_PAGE_HEADER              = 0x00U,
    MPI3_CONFIG_ACTION_PAGE_READ_CURRENT        = 0x01U,
    MPI3_CONFIG_ACTION_PAGE_WRITE_CURRENT       = 0x02U,
    MPI3_CONFIG_ACTION_PAGE_DEFAULT             = 0x03U,
    MPI3_CONFIG_ACTION_PAGE_WRITE_NVRAM         = 0x04U,
    MPI3_CONFIG_ACTION_PAGE_READ_DEFAULT        = 0x05U,
    MPI3_CONFIG_ACTION_PAGE_READ_NVRAM          = 0x06U,
} MPT3ConfigAction;

/* =========================================================================
 * SAS Device Page 0  (one per physical device)
 * ========================================================================= */

typedef struct {
    MPT3ConfigPageHeader    Header;
    uint16_t    Slot;
    uint16_t    EnclosureHandle;
    uint64_t    SASAddress;         /**< Device's SAS address               */
    uint16_t    ParentDevHandle;
    uint8_t     PhyNum;
    uint8_t     AccessStatus;
    uint16_t    DevHandle;          /**< Handle used in SCSI IO requests    */
    uint8_t     AttachedPhyIdentifier;
    uint8_t     ZoneGroup;
    uint32_t    DeviceInfo;         /**< MPI3_SAS_DEVICE_INFO_*             */
    uint16_t    Flags;
    uint8_t     PhysicalPort;
    uint8_t     MaxPortConnections;
    uint64_t    DeviceName;
    uint8_t     PortGroups;
    uint8_t     DmaGroup;
    uint8_t     ControlGroup;
    uint8_t     EnclosureLevel;
    uint32_t    ConnectorName[4];
    uint32_t    Reserved3;
} MPT3SASDevicePage0;

/* DeviceInfo flags */
#define MPI3_SAS_DEVICE_INFO_SATA_DEVICE        (1U <<  0)
#define MPI3_SAS_DEVICE_INFO_SMP_INITIATOR      (1U <<  4)
#define MPI3_SAS_DEVICE_INFO_STP_INITIATOR      (1U <<  5)
#define MPI3_SAS_DEVICE_INFO_SSP_INITIATOR      (1U <<  6)
#define MPI3_SAS_DEVICE_INFO_SMP_TARGET         (1U <<  8)
#define MPI3_SAS_DEVICE_INFO_STP_TARGET         (1U <<  9)
#define MPI3_SAS_DEVICE_INFO_SSP_TARGET         (1U << 10)
#define MPI3_SAS_DEVICE_INFO_DIRECT_ATTACH      (1U << 11)
#define MPI3_SAS_DEVICE_INFO_IS_EXPANDER        (1U << 12)
#define MPI3_SAS_DEVICE_INFO_ATAPI_DEVICE       (1U << 13)
#define MPI3_SAS_DEVICE_INFO_LSI_DEVICE         (1U << 14)
#define MPI3_SAS_DEVICE_INFO_SATA_HDD           (1U << 15)

#pragma pack(pop)
