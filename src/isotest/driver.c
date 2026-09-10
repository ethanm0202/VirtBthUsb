/*
 * driver.c - DriverEntry, PnP lifecycle, and registry breadcrumbs for DeckBtIsoTest (Stage 2).
 */

#include <initguid.h>
#include "isotest.h"
#include <usbiodef.h>

static WDFDRIVER g_IsoTestDriver = NULL;

VOID
IsoTestRecordStep(_In_ ULONG Step, _In_ NTSTATUS Status)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameStep;
    UNICODE_STRING nameStatus;

    if (g_IsoTestDriver == NULL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_IsoTestDriver, KEY_SET_VALUE,
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
IsoTestLogConfigure(
    _In_ PISOTEST_CONTROLLER Controller,
    _In_ ULONG Type,
    _In_ UCHAR Iface,
    _In_ UCHAR Setting)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameCount;
    UNICODE_STRING nameType;
    UNICODE_STRING nameIface;
    UNICODE_STRING nameSetting;
    UNICODE_STRING nameCurrentAlt;

    WdfSpinLockAcquire(Controller->Lock);
    Controller->EndpointsConfigureCount++;
    Controller->LastConfigureType = Type;
    Controller->LastConfigureInterface = Iface;
    Controller->LastConfigureSetting = Setting;
    if (Iface == ISOTEST_IFACE_ISOCH) {
        Controller->CurrentAltSetting = Setting;
    }
    WdfSpinLockRelease(Controller->Lock);

    if (g_IsoTestDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_IsoTestDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&nameCount, L"EndpointsConfigureCount");
    RtlInitUnicodeString(&nameType, L"LastConfigureType");
    RtlInitUnicodeString(&nameIface, L"LastConfigureInterface");
    RtlInitUnicodeString(&nameSetting, L"LastConfigureSetting");
    RtlInitUnicodeString(&nameCurrentAlt, L"CurrentAltSetting");

    (void)WdfRegistryAssignULong(key, &nameCount, Controller->EndpointsConfigureCount);
    (void)WdfRegistryAssignULong(key, &nameType, Type);
    (void)WdfRegistryAssignULong(key, &nameIface, (ULONG)Iface);
    (void)WdfRegistryAssignULong(key, &nameSetting, (ULONG)Setting);
    (void)WdfRegistryAssignULong(key, &nameCurrentAlt, (ULONG)Controller->CurrentAltSetting);

    WdfRegistryClose(key);
}

VOID
IsoTestLogIsoch(
    _In_ PISOTEST_CONTROLLER Controller,
    _In_ UCHAR Endpoint,
    _In_ UCHAR Direction,
    _In_ ULONG NumPackets,
    _In_ ULONG BufferLen,
    _In_ ULONG PacketLen0,
    _In_ ULONG UsbdStatus,
    _In_ ULONG BytesDone,
    _In_ ULONG Flags,
    _In_ ULONG EndpointMaxPacketSize)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameCount;
    UNICODE_STRING nameLastEp;
    UNICODE_STRING nameLastPackets;
    UNICODE_STRING nameLastLen;
    UNICODE_STRING nameLastPkt0;
    UNICODE_STRING nameLastUsbd;
    UNICODE_STRING nameLastMaxPkt;
    UNICODE_STRING nameHistory;
    UNICODE_STRING nameUrbCountIn;
    UNICODE_STRING nameUrbCountOut;
    UNICODE_STRING nameNonSuccess;
    UNICODE_STRING nameShortComp;
    UNICODE_STRING nameMismatch;
    UNICODE_STRING nameReqIn;
    UNICODE_STRING nameCompIn;
    UNICODE_STRING nameReqOut;
    UNICODE_STRING nameCompOut;
    ULONG slot;
    ULONG totalCount;
    ULONG urbCountIn;
    ULONG urbCountOut;
    ULONG nonSuccess;
    ULONG shortComp;
    ULONG mismatch;
    ULONG reqIn;
    ULONG compIn;
    ULONG reqOut;
    ULONG compOut;

    WdfSpinLockAcquire(Controller->Lock);
    slot = Controller->IsochUrbCount % ISOTEST_URB_LOG_SLOTS;
    RtlZeroMemory(&Controller->UrbLog[slot], sizeof(ISOTEST_ISOCH_RECORD));
    Controller->UrbLog[slot].Sequence = Controller->IsochUrbCount;
    Controller->UrbLog[slot].Endpoint = Endpoint;
    Controller->UrbLog[slot].Direction = Direction;
    Controller->UrbLog[slot].AltSetting = Controller->CurrentAltSetting;
    Controller->UrbLog[slot].Reserved = 0;
    Controller->UrbLog[slot].NumberOfPackets = NumPackets;
    Controller->UrbLog[slot].TransferBufferLength = BufferLen;
    Controller->UrbLog[slot].PacketLength0 = PacketLen0;
    Controller->UrbLog[slot].UsbdStatus = UsbdStatus;
    Controller->UrbLog[slot].BytesCompleted = BytesDone;
    Controller->UrbLog[slot].Flags = Flags;
    Controller->UrbLog[slot].EndpointMaxPacketSize = EndpointMaxPacketSize;

    Controller->IsochUrbCount++;
    Controller->LastIsochEndpoint = Endpoint;
    Controller->LastIsochNumPackets = NumPackets;
    Controller->LastIsochBufferLength = BufferLen;
    Controller->LastIsochPacketLength0 = PacketLen0;
    Controller->LastIsochUsbdStatus = UsbdStatus;
    Controller->LastIsochMaxPacketSize = EndpointMaxPacketSize;

    /*
     * Whole-run aggregate updates:
     * The retained UrbLog window is only 16 entries (ISOTEST_URB_LOG_SLOTS);
     * tail records can never substantiate a claim about a whole run.
     * Update running counts for every URB under Controller->Lock.
     */
    if (Direction == 1) {
        Controller->IsochUrbCountIn++;
        Controller->IsochBytesRequestedIn += BufferLen;
        Controller->IsochBytesCompletedIn += BytesDone;
    } else {
        Controller->IsochUrbCountOut++;
        Controller->IsochBytesRequestedOut += BufferLen;
        Controller->IsochBytesCompletedOut += BytesDone;
    }

    if (UsbdStatus != (ULONG)USBD_STATUS_SUCCESS) {
        Controller->IsochNonSuccessCount++;
    }

    if (BytesDone != BufferLen) {
        Controller->IsochShortCompletionCount++;
    }

    if (EndpointMaxPacketSize > 0 &&
        BufferLen > 0 &&
        (BufferLen % EndpointMaxPacketSize) == 0 &&
        PacketLen0 != EndpointMaxPacketSize) {
        Controller->IsochPacketSizeMismatchCount++;
    }

    totalCount  = Controller->IsochUrbCount;
    urbCountIn  = Controller->IsochUrbCountIn;
    urbCountOut = Controller->IsochUrbCountOut;
    nonSuccess  = Controller->IsochNonSuccessCount;
    shortComp   = Controller->IsochShortCompletionCount;
    mismatch    = Controller->IsochPacketSizeMismatchCount;
    reqIn       = Controller->IsochBytesRequestedIn;
    compIn      = Controller->IsochBytesCompletedIn;
    reqOut      = Controller->IsochBytesRequestedOut;
    compOut     = Controller->IsochBytesCompletedOut;
    WdfSpinLockRelease(Controller->Lock);

    if (g_IsoTestDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_IsoTestDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&nameCount, L"IsochUrbCount");
    RtlInitUnicodeString(&nameLastEp, L"LastIsochEndpoint");
    RtlInitUnicodeString(&nameLastPackets, L"LastIsochNumPackets");
    RtlInitUnicodeString(&nameLastLen, L"LastIsochBufferLength");
    RtlInitUnicodeString(&nameLastPkt0, L"LastIsochPacketLength0");
    RtlInitUnicodeString(&nameLastUsbd, L"LastIsochUsbdStatus");
    RtlInitUnicodeString(&nameLastMaxPkt, L"LastIsochMaxPacketSize");
    RtlInitUnicodeString(&nameHistory, L"IsochUrbHistory");
    RtlInitUnicodeString(&nameUrbCountIn, L"IsochUrbCountIn");
    RtlInitUnicodeString(&nameUrbCountOut, L"IsochUrbCountOut");
    RtlInitUnicodeString(&nameNonSuccess, L"IsochNonSuccessCount");
    RtlInitUnicodeString(&nameShortComp, L"IsochShortCompletionCount");
    RtlInitUnicodeString(&nameMismatch, L"IsochPacketSizeMismatchCount");
    RtlInitUnicodeString(&nameReqIn, L"IsochBytesRequestedIn");
    RtlInitUnicodeString(&nameCompIn, L"IsochBytesCompletedIn");
    RtlInitUnicodeString(&nameReqOut, L"IsochBytesRequestedOut");
    RtlInitUnicodeString(&nameCompOut, L"IsochBytesCompletedOut");

    (void)WdfRegistryAssignULong(key, &nameCount, totalCount);
    (void)WdfRegistryAssignULong(key, &nameLastEp, (ULONG)Endpoint);
    (void)WdfRegistryAssignULong(key, &nameLastPackets, NumPackets);
    (void)WdfRegistryAssignULong(key, &nameLastLen, BufferLen);
    (void)WdfRegistryAssignULong(key, &nameLastPkt0, PacketLen0);
    (void)WdfRegistryAssignULong(key, &nameLastUsbd, UsbdStatus);
    (void)WdfRegistryAssignULong(key, &nameLastMaxPkt, EndpointMaxPacketSize);
    (void)WdfRegistryAssignValue(key, &nameHistory, REG_BINARY,
                                 sizeof(Controller->UrbLog), &Controller->UrbLog[0]);
    (void)WdfRegistryAssignULong(key, &nameUrbCountIn, urbCountIn);
    (void)WdfRegistryAssignULong(key, &nameUrbCountOut, urbCountOut);
    (void)WdfRegistryAssignULong(key, &nameNonSuccess, nonSuccess);
    (void)WdfRegistryAssignULong(key, &nameShortComp, shortComp);
    (void)WdfRegistryAssignULong(key, &nameMismatch, mismatch);
    (void)WdfRegistryAssignULong(key, &nameReqIn, reqIn);
    (void)WdfRegistryAssignULong(key, &nameCompIn, compIn);
    (void)WdfRegistryAssignULong(key, &nameReqOut, reqOut);
    (void)WdfRegistryAssignULong(key, &nameCompOut, compOut);

    WdfRegistryClose(key);
}

VOID
IsoTestResetAggregates(_In_ PISOTEST_CONTROLLER Controller)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameCount;
    UNICODE_STRING nameHistory;
    UNICODE_STRING nameUrbCountIn;
    UNICODE_STRING nameUrbCountOut;
    UNICODE_STRING nameNonSuccess;
    UNICODE_STRING nameShortComp;
    UNICODE_STRING nameMismatch;
    UNICODE_STRING nameReqIn;
    UNICODE_STRING nameCompIn;
    UNICODE_STRING nameReqOut;
    UNICODE_STRING nameCompOut;

    /*
     * Whole-run aggregate counters exist because the retained UrbLog window
     * is only 16 entries (ISOTEST_URB_LOG_SLOTS); tail records can never
     * substantiate a claim about a whole run. Resetting all counters when
     * the controller context is initialized ensures metrics describe exactly
     * one run rather than accumulating across devnode lifetimes.
     */
    WdfSpinLockAcquire(Controller->Lock);
    Controller->IsochUrbCount = 0;
    Controller->IsochUrbCountIn = 0;
    Controller->IsochUrbCountOut = 0;
    Controller->IsochNonSuccessCount = 0;
    Controller->IsochShortCompletionCount = 0;
    Controller->IsochPacketSizeMismatchCount = 0;
    Controller->IsochBytesRequestedIn = 0;
    Controller->IsochBytesCompletedIn = 0;
    Controller->IsochBytesRequestedOut = 0;
    Controller->IsochBytesCompletedOut = 0;
    WdfSpinLockRelease(Controller->Lock);

    if (g_IsoTestDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_IsoTestDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&nameCount, L"IsochUrbCount");
    RtlInitUnicodeString(&nameHistory, L"IsochUrbHistory");
    RtlInitUnicodeString(&nameUrbCountIn, L"IsochUrbCountIn");
    RtlInitUnicodeString(&nameUrbCountOut, L"IsochUrbCountOut");
    RtlInitUnicodeString(&nameNonSuccess, L"IsochNonSuccessCount");
    RtlInitUnicodeString(&nameShortComp, L"IsochShortCompletionCount");
    RtlInitUnicodeString(&nameMismatch, L"IsochPacketSizeMismatchCount");
    RtlInitUnicodeString(&nameReqIn, L"IsochBytesRequestedIn");
    RtlInitUnicodeString(&nameCompIn, L"IsochBytesCompletedIn");
    RtlInitUnicodeString(&nameReqOut, L"IsochBytesRequestedOut");
    RtlInitUnicodeString(&nameCompOut, L"IsochBytesCompletedOut");

    (void)WdfRegistryAssignULong(key, &nameCount, 0);
    (void)WdfRegistryAssignValue(key, &nameHistory, REG_BINARY,
                                 sizeof(Controller->UrbLog), &Controller->UrbLog[0]);
    (void)WdfRegistryAssignULong(key, &nameUrbCountIn, 0);
    (void)WdfRegistryAssignULong(key, &nameUrbCountOut, 0);
    (void)WdfRegistryAssignULong(key, &nameNonSuccess, 0);
    (void)WdfRegistryAssignULong(key, &nameShortComp, 0);
    (void)WdfRegistryAssignULong(key, &nameMismatch, 0);
    (void)WdfRegistryAssignULong(key, &nameReqIn, 0);
    (void)WdfRegistryAssignULong(key, &nameCompIn, 0);
    (void)WdfRegistryAssignULong(key, &nameReqOut, 0);
    (void)WdfRegistryAssignULong(key, &nameCompOut, 0);

    WdfRegistryClose(key);
}
VOID
IsoTestLogHoldTelemetry(_In_ PISOTEST_CONTROLLER Controller)
{
    WDFKEY key = NULL;
    UNICODE_STRING names[5];
    ULONG values[5];
    WdfSpinLockAcquire(Controller->Lock);
    values[0] = Controller->HoldCommandCount;
    values[1] = Controller->HeldTransferCount;
    values[2] = Controller->CancelCallbackCount;
    values[3] = Controller->ReleaseCommandCount;
    values[4] = Controller->HoldState == IsoTestHoldIdle ? 0u : 1u;
    WdfSpinLockRelease(Controller->Lock);

    if (g_IsoTestDriver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_IsoTestDriver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    RtlInitUnicodeString(&names[0], L"HoldCommandCount");
    RtlInitUnicodeString(&names[1], L"HeldTransferCount");
    RtlInitUnicodeString(&names[2], L"CancelCallbackCount");
    RtlInitUnicodeString(&names[3], L"ReleaseCommandCount");
    RtlInitUnicodeString(&names[4], L"HeldRequestActive");
    for (ULONG i = 0; i < RTL_NUMBER_OF(values); i++) {
        (void)WdfRegistryAssignULong(key, &names[i], values[i]);
    }
    WdfRegistryClose(key);
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, IsoTestEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                             &g_IsoTestDriver);
    if (NT_SUCCESS(status)) {
        IsoTestRecordStep(ISOTEST_STEP_ENTER, status);
    }
    return status;
}

NTSTATUS
IsoTestEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPower;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    UDECX_WDF_DEVICE_CONFIG udecxConfig;
    WDFDEVICE device;
    PISOTEST_CONTROLLER controller;

    UNREFERENCED_PARAMETER(Driver);

    status = UdecxInitializeWdfDeviceInit(DeviceInit);
    IsoTestRecordStep(ISOTEST_STEP_UDECX_INIT, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPower);
    pnpPower.EvtDevicePrepareHardware = IsoTestEvtDevicePrepareHardware;
    pnpPower.EvtDeviceReleaseHardware = IsoTestEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ISOTEST_CONTROLLER);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    IsoTestRecordStep(ISOTEST_STEP_DEVICE_CREATE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_DEVINTERFACE_USB_HOST_CONTROLLER, NULL);
    IsoTestRecordStep(ISOTEST_STEP_DEVICE_INTERFACE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    controller = IsoTestGetController(device);
    RtlZeroMemory(controller, sizeof(*controller));
    controller->WdfDevice = device;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &controller->Lock);
    IsoTestRecordStep(ISOTEST_STEP_SPINLOCK, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    IsoTestResetAggregates(controller);

    /* Framework-owned manual queue holds one transfer and reports cancellation reliably. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoCanceledOnQueue = IsoTestEvtHeldRequestCanceled;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ISOTEST_ENDPOINT);
    attributes.ParentObject = device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queueConfig, &attributes,
                              &controller->PendingIsochQueue);
    IsoTestRecordStep(ISOTEST_STEP_QUEUE_CREATE, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    IsoTestGetEndpoint(controller->PendingIsochQueue)->Controller = controller;

    UDECX_WDF_DEVICE_CONFIG_INIT(&udecxConfig, IsoTestEvtQueryUsbCapability);
    status = UdecxWdfDeviceAddUsbDeviceEmulation(device, &udecxConfig);
    IsoTestRecordStep(ISOTEST_STEP_ADD_EMULATION, status);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    IsoTestRecordStep(ISOTEST_STEP_ADD_DONE, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS
IsoTestEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    IsoTestRecordStep(ISOTEST_STEP_PREPARE_HW, STATUS_SUCCESS);
    status = IsoTestCreateUsbDevice(IsoTestGetController(Device));
    IsoTestRecordStep(ISOTEST_STEP_UDEV_PLUGIN, status);
    return status;
}

NTSTATUS
IsoTestEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PISOTEST_CONTROLLER controller = IsoTestGetController(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    IsoTestReleaseHeldRequest(controller, TRUE);
    WdfIoQueuePurgeSynchronously(controller->PendingIsochQueue);
    if (controller->UsbDevice != NULL) {
        UDECXUSBDEVICE usbDevice = controller->UsbDevice;
        controller->UsbDevice = NULL;
        return UdecxUsbDevicePlugOutAndDelete(usbDevice);
    }
    return STATUS_SUCCESS;
}
