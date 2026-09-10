/*
 * qca_init_fsm.h - Transport-agnostic Qualcomm WCN6855 bring-up state machine.
 *
 * Sequence derived from Linux qca_uart_setup() in drivers/bluetooth/btqca.c:
 *   1. EDL version request           (0xFC00 / 0x19)      at 115200
 *   2. set baud rate                 (0xFC48)             -> host re-programs UART
 *   3. stream the rampatch TLV       (0xFC00 / 0x1E)      hpbtfw21.tlv
 *   4. stream the NVM TLV            (0xFC00 / 0x1E)      hpnv21.bin or hpnv21g.bin
 *   5. disable SOC logging           (0xFC17 / 0x14)      WCN6855-specific
 *   6. get firmware build info       (0xFC00 / 0x20)
 *   7. HCI_Reset                     (0x0C03)
 *   8. HCI_Read_Local_Version        (0x1001)
 */

#pragma once

#include "qca_protocol.h"

typedef enum _QCA_FSM_STATE {
    QcaFsmStateStart = 0,
    QcaFsmStateVersionRequest,
    QcaFsmStateSetBaudRate,
    QcaFsmStateSwitchHostBaud,
    QcaFsmStatePatchDownload,
    QcaFsmStateNvmDownload,
    QcaFsmStateDisableLogging,
    QcaFsmStateBuildInfo,
    QcaFsmStateHciReset,
    QcaFsmStateReadLocalVersion,
    QcaFsmStateReady,
    QcaFsmStateFailed
} QCA_FSM_STATE;

typedef enum _QCA_FSM_ACTION {
    QcaFsmActionSend = 0,      /* write Buffer, then wait for an event before pumping again */
    QcaFsmActionSendNoWait,    /* write Buffer, pump again immediately (unacked TLV segment)  */
    QcaFsmActionSetHostBaud,   /* reprogram the host UART to BaudRate, then pump again        */
    QcaFsmActionDone,
    QcaFsmActionFailed
} QCA_FSM_ACTION;

#define QCA_FSM_MAX_COMMAND (QCA_MAX_SIZE_PER_TLV_SEGMENT + 16u)

typedef struct _QCA_INIT_FSM {
    QCA_FSM_STATE  State;

    const UCHAR   *Patch;
    ULONG          PatchSize;
    UCHAR          PatchDownloadMode;
    ULONG          PatchSegments;
    ULONG          PatchIndex;

    const UCHAR   *Nvm;
    ULONG          NvmSize;
    UCHAR          NvmDownloadMode;
    ULONG          NvmSegments;
    ULONG          NvmIndex;

    ULONG          OperBaudRate;
    UCHAR          OperBaudIndex;

    /* Observability - a stuck bring-up is diagnosed from these, not from a debugger. */
    ULONG          CommandsSent;
    ULONG          EventsHandled;
    ULONG          LastFailureState;
    ULONG          UnexpectedEvents;

    QCA_TLV_INFO   PatchInfo;
    QCA_TLV_INFO   NvmInfo;
} QCA_INIT_FSM, *PQCA_INIT_FSM;

/*
 * Validates both images and arms the machine. Returns FALSE (and sets State to Failed) if either
 * TLV fails validation or the requested baud rate is unsupported - the point being that a bad
 * image is rejected before a single byte reaches the chip.
 */
BOOLEAN QcaFsmInit(
    _Out_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(PatchSize) const UCHAR *Patch,
    _In_ ULONG PatchSize,
    _In_reads_bytes_(NvmSize) const UCHAR *Nvm,
    _In_ ULONG NvmSize,
    _In_ ULONG OperBaudRate);

/*
 * Produces the next thing the caller must do. On QcaFsmActionSend/SendNoWait, *Length bytes of
 * *Buffer must be written to the UART. On QcaFsmActionSetHostBaud, *BaudRate is the new speed.
 */
QCA_FSM_ACTION QcaFsmNext(
    _Inout_ PQCA_INIT_FSM Fsm,
    _Out_writes_bytes_to_(QCA_FSM_MAX_COMMAND, *Length) UCHAR *Buffer,
    _Out_ ULONG *Length,
    _Out_ ULONG *BaudRate);

/*
 * Feeds one complete received packet (H4 byte included). Returns FALSE if the packet is not the
 * response the current state was waiting for, in which case UnexpectedEvents is incremented and
 * the state machine does NOT advance - a resend or timeout is the caller's decision.
 */
BOOLEAN QcaFsmOnPacket(
    _Inout_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length);

const char *QcaFsmStateName(_In_ QCA_FSM_STATE State);
