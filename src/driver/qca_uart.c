/*
 * qca_uart.c - Qualcomm QCA2066 SerCx2 UART backend for DeckBtUsb.
 *
 * Implements the physical UART transport connecting the virtual USB front end
 * (src/driver/endpoints.c) and steady-state HCI bridge (src/common/hci_bridge.c)
 * to the Qualcomm QCA2066 controller over ACPI\QCOM2066 SerCx2 UART.
 *
 * Upstream Linux references:
 *   btqca.c: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 *   hci_qca.c: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 *   btqca.h: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.h?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 *
 * Protocol constraints:
 * 1. Baud switch opcode: 0xFC48 ({ 0x01, 0x48, 0xFC, 0x01, <rate_idx> }), hci_qca.c (qca_set_baudrate).
 * 2. Baud rate index: enum index (0=115200, 10=921600, 11=1000000, 13=2000000, 14=3000000). Rates above
 *    3,200,000 (index 17) are rejected. Upstream btqca.h (enum qca_baudrate).
 * 3. Operational baud rate: 3,000,000 baud (index 14), hci_qca.c (qca_setup).
 * 4. TLV download opcode: 0xFC00 with sub-command 0x1E (EDL_PATCH_TLV_REQ_CMD). Segment size: 243 bytes
 *    (QCA_MAX_SIZE_PER_TLV_SEGMENT). Parameter length = seglen + 2. Upstream btqca.c (qca_tlv_send_segment).
 * 5. Last segment / short segment: always acknowledged by the controller, btqca.c (qca_download_firmware).
 * 6. Vendor event code: HCI_EV_VENDOR = 0xFF, btqca.c.
 * 7. Disable SoC logging: opcode 0xFC17, sub-op 0x14, answered by Command_Complete, btqca.c (qca_disable_soc_logging).
 * 8. QCA2066 baud switch flow control bracketing: flow control is disabled before changing baud, and re-enabled afterward,
 *    hci_qca.c (qca_set_baudrate).
 * 9. Hardware flow control: RTS/CTS is required on Qualcomm Bluetooth UART, hci_qca.c (qca_setup).
 *
 * Locking and notification rules:
 * Uart->Lock is the front end's controller spin lock, adopted in QcaUartBindTransport. HCI_BRIDGE
 * has exactly one lock domain: the front end mutates FIFO indices and ACL credits under that lock,
 * and this backend mutates the same fields from the read-completion DPC.
 *
 * HciTransportNotify is called with no lock held that any Submit/Has/Pop/Reset path acquires.
 * In Phase 2 the packet is queued to the bridge under Uart->Lock, the lock is released, and
 * only then is Notify invoked, and only if the bridge accepted the packet.
 *
 * WriteLock serialises write-slot allocation. Lock order is always Uart->Lock then WriteLock,
 * never the reverse. Outbound packets from SubmitCommand/SubmitAcl/SubmitSco operate at <= DISPATCH_LEVEL
 * with spin locks held and use pre-allocated asynchronous write slots without blocking.
 */

#include <ntddk.h>
#include <wdf.h>
#include <ntddser.h>
#include <ntstrsafe.h>
#include <reshub.h>

#include "deckbtusb.h"
#include "qca_uart.h"

#define QCA_UART_POOL_TAG               DECKBT_POOL_TAG

/*
 * Stable package-owned locations.  The INF must deliberately stage these payloads; never reach
 * into another driver's hash-named DriverStore directory, whose name and lifetime it controls.
 */
#define QCA_FW_DEFAULT_PATCH_PATH L"\\SystemRoot\\System32\\drivers\\DeckBtUsb\\hpbtfw21.tlv"

typedef struct _QCA_NVM_FILE_ENTRY {
    const char *Name;
    PCWSTR      Path;
} QCA_NVM_FILE_ENTRY;

static const QCA_NVM_FILE_ENTRY g_QcaNvmFiles[] = {
    { "hpnv21.bin",   L"\\SystemRoot\\System32\\drivers\\DeckBtUsb\\hpnv21.bin" },
    { "hpnv21g.bin",  L"\\SystemRoot\\System32\\drivers\\DeckBtUsb\\hpnv21g.bin" },
    { "hpnv21.309",   L"\\SystemRoot\\System32\\drivers\\DeckBtUsb\\hpnv21.309" },
    { "hpnv21g.309",  L"\\SystemRoot\\System32\\drivers\\DeckBtUsb\\hpnv21g.309" }
};
#define QCA_NVM_FILE_COUNT (sizeof(g_QcaNvmFiles) / sizeof(g_QcaNvmFiles[0]))

/* Forward declarations */
static EVT_WDF_REQUEST_COMPLETION_ROUTINE QcaUartReadCompletion;
static EVT_WDF_REQUEST_COMPLETION_ROUTINE QcaUartWriteSlotCompletion;
static void QcaUartOnH4Packet(void *Context, unsigned char Type, const unsigned char *Payload, unsigned long Length);
static unsigned char QcaUartWireSendCommand(void *Context, const unsigned char *Packet, unsigned long Length);
static unsigned char QcaUartWireSendAcl(void *Context, const unsigned char *Packet, unsigned long Length);
static unsigned char QcaUartWireSendSco(void *Context, const unsigned char *Packet, unsigned long Length);
static NTSTATUS QcaUartWakeController(_Inout_ PQCA_UART Uart, _Inout_ PQCA_UART_RECORD Record);
static BOOLEAN QcaUartOnIbsByte(void *Context, unsigned char Byte);
static NTSTATUS QcaUartServeSteady(_Inout_ PQCA_UART Uart);

static unsigned char QcaUartSubmitCommand(HCI_TRANSPORT *Transport, const unsigned char *Packet, unsigned long Length);
static unsigned char QcaUartSubmitAcl(HCI_TRANSPORT *Transport, const unsigned char *Packet, unsigned long Length);
static unsigned char QcaUartSubmitSco(HCI_TRANSPORT *Transport, const unsigned char *Packet, unsigned long Length);
static unsigned char QcaUartHasStream(const HCI_TRANSPORT *Transport, HCI_STREAM Stream);
static unsigned char QcaUartPopStream(HCI_TRANSPORT *Transport, HCI_STREAM Stream, unsigned char *Buffer, unsigned long Capacity, unsigned long *Written);
static unsigned long QcaUartLastEventLength(const HCI_TRANSPORT *Transport);
static void QcaUartResetTransport(HCI_TRANSPORT *Transport);
/* Wire ops structure provided to HCI_BRIDGE */
static const HCI_BRIDGE_WIRE_OPS g_QcaUartWireOps = {
    QcaUartWireSendCommand,
    QcaUartWireSendAcl,
    QcaUartWireSendSco
};


static const HCI_TRANSPORT_OPS g_QcaUartTransportOps = {
    QcaUartSubmitCommand,
    QcaUartSubmitAcl,
    QcaUartSubmitSco,
    QcaUartHasStream,
    QcaUartPopStream,
    QcaUartLastEventLength,
    QcaUartResetTransport
};

static ULONG
QcaUartRequestBudget(_Inout_ PQCA_UART Uart, _In_ ULONG MaximumMs)
{
    ULONGLONG elapsed;
    ULONG remaining;
    if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
        return 0;
    }
    if (InterlockedCompareExchange(&Uart->ProbeActive, 0, 0) == 0 ||
        InterlockedCompareExchange(&Uart->BudgetUnbounded, 0, 0) != 0) {
        /* No detached work, or a steady session serving BTHUSB: only a stop request ends it. */
        return MaximumMs;
    }
    elapsed = (KeQueryInterruptTime() - Uart->ProbeStarted) / 10000;
    if (elapsed >= 60000) {
        InterlockedExchange(&Uart->StopRequested, 1);
        KeSetEvent(&Uart->ProbeStop, IO_NO_INCREMENT, FALSE);
        KeSetEvent(&Uart->FsmEvent, IO_NO_INCREMENT, FALSE);
        return 0;
    }
    remaining = 60000 - (ULONG)elapsed;
    return remaining < MaximumMs ? remaining : MaximumMs;
}

static BOOLEAN
QcaUartIsIoIdle(_Inout_ PQCA_UART Uart)
{
    KIRQL irql;
    BOOLEAN idle;
    /* Zero is not retirement until the last callback has also finished signaling IoIdle. */
    KeAcquireSpinLock(&Uart->TargetLock, &irql);
    idle = Uart->ActiveIoCount == 0;
    KeReleaseSpinLock(&Uart->TargetLock, irql);
    return idle;
}

static VOID
QcaUartIoCompleted(_Inout_ PQCA_UART Uart)
{
    KIRQL irql;
    KeAcquireSpinLock(&Uart->TargetLock, &irql);
    if (InterlockedDecrement(&Uart->ActiveIoCount) == 0) {
        KeSetEvent(&Uart->IoIdle, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&Uart->TargetLock, irql);
}

/*
 * Sends Request with the worker's remaining budget as its timeout. A steady-state read is the
 * controller's idle line and may pend for as long as nothing is said; it is sent without a
 * timeout and retires only through cancellation (reader quiesce or terminal purge).
 */
static NTSTATUS
QcaUartSendTimed(_Inout_ PQCA_UART Uart, _In_ WDFREQUEST Request, _In_ BOOLEAN Read)
{
    WDF_REQUEST_SEND_OPTIONS options;
    KIRQL irql;
    NTSTATUS status;
    ULONG budgetMs = QcaUartRequestBudget(Uart, 5000);
    if (budgetMs == 0) {
        return STATUS_CANCELLED;
    }
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, 0);
    if (!Read || InterlockedCompareExchange(&Uart->BudgetUnbounded, 0, 0) == 0) {
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(budgetMs));
    }
    KeAcquireSpinLock(&Uart->TargetLock, &irql);
    InterlockedIncrement(&Uart->ActiveIoCount);
    KeClearEvent(&Uart->IoIdle);
    KeReleaseSpinLock(&Uart->TargetLock, irql);
    if (WdfRequestSend(Request, Uart->IoTarget, &options)) {
        return STATUS_SUCCESS;
    }
    status = WdfRequestGetStatus(Request);
    QcaUartIoCompleted(Uart);
    return status;
}

static VOID
QcaUartPurgeTarget(_Inout_ PQCA_UART Uart)
{
    WDFIOTARGET target;
    KIRQL irql;

    KeAcquireSpinLock(&Uart->TargetLock, &irql);
    target = Uart->CancelTarget;
    if (target != NULL) {
        /*
         * Terminal, one-shot purge per opened target. Claiming the target prevents concurrent
         * PnP/monitor/worker purges; WDF requires its target-state operations to be serialized.
         * A reference alone does not prevent Close, so retain counted ownership until return.
         */
        Uart->CancelTarget = NULL;
        WdfObjectReference(target);
        InterlockedIncrement(&Uart->ActiveIoCount);
        KeClearEvent(&Uart->IoIdle);
    }
    KeReleaseSpinLock(&Uart->TargetLock, irql);
    if (target != NULL) {
        /* Never hold TargetLock across a WDF call that can complete requests inline. */
        WdfIoTargetPurge(target, WdfIoTargetPurgeIo);
        WdfObjectDereference(target);
        QcaUartIoCompleted(Uart);
    }
}

VOID
QcaUartCancelProbe(_Inout_ PQCA_UART Uart)
{
    if (InterlockedCompareExchange(&Uart->ProbeActive, 0, 0) == 0) {
        return;
    }
    InterlockedExchange(&Uart->StopRequested, 1);
    InterlockedExchange(&Uart->ReadPumpRunning, 0);
    KeSetEvent(&Uart->ProbeStop, IO_NO_INCREMENT, FALSE);
    KeSetEvent(&Uart->FsmEvent, IO_NO_INCREMENT, FALSE);
    QcaUartPurgeTarget(Uart);
}

VOID
QcaUartRequestStop(_Inout_ PQCA_UART Uart)
{
    LARGE_INTEGER timeout;

    if (InterlockedCompareExchange(&Uart->ProbeActive, 0, 0) == 0) {
        return;
    }
    if (Uart->ProbeMode != QcaProbeModeSteady || KeReadStateEvent(&Uart->SteadyReady) == 0) {
        QcaUartCancelProbe(Uart);
        return;
    }
    InterlockedExchange(&Uart->ShutdownRequested, 1);
    KeSetEvent(&Uart->ShutdownEvent, IO_NO_INCREMENT, FALSE);
    /* The worker's own bounded handback, never the lower stack: escalate if it overruns. */
    timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_SHUTDOWN_WAIT_MS);
    if (KeWaitForSingleObject(&Uart->SteadyStopped, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS) {
        QcaUartCancelProbe(Uart);
    }
}


VOID
QcaUartPublishProbe(_Inout_ PQCA_UART Uart)
{
    LARGE_INTEGER timeout;
    KIRQL irql;
    timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(100);
    while (InterlockedCompareExchange(&Uart->ReportingStopped, 0, 0) == 0 &&
           InterlockedCompareExchange(&Uart->StopRequested, 0, 0) == 0) {
        if (KeWaitForSingleObject(&Uart->RecordGate, Executive, KernelMode,
                                  FALSE, &timeout) != STATUS_SUCCESS) {
            continue;
        }
        if (InterlockedCompareExchange(&Uart->ReportingStopped, 0, 0) == 0 &&
            InterlockedCompareExchange(&Uart->StopRequested, 0, 0) == 0) {
            Uart->Record.ElapsedMs = (ULONG)((KeQueryInterruptTime() - Uart->ProbeStarted) / 10000);
            if (Uart->ProbeMode == QcaProbeModeIdentify) {
                Uart->Record.IdentifyAttempts = Uart->Identify.Attempts;
                if (Uart->Identify.Answered) {
                    Uart->Record.IdentifyBaud = Uart->Identify.AnsweredRate;
                    Uart->Record.SocId = Uart->Identify.Version.SocId;
                    Uart->Record.RomVersion = (ULONG)Uart->Identify.Version.RomVersion;
                    Uart->Record.ProductId = Uart->Identify.Version.ProductId;
                    Uart->Record.PatchVersion = (ULONG)Uart->Identify.Version.PatchVersion;
                }
            } else {
                Uart->Record.SocId = Uart->Fsm.SocVersion.SocId;
                Uart->Record.RomVersion = (ULONG)Uart->Fsm.SocVersion.RomVersion;
                Uart->Record.BoardId = (ULONG)Uart->Fsm.BoardId;
                Uart->Record.BoardIdValid = Uart->Fsm.BoardIdValid ? 1 : 0;
                Uart->Record.NvmFallback = Uart->Fsm.NvmFallbackUsed ? 1 : 0;
                if (Uart->Fsm.SelectedNvmName[0] != '\0') {
                    ULONG cIdx = 0;
                    while (cIdx + 1 < ARRAYSIZE(Uart->Record.NvmSelected) && Uart->Fsm.SelectedNvmName[cIdx] != '\0') {
                        Uart->Record.NvmSelected[cIdx] = (WCHAR)Uart->Fsm.SelectedNvmName[cIdx];
                        cIdx++;
                    }
                    Uart->Record.NvmSelected[cIdx] = L'\0';
                }
            }
            KeAcquireSpinLock(&Uart->RecordLock, &irql);
            Uart->PublishedRecord = Uart->Record;
            KeReleaseSpinLock(&Uart->RecordLock, irql);
            DeckBtRecordProbeProgress(&Uart->Record);
            if (Uart->Record.Completion != QcaUartRecordInProgress) {
                InterlockedExchange(&Uart->ReportingStopped, 1);
            }
        }
        KeSetEvent(&Uart->RecordGate, IO_NO_INCREMENT, FALSE);
        return;
    }
}

static VOID
QcaUartProbeWorker(_In_ PVOID Context)
{
    PQCA_UART uart = (PQCA_UART)Context;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    WDFDEVICE ioOwner;
    uart->ProbeRecord = &uart->Record;
    QcaUartPublishProbe(uart);
    status = QcaUartPrepareHardware(uart);
    if (NT_SUCCESS(status) && QcaUartRequestBudget(uart, 1) != 0) {
        if (uart->ProbeMode == QcaProbeModeIdentify) {
            status = QcaUartRunIdentify(uart, &uart->Record);
        } else {
            status = QcaUartRunProbe(uart, &uart->Record);
            if (uart->ProbeMode == QcaProbeModeSteady && NT_SUCCESS(status)) {
                status = QcaUartServeSteady(uart);
            }
        }
    }
    uart->Record.LastStatus = (ULONG)status;
    QcaUartPublishProbe(uart);
    QcaUartReleaseHardware(uart);
    uart->Record.Completion = QcaUartRecordReleased;
    QcaUartPublishProbe(uart);
    /*
     * UART released and the final record committed: a graceful stop may stop waiting. Signalled
     * no earlier, or device cleanup's hard cancel could suppress this record and the monitor
     * would report the retirement as unconfirmed (QcaUartRecordAbandoned).
     */
    KeSetEvent(&uart->SteadyStopped, IO_NO_INCREMENT, FALSE);
    uart->ProbeRecord = NULL;
    ioOwner = uart->IoOwner;
    uart->IoOwner = NULL;
    KeSetEvent(&uart->ProbeDone, IO_NO_INCREMENT, FALSE);
    timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(100);
    while (KeWaitForSingleObject(&uart->MonitorDone, Executive, KernelMode,
                                 FALSE, &timeout) != STATUS_SUCCESS) {
        /* Only detached ownership remains; no PnP or power callback joins this thread. */
    }
    InterlockedExchange(&uart->ProbeActive, 0);
    if (ioOwner != NULL) {
        /*
         * Framework behavior avoided: FxIoTarget::Dispose cancels and waits during object
         * cleanup; parenting the target to the physical FDO would cause PnP removal to wait
         * on a lower stack that ignores cancellation.
         * Tradeoff accepted: A worker parked on a lower stack that ignores cancellation
         * retains this private CDO and blocks service unload, but never blocks physical PnP/power.
         * The worker deletes the CDO directly off all PnP and power paths.
         */
        WdfObjectDelete(ioOwner);
        WdfObjectDereference(ioOwner);
    }
    WdfObjectDereference(uart->Lock);
    WdfObjectDereference(uart->WdfDevice);
    ExReleaseRundownProtection(&DeckBtProbeRundown);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
QcaUartProbeMonitor(_In_ PVOID Context)
{
    PQCA_UART uart = (PQCA_UART)Context;
    LARGE_INTEGER timeout;
    PVOID events[3];
    QCA_UART_RECORD record;
    KIRQL irql;
    NTSTATUS status;
    ULONG budgetMs;

    if (!ExAcquireRundownProtection(&DeckBtProbeRundown)) {
        status = STATUS_DELETE_PENDING;
    } else {
        WdfObjectReference(uart->WdfDevice);
        WdfObjectReference(uart->Lock);
        status = DeckBtCreateProbeThread(QcaUartProbeWorker, uart);
        if (!NT_SUCCESS(status)) {
            WdfObjectDereference(uart->Lock);
            WdfObjectDereference(uart->WdfDevice);
            ExReleaseRundownProtection(&DeckBtProbeRundown);
        }
    }
    if (!NT_SUCCESS(status)) {
        uart->Record.LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(uart->Record.FailurePhase,
                              ARRAYSIZE(uart->Record.FailurePhase), L"WorkerCreate");
        uart->Record.Aborted = InterlockedCompareExchange(&uart->StopRequested, 0, 0) != 0 ? 1u : 0u;
        uart->Record.ElapsedMs = (ULONG)((KeQueryInterruptTime() - uart->ProbeStarted) / 10000);
        uart->Record.Completion = QcaUartRecordReleased;
        DeckBtRecordProbeProgress(&uart->Record);
        InterlockedExchange(&uart->ProbeActive, 0);
    } else {
        events[0] = &uart->ProbeDone;
        events[1] = &uart->ProbeStop;
        events[2] = &uart->SteadyReady;
        budgetMs = QcaUartRequestBudget(uart, 60000);
        timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(budgetMs);
        status = KeWaitForMultipleObjects(3, events, WaitAny, Executive, KernelMode,
                                          FALSE, &timeout, NULL);
        if (status == STATUS_WAIT_2) {
            /*
             * Steady: bring-up is over and the bridge serves BTHUSB for as long as the device
             * runs. A graceful stop then gets its own bounded budget before the hard cancel.
             */
            events[2] = &uart->ShutdownEvent;
            status = KeWaitForMultipleObjects(3, events, WaitAny, Executive, KernelMode,
                                              FALSE, NULL, NULL);
            if (status == STATUS_WAIT_2) {
                timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_SHUTDOWN_BUDGET_MS);
                status = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode,
                                                  FALSE, &timeout, NULL);
            }
        }
        if (status != STATUS_WAIT_0 ||
            InterlockedCompareExchange(&uart->StopRequested, 0, 0) != 0) {
            /*
             * A lower driver may retain one cancelled request indefinitely; only detached
             * ownership and DriverUnload then wait, never a PnP or power IRP.
             */
            QcaUartCancelProbe(uart);
            /* Serialize final registry publication with a publication already in progress. */
            timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(100);
            while (KeWaitForSingleObject(&uart->RecordGate, Executive, KernelMode,
                                         FALSE, &timeout) != STATUS_SUCCESS) {
            }
            if (InterlockedCompareExchange(&uart->ReportingStopped, 1, 0) == 0) {
                /* A committed released record is final; cancellation cannot overwrite it. */
                KeAcquireSpinLock(&uart->RecordLock, &irql);
                record = uart->PublishedRecord;
                KeReleaseSpinLock(&uart->RecordLock, irql);
                record.Completion = QcaUartRecordAbandoned;
                record.Aborted = 1;
                record.ElapsedMs = (ULONG)((KeQueryInterruptTime() - uart->ProbeStarted) / 10000);
                record.LastStatus = (ULONG)(record.ElapsedMs >= 60000 ?
                                            STATUS_IO_TIMEOUT : STATUS_CANCELLED);
                DeckBtRecordProbeProgress(&record);
            }
            KeSetEvent(&uart->RecordGate, IO_NO_INCREMENT, FALSE);
        }
        KeSetEvent(&uart->MonitorDone, IO_NO_INCREMENT, FALSE);
    }
    WdfObjectDereference(uart->Lock);
    WdfObjectDereference(uart->WdfDevice);
    ExReleaseRundownProtection(&DeckBtProbeRundown);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/*
 * Arms the worker on the serial connection recorded in Uart->SerialIdLow/High (SerialFound). The
 * caller has claimed ProbeActive; every failure path here releases it.
 */
static NTSTATUS
QcaUartArmStored(_Inout_ PQCA_UART Uart, _In_ QCA_PROBE_MODE Mode)
{
    NTSTATUS status;

    Uart->ProbeMode = Mode;
    if (!Uart->SerialFound) {
        RtlZeroMemory(&Uart->Record, sizeof(Uart->Record));
        if (Mode == QcaProbeModeIdentify) {
            Uart->Record.IdentifyRan = 1;
            Uart->Record.IdentifyBaud = 0;
            Uart->Record.IdentifyAttempts = 0;
            Uart->Record.IdentifyRawHex[0] = L'\0';
        } else {
            Uart->Record.ProbeRan = 1;
        }
        Uart->Record.SerialOpened = 0;
        Uart->Record.BaudInitial = QCA_INIT_BAUD_RATE;
        Uart->Record.BaudFinal = QCA_INIT_BAUD_RATE;
        Uart->Record.HciResetStatus = (ULONG)STATUS_PENDING;
        Uart->Record.LastStep = DECKBT_STEP_QCA_FIND_RES;
        Uart->Record.LastStatus = (ULONG)STATUS_NOT_FOUND;
        Uart->Record.Aborted = 0;
        Uart->Record.ElapsedMs = 0;
        (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                              ARRAYSIZE(Uart->Record.FailurePhase), L"MissingSerialResource");
        DeckBtRecordStep(DECKBT_STEP_QCA_FIND_RES, STATUS_NOT_FOUND);
        Uart->Record.Completion = QcaUartRecordReleased;
        DeckBtRecordProbeProgress(&Uart->Record);
        InterlockedExchange(&Uart->ProbeActive, 0);
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(&Uart->Record, sizeof(Uart->Record));
    if (Mode == QcaProbeModeIdentify) {
        Uart->Record.IdentifyRan = 1;
        Uart->Record.IdentifyBaud = 0;
        Uart->Record.IdentifyAttempts = 0;
        Uart->Record.IdentifyRawHex[0] = L'\0';
        Uart->Record.LastStep = DECKBT_STEP_QCA_IDENTIFY_ENTER;
    } else {
        Uart->Record.ProbeRan = 1;
        Uart->Record.LastStep = DECKBT_STEP_PROBE_ENTER;
    }
    Uart->Record.BaudInitial = QCA_INIT_BAUD_RATE;
    Uart->Record.BaudFinal = QCA_INIT_BAUD_RATE;
    Uart->Record.HciResetStatus = (ULONG)STATUS_PENDING;
    if (Mode == QcaProbeModeSteady) {
        Uart->Record.UsbPlugStatus = (ULONG)STATUS_PENDING;
    }
    (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                          ARRAYSIZE(Uart->Record.FailurePhase), L"Armed");
    Uart->PublishedRecord = Uart->Record;
    Uart->ProbeStarted = KeQueryInterruptTime();
    InterlockedExchange(&Uart->StopRequested, 0);
    InterlockedExchange(&Uart->ReportingStopped, 0);
    KeClearEvent(&Uart->ProbeDone);
    KeClearEvent(&Uart->ProbeStop);
    KeClearEvent(&Uart->MonitorDone);
    KeClearEvent(&Uart->SteadyReady);
    KeClearEvent(&Uart->ShutdownEvent);
    KeClearEvent(&Uart->SteadyStopped);
    KeClearEvent(&Uart->IbsAckEvent);
    KeClearEvent(&Uart->IbsWorkEvent);
    InterlockedExchange(&Uart->IbsAckPending, 0);
    InterlockedExchange(&Uart->ShutdownRequested, 0);
    InterlockedExchange(&Uart->BudgetUnbounded, 0);
    InterlockedExchange(&Uart->IbsTxAwake, 0);
    Uart->IbsWakeIndRx = 0;
    Uart->IbsWakeAckTx = 0;
    Uart->IbsSleepIndRx = 0;
    Uart->IbsAckCtsLow = 0;
    Uart->IbsAckFailures = 0;
    Uart->IbsAckLastStatus = 0;
    Uart->LastWriteStatus = 0;
    Uart->EventTraceCount = 0;
    Uart->AdvReports = 0;
    Uart->AdvSeenCount = 0;
    RtlZeroMemory(Uart->AdvSeen, sizeof(Uart->AdvSeen));
    RtlZeroMemory(Uart->EventTrace, sizeof(Uart->EventTrace));
    Uart->ScoLinkEvents = 0;
    RtlZeroMemory(Uart->ScoLinkEvent, sizeof(Uart->ScoLinkEvent));
    if (!ExAcquireRundownProtection(&DeckBtProbeRundown)) {
        Uart->Record.LastStatus = (ULONG)STATUS_DELETE_PENDING;
        (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                              ARRAYSIZE(Uart->Record.FailurePhase), L"DriverRundown");
        Uart->Record.Completion = QcaUartRecordReleased;
        DeckBtRecordProbeProgress(&Uart->Record);
        InterlockedExchange(&Uart->ProbeActive, 0);
        return STATUS_DELETE_PENDING;
    }
    WdfObjectReference(Uart->WdfDevice);
    WdfObjectReference(Uart->Lock);
    status = DeckBtCreateProbeThread(QcaUartProbeMonitor, Uart);
    if (!NT_SUCCESS(status)) {
        Uart->Record.LastStatus = (ULONG)status;
        Uart->Record.ElapsedMs = (ULONG)((KeQueryInterruptTime() - Uart->ProbeStarted) / 10000);
        (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                              ARRAYSIZE(Uart->Record.FailurePhase), L"MonitorCreate");
        Uart->Record.Completion = QcaUartRecordReleased;
        DeckBtRecordProbeProgress(&Uart->Record);
        WdfObjectDereference(Uart->Lock);
        WdfObjectDereference(Uart->WdfDevice);
        ExReleaseRundownProtection(&DeckBtProbeRundown);
        InterlockedExchange(&Uart->ProbeActive, 0);
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
QcaUartArmProbe(_Inout_ PQCA_UART Uart, _In_ WDFCMRESLIST ResourcesTranslated, _In_ QCA_PROBE_MODE Mode)
{
    ULONG i;

    if (InterlockedCompareExchange(&Uart->ProbeActive, 1, 0) != 0) {
        return STATUS_DEVICE_BUSY;
    }
    Uart->SerialFound = FALSE;
    Uart->GpioExposed = FALSE;
    for (i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc != NULL && desc->Type == CmResourceTypeConnection) {
            if (desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_UART) {
                Uart->SerialIdLow = desc->u.Connection.IdLowPart;
                Uart->SerialIdHigh = desc->u.Connection.IdHighPart;
                Uart->SerialFound = TRUE;
            } else if (desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_GPIO &&
                       desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_GPIO_IO) {
                Uart->GpioExposed = TRUE;
            }
        }
    }
    /* Read here, at PASSIVE_LEVEL in PrepareHardware; a resume re-arm never repeats the test. Not a
     * Sco* name: uart-probe clears Uart* and Sco* values when it arms a session. */
    Uart->ScoLoopbackRequested = (BOOLEAN)(Mode == QcaProbeModeSteady &&
                                           DeckBtReadParameter(L"SelfTestScoLoopback", 0) != 0);
    return QcaUartArmStored(Uart, Mode);
}
/* ---------------------------------------------------------------- Initialisation */

VOID
QcaUartInit(
    _Out_ PQCA_UART Uart,
    _In_ WDFDEVICE Device)
{
    RtlZeroMemory(Uart, sizeof(QCA_UART));
    Uart->WdfDevice = Device;
    Uart->CurrentBaudRate = QCA_INIT_BAUD_RATE;
    Uart->Phase = 0;
    KeInitializeEvent(&Uart->FsmEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&Uart->ProbeDone, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->ProbeStop, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->MonitorDone, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->RecordGate, SynchronizationEvent, TRUE);
    KeInitializeEvent(&Uart->IoIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&Uart->IbsAckEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->IbsWorkEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->SteadyReady, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->ShutdownEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&Uart->SteadyStopped, NotificationEvent, FALSE);
    KeInitializeSpinLock(&Uart->TargetLock);
    KeInitializeSpinLock(&Uart->RecordLock);
}

/* ---------------------------------------------------------------- Hardware Resource Extraction */

NTSTATUS
QcaUartPrepareHardware(
    _Inout_ PQCA_UART Uart)
{
    NTSTATUS status = STATUS_SUCCESS;
    WCHAR serialPathBuf[RESOURCE_HUB_CONNECTION_PATH_CHARS];
    UNICODE_STRING serialPath;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    WDF_OBJECT_ATTRIBUTES objAttribs;
    KIRQL irql;
    PWDFDEVICE_INIT ownerInit;
    UNICODE_STRING ownerSddl;

    RtlInitUnicodeString(&ownerSddl, L"D:P(A;;GA;;;SY)");
    if (!Uart->SerialFound) {
        Uart->Record.LastStatus = (ULONG)STATUS_NOT_FOUND;
        Uart->Record.LastStep = DECKBT_STEP_QCA_FIND_RES;
        (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                              ARRAYSIZE(Uart->Record.FailurePhase), L"MissingSerialResource");
        QcaUartPublishProbe(Uart);
        return STATUS_NOT_FOUND;
    }
    if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
        return STATUS_CANCELLED;
    }

    /*
     * Build Resource Hub path: \Device\RESOURCE_HUB\%08x%08x
     * Format: HighPart then LowPart.
     */
    status = RtlStringCchPrintfW(
        serialPathBuf,
        ARRAYSIZE(serialPathBuf),
        L"\\Device\\RESOURCE_HUB\\%08x%08x",
        Uart->SerialIdHigh,
        Uart->SerialIdLow);
    if (!NT_SUCCESS(status)) {
        DeckBtRecordStep(DECKBT_STEP_QCA_FIND_RES, status);
        return status;
    }
    RtlInitUnicodeString(&serialPath, serialPathBuf);

    /*
     * FxIoTarget::Dispose cancels and waits even when references defer destruction. Never
     * parent this target to the physical FDO: its PnP removal would inherit that wait.
     * A private, SYSTEM-only control device has no PnP/power stack or public symbolic link.
     * The detached worker exclusively owns its target and eventual control-device deletion.
     */
    (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                          ARRAYSIZE(Uart->Record.FailurePhase), L"IoOwnerCreate");
    QcaUartPublishProbe(Uart);
    ownerInit = WdfControlDeviceInitAllocate(WdfGetDriver(), &ownerSddl);
    if (ownerInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = WdfDeviceCreate(&ownerInit, WDF_NO_OBJECT_ATTRIBUTES, &Uart->IoOwner);
    if (!NT_SUCCESS(status)) {
        if (ownerInit != NULL) {
            WdfDeviceInitFree(ownerInit);
        }
        return status;
    }
    WdfObjectReference(Uart->IoOwner);
    WdfControlFinishInitializing(Uart->IoOwner);

    /* Create SerCx2 I/O Target */
    WDF_OBJECT_ATTRIBUTES_INIT(&objAttribs);
    objAttribs.ParentObject = Uart->IoOwner;
    status = WdfIoTargetCreate(Uart->IoOwner, &objAttribs, &Uart->IoTarget);
    if (!NT_SUCCESS(status)) {
        if (Uart->ProbeRecord != NULL) {
            Uart->ProbeRecord->LastStep = DECKBT_STEP_QCA_TARGET_CREATE;
            Uart->ProbeRecord->LastStatus = (ULONG)status;
            (void)RtlStringCchCopyW(Uart->ProbeRecord->FailurePhase, ARRAYSIZE(Uart->ProbeRecord->FailurePhase), L"TargetCreate");
            QcaUartPublishProbe(Uart);
        }
        DeckBtRecordStep(DECKBT_STEP_QCA_TARGET_CREATE, status);
        return status;
    }
    WdfObjectReference(Uart->IoTarget);
    DeckBtRecordStep(DECKBT_STEP_QCA_TARGET_CREATE, STATUS_SUCCESS);

    /* Open target by Resource Hub path */
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(
        &openParams,
        &serialPath,
        GENERIC_READ | GENERIC_WRITE);
    openParams.ShareAccess = 0;
    openParams.CreateDisposition = FILE_OPEN;
    (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                          ARRAYSIZE(Uart->Record.FailurePhase), L"TargetOpen");
    QcaUartPublishProbe(Uart);

    status = WdfIoTargetOpen(Uart->IoTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        if (Uart->ProbeRecord != NULL) {
            Uart->ProbeRecord->LastStep = DECKBT_STEP_QCA_TARGET_OPEN;
            Uart->ProbeRecord->LastStatus = (ULONG)status;
            (void)RtlStringCchCopyW(Uart->ProbeRecord->FailurePhase, ARRAYSIZE(Uart->ProbeRecord->FailurePhase), L"TargetOpen");
            QcaUartPublishProbe(Uart);
        }
        DeckBtRecordStep(DECKBT_STEP_QCA_TARGET_OPEN, status);
        WdfObjectDelete(Uart->IoTarget);
        WdfObjectDereference(Uart->IoTarget);
        Uart->IoTarget = NULL;
        return status;
    }
    Uart->IoTargetOpened = TRUE;
    KeAcquireSpinLock(&Uart->TargetLock, &irql);
    Uart->CancelTarget = Uart->IoTarget;
    KeReleaseSpinLock(&Uart->TargetLock, irql);
    Uart->Record.SerialOpened = 1;
    if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
        return STATUS_CANCELLED;
    }
    if (Uart->ProbeRecord != NULL) {
        Uart->ProbeRecord->SerialOpened = 1;
        Uart->ProbeRecord->LastStep = DECKBT_STEP_QCA_TARGET_OPEN;
        Uart->ProbeRecord->LastStatus = (ULONG)STATUS_SUCCESS;
        QcaUartPublishProbe(Uart);
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_TARGET_OPEN, STATUS_SUCCESS);
    /*
     * Create the write-slot spin lock. This one is genuinely backend-private: it serialises
     * allocation of the pre-allocated asynchronous write slots and nothing above this layer
     * touches them.
     *
     * Uart->Lock is deliberately not created here. It is the front end's controller spin lock,
     * handed over by QcaUartBindTransport, because the front end mutates the same
     * HCI_BRIDGE state from its Submit/Has/Pop/Reset paths. Two locks over one piece of state
     * is the same as no lock.
     */
    if (Uart->WriteLock == NULL) {
        WDF_OBJECT_ATTRIBUTES_INIT(&objAttribs);
        objAttribs.ParentObject = Uart->IoOwner;
        status = WdfSpinLockCreate(&objAttribs, &Uart->WriteLock);
        if (!NT_SUCCESS(status)) {
            QcaUartReleaseHardware(Uart);
            return status;
        }
        WdfObjectReference(Uart->WriteLock);
    }

    /* Configure initial serial parameters: 115200 baud, 8N1, hardware flow control */
    (void)RtlStringCchCopyW(Uart->Record.FailurePhase,
                          ARRAYSIZE(Uart->Record.FailurePhase), L"BaudSet");
    QcaUartPublishProbe(Uart);
    status = QcaUartSetBaudRate(Uart, QCA_INIT_BAUD_RATE);
    if (!NT_SUCCESS(status)) {
        if (Uart->ProbeRecord != NULL) {
            Uart->ProbeRecord->LastStep = DECKBT_STEP_QCA_BAUD_SET;
            Uart->ProbeRecord->LastStatus = (ULONG)status;
            (void)RtlStringCchCopyW(Uart->ProbeRecord->FailurePhase, ARRAYSIZE(Uart->ProbeRecord->FailurePhase), L"BaudSet");
            QcaUartPublishProbe(Uart);
        }
        DeckBtRecordStep(DECKBT_STEP_QCA_BAUD_SET, status);
        QcaUartReleaseHardware(Uart);
        return status;
    }

    /*
     * Mirror of the vendor's D0Entry order: open, configure, then wake the controller before
     * the first byte is sent. A CTS that never asserts ends the probe here, before any write
     * can stall inside the lower stack.
     */
    status = QcaUartWakeController(Uart, &Uart->Record);
    if (!NT_SUCCESS(status)) {
        QcaUartPublishProbe(Uart);
        QcaUartReleaseHardware(Uart);
        return status;
    }

    return STATUS_SUCCESS;
}

VOID
QcaUartReleaseHardware(
    _Inout_ PQCA_UART Uart)
{
    ULONG i;
    LARGE_INTEGER timeout;

    DeckBtRecordStep(DECKBT_STEP_QCA_STOP, STATUS_SUCCESS);
    QcaUartStop(Uart);

    /*
     * This function is worker-only. Stop has detached the cancellation target; no new purge
     * borrower can enter. Retain every request, buffer, and owner until both the last completion
     * and the nonwaiting purge call have retired. A broken lower driver may retain them forever.
     * Each wait is bounded; power/removal never joins this drain. Unload owns final rundown.
     */
    timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(100);
    while (!QcaUartIsIoIdle(Uart)) {
        (void)KeWaitForSingleObject(&Uart->IoIdle, Executive, KernelMode, FALSE, &timeout);
    }
    for (i = 0; i < QCA_UART_READ_REQUESTS; i++) {
        if (Uart->ReadMemories[i] != NULL) {
            WdfObjectDelete(Uart->ReadMemories[i]);
            WdfObjectDereference(Uart->ReadMemories[i]);
        }
        if (Uart->ReadRequests[i] != NULL) {
            WdfObjectDelete(Uart->ReadRequests[i]);
            WdfObjectDereference(Uart->ReadRequests[i]);
            Uart->ReadRequests[i] = NULL;
        }
        Uart->ReadMemories[i] = NULL;
        Uart->ReadBuffers[i] = NULL;
        Uart->ReadRequestSent[i] = FALSE;
    }
    for (i = 0; i < QCA_UART_WRITE_SLOTS; i++) {
        if (Uart->WriteSlots[i].Memory != NULL) {
            WdfObjectDelete(Uart->WriteSlots[i].Memory);
            WdfObjectDereference(Uart->WriteSlots[i].Memory);
        }
        if (Uart->WriteSlots[i].Request != NULL) {
            WdfObjectDelete(Uart->WriteSlots[i].Request);
            WdfObjectDereference(Uart->WriteSlots[i].Request);
            Uart->WriteSlots[i].Request = NULL;
        }
        Uart->WriteSlots[i].Memory = NULL;
        Uart->WriteSlots[i].InUse = FALSE;
        Uart->WriteSlots[i].HasBeenSent = FALSE;
        Uart->WriteSlots[i].Owner = NULL;
    }

    if (Uart->IoTarget != NULL) {
        if (Uart->IoTargetOpened) {
            WdfIoTargetClose(Uart->IoTarget);
        }
        WdfObjectDelete(Uart->IoTarget);
        WdfObjectDereference(Uart->IoTarget);
        Uart->IoTarget = NULL;
        Uart->IoTargetOpened = FALSE;
    }

    if (Uart->GpioTarget != NULL) {
        WdfIoTargetClose(Uart->GpioTarget);
        WdfObjectDelete(Uart->GpioTarget);
        Uart->GpioTarget = NULL;
    }
    if (Uart->WriteLock != NULL) {
        WdfObjectDelete(Uart->WriteLock);
        WdfObjectDereference(Uart->WriteLock);
        Uart->WriteLock = NULL;
    }
}

/* ---------------------------------------------------------------- Line Configuration & Baud Rate */

static NTSTATUS
QcaUartSendIoctlSynchronously(
    _Inout_ PQCA_UART Uart,
    _In_ ULONG IoctlCode,
    _In_opt_ PVOID InBuffer,
    _In_ ULONG InBufferSize,
    _Out_opt_ PVOID OutBuffer,
    _In_ ULONG OutBufferSize)
{
    WDF_MEMORY_DESCRIPTOR inDesc;
    WDF_MEMORY_DESCRIPTOR outDesc;
    PWDF_MEMORY_DESCRIPTOR pIn = NULL;
    PWDF_MEMORY_DESCRIPTOR pOut = NULL;
    WDF_REQUEST_SEND_OPTIONS sendOptions;
    ULONG_PTR bytesReturned = 0;
    ULONG budgetMs = QcaUartRequestBudget(Uart, 2000);
    if (budgetMs == 0) {
        return STATUS_CANCELLED;
    }

    if (InBuffer != NULL && InBufferSize > 0) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, InBuffer, InBufferSize);
        pIn = &inDesc;
    }
    if (OutBuffer != NULL && OutBufferSize > 0) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, OutBuffer, OutBufferSize);
        pOut = &outDesc;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_MS(budgetMs));

    return WdfIoTargetSendIoctlSynchronously(
        Uart->IoTarget,
        NULL,
        IoctlCode,
        pIn,
        pOut,
        &sendOptions,
        &bytesReturned);
}

static NTSTATUS
QcaUartSetHardwareFlow(
    _Inout_ PQCA_UART Uart,
    _In_ BOOLEAN Enabled)
{
    SERIAL_HANDFLOW handflow;
    NTSTATUS status;

    RtlZeroMemory(&handflow, sizeof(handflow));
    if (Enabled) {
        handflow.ControlHandShake = SERIAL_CTS_HANDSHAKE;
        handflow.FlowReplace = SERIAL_RTS_HANDSHAKE;
        handflow.XonLimit = (LONG)QCA_UART_FLOW_LIMIT;
        handflow.XoffLimit = (LONG)QCA_UART_FLOW_LIMIT;
    }

    status = QcaUartSendIoctlSynchronously(
        Uart,
        IOCTL_SERIAL_SET_HANDFLOW,
        &handflow,
        sizeof(handflow),
        NULL,
        0);
    DeckBtRecordStep(DECKBT_STEP_QCA_FLOW_SET, status);
    return status;
}

NTSTATUS
QcaUartSetBaudRate(
    _Inout_ PQCA_UART Uart,
    _In_ ULONG BaudRate)
{
    NTSTATUS status;
    SERIAL_BAUD_RATE baud;
    SERIAL_LINE_CONTROL lineControl;
    SERIAL_TIMEOUTS timeouts;

    if (Uart->IoTarget == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    /*
     * Program the exact rate already selected for the controller. Falling back here would leave
     * the chip at the 0xFC48 rate and the host at another rate, making every later byte garbage.
     */
    baud.BaudRate = BaudRate;
    status = QcaUartSendIoctlSynchronously(
        Uart,
        IOCTL_SERIAL_SET_BAUD_RATE,
        &baud,
        sizeof(baud),
        NULL,
        0);
    DeckBtRecordStep(DECKBT_STEP_QCA_BAUD_SET, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    lineControl.StopBits = STOP_BIT_1;
    lineControl.Parity = NO_PARITY;
    lineControl.WordLength = 8;
    status = QcaUartSendIoctlSynchronously(
        Uart,
        IOCTL_SERIAL_SET_LINE_CONTROL,
        &lineControl,
        sizeof(lineControl),
        NULL,
        0);
    DeckBtRecordStep(DECKBT_STEP_QCA_LINE_CONFIG, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(&timeouts, sizeof(timeouts));
    timeouts.ReadIntervalTimeout = QCA_UART_READ_INTERVAL_MS;
    timeouts.WriteTotalTimeoutConstant = QCA_UART_WRITE_TIMEOUT_MS;
    status = QcaUartSendIoctlSynchronously(
        Uart,
        IOCTL_SERIAL_SET_TIMEOUTS,
        &timeouts,
        sizeof(timeouts),
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = QcaUartSetHardwareFlow(Uart, TRUE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Uart->CurrentBaudRate = BaudRate;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- Controller wake (CTS) */

static NTSTATUS
QcaUartGetModemStatus(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record,
    _Out_ PULONG ModemStatus)
{
    NTSTATUS status;

    *ModemStatus = 0;
    status = QcaUartSendIoctlSynchronously(
        Uart,
        IOCTL_SERIAL_GET_MODEMSTATUS,
        NULL,
        0,
        ModemStatus,
        sizeof(*ModemStatus));
    if (NT_SUCCESS(status)) {
        if (Record->ModemStatusReads == 0) {
            Record->ModemStatusFirst = *ModemStatus;
        }
        Record->ModemStatusReads++;
        Record->ModemStatusLast = *ModemStatus;
    }
    return status;
}

static NTSTATUS
QcaUartSetManualRts(
    _Inout_ PQCA_UART Uart)
{
    SERIAL_HANDFLOW handflow;

    /* CTS still gates transmit; RTS follows IOCTL_SERIAL_SET_RTS / CLR_RTS instead of the FIFO. */
    RtlZeroMemory(&handflow, sizeof(handflow));
    handflow.ControlHandShake = SERIAL_CTS_HANDSHAKE;
    handflow.FlowReplace = SERIAL_RTS_CONTROL;
    handflow.XonLimit = (LONG)QCA_UART_FLOW_LIMIT;
    handflow.XoffLimit = (LONG)QCA_UART_FLOW_LIMIT;
    return QcaUartSendIoctlSynchronously(
        Uart,
        IOCTL_SERIAL_SET_HANDFLOW,
        &handflow,
        sizeof(handflow),
        NULL,
        0);
}

/*
 * Returns success only with CTS observed asserted. The vendor sequence: read modem status; if
 * CTS is low, switch RTS to manual control, pulse it low/high (5 ms each) up to five times until
 * CTS asserts, leave it high, and restore RTS handshake. Any IOCTL failure fails closed: an
 * unverifiable CTS is treated as a stalled line, because the next write could never retire.
 */
static NTSTATUS
QcaUartWakeController(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    NTSTATUS status;
    NTSTATUS restoreStatus;
    ULONG modem = 0;
    ULONG pulse;
    LARGE_INTEGER delay;

    Record->LastStep = DECKBT_STEP_QCA_CTS_CHECK;
    (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"CtsCheck");
    QcaUartPublishProbe(Uart);
    status = QcaUartGetModemStatus(Uart, Record, &modem);
    DeckBtRecordStep(DECKBT_STEP_QCA_CTS_CHECK, status);
    if (!NT_SUCCESS(status)) {
        Record->CtsAsserted = 0;
        Record->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"ModemStatus");
        return status;
    }
    if ((modem & SERIAL_CTS_STATE) != 0) {
        Record->CtsAsserted = 1;
        return STATUS_SUCCESS;
    }

    Record->LastStep = DECKBT_STEP_QCA_CTS_WAKE;
    (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"CtsWake");
    QcaUartPublishProbe(Uart);
    status = QcaUartSetManualRts(Uart);
    for (pulse = 0; NT_SUCCESS(status) && pulse < QCA_UART_WAKE_PULSES; pulse++) {
        status = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_CLR_RTS, NULL, 0, NULL, 0);
        if (!NT_SUCCESS(status)) {
            break;
        }
        delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_WAKE_PULSE_MS);
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
        status = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_SET_RTS, NULL, 0, NULL, 0);
        if (!NT_SUCCESS(status)) {
            break;
        }
        delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_WAKE_PULSE_MS);
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
        Record->WakePulses++;
        status = QcaUartGetModemStatus(Uart, Record, &modem);
        if (NT_SUCCESS(status) && (modem & SERIAL_CTS_STATE) != 0) {
            break;
        }
    }
    /* Leave RTS asserted and hand it back to the handshake, whatever the pulses did. */
    restoreStatus = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_SET_RTS, NULL, 0, NULL, 0);
    if (NT_SUCCESS(restoreStatus)) {
        restoreStatus = QcaUartSetHardwareFlow(Uart, TRUE);
    }
    if (NT_SUCCESS(status) && !NT_SUCCESS(restoreStatus)) {
        status = restoreStatus;
    }
    Record->CtsAsserted = (NT_SUCCESS(status) && (modem & SERIAL_CTS_STATE) != 0) ? 1u : 0u;
    DeckBtRecordStep(DECKBT_STEP_QCA_CTS_WAKE, status);
    if (!NT_SUCCESS(status)) {
        Record->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"CtsWake");
        return status;
    }
    if (Record->CtsAsserted == 0) {
        status = STATUS_DEVICE_NOT_READY;
        Record->LastStep = DECKBT_STEP_QCA_CTS_BLOCKED;
        Record->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"CtsNotAsserted");
        DeckBtRecordStep(DECKBT_STEP_QCA_CTS_BLOCKED, status);
    }
    return status;
}

/* ---------------------------------------------------------------- Asynchronous Read Pump */

static VOID
QcaUartReadCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PQCA_UART uart = (PQCA_UART)Context;
    NTSTATUS status = Params->IoStatus.Status;
    ULONG_PTR bytesRead = Params->IoStatus.Information;
    WDF_REQUEST_REUSE_PARAMS reuse;

    UNREFERENCED_PARAMETER(Target);

    InterlockedIncrement((volatile LONG *)&uart->ReadCompletions);
    InterlockedExchange((volatile LONG *)&uart->LastIoStatus, (LONG)status);

    if (NT_SUCCESS(status) && bytesRead != 0) {
        ULONG notifyMask;
        HCI_STREAM stream;

        InterlockedExchangeAdd((volatile LONG *)&uart->TotalBytesRead, (LONG)bytesRead);
        /*
         * The controller lock serialises DecoderFeed with Reset and protects every bridge queue
         * mutation made by packet callbacks. Callbacks only set notification bits; notifications
         * themselves happen after release, as required by the transport lock-order contract.
         */
        WdfSpinLockAcquire(uart->Lock);
        H4DecoderFeedEx(
            &uart->Decoder,
            uart->ReadBuffers[0],
            (ULONG)bytesRead,
            QcaUartOnH4Packet,
            QcaUartOnIbsByte,
            uart);
        notifyMask = uart->PendingNotifyMask;
        uart->PendingNotifyMask = 0;
        WdfSpinLockRelease(uart->Lock);

        for (stream = HciStreamEvent; stream < HciStreamMax; stream++) {
            if ((notifyMask & (1u << (ULONG)stream)) != 0 && uart->Transport != NULL) {
                HciTransportNotify(uart->Transport, stream);
            }
        }
    } else if (!NT_SUCCESS(status) && status != STATUS_CANCELLED) {
        InterlockedIncrement((volatile LONG *)&uart->ReadErrors);
    }

    if (uart->ProbeMode == QcaProbeModeIdentify && !NT_SUCCESS(status) &&
        InterlockedCompareExchange(&uart->ReadPumpRunning, 0, 0) != 0) {
        /* An active reader failure is not controller silence. Intentional drain cancellation
         * arrives with ReadPumpRunning already clear and must not become a false fault. */
        InterlockedCompareExchange(&uart->IdentifyReadStatus, (LONG)status, STATUS_SUCCESS);
        InterlockedExchange(&uart->ReadPumpRunning, 0);
        KeSetEvent(&uart->FsmEvent, IO_NO_INCREMENT, FALSE);
    }

    /*
     * A cancelled read ends the pump, except in steady state: there the only cancellations that
     * are not a quiesce or a stop (both clear the flags tested first) are the bring-up read's
     * timeout firing after the handoff, and the line must stay read.
     */
    if (InterlockedCompareExchange(&uart->ReadPumpRunning, 0, 0) == 0 ||
        InterlockedCompareExchange(&uart->StopRequested, 0, 0) != 0 ||
        (status == STATUS_CANCELLED && InterlockedCompareExchange(&uart->BudgetUnbounded, 0, 0) == 0)) {
        QcaUartIoCompleted(uart);
        return;
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    status = WdfRequestReuse(Request, &reuse);
    if (NT_SUCCESS(status)) {
        status = WdfIoTargetFormatRequestForRead(
            uart->IoTarget,
            Request,
            uart->ReadMemories[0],
            NULL,
            NULL);
    }
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(Request, QcaUartReadCompletion, uart);
        status = QcaUartSendTimed(uart, Request, TRUE);
        if (NT_SUCCESS(status)) {
            QcaUartIoCompleted(uart);
            return;
        }
    }

    InterlockedIncrement((volatile LONG *)&uart->ReadErrors);
    InterlockedExchange((volatile LONG *)&uart->LastIoStatus, (LONG)status);
    InterlockedExchange(&uart->ReadPumpRunning, 0);
    if (uart->ProbeMode == QcaProbeModeIdentify) {
        InterlockedCompareExchange(&uart->IdentifyReadStatus, (LONG)status, STATUS_SUCCESS);
        KeSetEvent(&uart->FsmEvent, IO_NO_INCREMENT, FALSE);
    }
    QcaUartIoCompleted(uart);
}

static NTSTATUS
QcaUartStartReadPump(
    _Inout_ PQCA_UART Uart)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES reqAttribs;
    WDF_OBJECT_ATTRIBUTES memAttribs;
    WDF_REQUEST_REUSE_PARAMS reuse;

    if (QcaUartRequestBudget(Uart, 1) == 0) {
        return STATUS_CANCELLED;
    }

    if (InterlockedCompareExchange(&Uart->ReadPumpRunning, 0, 0) != 0) {
        return STATUS_SUCCESS;
    }

    H4DecoderInit(&Uart->Decoder);

    if (Uart->ReadRequests[0] == NULL) {
        WDF_OBJECT_ATTRIBUTES_INIT(&reqAttribs);
        reqAttribs.ParentObject = Uart->IoTarget;
        status = WdfRequestCreate(&reqAttribs, Uart->IoTarget, &Uart->ReadRequests[0]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        WdfObjectReference(Uart->ReadRequests[0]);
    }

    if (Uart->ReadMemories[0] == NULL) {
        WDF_OBJECT_ATTRIBUTES_INIT(&memAttribs);
        memAttribs.ParentObject = Uart->ReadRequests[0];
        status = WdfMemoryCreate(
            &memAttribs,
            NonPagedPoolNx,
            QCA_UART_POOL_TAG,
            QCA_UART_READ_BUF_SIZE,
            &Uart->ReadMemories[0],
            (PVOID *)&Uart->ReadBuffers[0]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        WdfObjectReference(Uart->ReadMemories[0]);
    }

    if (Uart->ReadRequestSent[0]) {
        /* Identify restarts only after the prior completion has retired. */
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
        status = WdfRequestReuse(Uart->ReadRequests[0], &reuse);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        Uart->ReadRequestSent[0] = FALSE;
    }

    status = WdfIoTargetFormatRequestForRead(
        Uart->IoTarget,
        Uart->ReadRequests[0],
        Uart->ReadMemories[0],
        NULL,
        NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    InterlockedExchange(&Uart->ReadPumpRunning, 1);
    WdfRequestSetCompletionRoutine(Uart->ReadRequests[0], QcaUartReadCompletion, Uart);
    status = QcaUartSendTimed(Uart, Uart->ReadRequests[0], TRUE);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&Uart->ReadPumpRunning, 0);
        InterlockedIncrement((volatile LONG *)&Uart->ReadErrors);
        return status;
    }
    Uart->ReadRequestSent[0] = TRUE;
    DeckBtRecordStep(DECKBT_STEP_QCA_READ_PUMP_START, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

static VOID
QcaUartStopReadPump(
    _Inout_ PQCA_UART Uart)
{
    InterlockedExchange(&Uart->ReadPumpRunning, 0);
    QcaUartPurgeTarget(Uart);
}

/* ---------------------------------------------------------------- Outbound Write Path */

static VOID
QcaUartWriteSlotCompletion(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target,
    _In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
    _In_ WDFCONTEXT Context)
{
    PQCA_UART_WRITE_SLOT slot = (PQCA_UART_WRITE_SLOT)Context;
    PQCA_UART uart = (PQCA_UART)slot->Owner;

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    WdfSpinLockAcquire(uart->WriteLock);
    slot->InUse = FALSE;
    if (NT_SUCCESS(Params->IoStatus.Status)) {
        uart->WriteCompletions++;
        uart->TotalBytesWritten += (ULONG)Params->IoStatus.Information;
    } else if (Params->IoStatus.Status != STATUS_CANCELLED) {
        uart->WriteErrors++;
    }
    uart->LastIoStatus = (ULONG)Params->IoStatus.Status;
    uart->LastWriteStatus = (ULONG)Params->IoStatus.Status;
    WdfSpinLockRelease(uart->WriteLock);
    QcaUartIoCompleted(uart);
}

/*
 * Asynchronous, non-blocking outbound packet transmission.
 * Called at <= DISPATCH_LEVEL with controller spin lock held.
 */
static unsigned char
QcaUartSendAsync(
    _Inout_ PQCA_UART Uart,
    _In_ UCHAR Type,
    _In_reads_bytes_(Length) const UCHAR *Packet,
    _In_ ULONG Length)
{
    ULONG i;
    PQCA_UART_WRITE_SLOT chosenSlot = NULL;
    ULONG framedLen;
    WDFMEMORY_OFFSET memOffset;
    NTSTATUS status = STATUS_SUCCESS;

    if (Uart->IoTarget == NULL || Uart->WriteLock == NULL || Length == 0 ||
        InterlockedCompareExchange(&Uart->Phase, 0, 0) != 2) {
        return (unsigned char)0;
    }

    /*
     * The caller already holds the controller lock. Keep the private slot lock through buffer
     * population, request reuse/format and submission; completion takes this same lock before
     * publishing the slot free, so no CPU can rewrite an in-flight buffer.
     */
    WdfSpinLockAcquire(Uart->WriteLock);
    for (i = 0; i < QCA_UART_WRITE_SLOTS; i++) {
        if (!Uart->WriteSlots[i].InUse) {
            chosenSlot = &Uart->WriteSlots[i];
            chosenSlot->InUse = TRUE;
            break;
        }
    }

    if (chosenSlot == NULL) {
        Uart->WriteErrors++;
        WdfSpinLockRelease(Uart->WriteLock);
        return (unsigned char)0;
    }

    framedLen = H4EncodePacket(
        Type,
        Packet,
        Length,
        chosenSlot->Buffer,
        sizeof(chosenSlot->Buffer));
    if (framedLen == 0) {
        status = STATUS_INVALID_PARAMETER;
    }
    if (NT_SUCCESS(status) && chosenSlot->HasBeenSent) {
        WDF_REQUEST_REUSE_PARAMS reuse;
        WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
        status = WdfRequestReuse(chosenSlot->Request, &reuse);
    }
    if (NT_SUCCESS(status)) {
        memOffset.BufferOffset = 0;
        memOffset.BufferLength = framedLen;
        status = WdfIoTargetFormatRequestForWrite(
            Uart->IoTarget,
            chosenSlot->Request,
            chosenSlot->Memory,
            &memOffset,
            NULL);
    }
    if (!NT_SUCCESS(status)) {
        chosenSlot->InUse = FALSE;
        Uart->WriteErrors++;
        Uart->LastIoStatus = (ULONG)status;
        WdfSpinLockRelease(Uart->WriteLock);
        return (unsigned char)0;
    }

    WdfRequestSetCompletionRoutine(
        chosenSlot->Request,
        QcaUartWriteSlotCompletion,
        chosenSlot);
    chosenSlot->HasBeenSent = TRUE;
    /*
     * Do not hold WriteLock across WdfRequestSend: a target may complete inline and invoke the
     * callback before Send returns. The controller lock still serialises all submit callers.
     */
    WdfSpinLockRelease(Uart->WriteLock);
    status = QcaUartSendTimed(Uart, chosenSlot->Request, FALSE);
    if (NT_SUCCESS(status)) {
        return (unsigned char)1;
    }

    WdfSpinLockAcquire(Uart->WriteLock);
    chosenSlot->InUse = FALSE;
    Uart->WriteErrors++;
    Uart->LastIoStatus = (ULONG)status;
    WdfSpinLockRelease(Uart->WriteLock);
    return (unsigned char)0;
}

/*
 * In-band sleep, controller to host (upstream hci_qca.c device_want_to_wakeup,
 * device_want_to_sleep, device_woke_up). Called by the H4 decoder only between packets, under
 * Uart->Lock in the read completion. Bring-up runs with IBS off, as upstream does
 * (QCA_IBS_DISABLED during setup): outside steady Phase 2 nothing is claimed.
 *
 * A controller WAKE_IND is acknowledged by the steady worker at PASSIVE_LEVEL, never from this
 * completion: acknowledgments transmitted from the completion path fail to serialize properly,
 * causing controller event delivery to stall. Repeated WAKE_INDs before the ack collapse into
 * one pending ack.
 */
static BOOLEAN
QcaUartOnIbsByte(
    void *Context,
    unsigned char Byte)
{
    PQCA_UART uart = (PQCA_UART)Context;

    if (uart->ProbeMode != QcaProbeModeSteady || InterlockedCompareExchange(&uart->Phase, 0, 0) != 2) {
        return FALSE;
    }
    switch (Byte) {
    case QCA_IBS_WAKE_IND:
        /* The controller holds its events until the host acknowledges; always acknowledge. */
        uart->IbsWakeIndRx++;
        InterlockedExchange(&uart->IbsAckPending, 1);
        KeSetEvent(&uart->IbsWorkEvent, IO_NO_INCREMENT, FALSE);
        return TRUE;
    case QCA_IBS_SLEEP_IND:
        uart->IbsSleepIndRx++;
        return TRUE;
    case QCA_IBS_WAKE_ACK:
        InterlockedExchange(&uart->IbsTxAwake, 1);
        KeSetEvent(&uart->IbsAckEvent, IO_NO_INCREMENT, FALSE);
        return TRUE;
    default:
        return FALSE;
    }
}

/* HCI_BRIDGE_WIRE_OPS implementations */

static unsigned char
QcaUartWireSendCommand(
    void *Context,
    const unsigned char *Packet,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Context;
    return QcaUartSendAsync(uart, H4_PKT_COMMAND, Packet, Length);
}

static unsigned char
QcaUartWireSendAcl(
    void *Context,
    const unsigned char *Packet,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Context;
    return QcaUartSendAsync(uart, H4_PKT_ACL, Packet, Length);
}

static unsigned char
QcaUartWireSendSco(
    void *Context,
    const unsigned char *Packet,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Context;
    return QcaUartSendAsync(uart, H4_PKT_SCO, Packet, Length);
}

/* ---------------------------------------------------------------- H4 Inbound Packet Routing */

/*
 * Notes the advertiser of a single-report LE advertising event (legacy 0x02 and direct 0x0B:
 * event type at [4], address type [5], address [6..11]; extended 0x0D: event type LE16 at [4],
 * address type [6], address [7..12]). Multi-report events use a per-field array layout and are
 * only counted. Under Uart->Lock in the read completion; a full table keeps the first comers.
 */
static VOID
QcaUartNoteAdvertiser(
    _Inout_ PQCA_UART Uart,
    _In_reads_bytes_(Length) const UCHAR *Payload,
    _In_ ULONG Length)
{
    const UCHAR *address;
    UCHAR addressType;
    UCHAR eventType;
    ULONG i;

    if (Payload[2] == HCI_LE_SUBEV_EXT_ADV_REPORT) {
        if (Length < 13u || Payload[3] != 1u) {
            return;
        }
        eventType = Payload[4];
        addressType = Payload[6];
        address = &Payload[7];
    } else {
        if (Length < 12u || Payload[3] != 1u) {
            return;
        }
        eventType = Payload[4];
        addressType = Payload[5];
        address = &Payload[6];
    }
    for (i = 0; i < Uart->AdvSeenCount; i++) {
        if (Uart->AdvSeen[i].AddressType == addressType &&
            RtlEqualMemory(Uart->AdvSeen[i].Address, address, sizeof(Uart->AdvSeen[i].Address))) {
            Uart->AdvSeen[i].EventType = eventType;
            Uart->AdvSeen[i].Count++;
            return;
        }
    }
    if (Uart->AdvSeenCount < QCA_UART_ADV_TABLE_SLOTS) {
        QCA_ADV_SEEN *seen = &Uart->AdvSeen[Uart->AdvSeenCount++];
        RtlCopyMemory(seen->Address, address, sizeof(seen->Address));
        seen->AddressType = addressType;
        seen->EventType = eventType;
        seen->Count = 1;
    }
}

/*
 * Called by H4DecoderFeed whenever a complete packet is decoded.
 * Runs in the read completion DPC path (at DISPATCH_LEVEL).
 */
static void
QcaUartOnH4Packet(
    void *Context,
    unsigned char Type,
    const unsigned char *Payload,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Context;

    if (uart->ProbeMode == QcaProbeModeIdentify) {
        if (!uart->FsmWaitingForEvent) {
            return;
        }
        UCHAR idBuf[HCI_MAX_EVENT_SIZE + 1];
        if (Length + 1 <= sizeof(idBuf)) {
            idBuf[0] = Type;
            RtlCopyMemory(&idBuf[1], Payload, Length);

            if (QcaIdentifyOnPacket(&uart->Identify, idBuf, Length + 1)) {
                ULONG copyLen = Length + 1;
                if (copyLen > sizeof(uart->LastIdentifyPacket)) {
                    copyLen = sizeof(uart->LastIdentifyPacket);
                }
                RtlCopyMemory(uart->LastIdentifyPacket, idBuf, copyLen);
                uart->LastIdentifyPacketLen = copyLen;
                uart->FsmEventMatched = TRUE;
                KeSetEvent(&uart->FsmEvent, IO_NO_INCREMENT, FALSE);
            }
        }
        return;
    }

    if (uart->Phase == 1) {
        /*
         * Phase 1: Bring-up FSM.
         * QcaFsmOnPacket expects the packet with the H4 prefix byte included.
         */
        UCHAR fsmBuf[HCI_MAX_EVENT_SIZE + 1];
        if (Length + 1 <= sizeof(fsmBuf)) {
            QCA_FSM_STATE responseState = uart->Fsm.State;
            fsmBuf[0] = Type;
            RtlCopyMemory(&fsmBuf[1], Payload, Length);

            if (uart->Fsm.State == QcaFsmStateHciReset && Type == H4_PKT_EVENT) {
                ULONG copyLen = Length;
                if (copyLen > sizeof(uart->LastHciResetEvent)) {
                    copyLen = sizeof(uart->LastHciResetEvent);
                }
                RtlCopyMemory(uart->LastHciResetEvent, Payload, copyLen);
                uart->LastHciResetEventLen = copyLen;
                uart->HciResetCaptured = TRUE;
            }

            if (uart->FsmWaitingForEvent &&
                QcaFsmOnPacket(&uart->Fsm, fsmBuf, Length + 1)) {
                if (responseState == QcaFsmStatePatchDownload ||
                    responseState == QcaFsmStateNvmDownload) {
                    InterlockedIncrement((LONG*)&uart->ProbeTlvSegmentsAcked);
                }
                uart->FsmWaitingForEvent = FALSE;
                uart->FsmEventMatched = TRUE;
                KeSetEvent(&uart->FsmEvent, IO_NO_INCREMENT, FALSE);
            }
        }
    } else if (uart->Phase == 2) {
        unsigned char queued = (unsigned char)0;
        HCI_STREAM stream = HciStreamEvent;

        /*
         * QcaUartReadCompletion holds the shared controller lock across DecoderFeed. That makes
         * decoder reset, packet assembly and bridge enqueue one ordering domain. Do not acquire
         * or release it here; doing so would recurse the WDF spin lock.
         */
        if (Type == H4_PKT_EVENT) {
            UCHAR fixed[6];
            if (Length >= 3u && Payload[0] == HCI_EV_LE_META &&
                (Payload[2] == HCI_LE_SUBEV_ADV_REPORT || Payload[2] == HCI_LE_SUBEV_DIRECT_ADV_REPORT ||
                 Payload[2] == HCI_LE_SUBEV_EXT_ADV_REPORT)) {
                /* Advertising reports are counted, not traced, to avoid rapidly flushing the event trace ring. */
                uart->AdvReports++;
                QcaUartNoteAdvertiser(uart, Payload, Length);
            } else {
                PUCHAR slot = uart->EventTrace[uart->EventTraceCount % QCA_UART_EVENT_TRACE_SLOTS];
                RtlZeroMemory(slot, QCA_UART_EVENT_TRACE_STRIDE);
                RtlCopyMemory(slot, Payload, Length < QCA_UART_EVENT_TRACE_STRIDE ? Length : QCA_UART_EVENT_TRACE_STRIDE);
                uart->EventTraceCount++;
            }
            if (Payload[0] == HCI_EV_SYNC_CONN_COMPLETE || Payload[0] == HCI_EV_SYNC_CONN_CHANGED) {
                /* The 8-byte trace slot stops before link type, air mode and packet lengths. */
                RtlZeroMemory(uart->ScoLinkEvent, sizeof(uart->ScoLinkEvent));
                RtlCopyMemory(uart->ScoLinkEvent, Payload,
                              Length < sizeof(uart->ScoLinkEvent) ? Length : sizeof(uart->ScoLinkEvent));
                uart->ScoLinkEvents++;
            }
            /*
             * BTHPORT sends Write_LE_Host_Support with Simultaneous_LE_Host = 1 (6D 0C 02 01 01),
             * which this controller rejects with 0x11 (Unsupported Feature or Parameter Value).
             * If rejected, BTHPORT does not start LE scanning. The completion status is therefore
             * rewritten to success (0x00) for BTHPORT, while the event trace retains the raw answer.
             */
            if (Length == sizeof(fixed) && Payload[0] == HCI_EV_COMMAND_COMPLETE && Payload[1] == 4u &&
                Payload[3] == (UCHAR)(HCI_OP_WRITE_LE_HOST_SUPPORTED & 0xFFu) &&
                Payload[4] == (UCHAR)(HCI_OP_WRITE_LE_HOST_SUPPORTED >> 8) &&
                Payload[5] == HCI_ERR_UNSUPPORTED_FEATURE) {
                RtlCopyMemory(fixed, Payload, sizeof(fixed));
                fixed[5] = 0x00;
                Payload = fixed;
            }
            /* A voice setup this bridge rewrote to its Enhanced form is answered to BTHPORT under
             * the legacy opcode it sent (sco_route.h); the trace above keeps the controller's own. */
            if (Length == sizeof(fixed) && Payload != fixed) {
                RtlCopyMemory(fixed, Payload, sizeof(fixed));
                if (ScoRouteRestoreEvent(&uart->ScoRoute, fixed, sizeof(fixed))) {
                    Payload = fixed;
                }
            }
            stream = HciStreamEvent;
            queued = HciBridgeOnEvent(&uart->Bridge, Payload, Length);
        } else if (Type == H4_PKT_ACL) {
            stream = HciStreamAcl;
            queued = HciBridgeOnAcl(&uart->Bridge, Payload, Length);
        } else if (Type == H4_PKT_SCO) {
            stream = HciStreamSco;
            queued = HciBridgeOnSco(&uart->Bridge, Payload, Length);
        }
        if (queued) {
            uart->PendingNotifyMask |= (1u << (ULONG)stream);
        }
    }
}

/* ---------------------------------------------------------------- Phase 1 Bring-Up FSM */

static NTSTATUS
QcaUartLoadFirmwareFile(
    _In_ PCWSTR FilePath,
    _Outptr_result_bytebuffer_(*FileSize) PUCHAR *FileBuffer,
    _Out_ PULONG FileSize)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK ioStatus;
    UNICODE_STRING uPath;
    HANDLE fileHandle = NULL;
    FILE_STANDARD_INFORMATION fileInfo;
    PUCHAR buffer = NULL;
    LARGE_INTEGER byteOffset;

    *FileBuffer = NULL;
    *FileSize = 0;

    RtlInitUnicodeString(&uPath, FilePath);
    InitializeObjectAttributes(
        &oa,
        &uPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    status = ZwCreateFile(
        &fileHandle,
        FILE_GENERIC_READ,
        &oa,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwQueryInformationFile(
        fileHandle,
        &ioStatus,
        &fileInfo,
        sizeof(fileInfo),
        FileStandardInformation);
    if (!NT_SUCCESS(status) ||
        fileInfo.EndOfFile.HighPart != 0 ||
        fileInfo.EndOfFile.LowPart == 0) {
        ZwClose(fileHandle);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        return (fileInfo.EndOfFile.LowPart == 0) ? STATUS_END_OF_FILE : STATUS_FILE_TOO_LARGE;
    }

    /* Firmware parsing and segment copies also run under Uart->Lock at DISPATCH_LEVEL. */
    buffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        fileInfo.EndOfFile.LowPart,
        QCA_UART_POOL_TAG);
    if (buffer == NULL) {
        ZwClose(fileHandle);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    byteOffset.QuadPart = 0;
    status = ZwReadFile(
        fileHandle,
        NULL,
        NULL,
        NULL,
        &ioStatus,
        buffer,
        fileInfo.EndOfFile.LowPart,
        &byteOffset,
        NULL);
    ZwClose(fileHandle);

    if (!NT_SUCCESS(status) || ioStatus.Information != fileInfo.EndOfFile.LowPart) {
        ExFreePoolWithTag(buffer, QCA_UART_POOL_TAG);
        return NT_SUCCESS(status) ? STATUS_END_OF_FILE : status;
    }

    *FileBuffer = buffer;
    *FileSize = fileInfo.EndOfFile.LowPart;
    return STATUS_SUCCESS;
}

static NTSTATUS
QcaUartWriteSynchronous(
    _Inout_ PQCA_UART Uart,
    _In_reads_bytes_(Length) const UCHAR *Buffer,
    _In_ ULONG Length)
{
    WDF_MEMORY_DESCRIPTOR memDesc;
    WDF_REQUEST_SEND_OPTIONS sendOptions;
    ULONG_PTR bytesWritten = 0;
    ULONG budgetMs = QcaUartRequestBudget(Uart, 5000);
    NTSTATUS status;
    if (budgetMs == 0) {
        return STATUS_CANCELLED;
    }

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memDesc, (PVOID)Buffer, Length);
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&sendOptions, WDF_REL_TIMEOUT_IN_MS(budgetMs));

    status = WdfIoTargetSendWriteSynchronously(
        Uart->IoTarget,
        NULL,
        &memDesc,
        NULL,
        &sendOptions,
        &bytesWritten);
    InterlockedExchangeAdd((volatile LONG *)&Uart->TotalBytesWritten, (LONG)bytesWritten);
    return NT_SUCCESS(status) && bytesWritten != Length ? STATUS_DATA_ERROR : status;
}


/*
 * Publish the operation about to run. A synchronous call that never returns leaves this as the
 * last committed record, so a watchdog abandonment names the exact blocked call.
 */
static VOID
QcaUartPublishPhase(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record,
    _In_ ULONG Step,
    _In_ PCWSTR Phase)
{
    Record->LastStep = Step;
    (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), Phase);
    QcaUartPublishProbe(Uart);
}

/*
 * Stop only the reader, not the WDF target. Target purge would stop accepting
 * subsequent configuration/requests, and restarting it could race a global probe cancel.
 * Cancellation delivery is not completion: do not change baud, reset the decoder, or
 * reuse the request until the last callback has retired. A stuck lower driver retains
 * ownership in the detached worker's existing release path, never a PnP/power callback.
 */
static NTSTATUS
QcaUartQuiesceRead(_Inout_ PQCA_UART Uart)
{
    ULONGLONG started = KeQueryInterruptTime();
    ULONG budgetMs = QcaUartRequestBudget(Uart, 2000);
    LARGE_INTEGER timeout;

    WdfSpinLockAcquire(Uart->Lock);
    Uart->FsmWaitingForEvent = FALSE;
    InterlockedExchange(&Uart->ReadPumpRunning, 0);
    WdfSpinLockRelease(Uart->Lock);
    while (!QcaUartIsIoIdle(Uart)) {
        ULONG elapsed;
        ULONG remaining;
        if (Uart->ReadRequests[0] != NULL) {
            (void)WdfRequestCancelSentRequest(Uart->ReadRequests[0]);
        }
        if (QcaUartIsIoIdle(Uart)) {
            break;
        }
        if (budgetMs == 0 || InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
            return STATUS_CANCELLED;
        }
        elapsed = (ULONG)((KeQueryInterruptTime() - started) / 10000);
        if (elapsed >= budgetMs) {
            return STATUS_IO_TIMEOUT;
        }
        remaining = budgetMs - elapsed;
        timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(remaining < 100 ? remaining : 100);
        (void)KeWaitForSingleObject(&Uart->IoIdle, Executive, KernelMode, FALSE, &timeout);
    }
    return STATUS_SUCCESS;
}

/*
 * Host side of the 0xFC48 switch, in the order the vendor driver used on this Deck (send, sleep,
 * purge, switch) and upstream uses for QCA2066 (send, sleep, switch): nothing is decoded while the
 * two rates disagree. The reader is retired, the controller gets QCA_UART_BAUD_SETTLE_MS to
 * switch, the host follows, whatever arrived meanwhile is purged, and a fresh reader with a reset
 * H4 decoder starts at the new rate. No reply to 0xFC48 is expected (qca_init_fsm.c).
 */
static NTSTATUS
QcaUartSwitchBaud(
    _Inout_ PQCA_UART Uart,
    _In_ ULONG BaudRate)
{
    NTSTATUS status;
    LARGE_INTEGER delay;
    ULONG purgeMask = SERIAL_PURGE_TXABORT | SERIAL_PURGE_RXABORT |
                      SERIAL_PURGE_TXCLEAR | SERIAL_PURGE_RXCLEAR;

    status = QcaUartQuiesceRead(Uart);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_BAUD_SETTLE_MS);
    (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    if (QcaUartRequestBudget(Uart, 1) == 0) {
        return STATUS_CANCELLED;
    }
    status = QcaUartSetBaudRate(Uart, BaudRate);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_PURGE, &purgeMask, sizeof(purgeMask), NULL, 0);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    return QcaUartStartReadPump(Uart);
}

NTSTATUS
QcaUartRunIdentify(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    NTSTATUS status;
    NTSTATUS drainStatus;
    NTSTATUS readStatus;
    PCWSTR failurePhase = L"DeviceState";
    ULONG failureStep = DECKBT_STEP_QCA_IDENTIFY_ENTER;
    ULONG currentRate = 0;
    ULONG budgetMs;
    PVOID waitObjects[2];
    LARGE_INTEGER timeout;
    UCHAR reqBuf[5];
    ULONG reqLen;
    ULONG i;
    BOOLEAN nextRate;
    BOOLEAN answered;

    PAGED_CODE();

    Record->IdentifyRan = 1;
    Record->IdentifyBaud = 0;
    Record->IdentifyAttempts = 0;
    Record->IdentifyRawHex[0] = L'\0';
    Record->LastStep = DECKBT_STEP_QCA_IDENTIFY_ENTER;
    Record->LastStatus = (ULONG)STATUS_SUCCESS;
    (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"IdentifyEnter");
    DeckBtRecordStep(DECKBT_STEP_QCA_IDENTIFY_ENTER, STATUS_SUCCESS);
    QcaUartPublishProbe(Uart);

    if (Uart->IoTarget == NULL || !Uart->IoTargetOpened ||
        Uart->Lock == NULL || Uart->Transport == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        Record->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), failurePhase);
        DeckBtRecordStep(failureStep, status);
        QcaUartPublishProbe(Uart);
        return status;
    }

    Record->SerialOpened = 1;
    WdfSpinLockAcquire(Uart->Lock);
    Uart->FsmWaitingForEvent = FALSE;
    QcaIdentifyInit(&Uart->Identify);
    Uart->LastIdentifyPacketLen = 0;
    WdfSpinLockRelease(Uart->Lock);
    InterlockedExchange(&Uart->IdentifyReadStatus, STATUS_SUCCESS);
    waitObjects[0] = &Uart->FsmEvent;
    waitObjects[1] = &Uart->ProbeStop;
    reqLen = QcaIdentifyBuildRequest(reqBuf, sizeof(reqBuf));
    status = reqLen == 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    failurePhase = L"IdentifyRequest";

    while (NT_SUCCESS(status)) {
        failurePhase = L"ReadDrain";
        failureStep = DECKBT_STEP_QCA_IDENTIFY_RUNG;
        status = QcaUartQuiesceRead(Uart);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (QcaUartRequestBudget(Uart, 1) == 0) {
            status = STATUS_CANCELLED;
            failurePhase = L"BudgetExhausted";
            break;
        }
        readStatus = (NTSTATUS)InterlockedCompareExchange(&Uart->IdentifyReadStatus, 0, 0);
        if (!NT_SUCCESS(readStatus)) {
            status = readStatus;
            failurePhase = L"IdentifyRead";
            break;
        }

        WdfSpinLockAcquire(Uart->Lock);
        answered = Uart->Identify.Answered;
        nextRate = answered ? FALSE : QcaIdentifyNextRate(&Uart->Identify, &currentRate);
        WdfSpinLockRelease(Uart->Lock);
        if (answered) {
            status = STATUS_SUCCESS;
            break;
        }
        if (!nextRate) {
            status = STATUS_NOT_FOUND;
            failurePhase = L"NoResponse";
            failureStep = DECKBT_STEP_QCA_IDENTIFY_DONE;
            break;
        }
        Record->IdentifyAttempts = Uart->Identify.Attempts;
        Record->BaudFinal = currentRate;
        Record->LastStep = DECKBT_STEP_QCA_IDENTIFY_RUNG;
        (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"IdentifyRung");
        DeckBtRecordStep(DECKBT_STEP_QCA_IDENTIFY_RUNG, STATUS_SUCCESS);
        QcaUartPublishProbe(Uart);

        /* No previous read remains while changing the line or clearing serial buffers. */
        failurePhase = L"BaudSet";
        failureStep = DECKBT_STEP_QCA_BAUD_SET;
        QcaUartPublishPhase(Uart, Record, failureStep, failurePhase);
        status = QcaUartSetBaudRate(Uart, currentRate);
        if (status == STATUS_INVALID_PARAMETER || status == STATUS_NOT_SUPPORTED) {
            /*
             * The host UART cannot produce this rate: the controller rejects 3,200,000
             * (48 MHz / 16 tops out at 3,000,000 baud). The line retains its previous rate
             * and nothing was sent at this rate, so the next rung remains valid.
             */
            status = STATUS_SUCCESS;
            continue;
        }
        if (!NT_SUCCESS(status)) {
            break;
        }
        {
            ULONG purgeMask = SERIAL_PURGE_TXABORT | SERIAL_PURGE_RXABORT |
                              SERIAL_PURGE_TXCLEAR | SERIAL_PURGE_RXCLEAR;
            failurePhase = L"Purge";
            failureStep = DECKBT_STEP_QCA_IDENTIFY_RUNG;
            QcaUartPublishPhase(Uart, Record, failureStep, failurePhase);
            status = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_PURGE,
                                                   &purgeMask, sizeof(purgeMask), NULL, 0);
        }
        if (!NT_SUCCESS(status)) {
            break;
        }

        /*
         * Never transmit into a deasserted CTS: the write would stall in the lower driver and has been
         * observed not to retire after cancellation. Checked every rung, before the reader
         * starts, because the controller may have gone back to sleep since the last rung.
         */
        failurePhase = L"CtsCheck";
        failureStep = DECKBT_STEP_QCA_CTS_CHECK;
        status = QcaUartWakeController(Uart, Record);
        if (!NT_SUCCESS(status)) {
            failureStep = Record->LastStep;
            failurePhase = failureStep == DECKBT_STEP_QCA_CTS_BLOCKED ? L"CtsNotAsserted" :
                           failureStep == DECKBT_STEP_QCA_CTS_WAKE ? L"CtsWake" : L"ModemStatus";
            break;
        }

        /* Clear the prior rung's signal before a new completion can report an error. */
        WdfSpinLockAcquire(Uart->Lock);
        KeClearEvent(&Uart->FsmEvent);
        Uart->FsmEventMatched = FALSE;
        WdfSpinLockRelease(Uart->Lock);

        failurePhase = L"ReadPumpStart";
        failureStep = DECKBT_STEP_QCA_READ_PUMP_START;
        QcaUartPublishPhase(Uart, Record, failureStep, failurePhase);
        status = QcaUartStartReadPump(Uart);
        if (!NT_SUCCESS(status)) {
            break;
        }
        WdfSpinLockAcquire(Uart->Lock);
        readStatus = (NTSTATUS)InterlockedCompareExchange(&Uart->IdentifyReadStatus, 0, 0);
        if (!NT_SUCCESS(readStatus)) {
            WdfSpinLockRelease(Uart->Lock);
            status = readStatus;
            failurePhase = L"IdentifyRead";
            break;
        }
        Uart->FsmWaitingForEvent = TRUE;
        WdfSpinLockRelease(Uart->Lock);

        failurePhase = L"IdentifyWrite";
        failureStep = DECKBT_STEP_QCA_IDENTIFY_REQ_SENT;
        QcaUartPublishPhase(Uart, Record, failureStep, failurePhase);
        status = QcaUartWriteSynchronous(Uart, reqBuf, reqLen);
        if (!NT_SUCCESS(status)) {
            /* A partial/failed command may have damaged H4 framing. Never try another rate. */
            break;
        }
        DeckBtRecordStep(DECKBT_STEP_QCA_IDENTIFY_REQ_SENT, STATUS_SUCCESS);

        budgetMs = QcaUartRequestBudget(Uart, 1500);
        if (budgetMs == 0) {
            status = STATUS_CANCELLED;
            failurePhase = L"BudgetExhausted";
            break;
        }
        failurePhase = L"IdentifyWait";
        QcaUartPublishPhase(Uart, Record, DECKBT_STEP_QCA_IDENTIFY_REQ_SENT, failurePhase);
        timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(budgetMs);
        status = KeWaitForMultipleObjects(2, waitObjects, WaitAny, Executive,
                                          KernelMode, FALSE, &timeout, NULL);
        if (status == STATUS_WAIT_1 ||
            InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
            status = STATUS_CANCELLED;
            failurePhase = L"Cancelled";
            break;
        }
        readStatus = (NTSTATUS)InterlockedCompareExchange(&Uart->IdentifyReadStatus, 0, 0);
        if (!NT_SUCCESS(readStatus)) {
            status = readStatus;
            failurePhase = L"IdentifyRead";
            break;
        }
        WdfSpinLockAcquire(Uart->Lock);
        answered = Uart->Identify.Answered;
        WdfSpinLockRelease(Uart->Lock);
        if (answered) {
            status = STATUS_SUCCESS;
            DeckBtRecordStep(DECKBT_STEP_QCA_IDENTIFY_RESP_RCV, status);
            break;
        }
        if (status != STATUS_TIMEOUT) {
            /* A signal without a validated reply is not a successful identification. */
            status = NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;
            break;
        }
        DeckBtRecordStep(DECKBT_STEP_QCA_IDENTIFY_TIMEOUT, STATUS_TIMEOUT);
        status = STATUS_SUCCESS;
    }

    drainStatus = QcaUartQuiesceRead(Uart);
    readStatus = (NTSTATUS)InterlockedCompareExchange(&Uart->IdentifyReadStatus, 0, 0);
    /* Retain the first transport fault. Cancellation/drain/read failure still outranks a reply
     * or healthy silence, including a reply that raced the failing write or final drain. */
    if (NT_SUCCESS(status) || status == STATUS_NOT_FOUND) {
        if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
            status = STATUS_CANCELLED;
            failurePhase = L"Cancelled";
        } else if (!NT_SUCCESS(drainStatus)) {
            status = drainStatus;
            failurePhase = L"ReadDrain";
        } else if (!NT_SUCCESS(readStatus)) {
            status = readStatus;
            failurePhase = L"IdentifyRead";
        }
    }

    Record->IdentifyAttempts = Uart->Identify.Attempts;
    Record->Aborted = status == STATUS_CANCELLED || status == STATUS_IO_TIMEOUT ||
        InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0 ? 1u : 0u;
    WdfSpinLockAcquire(Uart->Lock);
    if (NT_SUCCESS(status) && Uart->Identify.Answered) {
        static const WCHAR hexDigits[] = L"0123456789ABCDEF";
        ULONG hexIdx = 0;
        Record->IdentifyBaud = Uart->Identify.AnsweredRate;
        Record->SocId = Uart->Identify.Version.SocId;
        Record->RomVersion = (ULONG)Uart->Identify.Version.RomVersion;
        Record->ProductId = Uart->Identify.Version.ProductId;
        Record->PatchVersion = (ULONG)Uart->Identify.Version.PatchVersion;
        for (i = 0; i < Uart->LastIdentifyPacketLen && hexIdx + 2 < ARRAYSIZE(Record->IdentifyRawHex); i++) {
            UCHAR b = Uart->LastIdentifyPacket[i];
            Record->IdentifyRawHex[hexIdx++] = hexDigits[(b >> 4) & 0x0F];
            Record->IdentifyRawHex[hexIdx++] = hexDigits[b & 0x0F];
        }
        Record->IdentifyRawHex[hexIdx] = L'\0';
        failurePhase = L"Answered";
        failureStep = DECKBT_STEP_QCA_IDENTIFY_DONE;
    } else {
        Uart->Identify.Answered = FALSE;
        Uart->Identify.AnsweredRate = 0;
        Record->IdentifyBaud = 0;
        Record->IdentifyRawHex[0] = L'\0';
    }
    Uart->Identify.Done = TRUE;
    WdfSpinLockRelease(Uart->Lock);
    Record->LastStep = failureStep;
    Record->LastStatus = (ULONG)status;
    (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), failurePhase);
    DeckBtRecordStep(failureStep, status);
    QcaUartPublishProbe(Uart);
    return status;
}

/*
 * Hardware reset sequence: at the rate the controller is running, IBS wake 0xFD three times,
 * 10 ms settle time, SoC reset 0xFC40 (upstream qca_edl_reset_soc for product 0x13 / DEV_2066),
 * and 200 ms settle time. The controller then restarts in ROM at 115,200 baud; the host remains
 * at Baud. The CTS guard applies to every transmission.
 */
static NTSTATUS
QcaUartResetSoc(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Scratch,
    _In_ ULONG Baud)
{
    static const UCHAR ibsWake[] = { 0xFD, 0xFD, 0xFD };
    static const UCHAR socReset[] = { H4_PKT_COMMAND, 0x40, 0xFC, 0x00 };
    LARGE_INTEGER delay;
    NTSTATUS status;

    status = QcaUartQuiesceRead(Uart);
    if (NT_SUCCESS(status) && Uart->CurrentBaudRate != Baud) {
        status = QcaUartSetBaudRate(Uart, Baud);
    }
    if (NT_SUCCESS(status)) {
        status = QcaUartWakeController(Uart, Scratch);
    }
    if (NT_SUCCESS(status)) {
        status = QcaUartWriteSynchronous(Uart, ibsWake, sizeof(ibsWake));
    }
    if (NT_SUCCESS(status)) {
        delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_IBS_WAKE_SETTLE_MS);
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
        status = QcaUartWriteSynchronous(Uart, socReset, sizeof(socReset));
    }
    if (NT_SUCCESS(status)) {
        delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_SOC_RESET_SETTLE_MS);
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return status;
}

/*
 * Runs the identify ladder (115200, 3000000, 3200000) into Scratch without disturbing the caller's
 * probe mode. Returns the answering rate, 0 when the controller stayed silent (STATUS_NOT_FOUND)
 * or the ladder failed; *Status says which.
 */
static ULONG
QcaUartProbeRate(
    _Inout_ PQCA_UART Uart,
    _Out_ PQCA_UART_RECORD Scratch,
    _Out_ NTSTATUS *Status)
{
    QCA_PROBE_MODE mode = Uart->ProbeMode;

    RtlZeroMemory(Scratch, sizeof(*Scratch));
    Uart->ProbeMode = QcaProbeModeIdentify;
    *Status = QcaUartRunIdentify(Uart, Scratch);
    Uart->ProbeMode = mode;
    return NT_SUCCESS(*Status) ? Scratch->IdentifyBaud : 0u;
}

/*
 * Brings the controller to ROM at 115,200 baud from any previous state: ROM after a reboot
 * (answering at 115,200 baud), or a patched firmware running at 3,000,000 baud (answering
 * there, or silent while asleep in IBS). Any state other than an immediate 115,200 baud answer
 * triggers a SoC reset at the detected rate (or operating rate if silent), after which the
 * controller must answer at 115,200 baud. This avoids requiring a reboot between sessions.
 */
static NTSTATUS
QcaUartEnsureRom(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    QCA_UART_RECORD scratch;
    NTSTATUS status;
    ULONG baud;

    QcaUartPublishPhase(Uart, Record, DECKBT_STEP_QCA_ENSURE_ROM, L"EnsureRom");
    baud = QcaUartProbeRate(Uart, &scratch, &status);
    if (NT_SUCCESS(status) || status == STATUS_NOT_FOUND) {
        Record->EntryBaud = baud;
        if (baud == QCA_INIT_BAUD_RATE) {
            status = STATUS_SUCCESS;
        } else {
            Record->EntryReset = 1;
            status = QcaUartResetSoc(Uart, &scratch, baud != 0 ? baud : QCA_OPER_BAUD_RATE);
            if (NT_SUCCESS(status)) {
                baud = QcaUartProbeRate(Uart, &scratch, &status);
                if (NT_SUCCESS(status) && baud != QCA_INIT_BAUD_RATE) {
                    status = STATUS_INVALID_DEVICE_STATE;   /* reset did not take */
                }
            }
        }
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_ENSURE_ROM, status);
    if (!NT_SUCCESS(status)) {
        Record->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"EnsureRom");
    }
    QcaUartPublishProbe(Uart);
    return status;
}

/*
 * Return the controller to ROM after a probe. A controller left running patched firmware
 * at 3,000,000 baud is unreachable for drivers that start at 115,200 baud. Resets the SoC,
 * then verifies the result with the identify ladder: an answer at 115,200 baud confirms ROM.
 * Best effort; never changes the verdict, LastStep or FailurePhase of the probe itself.
 */
static VOID
QcaUartHandback(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    QCA_UART_RECORD scratch;
    WCHAR savedPhase[ARRAYSIZE(Record->FailurePhase)];
    ULONG savedStep = Record->LastStep;
    NTSTATUS status;

    RtlZeroMemory(&scratch, sizeof(scratch));
    RtlCopyMemory(savedPhase, Record->FailurePhase, sizeof(savedPhase));
    DeckBtRecordStep(DECKBT_STEP_QCA_HANDBACK, STATUS_SUCCESS);
    QcaUartPublishPhase(Uart, Record, DECKBT_STEP_QCA_HANDBACK, L"Handback");

    status = QcaUartResetSoc(Uart, &scratch, Uart->CurrentBaudRate);
    if (NT_SUCCESS(status)) {
        Record->HandbackBaud = QcaUartProbeRate(Uart, &scratch, &status);
    }
    Record->HandbackStatus = (ULONG)status;
    DeckBtRecordStep(DECKBT_STEP_QCA_HANDBACK, status);
    Record->LastStep = savedStep;
    RtlCopyMemory(Record->FailurePhase, savedPhase, sizeof(savedPhase));
    QcaUartPublishProbe(Uart);
}

/*
 * Samples the line and copies runtime counters into the record: diagnostic view of
 * BTHUSB traffic. PASSIVE_LEVEL, steady worker only (two synchronous serial IOCTLs).
 */
static VOID
QcaUartSnapshotSteady(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    const HCI_BRIDGE_COUNTERS *counters = &Uart->Bridge.Counters;
    SERIAL_STATUS line;
    ULONG modem = 0;
    ULONG eventCount;
    ULONG advCount;
    ULONG scoLinkCount;

    RtlZeroMemory(&line, sizeof(line));
    if (NT_SUCCESS(QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_GET_COMMSTATUS, NULL, 0,
                                                 &line, sizeof(line)))) {
        Record->LineErrors |= line.Errors;
        Record->LineHoldReasons = line.HoldReasons;
        Record->LineOutQueue = line.AmountInOutQueue;
    }
    if (NT_SUCCESS(QcaUartGetModemStatus(Uart, Record, &modem)) && (modem & SERIAL_CTS_STATE) == 0) {
        Record->CtsLowSamples++;
    }

    WdfSpinLockAcquire(Uart->Lock);
    Record->IbsWakeIndRx = Uart->IbsWakeIndRx;
    Record->IbsSleepIndRx = Uart->IbsSleepIndRx;
    Record->BridgeCommands = counters->CommandsSentToWire;
    Record->BridgeCommandsFailed = counters->CommandsFailedWire;
    Record->BridgeEventsReceived = counters->EventsReceived;
    Record->BridgeEventsQueued = counters->EventsQueued;
    Record->BridgeAclOut = counters->AclSentToWire;
    Record->BridgeAclIn = counters->AclQueued;
    Record->BridgeScoOut = counters->ScoSentToWire;
    Record->BridgeScoIn = counters->ScoReceived;
    Record->BridgeScoLost = counters->ScoOutboundOverwrites + counters->ScoInboundOverwrites +
                            counters->ScoOutDroppedNotReady + counters->ScoDroppedNotReady +
                            counters->ScoInboundDiscardedOversize;
    Record->ScoRouteRewritten = Uart->ScoRoute.Rewritten;
    Record->ScoRouteRestored = Uart->ScoRoute.Restored;
    RtlCopyMemory(Uart->EventTracePublished, Uart->EventTrace, sizeof(Uart->EventTracePublished));
    RtlCopyMemory(Uart->AdvSeenPublished, Uart->AdvSeen, sizeof(Uart->AdvSeenPublished));
    RtlCopyMemory(Uart->ScoLinkPublished, Uart->ScoLinkEvent, sizeof(Uart->ScoLinkPublished));
    eventCount = Uart->EventTraceCount;
    advCount = Uart->AdvSeenCount;
    scoLinkCount = Uart->ScoLinkEvents;
    WdfSpinLockRelease(Uart->Lock);
    DeckBtRecordTrace(L"EventLog", L"EventCount", &Uart->EventTracePublished[0][0],
                      sizeof(Uart->EventTracePublished), eventCount);
    DeckBtRecordTrace(L"AdvSeen", L"AdvSeenCount", (const UCHAR *)Uart->AdvSeenPublished,
                      sizeof(Uart->AdvSeenPublished), advCount);
    DeckBtRecordTrace(L"ScoLinkEvent", L"ScoLinkEvents", Uart->ScoLinkPublished,
                      sizeof(Uart->ScoLinkPublished), scoLinkCount);
    Record->IbsWakeAckTx = Uart->IbsWakeAckTx;
    Record->IbsAckCtsLow = Uart->IbsAckCtsLow;
    Record->IbsAckFailures = Uart->IbsAckFailures;
    Record->IbsAckLastStatus = Uart->IbsAckLastStatus;
    Record->WriteErrors = Uart->WriteErrors;
    Record->ReadErrors = Uart->ReadErrors;
    Record->ReadCompletions = Uart->ReadCompletions;
    Record->ReadBytes = Uart->TotalBytesRead;
    Record->AdvReports = Uart->AdvReports;
    Record->WriteCompletions = Uart->WriteCompletions;
    Record->LastWriteStatus = Uart->LastWriteStatus;
}

/*
 * Answers a pending controller WAKE_IND with WAKE_ACK, at PASSIVE_LEVEL on the steady worker.
 * The event is cleared before the pending flag is taken, so a WAKE_IND racing this call is
 * answered on the next pass. With CTS deasserted nothing is written: writes stalled on deasserted
 * CTS do not retire upon cancellation. The controller repeats an unanswered WAKE_IND, and skipped
 * acknowledgments are counted.
 */
static VOID
QcaUartServiceIbs(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    static const UCHAR wakeAck[] = { QCA_IBS_WAKE_ACK };
    ULONG modem = 0;
    NTSTATUS status;

    KeClearEvent(&Uart->IbsWorkEvent);
    if (InterlockedExchange(&Uart->IbsAckPending, 0) == 0) {
        return;
    }
    status = QcaUartGetModemStatus(Uart, Record, &modem);
    if (NT_SUCCESS(status) && (modem & SERIAL_CTS_STATE) == 0) {
        Uart->IbsAckCtsLow++;
        return;
    }
    if (NT_SUCCESS(status)) {
        status = QcaUartWriteSynchronous(Uart, wakeAck, sizeof(wakeAck));
    }
    if (NT_SUCCESS(status)) {
        Uart->IbsWakeAckTx++;
    } else {
        Uart->IbsAckFailures++;
        Uart->IbsAckLastStatus = (ULONG)status;
    }
}

/*
 * Drains events and SCO for up to TimeoutMs (5 ms steps), answering the controller's WAKE_IND as the
 * steady loop does. *Status = 0x100 | status of Opcode's Command Complete once seen (Opcode 0: just
 * wait). Loopback Connection Completes and SCO echoes are tallied into Record.
 */
static VOID
QcaUartScoLoopPoll(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record,
    _In_ USHORT Opcode,
    _Out_ PULONG Status,
    _In_ ULONG TimeoutMs)
{
    UCHAR packet[HCI_BRIDGE_MAX_EVENT_SIZE];
    LARGE_INTEGER delay;
    ULONG elapsed;
    ULONG written;
    unsigned char got;

    *Status = 0;
    delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(5);
    for (elapsed = 0; elapsed <= TimeoutMs; elapsed += 5) {
        for (;;) {
            written = 0;
            WdfSpinLockAcquire(Uart->Lock);
            got = HciTransportPopStream(&Uart->BridgeTransport, HciStreamEvent, packet, sizeof(packet), &written);
            WdfSpinLockRelease(Uart->Lock);
            if (!got) {
                break;
            }
            if (Opcode != 0 && written >= 6 && packet[0] == HCI_EV_COMMAND_COMPLETE &&
                packet[3] == (UCHAR)(Opcode & 0xFFu) && packet[4] == (UCHAR)(Opcode >> 8)) {
                *Status = 0x100u | packet[5];
            } else if (written >= 13 && packet[0] == 0x03u && packet[2] == 0) {
                Record->ScoLoopConnections++;
                if (packet[11] == 0x00u || packet[11] == 0x02u) {
                    Record->ScoLoopScoHandle = ((ULONG)packet[3] | ((ULONG)packet[4] << 8)) & 0x0FFFu;
                }
            }
        }
        for (;;) {
            written = 0;
            WdfSpinLockAcquire(Uart->Lock);
            got = HciTransportPopStream(&Uart->BridgeTransport, HciStreamSco, packet, sizeof(packet), &written);
            WdfSpinLockRelease(Uart->Lock);
            if (!got) {
                break;
            }
            Record->ScoLoopEchoed++;
            if (written == 3u + QCA_UART_SCO_LOOP_PAYLOAD && packet[2] == QCA_UART_SCO_LOOP_PAYLOAD &&
                (((ULONG)packet[0] | ((ULONG)packet[1] << 8)) & 0x0FFFu) == Record->ScoLoopScoHandle &&
                packet[3] < QCA_UART_SCO_LOOP_PACKETS) {
                ULONG j;
                BOOLEAN same = TRUE;

                for (j = 1; same && j < QCA_UART_SCO_LOOP_PAYLOAD; j++) {
                    same = (BOOLEAN)(packet[3 + j] == (UCHAR)(packet[3] * 7u + j));
                }
                if (same) {
                    Record->ScoLoopMatched++;
                }
            }
        }
        QcaUartServiceIbs(Uart, Record);
        if ((Opcode != 0 && *Status != 0) || InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
            return;
        }
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
}

/*
 * Opt-in SCO self-test (SelfTestScoLoopback=1 under the service Parameters), run once after the bridge
 * opens and before the USB child exists, so BTHPORT never sees it. HCI local loopback
 * (Write_Loopback_Mode 0x01, Core Vol 4 Part E 7.6.2) makes the controller report loopback links as
 * Connection Complete events (Link_Type 0x00 SCO or 0x02 eSCO) and echo host data sent on them.
 * Matching echoes prove that SCO packets cross the UART in both directions through the H4 codec and
 * the bridge's SCO FIFOs; they say nothing about the air path or BTHUSB's isochronous endpoints.
 * The test leaves loopback and resets the controller, so BTHPORT still starts from HCI_Reset. Its
 * traffic is included in the bridge counters (UartBridgeSco*) of that session.
 */
static VOID
QcaUartScoLoopback(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record)
{
    /* Not "leave": the kernel headers define it as the __leave keyword. */
    static const UCHAR loopOn[] = { 0x02, 0x18, 0x01, 0x01 };   /* Write_Loopback_Mode: local */
    static const UCHAR loopOff[] = { 0x02, 0x18, 0x01, 0x00 };  /* Write_Loopback_Mode: none */
    static const UCHAR hciReset[] = { 0x03, 0x0C, 0x00 };       /* HCI_Reset */
    UCHAR sco[3u + QCA_UART_SCO_LOOP_PAYLOAD];
    ULONG status;
    ULONG seq;
    ULONG j;

    Record->ScoLoopRan = 1;
    Record->ScoLoopScoHandle = 0xFFFFu;
    QcaUartPublishPhase(Uart, Record, DECKBT_STEP_QCA_BRIDGE_READY, L"ScoLoopback");
    WdfSpinLockAcquire(Uart->Lock);
    (void)HciTransportSubmitCommand(&Uart->BridgeTransport, loopOn, sizeof(loopOn));
    WdfSpinLockRelease(Uart->Lock);
    QcaUartScoLoopPoll(Uart, Record, QCA_UART_OP_WRITE_LOOPBACK, &status, 1000);
    Record->ScoLoopEnterStatus = status;
    /* The loopback links' Connection Complete events may trail the Command Complete. */
    QcaUartScoLoopPoll(Uart, Record, 0, &status, 300);

    if (Record->ScoLoopScoHandle != 0xFFFFu) {
        for (seq = 0; seq < QCA_UART_SCO_LOOP_PACKETS; seq++) {
            sco[0] = (UCHAR)(Record->ScoLoopScoHandle & 0xFFu);
            sco[1] = (UCHAR)(Record->ScoLoopScoHandle >> 8);
            sco[2] = (UCHAR)QCA_UART_SCO_LOOP_PAYLOAD;
            sco[3] = (UCHAR)seq;
            for (j = 1; j < QCA_UART_SCO_LOOP_PAYLOAD; j++) {
                sco[3 + j] = (UCHAR)(seq * 7u + j);
            }
            WdfSpinLockAcquire(Uart->Lock);
            if (HciTransportSubmitSco(&Uart->BridgeTransport, sco, sizeof(sco))) {
                Record->ScoLoopSent++;
            }
            WdfSpinLockRelease(Uart->Lock);
            QcaUartScoLoopPoll(Uart, Record, 0, &status, 5);   /* ~3 ms of 16-bit voice per packet */
        }
        QcaUartScoLoopPoll(Uart, Record, 0, &status, 300);
    }

    WdfSpinLockAcquire(Uart->Lock);
    (void)HciTransportSubmitCommand(&Uart->BridgeTransport, loopOff, sizeof(loopOff));
    WdfSpinLockRelease(Uart->Lock);
    QcaUartScoLoopPoll(Uart, Record, QCA_UART_OP_WRITE_LOOPBACK, &status, 1000);
    Record->ScoLoopLeaveStatus = status;
    WdfSpinLockAcquire(Uart->Lock);
    (void)HciTransportSubmitCommand(&Uart->BridgeTransport, hciReset, sizeof(hciReset));
    WdfSpinLockRelease(Uart->Lock);
    QcaUartScoLoopPoll(Uart, Record, QCA_UART_OP_HCI_RESET, &status, 2000);
    Record->ScoLoopResetStatus = status;

    /* BTHPORT must find an empty transport: nothing from the self-test may reach it. */
    WdfSpinLockAcquire(Uart->Lock);
    HciTransportReset(&Uart->BridgeTransport);
    WdfSpinLockRelease(Uart->Lock);
}

/*
 * Phase 2 steady-state operation. Entered after successful bring-up: reader active at the operating
 * rate, Phase 2, bridge not ready. Wakes the controller's receiver, opens the bridge, publishes
 * the USB child, serves BTHUSB until a stop, then closes the bridge and hands the controller back
 * to ROM so another driver can initialize it without a reboot. Returns the session verdict.
 */
static NTSTATUS
QcaUartServeSteady(_Inout_ PQCA_UART Uart)
{
    static const UCHAR wakeInd[] = { QCA_IBS_WAKE_IND };
    PQCA_UART_RECORD record = &Uart->Record;
    PVOID events[3];
    LARGE_INTEGER timeout;
    NTSTATUS status;
    NTSTATUS waitStatus;
    ULONG tries;
    ULONGLONG now;
    ULONGLONG nextPublish;

    /*
     * Steady reads complete on the first byte (SERIAL_TIMEOUTS remarks: ReadIntervalTimeout =
     * ReadTotalTimeoutMultiplier = MAXULONG, 0 < ReadTotalTimeoutConstant < MAXULONG).
     * Bring-up's 20 ms interval completes a read only after 20 ms of silence; a controller
     * repeating WAKE_IND faster than that is not heard or acknowledged, stalling event delivery.
     * The reader is quiesced first so no read request straddles the two timeout policies.
     */
    QcaUartPublishPhase(Uart, record, DECKBT_STEP_QCA_STEADY_READS, L"SteadyReads");
    status = QcaUartQuiesceRead(Uart);
    if (NT_SUCCESS(status)) {
        SERIAL_TIMEOUTS timeouts;
        RtlZeroMemory(&timeouts, sizeof(timeouts));
        timeouts.ReadIntervalTimeout = MAXULONG;
        timeouts.ReadTotalTimeoutMultiplier = MAXULONG;
        timeouts.ReadTotalTimeoutConstant = QCA_UART_STEADY_READ_IDLE_MS;
        timeouts.WriteTotalTimeoutConstant = QCA_UART_WRITE_TIMEOUT_MS;
        status = QcaUartSendIoctlSynchronously(Uart, IOCTL_SERIAL_SET_TIMEOUTS, &timeouts,
                                               sizeof(timeouts), NULL, 0);
    }
    if (NT_SUCCESS(status)) {
        status = QcaUartStartReadPump(Uart);
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_STEADY_READS, status);

    /*
     * Host wake (upstream hci_qca.c qca_wq_awake_device, hci_ibs_wake_retrans_timeout): WAKE_IND repeated
     * every 100 ms until WAKE_ACK. The host never sends SLEEP_IND afterwards, so one handshake keeps the
     * controller's receiver awake for the whole session and subsequent writes transmit immediately.
     * An unacknowledged wake is recorded (IbsHostAwake = 0), not fatal: BTHPORT request completion is the verdict.
     */
    if (NT_SUCCESS(status)) {
        QcaUartPublishPhase(Uart, record, DECKBT_STEP_QCA_IBS_WAKE, L"IbsWake");
        status = QcaUartWakeController(Uart, record);
    }
    events[0] = &Uart->IbsAckEvent;
    events[1] = &Uart->ProbeStop;
    for (tries = 0; NT_SUCCESS(status) && tries < QCA_UART_IBS_WAKE_TRIES &&
                    InterlockedCompareExchange(&Uart->IbsTxAwake, 0, 0) == 0; tries++) {
        KeClearEvent(&Uart->IbsAckEvent);
        status = QcaUartWriteSynchronous(Uart, wakeInd, sizeof(wakeInd));
        if (NT_SUCCESS(status)) {
            record->IbsWakeTries = tries + 1;
            timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_IBS_WAKE_RETRANS_MS);
            waitStatus = KeWaitForMultipleObjects(2, events, WaitAny, Executive, KernelMode,
                                                  FALSE, &timeout, NULL);
            if (waitStatus == STATUS_WAIT_1 ||
                InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
                status = STATUS_CANCELLED;
            } else {
                QcaUartServiceIbs(Uart, record);   /* the controller may want to talk meanwhile */
            }
        }
    }
    record->IbsHostAwake = InterlockedCompareExchange(&Uart->IbsTxAwake, 0, 0) != 0 ? 1u : 0u;
    DeckBtRecordStep(DECKBT_STEP_QCA_IBS_WAKE, status);

    if (NT_SUCCESS(status)) {
        WdfSpinLockAcquire(Uart->Lock);
        HciBridgeSetReady(&Uart->Bridge, 1);
        WdfSpinLockRelease(Uart->Lock);
        DeckBtRecordStep(DECKBT_STEP_QCA_BRIDGE_READY, STATUS_SUCCESS);
        QcaUartPublishPhase(Uart, record, DECKBT_STEP_QCA_BRIDGE_READY, L"BridgeReady");
        /* From here only a stop ends the session: no bring-up deadline, untimed reads. */
        InterlockedExchange(&Uart->BudgetUnbounded, 1);
        KeSetEvent(&Uart->SteadyReady, IO_NO_INCREMENT, FALSE);

        if (Uart->ScoLoopbackRequested) {
            Uart->ScoLoopbackRequested = FALSE;
            QcaUartScoLoopback(Uart, record);
        }

        /* Only a controller that answers through the bridge is ever shown to BTHUSB. */
        QcaUartPublishPhase(Uart, record, DECKBT_STEP_QCA_USB_PLUG, L"UsbPlugIn");
        status = DeckBtPublishUartDevice(Uart);
        record->UsbPlugStatus = (ULONG)status;
        DeckBtRecordStep(DECKBT_STEP_QCA_USB_PLUG, status);
    }

    if (NT_SUCCESS(status)) {
        record->SteadyReached = 1;
        record->LastStatus = (ULONG)STATUS_SUCCESS;
        QcaUartPublishPhase(Uart, record, DECKBT_STEP_QCA_START_DONE, L"Steady");
        events[0] = &Uart->ShutdownEvent;
        events[1] = &Uart->ProbeStop;
        events[2] = &Uart->IbsWorkEvent;
        nextPublish = KeQueryInterruptTime();
        for (;;) {
            now = KeQueryInterruptTime();
            if (now >= nextPublish) {
                QcaUartSnapshotSteady(Uart, record);
                QcaUartPublishProbe(Uart);
                nextPublish = now + (ULONGLONG)QCA_UART_STEADY_PUBLISH_MS * 10000ull;
            }
            now = KeQueryInterruptTime();
            timeout.QuadPart = -(LONGLONG)(nextPublish > now ? nextPublish - now : 0ull);
            waitStatus = KeWaitForMultipleObjects(3, events, WaitAny, Executive, KernelMode,
                                                  FALSE, &timeout, NULL);
            if (waitStatus == STATUS_WAIT_2) {
                QcaUartServiceIbs(Uart, record);
            } else if (waitStatus != STATUS_TIMEOUT) {
                break;
            }
        }
        if (waitStatus != STATUS_WAIT_0 || InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
            status = STATUS_CANCELLED;
        }
    } else {
        record->LastStatus = (ULONG)status;
    }

    /* Close the bridge before touching the controller: host traffic is held, never sent. */
    WdfSpinLockAcquire(Uart->Lock);
    InterlockedExchange(&Uart->Phase, 3);
    HciBridgeSetReady(&Uart->Bridge, 0);
    WdfSpinLockRelease(Uart->Lock);
    QcaUartSnapshotSteady(Uart, record);
    /* The handback is bounded again: restart the clock, then lift the steady exemption. */
    Uart->ProbeStarted = KeQueryInterruptTime();
    InterlockedExchange(&Uart->BudgetUnbounded, 0);
    if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) == 0) {
        if (NT_SUCCESS(status)) {
            QcaUartPublishPhase(Uart, record, DECKBT_STEP_QCA_SHUTDOWN, L"Shutdown");
        }
        QcaUartHandback(Uart, record);
    }
    QcaUartStop(Uart);
    if (NT_SUCCESS(status)) {
        record->LastStep = DECKBT_STEP_QCA_STOP;
        (void)RtlStringCchCopyW(record->FailurePhase, ARRAYSIZE(record->FailurePhase), L"Stopped");
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_STOP, status);
    return status;
}

NTSTATUS
QcaUartRunProbe(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD ProbeRecord)
{
    NTSTATUS status = STATUS_SUCCESS;
    PUCHAR patchData = NULL;
    ULONG patchSize = 0;
    QCA_NVM_CANDIDATE candidates[QCA_NVM_FILE_COUNT];
    PUCHAR nvmBuffers[QCA_NVM_FILE_COUNT];
    ULONG candidateCount = 0;
    ULONG i;

    RtlZeroMemory(candidates, sizeof(candidates));
    RtlZeroMemory(nvmBuffers, sizeof(nvmBuffers));
    WDF_OBJECT_ATTRIBUTES reqAttribs;
    WDF_OBJECT_ATTRIBUTES memAttribs;
    LARGE_INTEGER timeout;
    ULONG lastReportedAck = 0;

    PAGED_CODE();

    Uart->ProbeRecord = ProbeRecord;
    Uart->LastHciResetEventLen = 0;
    Uart->HciResetCaptured = FALSE;
    Uart->ProbeTlvSegmentsAcked = 0;

    if (Uart->IoTarget == NULL || !Uart->IoTargetOpened ||
        Uart->Lock == NULL || Uart->Transport == NULL) {
        status = STATUS_INVALID_DEVICE_STATE;
        ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_INIT;
        ProbeRecord->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"DeviceState");
        DeckBtRecordStep(DECKBT_STEP_QCA_FSM_INIT, status);
        QcaUartPublishProbe(Uart);
        return status;
    }

    for (i = 0; i < QCA_UART_WRITE_SLOTS; i++) {
        if (Uart->WriteSlots[i].Request == NULL) {
            WDF_OBJECT_ATTRIBUTES_INIT(&reqAttribs);
            reqAttribs.ParentObject = Uart->IoTarget;
            status = WdfRequestCreate(&reqAttribs, Uart->IoTarget, &Uart->WriteSlots[i].Request);
            if (!NT_SUCCESS(status)) {
                ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_INIT;
                ProbeRecord->LastStatus = (ULONG)status;
                (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"SlotAlloc");
                DeckBtRecordStep(DECKBT_STEP_QCA_FSM_INIT, status);
                QcaUartPublishProbe(Uart);
                goto Exit;
            }
            WdfObjectReference(Uart->WriteSlots[i].Request);
        }
        if (Uart->WriteSlots[i].Memory == NULL) {
            WDF_OBJECT_ATTRIBUTES_INIT(&memAttribs);
            memAttribs.ParentObject = Uart->WriteSlots[i].Request;
            status = WdfMemoryCreatePreallocated(
                &memAttribs,
                Uart->WriteSlots[i].Buffer,
                QCA_UART_WRITE_BUF_SIZE,
                &Uart->WriteSlots[i].Memory);
            if (!NT_SUCCESS(status)) {
                ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_INIT;
                ProbeRecord->LastStatus = (ULONG)status;
                (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"SlotAlloc");
                DeckBtRecordStep(DECKBT_STEP_QCA_FSM_INIT, status);
                QcaUartPublishProbe(Uart);
                goto Exit;
            }
            WdfObjectReference(Uart->WriteSlots[i].Memory);
        }
        Uart->WriteSlots[i].InUse = FALSE;
        Uart->WriteSlots[i].HasBeenSent = FALSE;
        Uart->WriteSlots[i].Owner = Uart;
    }

    /* Load firmware files */
    (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"PatchLoad");
    QcaUartPublishProbe(Uart);
    status = QcaUartLoadFirmwareFile(QCA_FW_DEFAULT_PATCH_PATH, &patchData, &patchSize);
    if (!NT_SUCCESS(status)) {
        ProbeRecord->LastStep = DECKBT_STEP_QCA_FW_NOT_FOUND;
        ProbeRecord->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"PatchLoad");
        DeckBtRecordStep(DECKBT_STEP_QCA_FW_NOT_FOUND, status);
        QcaUartPublishProbe(Uart);
        goto Exit;
    }
    if (QcaUartRequestBudget(Uart, 1) == 0) {
        status = STATUS_CANCELLED;
        goto Exit;
    }
    (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"NvmLoad");
    QcaUartPublishProbe(Uart);
    for (i = 0; i < QCA_NVM_FILE_COUNT; i++) {
        PUCHAR buf = NULL;
        ULONG sz = 0;
        status = QcaUartLoadFirmwareFile(g_QcaNvmFiles[i].Path, &buf, &sz);
        if (NT_SUCCESS(status)) {
            candidates[candidateCount].Name = g_QcaNvmFiles[i].Name;
            candidates[candidateCount].Data = buf;
            candidates[candidateCount].Size = sz;
            nvmBuffers[candidateCount] = buf;
            candidateCount++;
        }
    }
    if (candidateCount == 0) {
        status = STATUS_NOT_FOUND;
        ProbeRecord->LastStep = DECKBT_STEP_QCA_FW_NOT_FOUND;
        ProbeRecord->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"NvmLoad");
        DeckBtRecordStep(DECKBT_STEP_QCA_FW_NOT_FOUND, status);
        QcaUartPublishProbe(Uart);
        goto Exit;
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_FW_LOAD, STATUS_SUCCESS);
    ProbeRecord->LastStep = DECKBT_STEP_QCA_FW_LOAD;
    ProbeRecord->LastStatus = (ULONG)STATUS_SUCCESS;

    if (QcaUartRequestBudget(Uart, 1) == 0) {
        status = STATUS_CANCELLED;
        goto Exit;
    }
    if (!QcaFsmInit(&Uart->Fsm, patchData, patchSize, candidates, candidateCount, QCA_OPER_BAUD_RATE)) {
        status = STATUS_INVALID_PARAMETER;
        ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_INIT;
        ProbeRecord->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"FsmInit");
        DeckBtRecordStep(DECKBT_STEP_QCA_FSM_INIT, status);
        QcaUartPublishProbe(Uart);
        goto Exit;
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_FSM_INIT, STATUS_SUCCESS);

    /* Start from ROM at 115200 whatever the controller was left running; no reboot required. */
    status = QcaUartEnsureRom(Uart, ProbeRecord);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_INIT;
    ProbeRecord->LastStatus = (ULONG)STATUS_SUCCESS;
    (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"ReadPumpStart");
    QcaUartPublishProbe(Uart);

    InterlockedExchange(&Uart->Phase, 1);
    status = QcaUartStartReadPump(Uart);
    if (!NT_SUCCESS(status)) {
        ProbeRecord->LastStep = DECKBT_STEP_QCA_READ_PUMP_START;
        ProbeRecord->LastStatus = (ULONG)status;
        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"ReadPumpStart");
        DeckBtRecordStep(DECKBT_STEP_QCA_READ_PUMP_START, status);
        QcaUartPublishProbe(Uart);
        goto Exit;
    }
    DeckBtRecordStep(DECKBT_STEP_QCA_FSM_PUMP, STATUS_SUCCESS);
    ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_PUMP;
    ProbeRecord->LastStatus = (ULONG)STATUS_SUCCESS;
    (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"VersionRequest");
    QcaUartPublishProbe(Uart);

    /* Pump FSM until HCI_Reset is completed, or failure occurs */
    for (;;) {
        UCHAR cmdBuf[QCA_FSM_MAX_COMMAND];
        ULONG cmdLen = 0;
        ULONG newBaud = 0;
        BOOLEAN waitForEvent;
        QCA_FSM_ACTION action;
        QCA_FSM_STATE stateBefore;

        if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
            status = STATUS_CANCELLED;
            break;
        }
        stateBefore = Uart->Fsm.State;

        KeClearEvent(&Uart->FsmEvent);
        WdfSpinLockAcquire(Uart->Lock);
        action = QcaFsmNext(&Uart->Fsm, cmdBuf, &cmdLen, &newBaud);
        waitForEvent = (BOOLEAN)(action == QcaFsmActionSend);
        Uart->FsmEventMatched = FALSE;
        Uart->FsmWaitingForEvent = waitForEvent;
        WdfSpinLockRelease(Uart->Lock);

        /* Update progress counters based on state */
        ProbeRecord->TlvSegmentsAcked = Uart->ProbeTlvSegmentsAcked;
        if (stateBefore == QcaFsmStatePatchDownload) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"PatchDownload");
        } else if (stateBefore == QcaFsmStateBoardIdRequest) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"BoardIdRequest");
        } else if (stateBefore == QcaFsmStateNvmDownload) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"NvmDownload");
        } else if (stateBefore == QcaFsmStateDisableLogging) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"DisableLogging");
        } else if (stateBefore == QcaFsmStateBuildInfo) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"BuildInfo");
        } else if (stateBefore == QcaFsmStateHciReset) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"HciReset");
        }

        if (action == QcaFsmActionDone) {
            DeckBtRecordStep(DECKBT_STEP_QCA_FSM_READY, STATUS_SUCCESS);
            break;
        }
        if (action == QcaFsmActionFailed) {
            status = STATUS_UNSUCCESSFUL;
            ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_FAILED;
            ProbeRecord->LastStatus = (ULONG)status;
            DeckBtRecordStep(DECKBT_STEP_QCA_FSM_FAILED, status);
            QcaUartPublishProbe(Uart);
            break;
        }

        if (action == QcaFsmActionSendBaudAndSwitch) {
            (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"BaudSwitch");
            QcaUartPublishProbe(Uart);
        } else if (action != QcaFsmActionSend && action != QcaFsmActionSendNoWait) {
            status = STATUS_INVALID_DEVICE_STATE;
            ProbeRecord->LastStep = DECKBT_STEP_QCA_FSM_FAILED;
            ProbeRecord->LastStatus = (ULONG)status;
            DeckBtRecordStep(DECKBT_STEP_QCA_FSM_FAILED, status);
            QcaUartPublishProbe(Uart);
            break;
        }

        /* Publish the phase before touching a possibly silent controller. */
        if (stateBefore == QcaFsmStateBoardIdRequest) {
            /* Patch just applied; a lost board ID would silently select the .bin fallback. */
            LARGE_INTEGER settle;
            settle.QuadPart = WDF_REL_TIMEOUT_IN_MS(QCA_UART_NVM_SETTLE_MS);
            (void)KeDelayExecutionThread(KernelMode, FALSE, &settle);
            DeckBtRecordStep(DECKBT_STEP_QCA_BOARD_ID_REQ, STATUS_SUCCESS);
            ProbeRecord->LastStep = DECKBT_STEP_QCA_BOARD_ID_REQ;
        }
        QcaUartPublishProbe(Uart);
        status = QcaUartWriteSynchronous(Uart, cmdBuf, cmdLen);
        if (!NT_SUCCESS(status)) {
            if (stateBefore == QcaFsmStateBoardIdRequest) {
                /* Board ID read is optional: upstream ignores error and degrades to .bin */
                QcaFsmOnBoardIdTimeout(&Uart->Fsm);
                ProbeRecord->BoardIdValid = 0;
                ProbeRecord->BoardId = 0;
                ProbeRecord->LastStep = DECKBT_STEP_QCA_BOARD_ID_REQ;
                DeckBtRecordStep(DECKBT_STEP_QCA_BOARD_ID_REQ, status);
                QcaUartPublishProbe(Uart);
                continue;
            }
            ProbeRecord->LastStep = DECKBT_STEP_QCA_IO_ERROR;
            ProbeRecord->LastStatus = (ULONG)status;
            if (stateBefore == QcaFsmStateHciReset) {
                ProbeRecord->HciResetStatus = (ULONG)status;
            }
            DeckBtRecordStep(DECKBT_STEP_QCA_IO_ERROR, status);
            QcaUartPublishProbe(Uart);
            break;
        }
        if (stateBefore == QcaFsmStatePatchDownload && cmdLen >= 6) {
            ProbeRecord->PatchBytesSent += cmdLen - 6;
        } else if (stateBefore == QcaFsmStateNvmDownload && cmdLen >= 6) {
            ProbeRecord->NvmBytesSent += cmdLen - 6;
        } else if (stateBefore == QcaFsmStateHciReset) {
            ProbeRecord->HciResetSent = 1;
            ProbeRecord->LastStep = DECKBT_STEP_QCA_HCI_RESET_SENT;
        }
        QcaUartPublishProbe(Uart);

        if (action == QcaFsmActionSendBaudAndSwitch) {
            status = QcaUartSwitchBaud(Uart, newBaud);
            if (!NT_SUCCESS(status)) {
                ProbeRecord->LastStep = DECKBT_STEP_QCA_BAUD_SWITCH;
                ProbeRecord->LastStatus = (ULONG)status;
                DeckBtRecordStep(DECKBT_STEP_QCA_BAUD_SWITCH, status);
                QcaUartPublishProbe(Uart);
                break;
            }
            Uart->CurrentBaudRate = newBaud;
            ProbeRecord->BaudFinal = newBaud;
            DeckBtRecordStep(DECKBT_STEP_QCA_BAUD_SWITCH, STATUS_SUCCESS);
            ProbeRecord->LastStep = DECKBT_STEP_QCA_BAUD_SWITCH;
            ProbeRecord->LastStatus = (ULONG)STATUS_SUCCESS;
        }

        if (waitForEvent) {
            ULONG waitMs = QcaUartRequestBudget(Uart, 2000);
            if (waitMs == 0) {
                status = STATUS_CANCELLED;
                break;
            }
            timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(waitMs);
            status = KeWaitForSingleObject(
                &Uart->FsmEvent,
                Executive,
                KernelMode,
                FALSE,
                &timeout);
            if (InterlockedCompareExchange(&Uart->StopRequested, 0, 0) != 0) {
                status = STATUS_CANCELLED;
                break;
            }
            if (status == STATUS_TIMEOUT) {
                if (stateBefore == QcaFsmStateBoardIdRequest) {
                    /* Board ID read is optional: upstream ignores error and degrades to .bin */
                    QcaFsmOnBoardIdTimeout(&Uart->Fsm);
                    ProbeRecord->BoardIdValid = 0;
                    ProbeRecord->BoardId = 0;
                    ProbeRecord->LastStep = DECKBT_STEP_QCA_BOARD_ID_REQ;
                    DeckBtRecordStep(DECKBT_STEP_QCA_BOARD_ID_REQ, STATUS_TIMEOUT);
                    QcaUartPublishProbe(Uart);
                    continue;
                }
                if (stateBefore == QcaFsmStateNvmDownload && !Uart->Fsm.NvmFallbackUsed) {
                    QcaFsmOnNvmFailure(&Uart->Fsm);
                    if (Uart->Fsm.State != QcaFsmStateFailed) {
                        ProbeRecord->LastStep = DECKBT_STEP_QCA_NVM_FALLBACK;
                        DeckBtRecordStep(DECKBT_STEP_QCA_NVM_FALLBACK, STATUS_SUCCESS);
                        QcaUartPublishProbe(Uart);
                        continue;
                    }
                }
                status = STATUS_IO_TIMEOUT;
                ProbeRecord->LastStep = DECKBT_STEP_QCA_TIMEOUT;
                ProbeRecord->LastStatus = (ULONG)status;
                if (stateBefore == QcaFsmStateHciReset) {
                    ProbeRecord->HciResetStatus = (ULONG)status;
                }
                DeckBtRecordStep(DECKBT_STEP_QCA_TIMEOUT, status);
                QcaUartPublishProbe(Uart);
                break;
            }
            if (!NT_SUCCESS(status)) {
                if (stateBefore == QcaFsmStateBoardIdRequest) {
                    /* Board ID read is optional: upstream ignores error and degrades to .bin */
                    QcaFsmOnBoardIdTimeout(&Uart->Fsm);
                    ProbeRecord->BoardIdValid = 0;
                    ProbeRecord->BoardId = 0;
                    ProbeRecord->LastStep = DECKBT_STEP_QCA_BOARD_ID_REQ;
                    DeckBtRecordStep(DECKBT_STEP_QCA_BOARD_ID_REQ, status);
                    QcaUartPublishProbe(Uart);
                    continue;
                }
                if (stateBefore == QcaFsmStateNvmDownload && !Uart->Fsm.NvmFallbackUsed) {
                    QcaFsmOnNvmFailure(&Uart->Fsm);
                    if (Uart->Fsm.State != QcaFsmStateFailed) {
                        ProbeRecord->LastStep = DECKBT_STEP_QCA_NVM_FALLBACK;
                        DeckBtRecordStep(DECKBT_STEP_QCA_NVM_FALLBACK, STATUS_SUCCESS);
                        QcaUartPublishProbe(Uart);
                        continue;
                    }
                }
                ProbeRecord->LastStep = DECKBT_STEP_QCA_IO_ERROR;
                ProbeRecord->LastStatus = (ULONG)status;
                if (stateBefore == QcaFsmStateHciReset) {
                    ProbeRecord->HciResetStatus = (ULONG)status;
                }
                DeckBtRecordStep(DECKBT_STEP_QCA_IO_ERROR, status);
                QcaUartPublishProbe(Uart);
                break;
            }
            if (stateBefore == QcaFsmStateBoardIdRequest) {
                ProbeRecord->LastStep = DECKBT_STEP_QCA_BOARD_ID_DONE;
                DeckBtRecordStep(DECKBT_STEP_QCA_BOARD_ID_DONE, STATUS_SUCCESS);
            }
            /* Update segments acked */
            ProbeRecord->TlvSegmentsAcked = Uart->ProbeTlvSegmentsAcked;

            /* Check if this was the HCI_Reset response! */
            if (stateBefore == QcaFsmStateHciReset) {
                ULONG evLen = Uart->LastHciResetEventLen;
                ProbeRecord->HciResetEventLen = evLen;
                if (evLen > 0 && evLen <= HCI_MAX_EVENT_SIZE) {
                    ULONG hexIdx = 0;
                    static const WCHAR hexDigits[] = L"0123456789ABCDEF";
                    for (i = 0; i < evLen; i++) {
                        UCHAR b = Uart->LastHciResetEvent[i];
                        ProbeRecord->HciResetEventHex[hexIdx++] = hexDigits[(b >> 4) & 0x0F];
                        ProbeRecord->HciResetEventHex[hexIdx++] = hexDigits[b & 0x0F];
                    }
                    ProbeRecord->HciResetEventHex[hexIdx] = L'\0';
                }

                /* In Command Complete (0x0E), byte 5 is the HCI status */
                if (evLen >= 6 && Uart->LastHciResetEvent[0] == 0x0E) {
                    UCHAR hciStatus = Uart->LastHciResetEvent[5];
                    if (hciStatus == 0) {
                        ProbeRecord->HciResetStatus = (ULONG)STATUS_SUCCESS;
                        ProbeRecord->LastStep = DECKBT_STEP_QCA_HCI_RESET_DONE;
                        ProbeRecord->LastStatus = (ULONG)STATUS_SUCCESS;
                        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"None");
                        DeckBtRecordStep(DECKBT_STEP_QCA_HCI_RESET_DONE, STATUS_SUCCESS);
                        DeckBtRecordStep(DECKBT_STEP_QCA_PROBE_DONE, STATUS_SUCCESS);
                        QcaUartPublishProbe(Uart);
                        status = STATUS_SUCCESS;
                    } else {
                        status = STATUS_UNSUCCESSFUL;
                        ProbeRecord->HciResetStatus = (ULONG)status;
                        ProbeRecord->LastStep = DECKBT_STEP_QCA_HCI_RESET_DONE;
                        ProbeRecord->LastStatus = (ULONG)status;
                        (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"HciResetFailed");
                        DeckBtRecordStep(DECKBT_STEP_QCA_HCI_RESET_DONE, status);
                        QcaUartPublishProbe(Uart);
                    }
                } else {
                    status = STATUS_UNSUCCESSFUL;
                    ProbeRecord->HciResetStatus = (ULONG)status;
                    ProbeRecord->LastStep = DECKBT_STEP_QCA_HCI_RESET_DONE;
                    ProbeRecord->LastStatus = (ULONG)status;
                    (void)RtlStringCchCopyW(ProbeRecord->FailurePhase, ARRAYSIZE(ProbeRecord->FailurePhase), L"HciResetMalformed");
                    DeckBtRecordStep(DECKBT_STEP_QCA_HCI_RESET_DONE, status);
                    QcaUartPublishProbe(Uart);
                }

                /* Probe mode requirement: stop after HCI_Reset. */
                break;
            }

            /* Throttle registry writes during TLV download: write every 64 segments or on state transitions */
            if (ProbeRecord->TlvSegmentsAcked >= lastReportedAck + 64 ||
                Uart->Fsm.State != stateBefore) {
                lastReportedAck = ProbeRecord->TlvSegmentsAcked;
                QcaUartPublishProbe(Uart);
            }
        }
    }

Exit:
    if (Uart->ProbeMode == QcaProbeModeSteady && NT_SUCCESS(status)) {
        /*
         * Steady mode keeps the operating rate, the patch and the active reader. Phase 2 takes the
         * line under the decoder lock before the firmware buffers go, so no callback reaches the
         * FSM again; the bridge stays not-ready (host traffic held) until QcaUartServeSteady.
         */
        WdfSpinLockAcquire(Uart->Lock);
        Uart->FsmWaitingForEvent = FALSE;
        InterlockedExchange(&Uart->Phase, 2);
        WdfSpinLockRelease(Uart->Lock);
    } else {
        /*
         * Once the controller has been moved to the operating rate it no longer answers an
         * initial 115,200 baud bring-up. Hand it back in ROM before the terminal purge, while
         * the target still accepts requests. Skipped after cancellation: every request would
         * fail anyway.
         */
        if (ProbeRecord->BaudFinal != QCA_INIT_BAUD_RATE &&
            InterlockedCompareExchange(&Uart->StopRequested, 0, 0) == 0) {
            QcaUartHandback(Uart, ProbeRecord);
        }
        /* Always stop the read pump before returning */
        QcaUartStop(Uart);
    }
    /* Phase is retired (or handed to Phase 2) under the decoder lock: no callback touches firmware. */
    ProbeRecord->LastStatus = (ULONG)status;
    QcaUartPublishProbe(Uart);

    if (patchData != NULL) {
        ExFreePoolWithTag(patchData, QCA_UART_POOL_TAG);
    }
    for (i = 0; i < candidateCount; i++) {
        if (nvmBuffers[i] != NULL) {
            ExFreePoolWithTag(nvmBuffers[i], QCA_UART_POOL_TAG);
        }
    }
    return status;
}

VOID
QcaUartStop(
    _Inout_ PQCA_UART Uart)
{
    /*
     * Retiring the phase and clearing readiness is the mirror of the Phase 2 handoff and takes
     * the same single lock, so an in-flight read-completion DPC cannot be part-way through
     * queueing into the bridge while it is torn down. The read pump is stopped afterwards,
     * outside the lock, because cancellation may invoke completion inline.
     */
    if (Uart->Lock != NULL) {
        WdfSpinLockAcquire(Uart->Lock);
        InterlockedExchange(&Uart->Phase, 3);
        HciBridgeSetReady(&Uart->Bridge, 0);
        WdfSpinLockRelease(Uart->Lock);
    } else {
        InterlockedExchange(&Uart->Phase, 3);
    }

    /* Stop continuous read pump */
    QcaUartStopReadPump(Uart);
}

/* ---------------------------------------------------------------- Public transport adapter */

static unsigned char
QcaUartSubmitCommand(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Transport->Context;
    UCHAR enhanced[SCO_ROUTE_MAX_COMMAND];
    unsigned long length;

    /* Voice links go on the HCI data path, not the controller's default route (sco_route.h). */
    length = ScoRouteRewriteCommand(&uart->ScoRoute, Packet, Length, enhanced, sizeof(enhanced));
    if (length != 0) {
        return HciTransportSubmitCommand(&uart->BridgeTransport, enhanced, length);
    }
    return HciTransportSubmitCommand(&uart->BridgeTransport, Packet, Length);
}

static unsigned char
QcaUartSubmitAcl(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Transport->Context;
    return HciTransportSubmitAcl(&uart->BridgeTransport, Packet, Length);
}

static unsigned char
QcaUartSubmitSco(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    PQCA_UART uart = (PQCA_UART)Transport->Context;
    return HciTransportSubmitSco(&uart->BridgeTransport, Packet, Length);
}

static unsigned char
QcaUartHasStream(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream)
{
    const QCA_UART *uart = (const QCA_UART *)Transport->Context;
    return HciTransportHasStream(&uart->BridgeTransport, Stream);
}

static unsigned char
QcaUartPopStream(
    HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned char *Buffer,
    unsigned long Capacity,
    unsigned long *Written)
{
    PQCA_UART uart = (PQCA_UART)Transport->Context;
    return HciTransportPopStream(&uart->BridgeTransport, Stream, Buffer, Capacity, Written);
}

static unsigned long
QcaUartLastEventLength(
    const HCI_TRANSPORT *Transport)
{
    UNREFERENCED_PARAMETER(Transport);
    return 0; /* Event length is undefined for asynchronous backends. */
}

static void
QcaUartResetTransport(
    HCI_TRANSPORT *Transport)
{
    PQCA_UART uart = (PQCA_UART)Transport->Context;

    /*
     * The caller holds Uart->Lock. Read completion takes that same lock around DecoderFeed, so
     * resetting the partial H4 frame cannot splice pre-reset and post-reset bytes.
     */
    H4DecoderInit(&uart->Decoder);
    uart->PendingNotifyMask = 0;
    ScoRouteReset(&uart->ScoRoute);
    HciTransportReset(&uart->BridgeTransport);
}

VOID
QcaUartBindTransport(
    _Inout_ HCI_TRANSPORT *Transport,
    _Inout_ PQCA_UART Uart,
    _In_ WDFSPINLOCK ControllerLock)
{
    HCI_BRIDGE_WIRE wire;

    /*
     * ControllerLock is the front end's own spin lock - the one it holds across every
     * HciTransport* call. Adopted rather than creating a separate lock because the front end and
     * this backend mutate the same HCI_BRIDGE fields (EventHead/EventTail/EventCount, the ACL
     * credit pool, the SCO FIFOs). Two locks over one piece of state is the same as no lock.
     */
    Uart->Lock = ControllerLock;
    Uart->Transport = Transport;

    /*
     * Bind the generic bridge to a private transport, then publish a thin UART adapter. The
     * adapter adds the transport-specific decoder reset required by HCI_TRANSPORT without
     * duplicating bridge policy.
     */
    wire.Ops = &g_QcaUartWireOps;
    wire.Context = Uart;
    HciBridgeInit(&Uart->Bridge, &wire);
    RtlZeroMemory(&Uart->BridgeTransport, sizeof(Uart->BridgeTransport));
    HciBridgeBindTransport(&Uart->BridgeTransport, &Uart->Bridge);
    ScoRouteReset(&Uart->ScoRoute);

    Transport->Context = Uart;
    Transport->Ops = &g_QcaUartTransportOps;
    Transport->Backend = HCI_BACKEND_UART;
}

NTSTATUS
QcaUartRearmSteady(_Inout_ PQCA_UART Uart)
{
    HCI_BRIDGE_WIRE wire;

    if (!Uart->SerialFound || Uart->Lock == NULL) {
        return STATUS_NOT_FOUND;
    }
    if (InterlockedCompareExchange(&Uart->ProbeActive, 1, 0) != 0) {
        return STATUS_DEVICE_BUSY;
    }
    /*
     * Nothing from before the sleep may reach the reloaded controller: not a held command, not an
     * ACL credit or handle, not a queued event. The front end touches the bridge only under Lock.
     */
    wire.Ops = &g_QcaUartWireOps;
    wire.Context = Uart;
    WdfSpinLockAcquire(Uart->Lock);
    HciBridgeInit(&Uart->Bridge, &wire);
    RtlZeroMemory(&Uart->BridgeTransport, sizeof(Uart->BridgeTransport));
    HciBridgeBindTransport(&Uart->BridgeTransport, &Uart->Bridge);
    ScoRouteReset(&Uart->ScoRoute);
    WdfSpinLockRelease(Uart->Lock);
    Uart->ScoLoopbackRequested = FALSE;
    return QcaUartArmStored(Uart, QcaProbeModeSteady);
}

BOOLEAN
QcaUartIsReady(
    _In_ const QCA_UART *Uart)
{
    return (Uart->Phase == 2 && Uart->Bridge.Ready != 0);
}
