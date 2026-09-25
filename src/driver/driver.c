/*
 * driver.c - DriverEntry and PnP lifecycle for DeckBtUsb.
 *
 * The root-enumerated stub remains the safe default. The UART backend is selected explicitly
 * on a device that supplies the required serial connection resource; backend startup completes
 * before the emulated USB child is published.
 *
 * The driver records its progress in the registry. Without a debugger, a failing EvtDeviceAdd
 * surfaces only as CM_PROB_FAILED_ADD plus an NTSTATUS, which does not say which call failed.
 * DeckBtRecordStep writes the last attempted step and its status to
 *     HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters
 * so the failing call can be identified from user mode (tools\diag.ps1).
 */

#include <initguid.h>       /* must precede usbiodef.h so the GUID is instantiated here */
#include "deckbtusb.h"
#include <usbiodef.h>
#include <ntstrsafe.h>
DRIVER_INITIALIZE DriverEntry;

/* Captured in DriverEntry so DeckBtRecordStep can reach the driver's Parameters key. */
static WDFDRIVER g_DeckBtDriver = NULL;

/* Exactly one root/stub instance may consume and hold the process-wide one-shot arm. */
static volatile LONG g_DeckBtStubArmClaim = 0;
EX_RUNDOWN_REF DeckBtProbeRundown;
typedef struct _DECKBT_PROBE_THREAD {
    LIST_ENTRY Link;
    PETHREAD Thread;
    KEVENT Registered;
    PKSTART_ROUTINE StartRoutine;
    PVOID Context;
} DECKBT_PROBE_THREAD;
static LIST_ENTRY g_DeckBtProbeThreads;
static KSPIN_LOCK g_DeckBtProbeThreadsLock;

static VOID
DeckBtProbeThreadStart(_In_ PVOID Context)
{
    DECKBT_PROBE_THREAD *entry = (DECKBT_PROBE_THREAD *)Context;
    /* Referencing the current thread cannot fail, unlike lookup of an already-running handle. */
    entry->Thread = PsGetCurrentThread();
    ObReferenceObject(entry->Thread);
    KeSetEvent(&entry->Registered, IO_NO_INCREMENT, FALSE);
    entry->StartRoutine(entry->Context);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS
DeckBtCreateProbeThread(_In_ PKSTART_ROUTINE StartRoutine, _In_ PVOID Context)
{
    DECKBT_PROBE_THREAD *entry;
    OBJECT_ATTRIBUTES attributes;
    KIRQL irql;
    NTSTATUS status;
    HANDLE handle;
    if (!ExAcquireRundownProtection(&DeckBtProbeRundown)) {
        return STATUS_DELETE_PENDING;
    }
    entry = (DECKBT_PROBE_THREAD *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                 sizeof(*entry), DECKBT_POOL_TAG);
    if (entry == NULL) {
        ExReleaseRundownProtection(&DeckBtProbeRundown);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&entry->Registered, NotificationEvent, FALSE);
    entry->StartRoutine = StartRoutine;
    entry->Context = Context;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&handle, SYNCHRONIZE, &attributes,
                                  NULL, NULL, DeckBtProbeThreadStart, entry);
    if (NT_SUCCESS(status)) {
        KeAcquireSpinLock(&g_DeckBtProbeThreadsLock, &irql);
        InsertTailList(&g_DeckBtProbeThreads, &entry->Link);
        KeReleaseSpinLock(&g_DeckBtProbeThreadsLock, irql);
        ZwClose(handle);
    } else {
        ExFreePoolWithTag(entry, DECKBT_POOL_TAG);
    }
    ExReleaseRundownProtection(&DeckBtProbeRundown);
    return status;
}

VOID
DeckBtEvtDriverUnload(_In_ WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    /* Service stop may wait for a broken lower driver; PnP and power must not. */
    ExWaitForRundownProtectionRelease(&DeckBtProbeRundown);
    /* Rundown release precedes thread return. Join real thread objects before unloading code. */
    while (!IsListEmpty(&g_DeckBtProbeThreads)) {
        PLIST_ENTRY link = RemoveHeadList(&g_DeckBtProbeThreads);
        DECKBT_PROBE_THREAD *entry = CONTAINING_RECORD(link, DECKBT_PROBE_THREAD, Link);
        (void)KeWaitForSingleObject(&entry->Registered, Executive, KernelMode, FALSE, NULL);
        (void)KeWaitForSingleObject(entry->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(entry->Thread);
        ExFreePoolWithTag(entry, DECKBT_POOL_TAG);
    }
}

/* Writes one DWORD under the service Parameters key (PASSIVE_LEVEL). */
static VOID
DeckBtRecordValue(_In_z_ PCWSTR Name, _In_ ULONG Value)
{
    WDFKEY key = NULL;
    UNICODE_STRING name;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    RtlInitUnicodeString(&name, Name);
    (void)WdfRegistryAssignULong(key, &name, Value);
    WdfRegistryClose(key);
}

ULONG
DeckBtReadParameter(_In_z_ PCWSTR Name, _In_ ULONG Default)
{
    WDFKEY key = NULL;
    UNICODE_STRING name;
    ULONG value = Default;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return Default;
    }
    RtlInitUnicodeString(&name, Name);
    if (!NT_SUCCESS(WdfRegistryQueryULong(key, &name, &value))) {
        value = Default;
    }
    WdfRegistryClose(key);
    return value;
}

/*
 * Leaving D0 (disable, removal, system sleep): a steady UART session hands the controller back
 * to ROM here, bounded, so the vendor driver can own it again; everything else cancels at once.
 * A session that was serving BTHUSB and leaves for a sleep state (not D3Final: removal/disable)
 * is marked for re-arm on the next D0 entry.
 */
NTSTATUS
DeckBtEvtControllerD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDECKBT_CONTROLLER controller = DeckBtGetController(Device);

    controller->RearmOnD0Entry = (BOOLEAN)(TargetState != WdfPowerDeviceD3Final &&
                                           controller->BackendPrepared &&
                                           controller->ActiveBackend == HCI_BACKEND_UART &&
                                           controller->UsbDevice != NULL);
    QcaUartRequestStop(&controller->Qca);
    return STATUS_SUCCESS;
}

/*
 * Back in D0 after a sleep: bring the controller up again on the detached worker. The worker plugs
 * in a replacement USB child once the controller answers (DeckBtPublishUartDevice). D0 entry never
 * fails for this: a failed re-arm leaves the radio absent, recorded in ResumeLastStatus.
 */
NTSTATUS
DeckBtEvtControllerD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDECKBT_CONTROLLER controller = DeckBtGetController(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);
    if (!controller->RearmOnD0Entry) {
        return STATUS_SUCCESS;
    }
    controller->RearmOnD0Entry = FALSE;
    controller->ResumeStartedAt = KeQueryInterruptTime();
    status = QcaUartRearmSteady(&controller->Qca);
    controller->ResumeRearms++;
    DeckBtRecordValue(L"ResumeRearms", controller->ResumeRearms);
    DeckBtRecordValue(L"ResumeLastStatus", (ULONG)status);
    return STATUS_SUCCESS;
}

NTSTATUS
DeckBtPublishUartDevice(_In_ PQCA_UART Uart)
{
    PDECKBT_CONTROLLER controller = CONTAINING_RECORD(Uart, DECKBT_CONTROLLER, Qca);
    NTSTATUS status;
    BOOLEAN replaced = FALSE;
    ULONG attempt;

    (void)KeWaitForSingleObject(&controller->PlugGate, Executive, KernelMode, FALSE, NULL);
    if (!controller->BackendPrepared || controller->ActiveBackend != HCI_BACKEND_UART) {
        /* ReleaseHardware has begun: never plug in behind it. */
        status = STATUS_DEVICE_NOT_READY;
    } else {
        if (controller->UsbDevice != NULL) {
            /*
             * Resumed session: the child BTHPORT knows describes a controller that has since been
             * reloaded. Unplug it (surprise removal, like a dongle pulled out) and flush what it
             * left behind, so the replacement starts from a clean transport.
             */
            UDECXUSBDEVICE stale = controller->UsbDevice;

            controller->UsbDevice = NULL;
            replaced = TRUE;
            (void)UdecxUsbDevicePlugOutAndDelete(stale);
            controller->Ep0 = NULL;
            controller->EpEventIn = NULL;
            controller->EpAclOut = NULL;
            controller->EpAclIn = NULL;
            controller->EpScoOut = NULL;
            controller->EpScoIn = NULL;
            DeckBtScoFlush(controller);
            WdfSpinLockAcquire(controller->Lock);
            HciTransportReset(&controller->Transport);
            WdfSpinLockRelease(controller->Lock);
        }
        /* The port frees once UdeCx finishes removing a replaced child: retry for up to 5 s. */
        for (attempt = 0;; attempt++) {
            LARGE_INTEGER delay;

            status = DeckBtCreateUsbDevice(controller);
            if (NT_SUCCESS(status) || attempt >= 19) {
                break;
            }
            delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(250);
            (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
        if (NT_SUCCESS(status)) {
            DeckBtRecordStep(DECKBT_STEP_ARMED, STATUS_SUCCESS);
        }
        DeckBtRecordStep(DECKBT_STEP_PLUGGED_IN, status);
        if (replaced) {
            /* The resumed controller is back behind a fresh child. */
            if (NT_SUCCESS(status)) {
                controller->ResumeReplacements++;
            }
            DeckBtRecordValue(L"ResumeReplacements", controller->ResumeReplacements);
            DeckBtRecordValue(L"ResumeReadyMs",
                              (ULONG)((KeQueryInterruptTime() - controller->ResumeStartedAt) / 10000u));
            DeckBtRecordValue(L"ResumePlugAttempts", attempt + 1);
            DeckBtRecordValue(L"ResumePlugStatus", (ULONG)status);
        }
    }
    KeSetEvent(&controller->PlugGate, IO_NO_INCREMENT, FALSE);
    return status;
}

VOID
DeckBtEvtSurpriseRemoval(_In_ WDFDEVICE Device)
{
    QcaUartCancelProbe(&DeckBtGetController(Device)->Qca);
}

static VOID
DeckBtEvtControllerCleanup(_In_ WDFOBJECT Object)
{
    PDECKBT_CONTROLLER controller = DeckBtGetController((WDFDEVICE)Object);
    if (controller->WdfDevice != NULL) {
        QcaUartCancelProbe(&controller->Qca);
    }
}

/* Step codes, written to LastAddDeviceStep. Keep in sync with tools\diag.ps1. */
#define DECKBT_STEP_ENTER               1
#define DECKBT_STEP_UDECX_INIT          2
#define DECKBT_STEP_DEVICE_CREATE       3
#define DECKBT_STEP_DEVICE_INTERFACE    4
#define DECKBT_STEP_SPINLOCK            5
#define DECKBT_STEP_EVENT_QUEUE         6
#define DECKBT_STEP_ACL_QUEUE           7
#define DECKBT_STEP_ADD_EMULATION       8
#define DECKBT_STEP_ADD_DONE            9
#define DECKBT_STEP_SCO_PATH            10

VOID
DeckBtRecordStep(_In_ ULONG Step, _In_ NTSTATUS Status)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameStep;
    UNICODE_STRING nameStatus;

    if (g_DeckBtDriver == NULL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_DeckBtDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&nameStep, L"LastAddDeviceStep");
    RtlInitUnicodeString(&nameStatus, L"LastAddDeviceStatus");
    (void)WdfRegistryAssignULong(key, &nameStep, Step);
    (void)WdfRegistryAssignULong(key, &nameStatus, (ULONG)Status);

    WdfRegistryClose(key);
}

VOID
DeckBtRecordProbeProgress(_In_ const QCA_UART_RECORD *Record)
{
    WDFKEY key = NULL;
    UNICODE_STRING name;
    UNICODE_STRING strVal;
    NTSTATUS status;

    if (g_DeckBtDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_DeckBtDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    /*
     * Invalidate before touching any payload; commit only after every write succeeds.
     * Operators reread a terminal snapshot after observing the commit, since registry
     * value enumeration itself is not atomic. *Ran merely identifies the attempted mode.
     */
    RtlInitUnicodeString(&name, L"UartCompletion");
    status = WdfRegistryAssignULong(key, &name, QcaUartRecordInProgress);
    if (!NT_SUCCESS(status)) {
        goto Close;
    }

#define RECORD_DWORD(Field) \
    do { \
        RtlInitUnicodeString(&name, L"Uart" #Field); \
        status = WdfRegistryAssignULong(key, &name, Record->Field); \
        if (!NT_SUCCESS(status)) { goto Close; } \
    } while (0)
#define RECORD_STRING(Field) \
    do { \
        RtlInitUnicodeString(&name, L"Uart" #Field); \
        RtlInitUnicodeString(&strVal, Record->Field); \
        status = WdfRegistryAssignUnicodeString(key, &name, &strVal); \
        if (!NT_SUCCESS(status)) { goto Close; } \
    } while (0)

    RECORD_DWORD(ProbeRan);
    RECORD_DWORD(IdentifyRan);
    RECORD_DWORD(Aborted);
    RECORD_DWORD(ElapsedMs);
    RECORD_DWORD(SerialOpened);
    RECORD_DWORD(BaudInitial);
    RECORD_DWORD(BaudFinal);
    RECORD_DWORD(PatchBytesSent);
    RECORD_DWORD(NvmBytesSent);
    RECORD_DWORD(TlvSegmentsAcked);
    RECORD_DWORD(HciResetSent);
    RECORD_DWORD(HciResetStatus);
    RECORD_DWORD(HciResetEventLen);
    RECORD_STRING(HciResetEventHex);
    RECORD_DWORD(LastStep);
    RECORD_DWORD(LastStatus);
    RECORD_STRING(FailurePhase);
    RECORD_DWORD(SocId);
    RECORD_DWORD(RomVersion);
    RECORD_DWORD(BoardId);
    RECORD_DWORD(BoardIdValid);
    RECORD_STRING(NvmSelected);
    RECORD_DWORD(NvmFallback);
    RECORD_DWORD(IdentifyBaud);
    RECORD_DWORD(IdentifyAttempts);
    RECORD_DWORD(ProductId);
    RECORD_DWORD(PatchVersion);
    RECORD_STRING(IdentifyRawHex);
    RECORD_DWORD(ModemStatusReads);
    RECORD_DWORD(ModemStatusFirst);
    RECORD_DWORD(ModemStatusLast);
    RECORD_DWORD(WakePulses);
    RECORD_DWORD(CtsAsserted);
    RECORD_DWORD(HandbackBaud);
    RECORD_DWORD(HandbackStatus);
    RECORD_DWORD(EntryBaud);
    RECORD_DWORD(EntryReset);
    RECORD_DWORD(SteadyReached);
    RECORD_DWORD(UsbPlugStatus);
    RECORD_DWORD(IbsWakeTries);
    RECORD_DWORD(IbsHostAwake);
    RECORD_DWORD(IbsWakeIndRx);
    RECORD_DWORD(IbsWakeAckTx);
    RECORD_DWORD(IbsSleepIndRx);
    RECORD_DWORD(IbsAckCtsLow);
    RECORD_DWORD(IbsAckFailures);
    RECORD_DWORD(IbsAckLastStatus);
    RECORD_DWORD(BridgeCommands);
    RECORD_DWORD(BridgeCommandsFailed);
    RECORD_DWORD(BridgeEventsReceived);
    RECORD_DWORD(BridgeEventsQueued);
    RECORD_DWORD(BridgeAclOut);
    RECORD_DWORD(BridgeAclIn);
    RECORD_DWORD(BridgeScoOut);
    RECORD_DWORD(BridgeScoIn);
    RECORD_DWORD(BridgeScoLost);
    RECORD_DWORD(ScoLoopRan);
    RECORD_DWORD(ScoLoopEnterStatus);
    RECORD_DWORD(ScoLoopConnections);
    RECORD_DWORD(ScoLoopScoHandle);
    RECORD_DWORD(ScoLoopSent);
    RECORD_DWORD(ScoLoopEchoed);
    RECORD_DWORD(ScoLoopMatched);
    RECORD_DWORD(ScoLoopLeaveStatus);
    RECORD_DWORD(ScoLoopResetStatus);
    RECORD_DWORD(ScoRouteRewritten);
    RECORD_DWORD(ScoRouteRestored);
    RECORD_DWORD(WriteErrors);
    RECORD_DWORD(ReadErrors);
    RECORD_DWORD(ReadCompletions);
    RECORD_DWORD(ReadBytes);
    RECORD_DWORD(AdvReports);
    RECORD_DWORD(WriteCompletions);
    RECORD_DWORD(LastWriteStatus);
    RECORD_DWORD(LineErrors);
    RECORD_DWORD(LineHoldReasons);
    RECORD_DWORD(LineOutQueue);
    RECORD_DWORD(CtsLowSamples);
    RECORD_DWORD(Completion);

#undef RECORD_DWORD
#undef RECORD_STRING
Close:
    WdfRegistryClose(key);
}

VOID
DeckBtRecordTrace(_In_z_ PCWSTR LogName, _In_z_ PCWSTR CountName,
                  _In_reads_bytes_(Bytes) const UCHAR *Data, _In_ ULONG Bytes, _In_ ULONG Count)
{
    WDFKEY key = NULL;
    UNICODE_STRING name;

    if (g_DeckBtDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_DeckBtDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    RtlInitUnicodeString(&name, LogName);
    (void)WdfRegistryAssignValue(key, &name, REG_BINARY, Bytes, (PVOID)Data);
    RtlInitUnicodeString(&name, CountName);
    (void)WdfRegistryAssignULong(key, &name, Count);
    WdfRegistryClose(key);
}

VOID
DeckBtLogControl(_In_ PDECKBT_CONTROLLER Controller,
                 _In_reads_bytes_(8) const UCHAR *Setup,
                 _In_ ULONG Outcome,
                 _In_ USHORT Opcode,
                 _In_ UCHAR EventLength)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameBlob;
    UNICODE_STRING nameCount;
    ULONG slot;

    slot = Controller->CtlCount % DECKBT_CTL_LOG_SLOTS;
    RtlCopyMemory(&Controller->CtlLog[slot][0], Setup, 8);
    /* [8] outcome, [9] length of the event the stub produced, [10..11] HCI opcode LE.
     * The opcode is what identifies a command BTHUSB is unhappy with; without it a timeout or
     * a size complaint in the event log cannot be attributed to a specific request. */
    Controller->CtlLog[slot][8]  = (UCHAR)Outcome;
    Controller->CtlLog[slot][9]  = EventLength;
    Controller->CtlLog[slot][10] = (UCHAR)(Opcode & 0xFFu);
    Controller->CtlLog[slot][11] = (UCHAR)((Opcode >> 8) & 0xFFu);
    Controller->CtlCount++;

    if (g_DeckBtDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_DeckBtDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&nameBlob, L"ControlLog");
    RtlInitUnicodeString(&nameCount, L"ControlCount");
    (void)WdfRegistryAssignValue(key, &nameBlob, REG_BINARY,
                                 sizeof(Controller->CtlLog), &Controller->CtlLog[0][0]);
    (void)WdfRegistryAssignULong(key, &nameCount, Controller->CtlCount);

    WdfRegistryClose(key);
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, DeckBtEvtDeviceAdd);
    ExInitializeRundownProtection(&DeckBtProbeRundown);
    InitializeListHead(&g_DeckBtProbeThreads);
    KeInitializeSpinLock(&g_DeckBtProbeThreadsLock);
    config.EvtDriverUnload = DeckBtEvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                             &g_DeckBtDriver);
    if (NT_SUCCESS(status)) {
        DeckBtRecordStep(DECKBT_STEP_ENTER, status);
    }
    return status;
}

NTSTATUS
DeckBtEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    UDECX_WDF_DEVICE_CONFIG udecxConfig;
    WDFDEVICE device;
    PDECKBT_CONTROLLER controller;

    UNREFERENCED_PARAMETER(Driver);

    /* Must precede WdfDeviceCreate: this is what makes the WDFDEVICE a UDE host controller. */
    status = UdecxInitializeWdfDeviceInit(DeviceInit);
    DeckBtRecordStep(DECKBT_STEP_UDECX_INIT, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDevicePrepareHardware = DeckBtEvtDevicePrepareHardware;
    pnpPower.EvtDeviceReleaseHardware = DeckBtEvtDeviceReleaseHardware;
    pnpPower.EvtDeviceD0Entry = DeckBtEvtControllerD0Entry;
    pnpPower.EvtDeviceD0Exit = DeckBtEvtControllerD0Exit;
    pnpPower.EvtDeviceSurpriseRemoval = DeckBtEvtSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DECKBT_CONTROLLER);
    attributes.EvtCleanupCallback = DeckBtEvtControllerCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    DeckBtRecordStep(DECKBT_STEP_DEVICE_CREATE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    {
        WDF_DEVICE_PNP_CAPABILITIES pnpCaps;
        WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
        pnpCaps.Removable = WdfTrue;
        pnpCaps.SurpriseRemovalOK = WdfTrue;
        WdfDeviceSetPnpCapabilities(device, &pnpCaps);
    }

    /*
     * Publish the USB host controller interface. UCX/usbhub3 and the USB stack discover
     * emulated controllers through it; Microsoft's own UDE sample does this immediately after
     * WdfDeviceCreate, and omitting it was one of two deviations from that known-good sequence.
     */
    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
                                            NULL);
    DeckBtRecordStep(DECKBT_STEP_DEVICE_INTERFACE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    controller = DeckBtGetController(device);
    RtlZeroMemory(controller, sizeof(*controller));
    controller->WdfDevice = device;
    HciStubInit(&controller->Hci);
    QcaUartInit(&controller->Qca, device);
    controller->ActiveBackend = HCI_BACKEND_STUB;
    controller->BackendPrepared = FALSE;
    KeInitializeEvent(&controller->PlugGate, SynchronizationEvent, TRUE);
    HciStubBindTransport(&controller->Transport, &controller->Hci);
    controller->Transport.Notify = DeckBtTransportNotify;
    controller->Transport.NotifyContext = controller;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &controller->Lock);
    DeckBtRecordStep(DECKBT_STEP_SPINLOCK, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Manual queues parking IN requests until there is something to hand back. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              &controller->EventQueue);
    DeckBtRecordStep(DECKBT_STEP_EVENT_QUEUE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              &controller->AclInQueue);
    DeckBtRecordStep(DECKBT_STEP_ACL_QUEUE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* SCO voice: parked isochronous transfers and the timer that paces them (endpoints.c). */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              &controller->ScoOutQueue);
    if (NT_SUCCESS(status)) {
        WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
        queueConfig.PowerManaged = WdfFalse;
        status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                                  &controller->ScoInQueue);
    }
    if (NT_SUCCESS(status)) {
        WDF_TIMER_CONFIG timerConfig;

        /* One-shot, re-armed from its own callback while transfers are parked. 1 ms needs the
         * high-resolution timer; the default one fires on the 15.6 ms system tick. */
        WDF_TIMER_CONFIG_INIT(&timerConfig, DeckBtEvtScoTimer);
        timerConfig.AutomaticSerialization = FALSE;
        timerConfig.UseHighResolutionTimer = WdfTrue;
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = device;
        status = WdfTimerCreate(&timerConfig, &attributes, &controller->ScoTimer);
    }
    DeckBtRecordStep(DECKBT_STEP_SCO_PATH, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ScoUsbOutReset(&controller->ScoOut);
    ScoUsbInReset(&controller->ScoIn);

    /*
     * Leave the port counts at the UDECX_WDF_DEVICE_CONFIG_INIT defaults (one 2.0 and one 3.0
     * port). UCX may require a non-zero port count of each kind; the controller plugs into
     * the 2.0 port.
     */
    UDECX_WDF_DEVICE_CONFIG_INIT(&udecxConfig, DeckBtEvtQueryUsbCapability);

    status = UdecxWdfDeviceAddUsbDeviceEmulation(device, &udecxConfig);
    DeckBtRecordStep(DECKBT_STEP_ADD_EMULATION, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DeckBtRecordStep(DECKBT_STEP_ADD_DONE, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

VOID
DeckBtTransportNotify(void *NotifyContext, HCI_STREAM Stream)
{
    PDECKBT_CONTROLLER controller = (PDECKBT_CONTROLLER)NotifyContext;
    if (controller != NULL) {
        DeckBtDrainStream(controller, Stream);
    }
}

NTSTATUS
DeckBtEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status;
    NTSTATUS queryStatus;
    PDECKBT_CONTROLLER controller = DeckBtGetController(Device);
    WDFKEY paramKey = NULL;
    WDFKEY hwKey = NULL;
    ULONG enabled = 0;
    ULONG enabledLength = 0;
    ULONG enabledType = 0;
    UCHAR readSucceeded = 0;
    ULONG backend = HCI_BACKEND_STUB;
    ULONG probeVal = 0;
    ULONG probeLength = 0;
    ULONG probeType = 0;
    ULONG identifyVal = 0;
    ULONG identifyLength = 0;
    ULONG identifyType = 0;
    UNICODE_STRING valName;

    DeckBtRecordStep(DECKBT_STEP_PREPARE_HW, STATUS_SUCCESS);

    /* Open the service Parameters registry key. */
    status = WdfDriverOpenParametersRegistryKey(
        WdfGetDriver(), KEY_READ | KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &paramKey);
    if (!NT_SUCCESS(status)) {
        DeckBtRecordStep(DECKBT_STEP_DISARMED, status);
        return status;
    }

    /*
     * 1. Read UartProbe from service Parameters key with strict typing discipline.
     * Absent key/value defaults to 0 (normal behavior).
     * Any query failure other than NOT_FOUND fails loudly.
     * Wrong type (not REG_DWORD), wrong size, or value not in {0, 1} fails loudly.
     */
    RtlInitUnicodeString(&valName, L"UartProbe");
    queryStatus = WdfRegistryQueryValue(paramKey, &valName, sizeof(probeVal),
                                        &probeVal, &probeLength, &probeType);
    if (queryStatus == STATUS_OBJECT_NAME_NOT_FOUND ||
        queryStatus == STATUS_OBJECT_PATH_NOT_FOUND) {
        probeVal = 0;
    } else if (!NT_SUCCESS(queryStatus)) {
        WdfRegistryClose(paramKey);
        DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, queryStatus);
        return queryStatus;
    } else {
        if (probeType != REG_DWORD || probeLength != sizeof(ULONG)) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, STATUS_OBJECT_TYPE_MISMATCH);
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        if (probeVal != 0 && probeVal != 1) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, STATUS_INVALID_PARAMETER);
            return STATUS_INVALID_PARAMETER;
        }
    }

    /*
     * 2. Read UartIdentify from service Parameters key with identical strict discipline.
     */
    RtlInitUnicodeString(&valName, L"UartIdentify");
    queryStatus = WdfRegistryQueryValue(paramKey, &valName, sizeof(identifyVal),
                                        &identifyVal, &identifyLength, &identifyType);
    if (queryStatus == STATUS_OBJECT_NAME_NOT_FOUND ||
        queryStatus == STATUS_OBJECT_PATH_NOT_FOUND) {
        identifyVal = 0;
    } else if (!NT_SUCCESS(queryStatus)) {
        WdfRegistryClose(paramKey);
        DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, queryStatus);
        return queryStatus;
    } else {
        if (identifyType != REG_DWORD || identifyLength != sizeof(ULONG)) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, STATUS_OBJECT_TYPE_MISMATCH);
            return STATUS_OBJECT_TYPE_MISMATCH;
        }
        if (identifyVal != 0 && identifyVal != 1) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, STATUS_INVALID_PARAMETER);
            return STATUS_INVALID_PARAMETER;
        }
    }

    /*
     * 3. Mode selection, fail-closed:
     *    both present -> REFUSE: run neither, consume neither, record AmbiguousArmToken.
     *    identifyVal == 1 -> Identify mode (one-shot consume: write 0, flush, verify readback).
     *    probeVal == 1 -> Full probe mode (byte-for-byte unchanged behaviour).
     */
    if (probeVal == 1 && identifyVal == 1) {
        QCA_UART_RECORD record;
        RtlZeroMemory(&record, sizeof(record));
        record.ProbeRan = 0;
        record.IdentifyRan = 0;
        record.Completion = QcaUartRecordReleased;
        record.LastStatus = (ULONG)STATUS_INVALID_PARAMETER;
        record.LastStep = DECKBT_STEP_PROBE_MALFORMED;
        (void)RtlStringCchCopyW(record.FailurePhase, ARRAYSIZE(record.FailurePhase),
                                L"AmbiguousArmToken");
        WdfRegistryClose(paramKey);
        DeckBtRecordProbeProgress(&record);
        DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    if (identifyVal == 1) {
        RtlInitUnicodeString(&valName, L"UartIdentify");
        status = WdfRegistryAssignULong(paramKey, &valName, 0);
        if (NT_SUCCESS(status)) {
            status = ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
        }
        if (NT_SUCCESS(status)) {
            ULONG verifyVal = 1;
            ULONG verifyLen = 0;
            ULONG verifyType = 0;
            status = WdfRegistryQueryValue(paramKey, &valName, sizeof(verifyVal),
                                           &verifyVal, &verifyLen, &verifyType);
            if (NT_SUCCESS(status) &&
                (verifyType != REG_DWORD || verifyLen != sizeof(verifyVal) || verifyVal != 0)) {
                status = STATUS_DATA_ERROR;
            }
        }
        WdfRegistryClose(paramKey);
        if (!NT_SUCCESS(status)) {
            DeckBtRecordStep(DECKBT_STEP_PROBE_MALFORMED, status);
            return status;
        }
        if (InterlockedCompareExchange(&controller->Qca.ProbeActive, 0, 0) != 0) {
            return STATUS_DEVICE_BUSY;
        }
        QcaUartBindTransport(&controller->Transport, &controller->Qca, controller->Lock);
        return QcaUartArmProbe(&controller->Qca, ResourcesTranslated, QcaProbeModeIdentify);
    }

    if (probeVal == 1) {
        WdfRegistryClose(paramKey);
        if (InterlockedCompareExchange(&controller->Qca.ProbeActive, 0, 0) != 0) {
            return STATUS_DEVICE_BUSY;
        }
        QcaUartBindTransport(&controller->Transport, &controller->Qca, controller->Lock);
        return QcaUartArmProbe(&controller->Qca, ResourcesTranslated, QcaProbeModeFull);
    }

    /* Only exact REG_DWORD 1 arms; all other Enabled states leave a healthy idle devnode. */
    RtlInitUnicodeString(&valName, L"Enabled");
    status = WdfRegistryQueryValue(paramKey, &valName, sizeof(enabled),
                                   &enabled, &enabledLength, &enabledType);
    readSucceeded = (UCHAR)NT_SUCCESS(status);
    if (!DeckBtArmingGateArmed(readSucceeded, enabledType, enabledLength, enabled)) {
        if (paramKey != NULL) {
            WdfRegistryClose(paramKey);
        }
        DeckBtRecordStep(DECKBT_STEP_DISARMED, STATUS_SUCCESS);
        return STATUS_SUCCESS;
    }
    /*
     * A missing Backend value/key is the documented stub default. Every malformed value or
     * other access/query failure is returned and recorded verbatim; it must never become stub.
     */
    status = WdfDeviceOpenRegistryKey(Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                      WDF_NO_OBJECT_ATTRIBUTES, &hwKey);
    if (!NT_SUCCESS(status)) {
        if (status != STATUS_OBJECT_NAME_NOT_FOUND && status != STATUS_OBJECT_PATH_NOT_FOUND) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_BACKEND_UNKNOWN, status);
            return status;
        }
    } else {
        RtlInitUnicodeString(&valName, L"Backend");
        queryStatus = WdfRegistryQueryULong(hwKey, &valName, &backend);
        WdfRegistryClose(hwKey);
        if (queryStatus == STATUS_OBJECT_NAME_NOT_FOUND ||
            queryStatus == STATUS_OBJECT_PATH_NOT_FOUND) {
            backend = HCI_BACKEND_STUB;
        } else if (!NT_SUCCESS(queryStatus)) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_BACKEND_UNKNOWN, queryStatus);
            return queryStatus;
        }
    }

    if (backend != HCI_BACKEND_STUB && backend != HCI_BACKEND_UART) {
        WdfRegistryClose(paramKey);
        DeckBtRecordStep(DECKBT_STEP_BACKEND_UNKNOWN, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Root/stub arming is one-shot. The claim serializes multiple root instances; its winner
     * durably stores Enabled=0 before publication and holds the claim until ReleaseHardware.
     * UART policy remains unchanged pending an ACPI binding and rollback design.
     */
    if (backend == HCI_BACKEND_STUB) {
        DeckBtRecordStep(DECKBT_STEP_ARM_PENDING, STATUS_SUCCESS);
        if (InterlockedCompareExchange(&g_DeckBtStubArmClaim, 1, 0) != 0) {
            WdfRegistryClose(paramKey);
            DeckBtRecordStep(DECKBT_STEP_DISARMED, STATUS_DEVICE_BUSY);
            return STATUS_SUCCESS;
        }
        controller->StubArmClaimed = TRUE;

        RtlInitUnicodeString(&valName, L"Enabled");
        status = WdfRegistryAssignULong(paramKey, &valName, 0);
        if (NT_SUCCESS(status)) {
            status = ZwFlushKey(WdfRegistryWdmGetHandle(paramKey));
        }
        if (NT_SUCCESS(status)) {
            enabled = 1;
            enabledLength = 0;
            enabledType = 0;
            status = WdfRegistryQueryValue(paramKey, &valName, sizeof(enabled),
                                           &enabled, &enabledLength, &enabledType);
            if (NT_SUCCESS(status) &&
                (enabledType != REG_DWORD || enabledLength != sizeof(enabled) || enabled != 0)) {
                status = STATUS_DATA_ERROR;
            }
        }
        if (!NT_SUCCESS(status)) {
            WdfRegistryClose(paramKey);
            controller->StubArmClaimed = FALSE;
            InterlockedExchange(&g_DeckBtStubArmClaim, 0);
            DeckBtRecordStep(DECKBT_STEP_ARM_CONSUME, status);
            return status;
        }
        DeckBtRecordStep(DECKBT_STEP_ARM_CONSUME, STATUS_SUCCESS);
    }
    WdfRegistryClose(paramKey);

    controller->BackendPrepared = FALSE;
    controller->ActiveBackend = backend;
    if (backend == HCI_BACKEND_STUB) {
        HciStubInit(&controller->Hci);
        HciStubBindTransport(&controller->Transport, &controller->Hci);
        controller->BackendPrepared = TRUE;
        DeckBtRecordStep(DECKBT_STEP_BACKEND_STUB, STATUS_SUCCESS);
    } else {
        /*
         * UART: bring-up (seconds) runs on the detached worker, never on a PnP IRP whose lower
         * stack may ignore cancellation. The device starts now without a USB child; the worker
         * plugs one in through DeckBtPublishUartDevice only once the controller answers through
         * the bridge, and D0Exit/ReleaseHardware stop it gracefully (QcaUartRequestStop).
         */
        UNREFERENCED_PARAMETER(ResourcesRaw);
        if (InterlockedCompareExchange(&controller->Qca.ProbeActive, 0, 0) != 0) {
            /* A previous session's worker is still retiring; a restart will find it gone. */
            controller->ActiveBackend = HCI_BACKEND_STUB;
            DeckBtRecordStep(DECKBT_STEP_BACKEND_UART, STATUS_DEVICE_BUSY);
            return STATUS_DEVICE_BUSY;
        }
        QcaUartBindTransport(&controller->Transport, &controller->Qca, controller->Lock);
        controller->BackendPrepared = TRUE;
        status = QcaUartArmProbe(&controller->Qca, ResourcesTranslated, QcaProbeModeSteady);
        DeckBtRecordStep(DECKBT_STEP_BACKEND_UART, status);
        if (!NT_SUCCESS(status)) {
            controller->BackendPrepared = FALSE;
            controller->ActiveBackend = HCI_BACKEND_STUB;
            HciStubBindTransport(&controller->Transport, &controller->Hci);
        }
        return status;
    }

    /* No USB device is visible to BTHUSB until its selected backend is fully ready. */
    status = DeckBtCreateUsbDevice(controller);
    if (!NT_SUCCESS(status)) {
        controller->BackendPrepared = FALSE;
        controller->ActiveBackend = HCI_BACKEND_STUB;
        HciStubBindTransport(&controller->Transport, &controller->Hci);
        if (controller->StubArmClaimed) {
            controller->StubArmClaimed = FALSE;
            InterlockedExchange(&g_DeckBtStubArmClaim, 0);
        }
    } else {
        DeckBtRecordStep(DECKBT_STEP_ARMED, STATUS_SUCCESS);
    }
    DeckBtRecordStep(DECKBT_STEP_PLUGGED_IN, status);
    return status;
}

NTSTATUS
DeckBtEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDECKBT_CONTROLLER controller = DeckBtGetController(Device);
    NTSTATUS status = STATUS_SUCCESS;
    UDECXUSBDEVICE usbDevice;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    /*
     * A steady UART session is handed back to ROM first (bounded; waits only for the UART worker);
     * every other case cancels without waiting for the lower stack. Clearing BackendPrepared
     * under the gate means the worker can no longer plug a child in behind this release.
     */
    QcaUartRequestStop(&controller->Qca);
    (void)KeWaitForSingleObject(&controller->PlugGate, Executive, KernelMode, FALSE, NULL);
    controller->BackendPrepared = FALSE;
    usbDevice = controller->UsbDevice;
    controller->UsbDevice = NULL;
    KeSetEvent(&controller->PlugGate, IO_NO_INCREMENT, FALSE);
    if (usbDevice != NULL) {
        status = UdecxUsbDevicePlugOutAndDelete(usbDevice);
    }

    controller->Ep0 = NULL;
    controller->EpEventIn = NULL;
    controller->EpAclOut = NULL;
    controller->EpAclIn = NULL;
    controller->EpScoOut = NULL;
    controller->EpScoIn = NULL;
    DeckBtScoFlush(controller);
    if (controller->StubArmClaimed) {
        controller->StubArmClaimed = FALSE;
        InterlockedExchange(&g_DeckBtStubArmClaim, 0);
    }
    controller->ActiveBackend = HCI_BACKEND_STUB;
    HciStubInit(&controller->Hci);
    HciStubBindTransport(&controller->Transport, &controller->Hci);
    return status;
}
