/*
 * device.c - UDE controller and emulated USB device lifecycle for DeckBtIsoTest (Stage 2).
 */

#include "isotest.h"

NTSTATUS
IsoTestEvtQueryUsbCapability(
    _In_ WDFDEVICE UdecxWdfDevice,
    _In_ PGUID CapabilityType,
    _In_ ULONG OutputBufferLength,
    _Out_writes_to_opt_(OutputBufferLength, *ResultLength) PVOID OutputBuffer,
    _Out_ PULONG ResultLength)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(OutputBuffer);

    *ResultLength = 0;

    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_CHAINED_MDLS, sizeof(GUID)) == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_SELECTIVE_SUSPEND, sizeof(GUID)) == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_FUNCTION_SUSPEND, sizeof(GUID)) == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_DEVICE_CONNECTION_HIGH_SPEED_COMPATIBLE,
                         sizeof(GUID)) == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_HIGH_BANDWIDTH_ISOCH, sizeof(GUID)) == sizeof(GUID)) {
        return STATUS_NOT_SUPPORTED;
    }

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
IsoTestEvtDeviceD0Entry(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(UdecxUsbDevice);
    return STATUS_SUCCESS;
}

NTSTATUS
IsoTestEvtDeviceD0Exit(
    _In_ WDFDEVICE UdecxWdfDevice,
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ UDECX_USB_DEVICE_WAKE_SETTING WakeSetting)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(UdecxUsbDevice);
    UNREFERENCED_PARAMETER(WakeSetting);
    return STATUS_SUCCESS;
}

VOID
IsoTestEvtDeviceReset(
    _In_ WDFDEVICE UdecxWdfDevice,
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN AllDevicesReset)
{
    PISOTEST_CONTROLLER controller =
        ((PISOTEST_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, ISOTEST_ENDPOINT))->Controller;

    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(AllDevicesReset);

    IsoTestReleaseHeldRequest(controller, TRUE);
    WdfSpinLockAcquire(controller->Lock);
    controller->CurrentAltSetting = 0;
    WdfSpinLockRelease(controller->Lock);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

static NTSTATUS
IsoTestCreateEndpointQueue(
    _In_ PISOTEST_CONTROLLER Controller,
    _In_ UDECXUSBENDPOINT Endpoint,
    _In_ UCHAR Address,
    _In_ ULONG MaxPacketSize,
    _In_ BOOLEAN IsControl)
{
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFQUEUE queue;
    PISOTEST_ENDPOINT epContext;

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             IsControl ? WdfIoQueueDispatchSequential : WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = IsControl ? IsoTestEvtControlUrb : IsoTestEvtDataUrb;
    queueConfig.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ISOTEST_ENDPOINT);
    attributes.ParentObject = Endpoint;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfIoQueueCreate(Controller->WdfDevice, &queueConfig, &attributes, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    epContext = IsoTestGetEndpoint(queue);
    RtlZeroMemory(epContext, sizeof(ISOTEST_ENDPOINT));
    epContext->Controller = Controller;
    epContext->Address = Address;
    epContext->MaxPacketSize = MaxPacketSize;

    UdecxUsbEndpointSetWdfIoQueue(Endpoint, queue);
    return STATUS_SUCCESS;
}

NTSTATUS
IsoTestEvtDefaultEndpointAdd(
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ PUDECXUSBENDPOINT_INIT UdecxEndpointInit)
{
    NTSTATUS status;
    PISOTEST_CONTROLLER controller;
    UDECX_USB_ENDPOINT_CALLBACKS callbacks;
    UDECXUSBENDPOINT endpoint;
    WDF_OBJECT_ATTRIBUTES attributes;
    PISOTEST_ENDPOINT endpointContext;

    controller = ((PISOTEST_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, ISOTEST_ENDPOINT))->Controller;

    UDECX_USB_ENDPOINT_CALLBACKS_INIT(&callbacks, IsoTestEvtEndpointReset);
    UdecxUsbEndpointInitSetCallbacks(UdecxEndpointInit, &callbacks);
    UdecxUsbEndpointInitSetEndpointAddress(UdecxEndpointInit, USB_DEFAULT_ENDPOINT_ADDRESS);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ISOTEST_ENDPOINT);
    status = UdecxUsbEndpointCreate(&UdecxEndpointInit, &attributes, &endpoint);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    endpointContext = IsoTestGetEndpoint(endpoint);
    RtlZeroMemory(endpointContext, sizeof(ISOTEST_ENDPOINT));
    endpointContext->Controller = controller;
    endpointContext->Address = USB_DEFAULT_ENDPOINT_ADDRESS;
    endpointContext->MaxPacketSize = 64;
    controller->Ep0 = endpoint;
    return IsoTestCreateEndpointQueue(controller, endpoint, USB_DEFAULT_ENDPOINT_ADDRESS, 64, TRUE);
}

NTSTATUS
IsoTestEvtEndpointAdd(
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ PUDECX_USB_ENDPOINT_INIT_AND_METADATA EndpointToCreate)
{
    NTSTATUS status;
    PISOTEST_CONTROLLER controller;
    UDECX_USB_ENDPOINT_CALLBACKS callbacks;
    UDECXUSBENDPOINT endpoint;
    WDF_OBJECT_ATTRIBUTES attributes;
    PISOTEST_ENDPOINT endpointContext;
    UCHAR address;
    ULONG maxPacketSize;

    controller = ((PISOTEST_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, ISOTEST_ENDPOINT))->Controller;
    address = EndpointToCreate->EndpointDescriptor->bEndpointAddress;

    /*
     * Extract the endpoint's maximum packet size directly from the descriptor
     * configured by UDE. Dynamic endpoints are re-added on every interface
     * alternate setting change, so this value is authoritative and cannot go stale.
     * Mask off the USB 2.0 high-bandwidth transaction bits (bits 11..12)
     * so the stored size represents the exact byte count per interval.
     */
    maxPacketSize = (ULONG)(EndpointToCreate->EndpointDescriptor->wMaxPacketSize & 0x07FFu);

    UDECX_USB_ENDPOINT_CALLBACKS_INIT(&callbacks, IsoTestEvtEndpointReset);
    UdecxUsbEndpointInitSetCallbacks(EndpointToCreate->UdecxUsbEndpointInit, &callbacks);
    UdecxUsbEndpointInitSetEndpointAddress(EndpointToCreate->UdecxUsbEndpointInit, address);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ISOTEST_ENDPOINT);
    status = UdecxUsbEndpointCreate(&EndpointToCreate->UdecxUsbEndpointInit,
                                    &attributes, &endpoint);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    endpointContext = IsoTestGetEndpoint(endpoint);
    RtlZeroMemory(endpointContext, sizeof(ISOTEST_ENDPOINT));
    endpointContext->Controller = controller;
    endpointContext->Address = address;
    endpointContext->MaxPacketSize = maxPacketSize;

    switch (address) {
    case ISOTEST_EP_EVENT_IN:  controller->EpEventIn  = endpoint; break;
    case ISOTEST_EP_BULK_OUT:  controller->EpBulkOut  = endpoint; break;
    case ISOTEST_EP_BULK_IN:   controller->EpBulkIn   = endpoint; break;
    case ISOTEST_EP_ISOCH_OUT: controller->EpIsochOut = endpoint; break;
    case ISOTEST_EP_ISOCH_IN:  controller->EpIsochIn  = endpoint; break;
    default: break;
    }

    return IsoTestCreateEndpointQueue(controller, endpoint, address, maxPacketSize, FALSE);
}

VOID
IsoTestEvtEndpointsConfigure(
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ WDFREQUEST Request,
    _In_ PUDECX_ENDPOINTS_CONFIGURE_PARAMS Params)
{
    PISOTEST_CONTROLLER controller =
        ((PISOTEST_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, ISOTEST_ENDPOINT))->Controller;

    if (Params->ConfigureType == UdecxEndpointsConfigureTypeInterfaceSettingChange) {
        IsoTestLogConfigure(controller, (ULONG)Params->ConfigureType,
                            Params->InterfaceNumber, Params->NewInterfaceSetting);
        IsoTestRecordStep(ISOTEST_STEP_ALT_CHANGED, STATUS_SUCCESS);
    } else {
        IsoTestLogConfigure(controller, (ULONG)Params->ConfigureType, 0, 0);
    }

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

NTSTATUS
IsoTestCreateUsbDevice(_In_ PISOTEST_CONTROLLER Controller)
{
    NTSTATUS status;
    PUDECXUSBDEVICE_INIT deviceInit = NULL;
    UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS callbacks;
    UDECX_USB_DEVICE_PLUG_IN_OPTIONS plugInOptions;
    WDF_OBJECT_ATTRIBUTES attributes;
    UDECXUSBDEVICE usbDevice = NULL;
    PISOTEST_ENDPOINT deviceContext;

    deviceInit = UdecxUsbDeviceInitAllocate(Controller->WdfDevice);
    IsoTestRecordStep(ISOTEST_STEP_UDEV_INIT_ALLOC,
                      deviceInit == NULL ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS);
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* UDE constraint 1: Plug in as UdecxUsbHighSpeed */
    UdecxUsbDeviceInitSetSpeed(deviceInit, UdecxUsbHighSpeed);

    /* Dynamic endpoints: required for alternate settings */
    UdecxUsbDeviceInitSetEndpointsType(deviceInit, UdecxEndpointTypeDynamic);

    UDECX_USB_DEVICE_CALLBACKS_INIT(&callbacks);
    callbacks.EvtUsbDeviceLinkPowerEntry     = IsoTestEvtDeviceD0Entry;
    callbacks.EvtUsbDeviceLinkPowerExit      = IsoTestEvtDeviceD0Exit;
    callbacks.EvtUsbDeviceDefaultEndpointAdd = IsoTestEvtDefaultEndpointAdd;
    callbacks.EvtUsbDeviceEndpointAdd        = IsoTestEvtEndpointAdd;
    callbacks.EvtUsbDeviceEndpointsConfigure = IsoTestEvtEndpointsConfigure;
    callbacks.EvtUsbDeviceReset              = IsoTestEvtDeviceReset;
    UdecxUsbDeviceInitSetStateChangeCallbacks(deviceInit, &callbacks);

    status = UdecxUsbDeviceInitAddDescriptor(deviceInit,
                                             (PUCHAR)IsoTestDeviceDescriptor,
                                             (USHORT)IsoTestDeviceDescriptorSize);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = UdecxUsbDeviceInitAddDescriptor(deviceInit,
                                             (PUCHAR)IsoTestConfigDescriptor,
                                             (USHORT)IsoTestConfigDescriptorSize);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = UdecxUsbDeviceInitAddStringDescriptorRaw(deviceInit,
                                                      (PUCHAR)IsoTestStringLangIds,
                                                      (USHORT)IsoTestStringLangIds[0],
                                                      ISOTEST_ISTRING_LANGIDS,
                                                      0);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    for (UCHAR i = ISOTEST_ISTRING_MANUFACTURER; i < ISOTEST_ISTRING_COUNT; i++) {
        ULONG length = 0;
        const UCHAR *desc = IsoTestGetStringDescriptor(i, &length);
        if (desc == NULL) {
            status = STATUS_INTERNAL_ERROR;
            goto cleanup;
        }
        status = UdecxUsbDeviceInitAddStringDescriptorRaw(deviceInit,
                                                          (PUCHAR)desc,
                                                          (USHORT)length,
                                                          i,
                                                          ISOTEST_LANGID_EN_US);
        if (!NT_SUCCESS(status)) {
            goto cleanup;
        }
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ISOTEST_ENDPOINT);
    status = UdecxUsbDeviceCreate(&deviceInit, &attributes, &usbDevice);
    IsoTestRecordStep(ISOTEST_STEP_UDEV_CREATE, status);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }
    deviceInit = NULL;

    deviceContext = (PISOTEST_ENDPOINT)WdfObjectGetTypedContext(usbDevice, ISOTEST_ENDPOINT);
    RtlZeroMemory(deviceContext, sizeof(ISOTEST_ENDPOINT));
    deviceContext->Controller = Controller;
    deviceContext->Address = 0;
    deviceContext->MaxPacketSize = 0;

    UDECX_USB_DEVICE_PLUG_IN_OPTIONS_INIT(&plugInOptions);
    plugInOptions.Usb20PortNumber = 1;
    status = UdecxUsbDevicePlugIn(usbDevice, &plugInOptions);
    if (NT_SUCCESS(status)) {
        Controller->UsbDevice = usbDevice;
        usbDevice = NULL;
    }

cleanup:
    if (deviceInit != NULL) {
        UdecxUsbDeviceInitFree(deviceInit);
    }
    if (usbDevice != NULL) {
        WdfObjectDelete(usbDevice);
    }
    return status;
}
