/*
 * qca_selftest.c - validates src/common/qca_tlv.c against the REAL Qualcomm firmware files
 * shipped in this machine's DriverStore.
 *
 * Why this matters: a segmentation or framing error in the firmware download path is not a
 * benign bug. It leaves the WCN6855 with a partially written patch and no Bluetooth until a
 * power cycle. Since the Deck is the only test machine, the codec is proven against the actual
 * 155,044-byte patch and both NVM files here, before M3 ever opens the UART.
 *
 * Build: tools\selftest.cmd
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/include/qca_protocol.h"
static const char *GetFwDir(void)
{
    const char *env = getenv("QCA_FW_DIR");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return "C:\\Windows\\System32\\DriverStore\\FileRepository\\qcbtuart.inf_amd64_2617947e65699545\\";
}
static int g_fail = 0;

#define CHECK(cond, ...)                                   \
    do {                                                   \
        if (cond) { printf("  ok   "); }                   \
        else      { printf("  FAIL "); g_fail++; }         \
        printf(__VA_ARGS__); printf("\n");                 \
    } while (0)

static UCHAR *LoadFile(const char *path, ULONG *size)
{
    FILE *f = NULL;
    UCHAR *buf;
    long len;

    *size = 0;
    if (fopen_s(&f, path, "rb") != 0 || f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }

    buf = (UCHAR *)malloc((size_t)len);
    if (buf == NULL) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (ULONG)len;
    return buf;
}

static void TestCommandFraming(void)
{
    UCHAR out[16];
    ULONG n;

    printf("command framing against the literal byte arrays in hci_qca.c\n");

    /* hci_qca.c qca_set_baudrate: u8 cmd[] = { 0x01, 0x48, 0xFC, 0x01, 0x00 }; */
    n = QcaBuildBaudRateCommand(QCA_BAUDRATE_115200, out, sizeof(out));
    CHECK(n == 5, "baud command is 5 bytes (got %lu)", n);
    CHECK(out[0] == 0x01 && out[1] == 0x48 && out[2] == 0xFC && out[3] == 0x01 && out[4] == 0x00,
          "matches { 01 48 FC 01 00 } exactly: { %02X %02X %02X %02X %02X }",
          out[0], out[1], out[2], out[3], out[4]);

    n = QcaBuildBaudRateCommand(QCA_BAUDRATE_3000000, out, sizeof(out));
    CHECK(n == 5 && out[4] == 14, "3,000,000 baud encodes as enum index 14 (got %u)", out[4]);

    CHECK(QcaBuildBaudRateCommand(18, out, sizeof(out)) == 0,
          "index above QCA_BAUDRATE_3200000 (17) is rejected");
    CHECK(QcaBuildBaudRateCommand(QCA_BAUDRATE_115200, out, 4) == 0,
          "short output buffer is rejected");

    /* hci_qca.c: const u8 edl_reset_soc_cmd[] = { 0x01, 0x00, 0xFC, 0x01, 0x05 }; */
    n = QcaBuildEdlCommand(EDL_PATCH_RESET_SOC_CMD, out, sizeof(out));
    CHECK(n == 5 && out[0] == 0x01 && out[1] == 0x00 && out[2] == 0xFC &&
          out[3] == 0x01 && out[4] == 0x05,
          "SOC reset matches { 01 00 FC 01 05 }");

    n = QcaBuildEdlCommand(EDL_PATCH_VER_REQ_CMD, out, sizeof(out));
    CHECK(n == 5 && out[4] == 0x19, "version request carries sub-command 0x19");

    printf("baud rate mapping\n");
    {
        UCHAR idx = 0xFF;
        CHECK(QcaBaudRateToIndex(3000000, &idx) && idx == 14, "3000000 -> 14");
        CHECK(QcaBaudRateToIndex(115200, &idx) && idx == 0, "115200 -> 0");
        CHECK(QcaBaudRateToIndex(921600, &idx) && idx == 10, "921600 -> 10 (documented fallback)");
        CHECK(!QcaBaudRateToIndex(4000000, &idx), "4000000 rejected (above the 3,200,000 cap)");
    }
}

/*
 * Streams the firmware file through the segment builder, reassembles the
 * payloads, and asserts they match the input file.
 */
static void TestRealFirmware(const char *name, ULONG expectSize, UCHAR expectType,
                             ULONG expectSegments, ULONG expectLastSegment)
{
    char path[MAX_PATH];
    ULONG size = 0;
    UCHAR *fw;
    QCA_TLV_INFO info;
    ULONG segments;
    UCHAR *rebuilt;
    ULONG rebuiltLen = 0;
    ULONG acked = 0, unacked = 0;
    ULONG lastSegSize = 0;
    int framingOk = 1;

    sprintf_s(path, sizeof(path), "%s%s", GetFwDir(), name);
    printf("%s\n", path);

    fw = LoadFile(path, &size);
    if (fw == NULL) {
        printf("  FAIL cannot open (is the DriverStore package still present?)\n");
        g_fail++;
        return;
    }

    CHECK(size == expectSize, "size %lu bytes (want %lu)", size, expectSize);

    CHECK(QcaParseTlv(fw, size, &info), "TLV header parses and self-validates");
    CHECK(info.Type == expectType, "type %u (want %u)", info.Type, expectType);
    CHECK(info.Length == size - 4u, "declared length %lu == size-4", info.Length);

    if (info.Type == QCA_TLV_TYPE_PATCH) {
        CHECK(info.DownloadMode <= QCA_SKIP_EVT_VSE_CC,
              "download mode %u is a valid qca_tlv_dnld_mode", info.DownloadMode);
        CHECK(info.TotalSize > 0 && info.TotalSize <= size,
              "patch total_size %lu fits in the file", info.TotalSize);
        CHECK(info.DataLength < info.TotalSize,
              "data_length %lu < total_size %lu", info.DataLength, info.TotalSize);
        printf("       product=0x%04X rom_build=0x%04X patch_ver=0x%04X dnld_mode=%u\n",
               info.ProductId, info.RomBuild, info.PatchVersion, info.DownloadMode);
    }

    segments = QcaTlvSegmentCount(size);
    CHECK(segments == expectSegments, "%lu segments at 243 bytes (want %lu)", segments, expectSegments);

    rebuilt = (UCHAR *)malloc(size);
    if (rebuilt == NULL) { free(fw); printf("  FAIL out of memory\n"); g_fail++; return; }

    for (ULONG i = 0; i < segments; i++) {
        UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
        BOOLEAN ack = FALSE;
        ULONG n = QcaBuildTlvSegmentCommand(fw, size, i, info.DownloadMode,
                                            cmd, sizeof(cmd), &ack);
        ULONG segSize;

        if (n == 0) { printf("  FAIL segment %lu build failed\n", i); g_fail++; framingOk = 0; break; }

        segSize = n - 6u;
        if (cmd[0] != H4_PKT_COMMAND ||
            cmd[1] != (UCHAR)(EDL_PATCH_CMD_OPCODE & 0xFF) ||
            cmd[2] != (UCHAR)(EDL_PATCH_CMD_OPCODE >> 8) ||
            cmd[3] != (UCHAR)(segSize + 2u) ||
            cmd[4] != EDL_PATCH_TLV_REQ_CMD ||
            cmd[5] != (UCHAR)segSize) {
            printf("  FAIL segment %lu header wrong: %02X %02X %02X %02X %02X %02X\n",
                   i, cmd[0], cmd[1], cmd[2], cmd[3], cmd[4], cmd[5]);
            g_fail++; framingOk = 0; break;
        }

        if (segSize > QCA_MAX_SIZE_PER_TLV_SEGMENT) {
            printf("  FAIL segment %lu oversized: %lu\n", i, segSize); g_fail++; framingOk = 0; break;
        }

        memcpy(rebuilt + rebuiltLen, &cmd[6], segSize);
        rebuiltLen += segSize;
        lastSegSize = segSize;
        if (ack) { acked++; } else { unacked++; }
    }

    CHECK(framingOk, "every segment carries a well-formed 0xFC00/0x1E command header");
    CHECK(rebuiltLen == size, "reassembled %lu bytes == file size %lu", rebuiltLen, size);
    CHECK(rebuiltLen == size && memcmp(rebuilt, fw, size) == 0,
          "reassembled payload is byte-identical to the file");
    CHECK(lastSegSize == expectLastSegment, "final segment %lu bytes (want %lu)",
          lastSegSize, expectLastSegment);

    /* btqca.c: the last segment is always acked, whatever the download mode says. */
    {
        UCHAR cmd[QCA_MAX_SIZE_PER_TLV_SEGMENT + 16];
        BOOLEAN ack = FALSE;
        (void)QcaBuildTlvSegmentCommand(fw, size, segments - 1, QCA_SKIP_EVT_VSE_CC,
                                        cmd, sizeof(cmd), &ack);
        CHECK(ack, "final segment is acked even with download mode VSE_CC");
    }
    printf("       acked=%lu unacked=%lu (mode %u)\n", acked, unacked, info.DownloadMode);

    free(rebuilt);
    free(fw);
}

static void TestRejections(void)
{
    QCA_TLV_INFO info;
    UCHAR buf[64];
    UCHAR cmd[16];
    BOOLEAN ack;

    printf("malformed input is rejected rather than half-flashed\n");
    memset(buf, 0, sizeof(buf));

    CHECK(!QcaParseTlv(buf, 3, &info), "3-byte buffer rejected (header is 4 bytes)");

    /* type 1 (patch) with a length that disagrees with the buffer size */
    buf[0] = 0x01; buf[1] = 0xFF; buf[2] = 0xFF; buf[3] = 0x00;
    CHECK(!QcaParseTlv(buf, sizeof(buf), &info), "length/size mismatch rejected");

    /* well-formed length but an unknown type */
    {
        ULONG len = sizeof(buf) - 4u;
        buf[0] = 0x77;
        buf[1] = (UCHAR)(len & 0xFF);
        buf[2] = (UCHAR)((len >> 8) & 0xFF);
        buf[3] = (UCHAR)((len >> 16) & 0xFF);
        CHECK(!QcaParseTlv(buf, sizeof(buf), &info), "unknown TLV type rejected");
    }

    CHECK(QcaTlvSegmentCount(0) == 0, "zero-length file yields zero segments");
    CHECK(QcaTlvSegmentCount(243) == 1, "exactly 243 bytes is one segment");
    CHECK(QcaTlvSegmentCount(244) == 2, "244 bytes is two segments");

    CHECK(QcaBuildTlvSegmentCommand(buf, sizeof(buf), 99, 0, cmd, sizeof(cmd), &ack) == 0,
          "out-of-range segment index rejected");
    CHECK(QcaBuildTlvSegmentCommand(buf, sizeof(buf), 0, 0, cmd, 8, &ack) == 0,
          "insufficient output capacity rejected");
}

int main(void)
{
    TestCommandFraming();
    printf("\n");
    TestRejections();
    printf("\n");
    /* Expected values measured from these exact files on this machine. */
    TestRealFirmware("hpbtfw21.tlv", 155044, QCA_TLV_TYPE_PATCH, 639, 10);
    printf("\n");
    TestRealFirmware("hpnv21.bin",     6610, QCA_TLV_TYPE_NVM,    28, 49);
    printf("\n");
    TestRealFirmware("hpnv21g.bin",    6450, QCA_TLV_TYPE_NVM,    27, 132);

    printf("\n%s\n", g_fail ? "QCA SELFTEST FAILED" : "QCA SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
