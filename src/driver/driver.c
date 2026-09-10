/*
 * driver.c - DriverEntry and PnP lifecycle for the UdeCx host controller.
 *
 * Diagnostic step codes and execution status are recorded to the driver's
 * Parameters registry key to enable user-mode inspection without a kernel debugger.
 */

#include <initguid.h>       /* must precede usbiodef.h so the GUID is instantiated here */
#include "deckbtusb.h"
#include <usbiodef.h>

DRIVER_INITIALIZE DriverEntry;

/* Captured in DriverEntry so DeckBtRecordStep can reach the driver's Parameters key. */
static WDFDRIVER g_DeckBtDriver = NULL;

/* Step codes, written to LastAddDeviceStep. Keep in sync with tools\m1-diag.ps1. */
#define DECKBT_STEP_ENTER               1
#define DECKBT_STEP_UDECX_INIT          2
#define DECKBT_STEP_DEVICE_CREATE       3
#define DECKBT_STEP_DEVICE_INTERFACE    4
#define DECKBT_STEP_SPINLOCK            5
#define DECKBT_STEP_EVENT_QUEUE         6
#define DECKBT_STEP_ACL_QUEUE           7
#define DECKBT_STEP_ADD_EMULATION       8
#define DECKBT_STEP_ADD_DONE            9
#define DECKBT_STEP_PREPARE_HW         20
#define DECKBT_STEP_USB_DEVICE_CREATED 21
#define DECKBT_STEP_PLUGGED_IN         22

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
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DECKBT_CONTROLLER);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    DeckBtRecordStep(DECKBT_STEP_DEVICE_CREATE, status);
    if (!NT_SUCCESS(status)) {
        return status;
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

    /*
     * Leave the port counts at the UDECX_WDF_DEVICE_CONFIG_INIT defaults (one 2.0 and one 3.0
     * port). Forcing NumberOfUsb30Ports to 0 was the other deviation from the sample sequence
     * and is a candidate for the STATUS_INVALID_PARAMETER seen in M1's first run: UCX may
     * require a non-zero port count of each kind. We only ever plug into the 2.0 port.
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

NTSTATUS
DeckBtEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    DeckBtRecordStep(DECKBT_STEP_PREPARE_HW, STATUS_SUCCESS);

    /* Root-enumerated: no hardware resources to parse. M3 adds the ACPI\QCOM2066 UART here. */
    status = DeckBtCreateUsbDevice(DeckBtGetController(Device));
    DeckBtRecordStep(DECKBT_STEP_PLUGGED_IN, status);
    return status;
}

NTSTATUS
DeckBtEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDECKBT_CONTROLLER controller = DeckBtGetController(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (controller->UsbDevice != NULL) {
        NTSTATUS status = UdecxUsbDevicePlugOutAndDelete(controller->UsbDevice);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        controller->UsbDevice = NULL;
    }
    return STATUS_SUCCESS;
}
