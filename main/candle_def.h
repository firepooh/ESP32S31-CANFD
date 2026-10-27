/*
 * Candlelight (gs_usb) and ElmueSoft CANable 2.5 USB protocol definitions.
 *
 * Taken from candlelight_def.h of the CANable 2.5 firmware (MIT License):
 *   Copyright (c) 2025 ElmueSoft / Hubert Denkmair
 *   https://netcult.ch/elmue/CANable Firmware Update
 * Only the parts used by this firmware are kept; values and layouts are unchanged.
 */
#pragma once

#include <stdint.h>

#define CANDLE_PACKED __attribute__((packed))

/* ------------------------------------------------ SETUP requests ------------------------------------------------ */

typedef enum {
    GS_ReqSetHostFormat     = 0,    // uint32_t
    GS_ReqSetBitTiming      = 1,    // kBitTiming (nominal)
    GS_ReqSetDeviceMode     = 2,    // kDeviceMode
    GS_ReqBerrReport        = 3,
    GS_ReqGetCapabilities   = 4,    // kCapabilityClassic
    GS_ReqGetDeviceVersion  = 5,    // kDeviceVersion
    GS_ReqGetTimestamp      = 6,    // uint32_t, 1 us
    GS_ReqIdentify          = 7,    // uint32_t: 1 = blink, 0 = stop
    GS_ReqSetBitTimingFD    = 10,   // kBitTiming (data phase)
    GS_ReqGetCapabilitiesFD = 11,   // kCapabilityFD
    GS_ReqSetTermination    = 12,
    GS_ReqGetTermination    = 13,
    GS_ReqGetErrorState     = 14,   // kErrorState
    // Linux gs_usb / cangaroo extensions (not advertised, accepted for compatibility)
    GS_ReqSetTdc            = 18,
    GS_ReqBusOffRecovery    = 32,

    ELM_ReqFIRST            = 20,
    ELM_ReqGetBoardInfo     = 20,   // kBoardInfo
    ELM_ReqSetFilter        = 21,   // kFilter
    ELM_ReqGetLastError     = 22,   // uint8_t eFeedback
    ELM_ReqSetBusLoadReport = 23,   // uint8_t interval in 100 ms
    ELM_ReqSetPinStatus     = 24,
    ELM_ReqGetPinStatus     = 25,
    ELM_ReqReadFlash        = 26,
    ELM_ReqWriteFlash       = 27,
} eUsbRequest;

// ELM_ReqGetPinStatus: wValue = pin ID, the response is a 16 bit pin status
typedef enum {
    PINID_BOOT0 = 1,
} ePinID;

typedef enum {
    PINST_High    = 0x0001,
    PINST_Enabled = 0x0002,
} ePinStatus;

typedef enum {
    GS_DevFlagListenOnly     = 0x00001,
    GS_DevFlagLoopback       = 0x00002,
    GS_DevFlagOneShot        = 0x00008,
    GS_DevFlagTimestamp      = 0x00010,
    GS_DevFlagIdentify       = 0x00020,
    GS_DevFlagCAN_FD         = 0x00100,
    GS_DevFlagBitTimingFD    = 0x00400,
    GS_DevFlagTermination    = 0x00800,
    GS_DevFlagGetErrorState  = 0x02000,
    ELM_DevFlagProtocolElmue = 0x04000,
    ELM_DevFlagSendUsbBlobs  = 0x08000,
} eDeviceFlags;

typedef enum {
    GS_ModeReset = 0,
    GS_ModeStart = 1,
} eDeviceMode;

/* eFeedback, returned by ELM_ReqGetLastError */
typedef enum {
    FBK_Success             = 2,
    FBK_InvalidCommand      = '1',
    FBK_InvalidParameter,
    FBK_AdapterMustBeOpen,
    FBK_AdapterMustBeClosed,
    FBK_ErrorFromHAL,
    FBK_UnsupportedFeature,
    FBK_TxBufferFull,
    FBK_BusIsOff,
    FBK_NoTxInSilentMode,
    FBK_BaudrateNotSet,
    FBK_OptBytesProgrFailed,
    FBK_ResetRequired,
    FBK_ParamOutOfRange,
} eFeedback;

typedef struct {
    uint8_t  hal_ver_high;
    uint8_t  hal_ver_mid;
    uint8_t  hal_ver_low;
    uint8_t  icount;            // interface count - 1
    uint32_t sw_version_bcd;
    uint32_t hw_version_bcd;
} CANDLE_PACKED kDeviceVersion;

typedef struct {
    uint32_t mode;              // eDeviceMode
    uint32_t flags;             // eDeviceFlags
} CANDLE_PACKED kDeviceMode;

typedef struct {
    uint32_t prop;
    uint32_t seg1;
    uint32_t seg2;
    uint32_t sjw;
    uint32_t brp;
} CANDLE_PACKED kBitTiming;

typedef struct {
    uint32_t seg1_min;
    uint32_t seg1_max;          // including the propagation segment
    uint32_t seg2_min;
    uint32_t seg2_max;
    uint32_t sjw_max;
    uint32_t brp_min;
    uint32_t brp_max;
    uint32_t brp_inc;
} CANDLE_PACKED kTimeMinMax;

typedef struct {
    uint32_t    feature;
    uint32_t    fclk_can;
    kTimeMinMax time;
} CANDLE_PACKED kCapabilityClassic;

typedef struct {
    uint32_t    feature;
    uint32_t    fclk_can;
    kTimeMinMax time_nom;
    kTimeMinMax time_data;
} CANDLE_PACKED kCapabilityFD;

typedef enum {
    GS_BusActive = 0,
    GS_ErrorWarning,
    GS_ErrorPassive,
    GS_BusOff,
    GS_Stopped,
} eBusState;

typedef struct {
    uint32_t state;             // eBusState
    uint32_t rx_err;
    uint32_t tx_err;
} CANDLE_PACKED kErrorState;

typedef enum {
    BRD_Quartz_In_Use = 0x00000001,
    BRD_USB_HighSpeed = 0x00000002,
} eBoardFlags;

typedef struct {
    uint16_t McuDeviceID;
    char     McuName[25];
    char     BoardName[25];
    uint32_t BoardFlags;
} CANDLE_PACKED kBoardInfo;

/* ------------------------------------------------ Error report ------------------------------------------------ */

typedef enum {
    ERID_Tx_Timeout          = 0x0001,
    ERID_Arbitration_lost    = 0x0002,
    ERID_Controller_problem  = 0x0004,
    ERID_Protocol_violation  = 0x0008,
    ERID_No_ACK_received     = 0x0020,
    ERID_Bus_is_off          = 0x0040,
    ERID_Bus_error           = 0x0080,
    ERID_CRC_Error           = 0x0200,
} eErrFlagsCanID;

typedef enum {
    ER1_Rx_Buffer_Overflow         = 0x01,
    ER1_Tx_Buffer_Overflow         = 0x02,
    ER1_Rx_Errors_at_warning_level = 0x04,
    ER1_Tx_Errors_at_warning_level = 0x08,
    ER1_Rx_Passive_status_reached  = 0x10,
    ER1_Tx_Passive_status_reached  = 0x20,
    ER1_Bus_is_back_active         = 0x40,
} eErrFlagsByte1;

typedef enum {
    ER2_Single_bit_error             = 0x01,
    ER2_Frame_format_error           = 0x02,
    ER2_Bit_stuffing_error           = 0x04,
    ER2_Unable_to_send_dominant_bit  = 0x08,
    ER2_Unable_to_send_recessive_bit = 0x10,
} eErrFlagsByte2;

/* Firmware detected problems, sent in error data byte 5 (eErrorAppFlags) */
typedef enum {
    APP_CanRxFail     = 0x01,
    APP_CanTxFail     = 0x02,
    APP_CanTxOverflow = 0x04,
    APP_UsbInOverflow = 0x08,
    APP_CanTxTimeout  = 0x10,
} eErrorAppFlags;

/* ------------------------------------------------ Legacy frames ------------------------------------------------ */

#define CAN_ID_Error    0x20000000u
#define CAN_ID_RTR      0x40000000u
#define CAN_ID_29Bit    0x80000000u
#define CAN_MASK_11     0x000007FFu
#define CAN_MASK_29     0x1FFFFFFFu

#define FRM_FDF         0x02
#define FRM_BRS         0x04
#define FRM_ESI         0x08

#define ECHO_RxData     0xFFFFFFFFu

typedef struct {
    uint8_t  data[8];
    uint32_t timestamp_us;
} CANDLE_PACKED kPacketClassic;

typedef struct {
    uint8_t  data[64];
    uint32_t timestamp_us;
} CANDLE_PACKED kPacketFD;

typedef struct {            // 80 bytes
    uint32_t echo_id;
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    union {
        kPacketClassic pack_classic;
        kPacketFD      pack_FD;
        uint8_t        raw_data[sizeof(kPacketFD)];
    };
} CANDLE_PACKED kHostFrameLegacy;

/* ------------------------------------------------ ElmueSoft messages ------------------------------------------------ */

#define MAX_BLOB_SIZE   2048

typedef enum {
    MSG_TxFrame = 0x0A,
    MSG_TxEcho  = 0x0B,
    MSG_RxFrame = 0x0C,
    MSG_Error   = 0x0D,
    MSG_String  = 0x0E,
    MSG_Busload = 0x0F,
    MSG_TxBlob  = 0x10,
    MSG_RxBlob  = 0x11,
} eMessageType;

typedef struct {
    uint8_t frame_count;
    uint8_t msg_type;       // MSG_TxBlob / MSG_RxBlob
} CANDLE_PACKED kBlob;

typedef struct {
    uint8_t size;           // total length of the message
    uint8_t msg_type;
} CANDLE_PACKED kHeader;

typedef struct {
    kHeader  header;        // MSG_TxFrame
    uint8_t  flags;
    uint32_t can_id;
    uint8_t  marker;        // echoed back in MSG_TxEcho when > 0
    uint8_t  data_start[];
} CANDLE_PACKED kTxFrameElmue;

typedef struct {
    kHeader  header;        // MSG_RxFrame
    uint8_t  flags;
    uint32_t can_id;
    // uint32_t timestamp (only with GS_DevFlagTimestamp), then the data bytes
} CANDLE_PACKED kRxFrameElmue;

typedef struct {
    kHeader  header;        // MSG_TxEcho
    uint8_t  marker;
    uint32_t timestamp;     // only with GS_DevFlagTimestamp
} CANDLE_PACKED kTxEchoElmue;

typedef struct {
    kHeader  header;        // MSG_Error
    uint32_t err_id;
    uint8_t  err_data[8];
    uint32_t timestamp;     // only with GS_DevFlagTimestamp
} CANDLE_PACKED kErrorElmue;

typedef struct {
    kHeader  header;        // MSG_Busload
    uint8_t  bus_load;
} CANDLE_PACKED kBusloadElmue;

_Static_assert(sizeof(kHostFrameLegacy) == 80, "legacy frame must be 80 bytes");
_Static_assert(sizeof(kDeviceVersion) == 12, "kDeviceVersion layout");
_Static_assert(sizeof(kCapabilityFD) == 72, "kCapabilityFD layout");
