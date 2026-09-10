/*
 * endpoints.c - URB handling and cancellation testing for DeckBtIsoTest (Stage 2).
 */

#include "isotest.h"

VOID
IsoTestEvtEndpointReset(_In_ UDECXUSBENDPOINT UdecxUsbEndpoint, _In_ WDFREQUEST Request)
{
    PISOTEST_CONTROLLER controller = IsoTestGetEndpoint(UdecxUsbEndpoint)->Controller;
    IsoTestReleaseHeldRequest(controller, TRUE);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
IsoTestEvtHeldRequestCanceled(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request)
{
    PISOTEST_CONTROLLER controller = IsoTestGetEndpoint(Queue)->Controller;

    WdfSpinLockAcquire(controller->Lock);
    controller->HoldState = IsoTestHoldIdle;
    controller->PendingCancelled = TRUE;
    controller->CancelCallbackCount++;
    WdfSpinLockRelease(controller->Lock);

    IsoTestLogHoldTelemetry(controller);
    IsoTestRecordStep(ISOTEST_STEP_URB_CANCELLED, STATUS_CANCELLED);
    UdecxUrbSetBytesCompleted(Request, 0);
    UdecxUrbCompleteWithNtStatus(Request, STATUS_CANCELLED);
}

VOID
IsoTestReleaseHeldRequest(_In_ PISOTEST_CONTROLLER Controller, _In_ BOOLEAN Cancel)
{
    WDFREQUEST held = NULL;

    WdfSpinLockAcquire(Controller->Lock);
    if (Controller->HoldState == IsoTestHoldArmed) {
        Controller->HoldState = IsoTestHoldIdle;
    } else if (Controller->HoldState == IsoTestHoldActive &&
               NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Controller->PendingIsochQueue, &held))) {
        Controller->HoldState = IsoTestHoldIdle;
    }
    WdfSpinLockRelease(Controller->Lock);

    if (held != NULL) {
        UdecxUrbSetBytesCompleted(held, 0);
        if (Cancel) {
            UdecxUrbCompleteWithNtStatus(held, STATUS_CANCELLED);
        } else {
            UdecxUrbComplete(held, USBD_STATUS_SUCCESS);
            IsoTestRecordStep(ISOTEST_STEP_URB_RELEASED, STATUS_SUCCESS);
        }
    }
    IsoTestLogHoldTelemetry(Controller);
}

VOID
IsoTestEvtControlUrb(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    PISOTEST_ENDPOINT epContext = IsoTestGetEndpoint(Queue);
    PISOTEST_CONTROLLER controller = epContext->Controller;
    WDF_USB_CONTROL_SETUP_PACKET setup;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != (ULONG)IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    status = UdecxUrbRetrieveControlSetupPacket(Request, &setup);
    if (!NT_SUCCESS(status)) {
        UdecxUrbCompleteWithNtStatus(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Standard requests */
    if ((setup.Packet.bm.Byte & 0x60u) == 0u) {
        switch (setup.Packet.bRequest) {
        case 0x00: {   /* GET_STATUS */
            PUCHAR statusBuf;
            ULONG statusLen = 0;

            status = UdecxUrbRetrieveBuffer(Request, &statusBuf, &statusLen);
            if (!NT_SUCCESS(status) || statusLen < 2) {
                UdecxUrbCompleteWithNtStatus(Request, STATUS_INVALID_PARAMETER);
                return;
            }
            statusBuf[0] = 0;  /* bus-powered */
            statusBuf[1] = 0;
            UdecxUrbSetBytesCompleted(Request, 2);
            UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
            return;
        }
        case 0x01:   /* CLEAR_FEATURE */
        case 0x03:   /* SET_FEATURE */
            UdecxUrbSetBytesCompleted(Request, 0);
            UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
            return;
        default:
            break;
        }
    }

    /* Vendor requests for Stage 2 cancellation testing */
    if ((setup.Packet.bm.Byte & 0x60u) == 0x40u) {
        switch (setup.Packet.bRequest) {
        case ISOTEST_VENDOR_REQ_HOLD_NEXT:
            WdfSpinLockAcquire(controller->Lock);
            if (controller->HoldState != IsoTestHoldIdle) {
                WdfSpinLockRelease(controller->Lock);
                UdecxUrbSetBytesCompleted(Request, 0);
                UdecxUrbCompleteWithNtStatus(Request, STATUS_DEVICE_BUSY);
                return;
            }
            controller->HoldState = IsoTestHoldArmed;
            controller->PendingCancelled = FALSE;
            controller->HoldCommandCount++;
            WdfSpinLockRelease(controller->Lock);
            IsoTestLogHoldTelemetry(controller);
            UdecxUrbSetBytesCompleted(Request, 0);
            UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
            return;

        case ISOTEST_VENDOR_REQ_RELEASE_HELD:
            WdfSpinLockAcquire(controller->Lock);
            controller->ReleaseCommandCount++;
            WdfSpinLockRelease(controller->Lock);
            IsoTestReleaseHeldRequest(controller, FALSE);
            UdecxUrbSetBytesCompleted(Request, 0);
            UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
            return;
        default:
            break;
        }
    }

    UdecxUrbSetBytesCompleted(Request, 0);
    UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
}

VOID
IsoTestEvtDataUrb(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    PISOTEST_ENDPOINT epContext = IsoTestGetEndpoint(Queue);
    PISOTEST_CONTROLLER controller = epContext->Controller;
    WDF_REQUEST_PARAMETERS params;
    PURB urb;
    PUCHAR buffer = NULL;
    ULONG bufferLength = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != (ULONG)IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    urb = (PURB)params.Parameters.Others.Arg1;

    (void)UdecxUrbRetrieveBuffer(Request, &buffer, &bufferLength);

    switch (epContext->Address) {
    case ISOTEST_EP_EVENT_IN:
    case ISOTEST_EP_BULK_IN:
        /* Non-isoch IN endpoints: complete with zero bytes */
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
        return;

    case ISOTEST_EP_BULK_OUT:
        /* Non-isoch OUT endpoints: accept and discard */
        UdecxUrbSetBytesCompleted(Request, bufferLength);
        UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
        return;

    case ISOTEST_EP_ISOCH_OUT: {
        ULONG numPackets = 0, pktLen0 = 0, totalBytes = 0, flags = 0, errors = 0;
        USBD_STATUS overall = USBD_STATUS_SUCCESS;
        if (urb == NULL || urb->UrbHeader.Function != URB_FUNCTION_ISOCH_TRANSFER) {
            UdecxUrbSetBytesCompleted(Request, 0);
            UdecxUrbComplete(Request, USBD_STATUS_INVALID_PARAMETER);
            return;
        }
        {
            struct _URB_ISOCH_TRANSFER *isoch = &urb->UrbIsochronousTransfer;
            numPackets = isoch->NumberOfPackets;
            flags = isoch->TransferFlags;
            for (ULONG i = 0; i < numPackets; i++) {
                ULONG offset = isoch->IsoPacket[i].Offset;
                ULONG length = isoch->IsoPacket[i].Length;
                if (i == 0) pktLen0 = length;
                if (offset > bufferLength || length > bufferLength - offset) {
                    isoch->IsoPacket[i].Length = 0;
                    isoch->IsoPacket[i].Status = USBD_STATUS_DATA_OVERRUN;
                    overall = USBD_STATUS_DATA_OVERRUN;
                    errors++;
                } else {
                    isoch->IsoPacket[i].Status = USBD_STATUS_SUCCESS;
                    totalBytes += length;
                }
            }
            isoch->Hdr.Status = overall;
            isoch->ErrorCount = errors;
        }
        IsoTestLogIsoch(controller, epContext->Address, 0, numPackets,
                        bufferLength, pktLen0, overall, totalBytes, flags,
                        epContext->MaxPacketSize);
        IsoTestRecordStep(ISOTEST_STEP_URB_ISOCH_OUT,
                          overall == USBD_STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_INVALID_BUFFER_SIZE);
        UdecxUrbSetBytesCompleted(Request, totalBytes);
        UdecxUrbComplete(Request, overall);
        return;
    }

    case ISOTEST_EP_ISOCH_IN: {
        BOOLEAN shouldHold = FALSE;

        WdfSpinLockAcquire(controller->Lock);
        if (controller->HoldState == IsoTestHoldArmed) {
            status = WdfRequestForwardToIoQueue(Request, controller->PendingIsochQueue);
            if (NT_SUCCESS(status)) {
                controller->HoldState = IsoTestHoldActive;
                controller->HeldTransferCount++;
                shouldHold = TRUE;
            } else {
                controller->HoldState = IsoTestHoldIdle;
            }
        }
        WdfSpinLockRelease(controller->Lock);

        if (shouldHold) {
            IsoTestLogHoldTelemetry(controller);
            IsoTestRecordStep(ISOTEST_STEP_URB_HELD, STATUS_PENDING);
            return;
        }

        {
            ULONG numPackets = 0, pktLen0 = 0, totalBytes = 0, flags = 0, errors = 0;
            USBD_STATUS overall = USBD_STATUS_SUCCESS;
            if (urb == NULL || urb->UrbHeader.Function != URB_FUNCTION_ISOCH_TRANSFER ||
                buffer == NULL) {
                UdecxUrbSetBytesCompleted(Request, 0);
                UdecxUrbComplete(Request, USBD_STATUS_INVALID_PARAMETER);
                return;
            }
            {
                struct _URB_ISOCH_TRANSFER *isoch = &urb->UrbIsochronousTransfer;
                /*
                 * EvtUsbDeviceEndpointsConfigure was observed to arrive one interface-setting
                 * change behind the host's view. Endpoint-derived geometry from UDE endpoint
                 * creation is therefore the only safe source of truth for packet size.
                 */
                ULONG maxPkt = epContext->MaxPacketSize;
                numPackets = isoch->NumberOfPackets;
                flags = isoch->TransferFlags;
                for (ULONG i = 0; i < numPackets; i++) {
                    ULONG offset = isoch->IsoPacket[i].Offset;
                    ULONG pktBytes = 0;
                    if (offset <= bufferLength) {
                        pktBytes = min(maxPkt, bufferLength - offset);
                        for (ULONG b = 0; b < pktBytes; b++) {
                            buffer[offset + b] = (UCHAR)((totalBytes + b + 1) & 0xFFu);
                        }
                        isoch->IsoPacket[i].Status = USBD_STATUS_SUCCESS;
                        totalBytes += pktBytes;
                    } else {
                        isoch->IsoPacket[i].Status = USBD_STATUS_DATA_OVERRUN;
                        overall = USBD_STATUS_DATA_OVERRUN;
                        errors++;
                    }
                    isoch->IsoPacket[i].Length = pktBytes;
                    if (i == 0) pktLen0 = pktBytes;
                }
                isoch->Hdr.Status = overall;
                isoch->ErrorCount = errors;
            }
            IsoTestLogIsoch(controller, epContext->Address, 1, numPackets,
                            bufferLength, pktLen0, overall, totalBytes, flags,
                            epContext->MaxPacketSize);
            IsoTestRecordStep(ISOTEST_STEP_URB_ISOCH_IN,
                              overall == USBD_STATUS_SUCCESS ? STATUS_SUCCESS : STATUS_INVALID_BUFFER_SIZE);
            UdecxUrbSetBytesCompleted(Request, totalBytes);
            UdecxUrbComplete(Request, overall);
            return;
        }
    }

    default:
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_STALL_PID);
        return;
    }
}
