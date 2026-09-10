/*
 * qca_fsm_selftest.c - drives the real src/common/qca_init_fsm.c through a complete WCN6855
 * bring-up against a mock chip, using the actual firmware images from the DriverStore.
 *
 * This is the closest thing to a hardware test that is possible without the hardware: the
 * Deck's UART peripheral (ACPI\QCOM2066) is owned by qcbtuart.sys and is not reachable from
 * user mode at all, so the sequencing logic is proven here instead.
 *
 * Build: tools\selftest.cmd
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/include/qca_init_fsm.h"
static const char *GetFwDir(void)
{
    const char *env = getenv("QCA_FW_DIR");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return "C:\\Windows\\System32\\DriverStore\\FileRepository\\qcbtuart.inf_amd64_2617947e65699545\\";
}
static int g_fail = 0;

#define CHECK(cond, ...)                           \
    do {                                           \
        if (cond) { printf("  ok   "); }           \
        else      { printf("  FAIL "); g_fail++; } \
        printf(__VA_ARGS__); printf("\n");         \
    } while (0)

static UCHAR *LoadFile(const char *name, ULONG *size)
{
    char path[MAX_PATH];
    FILE *f = NULL;
    UCHAR *buf;
    long len;

    *size = 0;
    sprintf_s(path, sizeof(path), "%s%s", GetFwDir(), name);
    if (fopen_s(&f, path, "rb") != 0 || f == NULL) { return NULL; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    buf = (UCHAR *)malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (ULONG)len;
    return buf;
}

/* ---------------------------------------------------------------- mock chip */

typedef struct {
    ULONG BytesReceived;
    ULONG CommandsReceived;
    ULONG TlvSegmentsReceived;
    ULONG HostBaudSwitches;
    ULONG BaudAtFirstPatchSegment;   /* records the ordering bug if the switch comes too late */
    ULONG CurrentHostBaud;
    UCHAR Reply[64];
    ULONG ReplyLength;
} MOCK_CHIP;

static void MockVendorEvent(MOCK_CHIP *c, UCHAR rtype, UCHAR payload)
{
    c->Reply[0] = H4_PKT_EVENT;
    c->Reply[1] = HCI_EV_VENDOR;
    c->Reply[2] = 3;            /* plen: cresp, rtype, payload */
    c->Reply[3] = EDL_CMD_REQ_RES_EVT;
    c->Reply[4] = rtype;
    c->Reply[5] = payload;
    c->ReplyLength = 6;
}

static void MockCommandComplete(MOCK_CHIP *c, USHORT opcode, UCHAR status)
{
    c->Reply[0] = H4_PKT_EVENT;
    c->Reply[1] = 0x0E;
    c->Reply[2] = 4;
    c->Reply[3] = 1;
    c->Reply[4] = (UCHAR)(opcode & 0xFF);
    c->Reply[5] = (UCHAR)(opcode >> 8);
    c->Reply[6] = status;
    c->ReplyLength = 7;
}

/* Consumes one command and, when the chip would answer, stages the reply. */
static void MockConsume(MOCK_CHIP *c, const UCHAR *cmd, ULONG len)
{
    USHORT opcode;

    c->ReplyLength = 0;
    c->BytesReceived += len;
    if (len < 4 || cmd[0] != H4_PKT_COMMAND) { return; }
    c->CommandsReceived++;

    opcode = (USHORT)(cmd[1] | ((USHORT)cmd[2] << 8));

    if (opcode == EDL_PATCH_CMD_OPCODE) {
        UCHAR sub = cmd[4];
        if (sub == EDL_PATCH_VER_REQ_CMD) {
            MockVendorEvent(c, EDL_PATCH_VER_RES_EVT, 0);
        } else if (sub == EDL_PATCH_TLV_REQ_CMD) {
            c->TlvSegmentsReceived++;
            if (c->TlvSegmentsReceived == 1) {
                c->BaudAtFirstPatchSegment = c->CurrentHostBaud;
            }
            MockVendorEvent(c, EDL_TVL_DNLD_RES_EVT, 0);
        } else if (sub == EDL_GET_BUILD_INFO_CMD) {
            MockVendorEvent(c, EDL_CMD_REQ_RES_EVT, 0);
        }
        return;
    }
    if (opcode == QCA_BAUDRATE_CMD_OPCODE) {
        MockVendorEvent(c, EDL_SET_BAUDRATE_RSP_EVT, 0);
        return;
    }
    if (opcode == QCA_DISABLE_LOGGING) {
        MockCommandComplete(c, opcode, 0);
        return;
    }
    MockCommandComplete(c, opcode, 0);   /* HCI_Reset, Read_Local_Version */
}

/*
 * Pumps the FSM to completion. `deliver` controls whether staged replies are fed back, so the
 * same driver can be used to test the timeout/no-reply path.
 */
static QCA_FSM_ACTION RunToCompletion(QCA_INIT_FSM *fsm, MOCK_CHIP *chip, ULONG maxSteps,
                                      ULONG *steps)
{
    UCHAR cmd[QCA_FSM_MAX_COMMAND];
    ULONG len = 0, baud = 0;
    QCA_FSM_ACTION action = QcaFsmActionFailed;

    *steps = 0;
    while ((*steps)++ < maxSteps) {
        action = QcaFsmNext(fsm, cmd, &len, &baud);
        if (action == QcaFsmActionDone || action == QcaFsmActionFailed) { break; }

        if (action == QcaFsmActionSetHostBaud) {
            chip->HostBaudSwitches++;
            chip->CurrentHostBaud = baud;
            continue;
        }

        MockConsume(chip, cmd, len);
        if (action == QcaFsmActionSend) {
            if (chip->ReplyLength == 0) { return QcaFsmActionFailed; }
            if (!QcaFsmOnPacket(fsm, chip->Reply, chip->ReplyLength)) {
                return QcaFsmActionFailed;
            }
        }
    }
    return action;
}

int main(void)
{
    ULONG patchSize = 0, nvmSize = 0;
    UCHAR *patch = LoadFile("hpbtfw21.tlv", &patchSize);
    UCHAR *nvm   = LoadFile("hpnv21.bin",   &nvmSize);
    QCA_INIT_FSM fsm;
    MOCK_CHIP chip;
    ULONG steps = 0;
    QCA_FSM_ACTION action;
    ULONG expectedSegments;

    if (!patch || !nvm) {
        printf("FAIL cannot read the firmware images from %s\n", GetFwDir());
        printf("Set QCA_FW_DIR=<path> to specify the directory containing hpbtfw21.tlv and hpnv21.bin.\n");
        return 1;
    }

    printf("initialisation guards\n");
    CHECK(QcaFsmInit(&fsm, patch, patchSize, nvm, nvmSize, 3000000),
          "accepts the real images at 3,000,000 baud");
    CHECK(fsm.PatchSegments == 639 && fsm.NvmSegments == 28,
          "segment counts 639 / 28 (got %lu / %lu)", fsm.PatchSegments, fsm.NvmSegments);
    CHECK(fsm.PatchDownloadMode == QCA_SKIP_EVT_VSE_CC,
          "patch declares download mode 3, so intermediate segments go unacked");
    CHECK(!QcaFsmInit(&fsm, patch, patchSize, nvm, nvmSize, 4000000),
          "rejects an unsupported baud rate before touching the chip");
    CHECK(!QcaFsmInit(&fsm, nvm, nvmSize, nvm, nvmSize, 3000000),
          "rejects an NVM image supplied as the rampatch");
    CHECK(!QcaFsmInit(&fsm, patch, patchSize, patch, patchSize, 3000000),
          "rejects a rampatch supplied as the NVM image");
    CHECK(!QcaFsmInit(&fsm, patch, 3, nvm, nvmSize, 3000000),
          "rejects a truncated rampatch");

    printf("\nfull bring-up against the mock chip\n");
    memset(&chip, 0, sizeof(chip));
    chip.CurrentHostBaud = 115200;
    if (!QcaFsmInit(&fsm, patch, patchSize, nvm, nvmSize, 3000000)) {
        printf("  FAIL re-init failed\n");
        return 1;
    }

    action = RunToCompletion(&fsm, &chip, 2000, &steps);

    CHECK(action == QcaFsmActionDone, "reaches Done (action=%d, state=%s)",
          (int)action, QcaFsmStateName(fsm.State));
    CHECK(fsm.State == QcaFsmStateReady, "final state Ready (got %s)", QcaFsmStateName(fsm.State));
    CHECK(fsm.UnexpectedEvents == 0, "no unexpected events (got %lu)", fsm.UnexpectedEvents);

    expectedSegments = 639 + 28;
    CHECK(chip.TlvSegmentsReceived == expectedSegments,
          "chip received %lu TLV segments (want %lu)", chip.TlvSegmentsReceived, expectedSegments);

    /* version + baud + segments + disable-logging + build-info + reset + read-version */
    CHECK(chip.CommandsReceived == expectedSegments + 6,
          "chip received %lu commands (want %lu)", chip.CommandsReceived, expectedSegments + 6);
    CHECK(fsm.CommandsSent == chip.CommandsReceived,
          "FSM command counter agrees with the chip (%lu vs %lu)",
          fsm.CommandsSent, chip.CommandsReceived);

    printf("\nbaud rate switch ordering\n");
    CHECK(chip.HostBaudSwitches == 1, "host UART reprogrammed exactly once (got %lu)",
          chip.HostBaudSwitches);
    CHECK(chip.BaudAtFirstPatchSegment == 3000000,
          "first patch segment is sent at 3,000,000 baud, not %lu",
          chip.BaudAtFirstPatchSegment);

    printf("\nunexpected traffic does not advance the machine\n");
    {
        UCHAR junk[7] = { H4_PKT_EVENT, 0x0E, 4, 1, 0x99, 0x99, 0 };
        QCA_FSM_STATE before;
        memset(&chip, 0, sizeof(chip));
        QcaFsmInit(&fsm, patch, patchSize, nvm, nvmSize, 3000000);
        {
            UCHAR cmd[QCA_FSM_MAX_COMMAND];
            ULONG len = 0, baud = 0;
            (void)QcaFsmNext(&fsm, cmd, &len, &baud);   /* -> VersionRequest, awaiting a reply */
        }
        before = fsm.State;
        CHECK(!QcaFsmOnPacket(&fsm, junk, sizeof(junk)),
              "a Command_Complete for an unrelated opcode is rejected");
        CHECK(fsm.State == before, "state unchanged after a rejected packet");
        CHECK(fsm.UnexpectedEvents == 1, "the rejection is counted for diagnosis");
    }

    printf("\nsilent chip is reported, not spun on\n");
    {
        MOCK_CHIP dead;
        memset(&dead, 0, sizeof(dead));
        dead.CurrentHostBaud = 115200;
        QcaFsmInit(&fsm, patch, patchSize, nvm, nvmSize, 3000000);
        /* RunToCompletion bails out as soon as a Send gets no staged reply. */
        {
            UCHAR cmd[QCA_FSM_MAX_COMMAND];
            ULONG len = 0, baud = 0;
            QCA_FSM_ACTION a = QcaFsmNext(&fsm, cmd, &len, &baud);
            CHECK(a == QcaFsmActionSend, "first action is a send that expects a reply");
            a = QcaFsmNext(&fsm, cmd, &len, &baud);
            CHECK(a == QcaFsmActionFailed && fsm.State == QcaFsmStateFailed,
                  "pumping again without a reply fails loudly instead of resending blindly");
        }
    }

    free(patch);
    free(nvm);
    printf("\n%s\n", g_fail ? "QCA FSM SELFTEST FAILED" : "QCA FSM SELFTEST PASSED");
    return g_fail ? 1 : 0;
}
