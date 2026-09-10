/*
 * qca_tlv.c - Qualcomm WCN6855 TLV firmware framing and segmentation.
 */

#include "../include/qca_protocol.h"

static ULONG Rd32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static USHORT Rd16(const UCHAR *p)
{
    return (USHORT)((USHORT)p[0] | ((USHORT)p[1] << 8));
}

BOOLEAN
QcaParseTlv(
    _In_reads_bytes_(Size) const UCHAR *Data,
    _In_ ULONG Size,
    _Out_ PQCA_TLV_INFO Info)
{
    ULONG typeLen;

    RtlZeroMemory(Info, sizeof(*Info));

    /* struct tlv_type_hdr { __le32 type_len; __u8 data[]; } - type is the low byte. */
    if (Size < 4) {
        return FALSE;
    }

    typeLen        = Rd32(Data);
    Info->Type     = (UCHAR)(typeLen & 0xFFu);
    Info->Length   = typeLen >> 8;
    Info->FileSize = Size;

    /* The declared length covers everything after the 4-byte header. */
    if (Info->Length != Size - 4u) {
        return FALSE;
    }

    if (Info->Type == QCA_TLV_TYPE_PATCH) {
        /*
         * struct tlv_type_patch {
         *   __le32 total_size; __le32 data_length; __u8 format_version; __u8 signature;
         *   __u8 download_mode; __u8 reserved1; __le16 product_id; __le16 rom_build;
         *   __le16 patch_version; __le16 reserved2; __le32 entry;
         * }  -> 28 bytes, immediately after the header.
         */
        if (Size < 4u + 28u) {
            return FALSE;
        }
        Info->TotalSize    = Rd32(Data + 4);
        Info->DataLength   = Rd32(Data + 8);
        Info->DownloadMode = Data[14];
        Info->ProductId    = Rd16(Data + 16);
        Info->RomBuild     = Rd16(Data + 18);
        Info->PatchVersion = Rd16(Data + 20);

        if (Info->DownloadMode > QCA_SKIP_EVT_VSE_CC) {
            return FALSE;
        }
    } else if (Info->Type != QCA_TLV_TYPE_NVM && Info->Type != QCA_ELF_TYPE_PATCH) {
        return FALSE;
    }

    return TRUE;
}

ULONG
QcaTlvSegmentCount(_In_ ULONG FileSize)
{
    if (FileSize == 0) {
        return 0;
    }
    /* The whole file, header included, is streamed (btqca.c: segment = data; remain = size). */
    return (FileSize + QCA_MAX_SIZE_PER_TLV_SEGMENT - 1u) / QCA_MAX_SIZE_PER_TLV_SEGMENT;
}

ULONG
QcaBuildTlvSegmentCommand(
    _In_reads_bytes_(FileSize) const UCHAR *Data,
    _In_ ULONG FileSize,
    _In_ ULONG Index,
    _In_ UCHAR DownloadMode,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity,
    _Out_ BOOLEAN *AckExpected)
{
    ULONG offset;
    ULONG segSize;
    ULONG total;

    *AckExpected = TRUE;

    if (Data == NULL || FileSize == 0 || Index >= QcaTlvSegmentCount(FileSize)) {
        return 0;
    }

    offset  = Index * QCA_MAX_SIZE_PER_TLV_SEGMENT;
    segSize = FileSize - offset;
    if (segSize > QCA_MAX_SIZE_PER_TLV_SEGMENT) {
        segSize = QCA_MAX_SIZE_PER_TLV_SEGMENT;
    }

    /* H4 + opcode(2) + plen(1) + sub(1) + segsize(1) + payload */
    total = 6u + segSize;
    if (OutCapacity < total) {
        return 0;
    }

    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = (UCHAR)(segSize + 2u);          /* HCI parameter length */
    Out[4] = EDL_PATCH_TLV_REQ_CMD;
    Out[5] = (UCHAR)segSize;
    RtlCopyMemory(&Out[6], Data + offset, segSize);

    /*
     * btqca.c: the ack is skipped only for full-size intermediate segments, and only when the
     * file's download mode says so. A short segment or the final segment is always acked.
     */
    if (segSize == QCA_MAX_SIZE_PER_TLV_SEGMENT &&
        offset + segSize < FileSize &&
        (DownloadMode == QCA_SKIP_EVT_VSE_CC || DownloadMode == QCA_SKIP_EVT_CC)) {
        *AckExpected = FALSE;
    }

    return total;
}

ULONG
QcaBuildBaudRateCommand(
    _In_ UCHAR BaudRateIndex,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity)
{
    if (OutCapacity < 5u || BaudRateIndex > QCA_BAUDRATE_3200000) {
        return 0;   /* hci_qca.c rejects > QCA_BAUDRATE_3200000 */
    }

    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(QCA_BAUDRATE_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((QCA_BAUDRATE_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = 1u;
    Out[4] = BaudRateIndex;
    return 5u;
}

ULONG
QcaBuildEdlCommand(
    _In_ UCHAR SubCommand,
    _Out_writes_bytes_to_(OutCapacity, return) UCHAR *Out,
    _In_ ULONG OutCapacity)
{
    if (OutCapacity < 5u) {
        return 0;
    }

    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFFu);
    Out[2] = (UCHAR)((EDL_PATCH_CMD_OPCODE >> 8) & 0xFFu);
    Out[3] = 1u;
    Out[4] = SubCommand;
    return 5u;
}

BOOLEAN
QcaBaudRateToIndex(_In_ ULONG BitsPerSecond, _Out_ UCHAR *Index)
{
    switch (BitsPerSecond) {
    case 115200u:  *Index = QCA_BAUDRATE_115200;  return TRUE;
    case 921600u:  *Index = QCA_BAUDRATE_921600;  return TRUE;
    case 1000000u: *Index = QCA_BAUDRATE_1000000; return TRUE;
    case 2000000u: *Index = QCA_BAUDRATE_2000000; return TRUE;
    case 3000000u: *Index = QCA_BAUDRATE_3000000; return TRUE;
    default:       *Index = 0;                    return FALSE;
    }
}
