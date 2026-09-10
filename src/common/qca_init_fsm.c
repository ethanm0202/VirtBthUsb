/*
 * qca_init_fsm.c - WCN6855 bring-up state machine. See qca_init_fsm.h for the sequence and its
 * source in Linux qca_uart_setup().
 */

#include "../include/qca_init_fsm.h"

/* Standard HCI opcodes used at the tail of bring-up. */
#define HCI_OP_RESET_LOCAL          0x0C03u
#define HCI_OP_READ_LOCAL_VER_LOCAL 0x1001u

static ULONG BuildHciCommand(USHORT Opcode, UCHAR *Out, ULONG Capacity)
{
    if (Capacity < 4u) {
        return 0;
    }
    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(Opcode & 0xFFu);
    Out[2] = (UCHAR)((Opcode >> 8) & 0xFFu);
    Out[3] = 0;
    return 4u;
}

static ULONG BuildDisableLogging(UCHAR *Out, ULONG Capacity)
{
    if (Capacity < 5u) {
        return 0;
    }
    Out[0] = H4_PKT_COMMAND;
    Out[1] = (UCHAR)(QCA_DISABLE_LOGGING & 0xFFu);
    Out[2] = (UCHAR)((QCA_DISABLE_LOGGING >> 8) & 0xFFu);
    Out[3] = 1u;
    Out[4] = QCA_DISABLE_LOGGING_SUB_OP;
    return 5u;
}

BOOLEAN
QcaFsmInit(
    _Out_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(PatchSize) const UCHAR *Patch,
    _In_ ULONG PatchSize,
    _In_reads_bytes_(NvmSize) const UCHAR *Nvm,
    _In_ ULONG NvmSize,
    _In_ ULONG OperBaudRate)
{
    UCHAR baudIndex = 0;

    RtlZeroMemory(Fsm, sizeof(*Fsm));
    Fsm->State = QcaFsmStateFailed;

    if (!QcaParseTlv(Patch, PatchSize, &Fsm->PatchInfo) ||
        Fsm->PatchInfo.Type != QCA_TLV_TYPE_PATCH) {
        return FALSE;
    }
    if (!QcaParseTlv(Nvm, NvmSize, &Fsm->NvmInfo) ||
        Fsm->NvmInfo.Type != QCA_TLV_TYPE_NVM) {
        return FALSE;
    }
    if (!QcaBaudRateToIndex(OperBaudRate, &baudIndex)) {
        return FALSE;
    }

    Fsm->Patch             = Patch;
    Fsm->PatchSize         = PatchSize;
    Fsm->PatchDownloadMode = Fsm->PatchInfo.DownloadMode;
    Fsm->PatchSegments     = QcaTlvSegmentCount(PatchSize);

    Fsm->Nvm               = Nvm;
    Fsm->NvmSize           = NvmSize;
    /* NVM images carry no download-mode field, so every segment is acknowledged. */
    Fsm->NvmDownloadMode   = QCA_SKIP_EVT_NONE;
    Fsm->NvmSegments       = QcaTlvSegmentCount(NvmSize);

    Fsm->OperBaudRate      = OperBaudRate;
    Fsm->OperBaudIndex     = baudIndex;
    Fsm->State             = QcaFsmStateStart;
    return TRUE;
}

QCA_FSM_ACTION
QcaFsmNext(
    _Inout_ PQCA_INIT_FSM Fsm,
    _Out_writes_bytes_to_(QCA_FSM_MAX_COMMAND, *Length) UCHAR *Buffer,
    _Out_ ULONG *Length,
    _Out_ ULONG *BaudRate)
{
    ULONG n = 0;

    *Length = 0;
    *BaudRate = 0;

    switch (Fsm->State) {
    case QcaFsmStateStart:
        Fsm->State = QcaFsmStateVersionRequest;
        n = QcaBuildEdlCommand(EDL_PATCH_VER_REQ_CMD, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateSetBaudRate:
        n = QcaBuildBaudRateCommand(Fsm->OperBaudIndex, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateSwitchHostBaud:
        /*
         * The chip acknowledged the rate change and is now talking at the new speed; the host
         * UART must follow before anything else is sent. Linux does exactly this in
         * qca_setup() -> host_set_baudrate() after qca_set_baudrate().
         */
        *BaudRate = Fsm->OperBaudRate;
        Fsm->State = QcaFsmStatePatchDownload;
        return QcaFsmActionSetHostBaud;

    case QcaFsmStatePatchDownload: {
        BOOLEAN ack = TRUE;
        n = QcaBuildTlvSegmentCommand(Fsm->Patch, Fsm->PatchSize, Fsm->PatchIndex,
                                      Fsm->PatchDownloadMode, Buffer, QCA_FSM_MAX_COMMAND, &ack);
        if (n == 0) {
            Fsm->LastFailureState = (ULONG)Fsm->State;
            Fsm->State = QcaFsmStateFailed;
            return QcaFsmActionFailed;
        }
        *Length = n;
        Fsm->CommandsSent++;
        if (!ack) {
            /* Unacknowledged segment: no event will arrive, so advance here. */
            Fsm->PatchIndex++;
            return QcaFsmActionSendNoWait;
        }
        return QcaFsmActionSend;
    }

    case QcaFsmStateNvmDownload: {
        BOOLEAN ack = TRUE;
        n = QcaBuildTlvSegmentCommand(Fsm->Nvm, Fsm->NvmSize, Fsm->NvmIndex,
                                      Fsm->NvmDownloadMode, Buffer, QCA_FSM_MAX_COMMAND, &ack);
        if (n == 0) {
            Fsm->LastFailureState = (ULONG)Fsm->State;
            Fsm->State = QcaFsmStateFailed;
            return QcaFsmActionFailed;
        }
        *Length = n;
        Fsm->CommandsSent++;
        if (!ack) {
            Fsm->NvmIndex++;
            return QcaFsmActionSendNoWait;
        }
        return QcaFsmActionSend;
    }

    case QcaFsmStateDisableLogging:
        n = BuildDisableLogging(Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateBuildInfo:
        n = QcaBuildEdlCommand(EDL_GET_BUILD_INFO_CMD, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateHciReset:
        n = BuildHciCommand(HCI_OP_RESET_LOCAL, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateReadLocalVersion:
        n = BuildHciCommand(HCI_OP_READ_LOCAL_VER_LOCAL, Buffer, QCA_FSM_MAX_COMMAND);
        break;

    case QcaFsmStateReady:
        return QcaFsmActionDone;

    case QcaFsmStateVersionRequest:
    case QcaFsmStateFailed:
    default:
        /*
         * VersionRequest lands here only if the caller pumps again without delivering the
         * response; treat it as a protocol error rather than silently resending.
         */
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return QcaFsmActionFailed;
    }

    if (n == 0) {
        Fsm->LastFailureState = (ULONG)Fsm->State;
        Fsm->State = QcaFsmStateFailed;
        return QcaFsmActionFailed;
    }

    *Length = n;
    Fsm->CommandsSent++;
    return QcaFsmActionSend;
}

/* An EDL vendor event is: H4 0x04, evt 0xFF, plen, cresp, rtype, payload... */
static BOOLEAN IsVendorEvent(const UCHAR *P, ULONG Length, UCHAR *RType)
{
    if (Length < 5u || P[0] != H4_PKT_EVENT || P[1] != HCI_EV_VENDOR) {
        return FALSE;
    }
    *RType = P[4];
    return TRUE;
}

/* A Command_Complete is: H4 0x04, evt 0x0E, plen, ncmd, opcode lo, opcode hi, status... */
static BOOLEAN IsCommandComplete(const UCHAR *P, ULONG Length, USHORT *Opcode, UCHAR *Status)
{
    if (Length < 7u || P[0] != H4_PKT_EVENT || P[1] != 0x0Eu) {
        return FALSE;
    }
    *Opcode = (USHORT)(P[4] | ((USHORT)P[5] << 8));
    *Status = P[6];
    return TRUE;
}

BOOLEAN
QcaFsmOnPacket(
    _Inout_ PQCA_INIT_FSM Fsm,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    UCHAR  rtype = 0;
    USHORT opcode = 0;
    UCHAR  status = 0;

    switch (Fsm->State) {
    case QcaFsmStateVersionRequest:
        if (IsVendorEvent(Packet, Length, &rtype) && rtype == EDL_PATCH_VER_RES_EVT) {
            Fsm->State = QcaFsmStateSetBaudRate;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateSetBaudRate:
        /*
         * The chip answers the rate change with vendor sub-event 0x92. Some firmware answers
         * with a plain Command_Complete instead, so accept either - refusing one of them would
         * stall bring-up on a difference that carries no information.
         */
        if ((IsVendorEvent(Packet, Length, &rtype) && rtype == EDL_SET_BAUDRATE_RSP_EVT) ||
            (IsCommandComplete(Packet, Length, &opcode, &status) &&
             opcode == QCA_BAUDRATE_CMD_OPCODE && status == 0)) {
            Fsm->State = QcaFsmStateSwitchHostBaud;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStatePatchDownload:
        if (IsVendorEvent(Packet, Length, &rtype) &&
            (rtype == EDL_TVL_DNLD_RES_EVT || rtype == EDL_PATCH_TLV_REQ_CMD)) {
            Fsm->EventsHandled++;
            Fsm->PatchIndex++;
            if (Fsm->PatchIndex >= Fsm->PatchSegments) {
                Fsm->State = QcaFsmStateNvmDownload;
            }
            return TRUE;
        }
        break;

    case QcaFsmStateNvmDownload:
        if (IsVendorEvent(Packet, Length, &rtype) &&
            (rtype == EDL_TVL_DNLD_RES_EVT || rtype == EDL_PATCH_TLV_REQ_CMD)) {
            Fsm->EventsHandled++;
            Fsm->NvmIndex++;
            if (Fsm->NvmIndex >= Fsm->NvmSegments) {
                Fsm->State = QcaFsmStateDisableLogging;
            }
            return TRUE;
        }
        break;

    case QcaFsmStateDisableLogging:
        if (IsCommandComplete(Packet, Length, &opcode, &status) ||
            IsVendorEvent(Packet, Length, &rtype)) {
            Fsm->State = QcaFsmStateBuildInfo;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateBuildInfo:
        if (IsVendorEvent(Packet, Length, &rtype) || IsCommandComplete(Packet, Length, &opcode, &status)) {
            Fsm->State = QcaFsmStateHciReset;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateHciReset:
        if (IsCommandComplete(Packet, Length, &opcode, &status) &&
            opcode == HCI_OP_RESET_LOCAL) {
            if (status != 0) {
                break;   /* a failed reset is fatal, not something to advance past */
            }
            Fsm->State = QcaFsmStateReadLocalVersion;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    case QcaFsmStateReadLocalVersion:
        if (IsCommandComplete(Packet, Length, &opcode, &status) &&
            opcode == HCI_OP_READ_LOCAL_VER_LOCAL && status == 0) {
            Fsm->State = QcaFsmStateReady;
            Fsm->EventsHandled++;
            return TRUE;
        }
        break;

    default:
        break;
    }

    Fsm->UnexpectedEvents++;
    return FALSE;
}

const char *
QcaFsmStateName(_In_ QCA_FSM_STATE State)
{
    switch (State) {
    case QcaFsmStateStart:            return "Start";
    case QcaFsmStateVersionRequest:   return "VersionRequest";
    case QcaFsmStateSetBaudRate:      return "SetBaudRate";
    case QcaFsmStateSwitchHostBaud:   return "SwitchHostBaud";
    case QcaFsmStatePatchDownload:    return "PatchDownload";
    case QcaFsmStateNvmDownload:      return "NvmDownload";
    case QcaFsmStateDisableLogging:   return "DisableLogging";
    case QcaFsmStateBuildInfo:        return "BuildInfo";
    case QcaFsmStateHciReset:         return "HciReset";
    case QcaFsmStateReadLocalVersion: return "ReadLocalVersion";
    case QcaFsmStateReady:            return "Ready";
    case QcaFsmStateFailed:           return "Failed";
    default:                          return "?";
    }
}
