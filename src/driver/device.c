/*
 * device.c - UDE controller and emulated-device lifecycle for DeckBtUsb.
 */

#include "deckbtusb.h"

NTSTATUS
DeckBtEvtQueryUsbCapability(
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

    /*
     * Answer USB capabilities supported by an emulated high-speed controller.
     * Returning STATUS_NOT_SUPPORTED for all capabilities can cause client drivers
     * to treat the controller as invalid.
     */
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_CHAINED_MDLS, sizeof(GUID))
            == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_SELECTIVE_SUSPEND, sizeof(GUID))
            == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_FUNCTION_SUSPEND, sizeof(GUID))
            == sizeof(GUID)) {
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType,
                         &GUID_USB_CAPABILITY_DEVICE_CONNECTION_HIGH_SPEED_COMPATIBLE,
                         sizeof(GUID)) == sizeof(GUID)) {
        /* The device plugs in as UdecxUsbHighSpeed. */
        return STATUS_SUCCESS;
    }
    if (RtlCompareMemory(CapabilityType, &GUID_USB_CAPABILITY_HIGH_BANDWIDTH_ISOCH, sizeof(GUID))
            == sizeof(GUID)) {
        /* SCO needs one 1 ms packet per frame, never the multi-packet high-bandwidth form. */
        return STATUS_NOT_SUPPORTED;
    }

    /*
     * Everything else - SuperSpeed compatibility, static streams, SSP isoch flags, time sync -
     * is genuinely absent. NOT_IMPLEMENTED is what the reference client returns for these, and
     * it is distinguishable from an outright rejection.
     */
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS
DeckBtEvtDeviceD0Entry(_In_ WDFDEVICE UdecxWdfDevice, _In_ UDECXUSBDEVICE UdecxUsbDevice)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(UdecxUsbDevice);
    return STATUS_SUCCESS;
}

NTSTATUS
DeckBtEvtDeviceD0Exit(
    _In_ WDFDEVICE UdecxWdfDevice,
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ UDECX_USB_DEVICE_WAKE_SETTING WakeSetting)
{
    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(UdecxUsbDevice);
    UNREFERENCED_PARAMETER(WakeSetting);
    return STATUS_SUCCESS;
}

/*
 * A USB client driver that encounters an error during initialization recovers by resetting
 * the device via GUID_DEVICE_RESET_INTERFACE_STANDARD or GUID_REENUMERATE_SELF_INTERFACE_STANDARD.
 * Providing this reset callback allows the stack to recover from transient transport failures.
 */
VOID
DeckBtEvtDeviceReset(
    _In_ WDFDEVICE UdecxWdfDevice,
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ WDFREQUEST Request,
    _In_ BOOLEAN AllDevicesReset)
{
    PDECKBT_CONTROLLER controller =
        ((PDECKBT_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, DECKBT_ENDPOINT))->Controller;

    UNREFERENCED_PARAMETER(UdecxWdfDevice);
    UNREFERENCED_PARAMETER(AllDevicesReset);

    /* A reset means "forget everything": discard queued events and the SCO stream. */
    WdfSpinLockAcquire(controller->Lock);
    HciTransportReset(&controller->Transport);
    WdfSpinLockRelease(controller->Lock);
    DeckBtScoFlush(controller);

    DeckBtRecordStep(DECKBT_STEP_USB_RESET, STATUS_SUCCESS);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

/* Creates the endpoint's I/O queue and binds it. Shared by the default and data endpoints. */
static NTSTATUS
DeckBtCreateEndpointQueue(
    _In_ PDECKBT_CONTROLLER Controller,
    _In_ UDECXUSBENDPOINT Endpoint,
    _In_ UCHAR Address,
    _In_ USHORT MaxPacketSize,
    _In_ BOOLEAN IsControl)
{
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFQUEUE queue;
    PDECKBT_ENDPOINT epContext;

    /*
     * Control transfers are serialized: an HCI command and the event it produces must not race.
     * Data endpoints dispatch in parallel so that a pended IN request never blocks an OUT.
     */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig,
                             IsControl ? WdfIoQueueDispatchSequential : WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = IsControl ? DeckBtEvtControlUrb : DeckBtEvtDataUrb;
    queueConfig.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DECKBT_ENDPOINT);
    attributes.ParentObject = Endpoint;
    /*
     * PASSIVE_LEVEL so the control-request tracer can record trace entries to the registry
     * when diagnosing enumeration or transport failures without an attached kernel debugger.
     */
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfIoQueueCreate(Controller->WdfDevice, &queueConfig, &attributes, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    epContext = DeckBtGetEndpoint(queue);
    epContext->Controller = Controller;
    epContext->Address = Address;
    epContext->MaxPacketSize = MaxPacketSize;

    UdecxUsbEndpointSetWdfIoQueue(Endpoint, queue);
    return STATUS_SUCCESS;
}

NTSTATUS
DeckBtEvtDefaultEndpointAdd(
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ PUDECXUSBENDPOINT_INIT UdecxEndpointInit)
{
    NTSTATUS status;
    PDECKBT_CONTROLLER controller;
    UDECX_USB_ENDPOINT_CALLBACKS callbacks;
    UDECXUSBENDPOINT endpoint;

    /*
     * The endpoint-add callbacks receive only the UDECXUSBDEVICE, so the controller back-pointer
     * is carried in a DECKBT_ENDPOINT context attached to the UDE device in DeckBtCreateUsbDevice.
     */
    controller = ((PDECKBT_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, DECKBT_ENDPOINT))->Controller;

    UDECX_USB_ENDPOINT_CALLBACKS_INIT(&callbacks, DeckBtEvtEndpointReset);
    UdecxUsbEndpointInitSetCallbacks(UdecxEndpointInit, &callbacks);
    UdecxUsbEndpointInitSetEndpointAddress(UdecxEndpointInit, USB_DEFAULT_ENDPOINT_ADDRESS);

    status = UdecxUsbEndpointCreate(&UdecxEndpointInit, WDF_NO_OBJECT_ATTRIBUTES, &endpoint);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    controller->Ep0 = endpoint;
    status = DeckBtCreateEndpointQueue(controller, endpoint, USB_DEFAULT_ENDPOINT_ADDRESS, 64, TRUE);
    DeckBtRecordStep(DECKBT_STEP_EP_DEFAULT_ADD, status);
    return status;
}

NTSTATUS
DeckBtEvtEndpointAdd(
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ PUDECX_USB_ENDPOINT_INIT_AND_METADATA EndpointToCreate)
{
    NTSTATUS status;
    PDECKBT_CONTROLLER controller;
    UDECX_USB_ENDPOINT_CALLBACKS callbacks;
    UDECXUSBENDPOINT endpoint;
    UCHAR address;

    controller = ((PDECKBT_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, DECKBT_ENDPOINT))->Controller;
    address = EndpointToCreate->EndpointDescriptor->bEndpointAddress;

    UDECX_USB_ENDPOINT_CALLBACKS_INIT(&callbacks, DeckBtEvtEndpointReset);
    UdecxUsbEndpointInitSetCallbacks(EndpointToCreate->UdecxUsbEndpointInit, &callbacks);
    UdecxUsbEndpointInitSetEndpointAddress(EndpointToCreate->UdecxUsbEndpointInit, address);

    status = UdecxUsbEndpointCreate(&EndpointToCreate->UdecxUsbEndpointInit,
                                    WDF_NO_OBJECT_ATTRIBUTES, &endpoint);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    switch (address) {
    case DECKBT_EP_EVENT_IN: controller->EpEventIn = endpoint; break;
    case DECKBT_EP_ACL_OUT:  controller->EpAclOut  = endpoint; break;
    case DECKBT_EP_ACL_IN:   controller->EpAclIn   = endpoint; break;
    case DECKBT_EP_SCO_OUT:  controller->EpScoOut  = endpoint; break;
    case DECKBT_EP_SCO_IN:   controller->EpScoIn   = endpoint; break;
    default: break;
    }

    /* The SCO endpoints are recreated per alternate setting; each carries that setting's size. */
    status = DeckBtCreateEndpointQueue(controller, endpoint, address,
                                       (USHORT)(EndpointToCreate->EndpointDescriptor->wMaxPacketSize & 0x07FFu),
                                       FALSE);
    DeckBtRecordStep(DECKBT_STEP_EP_ADD, status);
    return status;
}

VOID
DeckBtEvtEndpointsConfigure(
    _In_ UDECXUSBDEVICE UdecxUsbDevice,
    _In_ WDFREQUEST Request,
    _In_ PUDECX_ENDPOINTS_CONFIGURE_PARAMS Params)
{
    PDECKBT_CONTROLLER controller =
        ((PDECKBT_ENDPOINT)WdfObjectGetTypedContext(UdecxUsbDevice, DECKBT_ENDPOINT))->Controller;

    /*
     * UdeCx is documented-by-practice to pass a wrong or late InterfaceNumber/NewInterfaceSetting
     * here (see usbip-win2 drivers/ude/device.cpp), so the SCO path never sizes packets from it:
     * each URB is sized from the endpoint it arrived on. Any SCO setting change ends the current
     * voice stream, so parked transfers are cancelled and framing restarts.
     */
    if (Params->ConfigureType == UdecxEndpointsConfigureTypeInterfaceSettingChange &&
        Params->InterfaceNumber == DECKBT_IFACE_SCO) {
        DeckBtScoFlush(controller);
        WdfSpinLockAcquire(controller->Lock);
        controller->ScoStats.AltSetting = Params->NewInterfaceSetting;
        controller->ScoStats.AltChanges++;
        WdfSpinLockRelease(controller->Lock);
        DeckBtScoPublish(controller, TRUE);
    }

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

NTSTATUS
DeckBtCreateUsbDevice(_In_ PDECKBT_CONTROLLER Controller)
{
    NTSTATUS status;
    PUDECXUSBDEVICE_INIT deviceInit = NULL;
    UDECX_USB_DEVICE_STATE_CHANGE_CALLBACKS callbacks;
    UDECX_USB_DEVICE_PLUG_IN_OPTIONS plugInOptions;
    UDECXUSBDEVICE usbDevice = NULL;
    WDF_OBJECT_ATTRIBUTES attributes;
    PDECKBT_ENDPOINT deviceContext;

    deviceInit = UdecxUsbDeviceInitAllocate(Controller->WdfDevice);
    DeckBtRecordStep(DECKBT_STEP_UDEV_INIT_ALLOC,
                     deviceInit == NULL ? STATUS_INSUFFICIENT_RESOURCES : STATUS_SUCCESS);
    if (deviceInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /*
     * HighSpeed even though a real Bluetooth dongle is full speed: UDE/USBHUB3 validates the
     * configuration descriptor against high-speed rules regardless of the declared speed, so a
     * full-speed-shaped descriptor is rejected with a configuration-descriptor validation
     * failure. The descriptors in usb_descriptors.c are shaped accordingly (bulk 512).
     */
    UdecxUsbDeviceInitSetSpeed(deviceInit, UdecxUsbHighSpeed);

    /* Dynamic, not Simple: Simple endpoints forbid alternate settings, and SCO needs alt 0..6. */
    UdecxUsbDeviceInitSetEndpointsType(deviceInit, UdecxEndpointTypeDynamic);

    UDECX_USB_DEVICE_CALLBACKS_INIT(&callbacks);
    callbacks.EvtUsbDeviceLinkPowerEntry        = DeckBtEvtDeviceD0Entry;
    callbacks.EvtUsbDeviceLinkPowerExit         = DeckBtEvtDeviceD0Exit;
    callbacks.EvtUsbDeviceDefaultEndpointAdd    = DeckBtEvtDefaultEndpointAdd;
    callbacks.EvtUsbDeviceEndpointAdd           = DeckBtEvtEndpointAdd;
    callbacks.EvtUsbDeviceEndpointsConfigure    = DeckBtEvtEndpointsConfigure;
    callbacks.EvtUsbDeviceReset                 = DeckBtEvtDeviceReset;
    UdecxUsbDeviceInitSetStateChangeCallbacks(deviceInit, &callbacks);

    status = UdecxUsbDeviceInitAddDescriptor(deviceInit,
                                             (PUCHAR)DeckBtDeviceDescriptor,
                                             (USHORT)DeckBtDeviceDescriptorSize);
    DeckBtRecordStep(DECKBT_STEP_UDEV_DESC_DEVICE, status);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = UdecxUsbDeviceInitAddDescriptor(deviceInit,
                                             (PUCHAR)DeckBtConfigDescriptor,
                                             (USHORT)DeckBtConfigDescriptorSize);
    DeckBtRecordStep(DECKBT_STEP_UDEV_DESC_CONFIG, status);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    status = UdecxUsbDeviceInitAddStringDescriptorRaw(deviceInit,
                                                      (PUCHAR)DeckBtStringLangIds,
                                                      (USHORT)DeckBtStringLangIds[0],
                                                      DECKBT_ISTRING_LANGIDS,
                                                      0);
    DeckBtRecordStep(DECKBT_STEP_UDEV_DESC_LANGIDS, status);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    for (UCHAR i = DECKBT_ISTRING_MANUFACTURER; i < DECKBT_ISTRING_COUNT; i++) {
        ULONG length = 0;
        const UCHAR *descriptor = DeckBtGetStringDescriptor(i, &length);

        if (descriptor == NULL) {
            status = STATUS_INTERNAL_ERROR;
            goto cleanup;
        }
        status = UdecxUsbDeviceInitAddStringDescriptorRaw(deviceInit,
                                                          (PUCHAR)descriptor,
                                                          (USHORT)length,
                                                          i,
                                                          DECKBT_LANGID_EN_US);
        if (!NT_SUCCESS(status)) {
            DeckBtRecordStep(DECKBT_STEP_UDEV_DESC_STRINGS, status);
            goto cleanup;
        }
    }
    DeckBtRecordStep(DECKBT_STEP_UDEV_DESC_STRINGS, STATUS_SUCCESS);

    /*
     * The UDE device carries a DECKBT_ENDPOINT context holding the controller back-pointer; the
     * endpoint-add callbacks receive only the UDECXUSBDEVICE, so this provides the controller context.
     */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DECKBT_ENDPOINT);
    status = UdecxUsbDeviceCreate(&deviceInit, &attributes, &usbDevice);
    DeckBtRecordStep(DECKBT_STEP_UDEV_CREATE, status);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }
    deviceInit = NULL;   /* ownership transferred on success */

    deviceContext = (PDECKBT_ENDPOINT)WdfObjectGetTypedContext(usbDevice, DECKBT_ENDPOINT);
    deviceContext->Controller = Controller;
    deviceContext->Address = 0;

    UDECX_USB_DEVICE_PLUG_IN_OPTIONS_INIT(&plugInOptions);
    plugInOptions.Usb20PortNumber = 1;

    status = UdecxUsbDevicePlugIn(usbDevice, &plugInOptions);
    DeckBtRecordStep(DECKBT_STEP_UDEV_PLUGIN, status);
    if (NT_SUCCESS(status)) {
        /* Publish only an active, plugged-in object. */
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
