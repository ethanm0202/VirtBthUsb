/*
 * qca_protocol.h - Qualcomm WCN6855 UART bring-up protocol definitions.
 *
 * Protocol constants and operational opcodes are derived from the upstream Linux kernel Bluetooth
 * drivers (drivers/bluetooth/btqca.h, btqca.c, and hci_qca.c), licensed under GPL-2.0, accessible at:
 *   https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/
 * These functional values are utilized solely for hardware protocol interoperability.
 */

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

/* ---------------------------------------------------------------- H4 framing (UART only)
 * On UART the packet type is a single leading byte. On the USB transport it does not exist -
 * endpoints separate the streams - so this framing belongs to the UART backend alone.
 */
#define H4_PKT_COMMAND   0x01u
#define H4_PKT_ACL       0x02u
#define H4_PKT_SCO       0x03u
#define H4_PKT_EVENT     0x04u

/* ---------------------------------------------------------------- vendor opcodes (btqca.h) */
#define EDL_PATCH_CMD_OPCODE        0xFC00u
#define EDL_NVM_ACCESS_OPCODE       0xFC0Bu
#define EDL_WRITE_BD_ADDR_OPCODE    0xFC14u
#define QCA_PRE_SHUTDOWN_CMD        0xFC08u
#define QCA_DISABLE_LOGGING         0xFC17u
#define QCA_DISABLE_LOGGING_SUB_OP  0x14u     /* btqca.h QCA_DISABLE_LOGGING_SUB_OP */
#define QCA_BAUDRATE_CMD_OPCODE     0xFC48u   /* hci_qca.c: { 0x01, 0x48, 0xFC, 0x01, <rate> } */

/* Sub-commands carried as the first parameter byte of EDL_PATCH_CMD_OPCODE. */
#define EDL_PATCH_VER_REQ_CMD       0x19u
#define EDL_PATCH_TLV_REQ_CMD       0x1Eu
#define EDL_GET_BUILD_INFO_CMD      0x20u
#define EDL_GET_BID_REQ_CMD         0x23u
#define EDL_PATCH_CONFIG_CMD        0x28u
#define EDL_PATCH_RESET_SOC_CMD     0x05u     /* hci_qca.c edl_reset_soc_cmd */

/* Vendor event types (btqca.h). HCI_EV_VENDOR is 0xFF. */
#define HCI_EV_VENDOR               0xFFu
#define EDL_CMD_REQ_RES_EVT         0x00u
#define EDL_PATCH_VER_RES_EVT       0x19u
#define EDL_TVL_DNLD_RES_EVT        0x04u
#define EDL_SET_BAUDRATE_RSP_EVT    0x92u
#define EDL_NVM_ACCESS_CODE_EVT     0x0Bu

/* ---------------------------------------------------------------- TLV */
#define QCA_MAX_SIZE_PER_TLV_SEGMENT 243u     /* btqca.h MAX_SIZE_PER_TLV_SEGMENT */

#define QCA_TLV_TYPE_PATCH  1u
#define QCA_TLV_TYPE_NVM    2u
#define QCA_ELF_TYPE_PATCH  3u

/* btqca.h qca_tlv_dnld_mode: whether the chip acks intermediate segments. */
#define QCA_SKIP_EVT_NONE       0u
#define QCA_SKIP_EVT_VSE        1u
#define QCA_SKIP_EVT_CC         2u
#define QCA_SKIP_EVT_VSE_CC     3u

/* ---------------------------------------------------------------- baud rates
 * btqca.h enum qca_baudrate. The wire value is the ENUM INDEX, not the bit rate.
 */
#define QCA_BAUDRATE_115200     0u
#define QCA_BAUDRATE_921600     10u
#define QCA_BAUDRATE_1000000    11u
#define QCA_BAUDRATE_2000000    13u
#define QCA_BAUDRATE_3000000    14u
#define QCA_BAUDRATE_3200000    17u   /* qca_set_baudrate rejects anything above this */

#define QCA_INIT_BAUD_RATE      115200u
#define QCA_OPER_BAUD_RATE      3000000u

/* ---------------------------------------------------------------- parsed TLV header */
typedef struct _QCA_TLV_INFO {
    UCHAR  Type;        /* QCA_TLV_TYPE_*                              */
    ULONG  Length;      /* declared payload length (type_len >> 8)     */
    ULONG  FileSize;    /* actual file size, for cross-checking        */
    /* Only meaningful when Type == QCA_TLV_TYPE_PATCH. */
    ULONG  TotalSize;
    ULONG  DataLength;
    UCHAR  DownloadMode;  /* QCA_SKIP_EVT_* declared by the patch file */
    USHORT ProductId;
    USHORT RomBuild;
    USHORT PatchVersion;
} QCA_TLV_INFO, *PQCA_TLV_INFO;

/*
 * Parses and validates the 4-byte tlv_type_hdr (and tlv_type_patch when present).
 * Returns FALSE if the buffer is too small or the declared length disagrees with FileSize.
 */
BOOLEAN QcaParseTlv(
    _In_reads_bytes_(Size) const UCHAR *Data,
    _In_ ULONG Size,
    _Out_ PQCA_TLV_INFO Info);

/* Number of 243-byte segments the whole file (header included) is split into. */
ULONG QcaTlvSegmentCount(_In_ ULONG FileSize);

/*
 * Builds one download command for segment `Index`:
 *     [0]      H4_PKT_COMMAND
 *     [1..2]   EDL_PATCH_CMD_OPCODE, little endian
 *     [3]      parameter length = segment size + 2
 *     [4]      EDL_PATCH_TLV_REQ_CMD
 *     [5]      segment size
 *     [6..]    segment bytes
 * Returns the number of bytes written, or 0 on error (bad index or short output buffer).
 * `*AckExpected` reports whether this segment is acknowledged: the last segment and any
 * short segment always are, regardless of the file's declared download mode
 * (btqca.c: "The last segment is always acked regardless download mode").
 */
ULONG QcaBuildTlvSegmentCommand(
    _In_reads_bytes_(FileSize) const UCHAR *Data,
    _In_ ULONG FileSize,
    _In_ ULONG Index,
    _In_ UCHAR DownloadMode,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity,
    _Out_ BOOLEAN *AckExpected);

/* { H4, 0x48, 0xFC, 0x01, BaudRateIndex }. Returns bytes written (5) or 0 if rejected. */
ULONG QcaBuildBaudRateCommand(_In_ UCHAR BaudRateIndex,
                              _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
                              _In_ ULONG OutCapacity);

/* { H4, 0x00, 0xFC, 0x01, sub }. Used for the version request and the SOC reset. */
ULONG QcaBuildEdlCommand(_In_ UCHAR SubCommand,
                         _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
                         _In_ ULONG OutCapacity);

/* Maps a bit rate to the enum index the chip expects. Returns FALSE if unsupported. */
BOOLEAN QcaBaudRateToIndex(_In_ ULONG BitsPerSecond, _Out_ UCHAR *Index);
