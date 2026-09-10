/*
 * endpoints.c - URB handling for DeckBtUsb (M1).
 *
 * Over the USB Bluetooth transport:
 *   EP0 class OUT data stage -> HCI command packet
 *   EP 0x81 interrupt IN     -> HCI events
 *   EP 0x02 / 0x82 bulk      -> HCI ACL data
 *   EP 0x03 / 0x83 isoch     -> SCO voice (M2; completed empty here)
 * There is no H4 one-byte packet-type prefix on USB: the endpoints separate the streams.
 */

#include "deckbtusb.h"

/* USB class-request recipient/type bits for an HCI command: host-to-device, class, device. */
#define DECKBT_BMREQUEST_HCI_COMMAND 0x20u

VOID
DeckBtEvtEndpointReset(_In_ UDECXUSBENDPOINT UdecxUsbEndpoint, _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(UdecxUsbEndpoint);

    /*
     * Nothing to reset in M1: there is no hardware pipe state behind these endpoints. Failing
     * the reset would abort the pipe from BTHUSB's point of view, so complete it successfully.
     */
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
DeckBtDrainEvents(_In_ PDECKBT_CONTROLLER Controller)
{
    for (;;) {
        NTSTATUS status;
        WDFREQUEST request;
        PUCHAR buffer;
        ULONG capacity = 0;
        ULONG written = 0;
        BOOLEAN popped;

        WdfSpinLockAcquire(Controller->Lock);
        if (!HciStubHasEvent(&Controller->Hci)) {
            WdfSpinLockRelease(Controller->Lock);
            return;
        }
        WdfSpinLockRelease(Controller->Lock);

        status = WdfIoQueueRetrieveNextRequest(Controller->EventQueue, &request);
        if (!NT_SUCCESS(status)) {
            return;   /* no reader posted; the event stays queued until one is */
        }

        status = UdecxUrbRetrieveBuffer(request, &buffer, &capacity);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(request, status);
            continue;
        }

        WdfSpinLockAcquire(Controller->Lock);
        popped = HciStubPopEvent(&Controller->Hci, buffer, capacity, &written);
        WdfSpinLockRelease(Controller->Lock);

        if (!popped) {
            /* Raced with another drain thread; return request to the queue */
            (void)WdfRequestForwardToIoQueue(request, Controller->EventQueue);
            return;
        }

        UdecxUrbSetBytesCompleted(request, written);
        UdecxUrbComplete(request, USBD_STATUS_SUCCESS);
    }
}

VOID
DeckBtEvtControlUrb(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    PDECKBT_ENDPOINT epContext = DeckBtGetEndpoint(Queue);
    PDECKBT_CONTROLLER controller = epContext->Controller;
    WDF_USB_CONTROL_SETUP_PACKET setup;
    PUCHAR buffer;
    ULONG length = 0;
    BOOLEAN accepted;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != (ULONG)IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    status = UdecxUrbRetrieveControlSetupPacket(Request, &setup);
    if (!NT_SUCCESS(status)) {
        UCHAR none[8] = { 0 };
        DeckBtLogControl(controller, none, DECKBT_CTL_NOTCONTROL, 0, 0);
        /* Not a control transfer on the default pipe - nothing sensible to do with it. */
        UdecxUrbCompleteWithNtStatus(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /*
     * UdeCx answers GET_DESCRIPTOR / SET_CONFIGURATION / SET_INTERFACE from the registered
     * descriptors, but not the whole standard set. The initial trace showed
     * GET_STATUS (bmRequestType 0x80, bRequest 0x00) arriving here and being stalled,
     * causing BTHUSB to reset the controller. Standard requests routed to EP0 are handled directly.
     */
    if ((setup.Packet.bm.Byte & 0x60u) == 0u) {   /* standard request */
        switch (setup.Packet.bRequest) {
        case 0x00: {   /* GET_STATUS - device, interface or endpoint */
            PUCHAR statusBuf;
            ULONG  statusLen = 0;

            status = UdecxUrbRetrieveBuffer(Request, &statusBuf, &statusLen);
            if (!NT_SUCCESS(status) || statusLen < 2) {
                DeckBtLogControl(controller, (const UCHAR *)&setup, DECKBT_CTL_BADBUFFER, 0, 0);
                UdecxUrbCompleteWithNtStatus(Request, STATUS_INVALID_PARAMETER);
                return;
            }
            /*
             * Bus powered (bit0 = 0) to match bmAttributes/bMaxPower, and remote wake not
             * currently armed (bit1 = 0). Endpoint requests report "not halted".
             */
            statusBuf[0] = 0;
            statusBuf[1] = 0;
            DeckBtLogControl(controller, (const UCHAR *)&setup, DECKBT_CTL_OK_HCI, 0, 2);
            UdecxUrbSetBytesCompleted(Request, 2);
            UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
            return;
        }
        case 0x01:   /* CLEAR_FEATURE - e.g. ENDPOINT_HALT after a stall */
        case 0x03:   /* SET_FEATURE   - e.g. DEVICE_REMOTE_WAKEUP        */
            DeckBtLogControl(controller, (const UCHAR *)&setup, DECKBT_CTL_OK_HCI, 0, 0);
            UdecxUrbSetBytesCompleted(Request, 0);
            UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
            return;
        default:
            break;   /* fall through to the stall below */
        }
    }

    if (setup.Packet.bm.Byte != DECKBT_BMREQUEST_HCI_COMMAND || setup.Packet.bRequest != 0) {
        DeckBtLogControl(controller, (const UCHAR *)&setup, DECKBT_CTL_STALLED, 0, 0);
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_STALL_PID);
        return;
    }

    status = UdecxUrbRetrieveBuffer(Request, &buffer, &length);
    if (!NT_SUCCESS(status)) {
        DeckBtLogControl(controller, (const UCHAR *)&setup, DECKBT_CTL_BADBUFFER, 0, 0);
        UdecxUrbCompleteWithNtStatus(Request, status);
        return;
    }

    /* Record the opcode and the size of the reply the stub generated, so the event log's
     * "wrong event size" / "command timed out" complaints can be tied to a specific command. */
    {
        USHORT opcode = (length >= 2u)
                      ? (USHORT)(buffer[0] | ((USHORT)buffer[1] << 8))
                      : (USHORT)0;
        UCHAR  evtLen = 0;

        WdfSpinLockAcquire(controller->Lock);
        accepted = HciStubSubmitCommand(&controller->Hci, buffer, length);
        if (accepted && controller->Hci.Count > 0) {
            ULONG last = (controller->Hci.Tail + HCI_EVENT_FIFO_DEPTH - 1) % HCI_EVENT_FIFO_DEPTH;
            evtLen = (UCHAR)min(controller->Hci.Fifo[last].Length, 255u);
        }
        WdfSpinLockRelease(controller->Lock);

        DeckBtLogControl(controller, (const UCHAR *)&setup,
                         accepted ? DECKBT_CTL_OK_HCI : DECKBT_CTL_STALLED, opcode, evtLen);
    }

    if (!accepted) {
        /* Malformed or unanswerable command packet: stall rather than hang the caller. */
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_STALL_PID);
        return;
    }

    UdecxUrbSetBytesCompleted(Request, length);
    UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);

    /* The event this command produced may already have a reader waiting. */
    DeckBtDrainEvents(controller);
}

VOID
DeckBtEvtDataUrb(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    NTSTATUS status;
    PDECKBT_ENDPOINT epContext = DeckBtGetEndpoint(Queue);
    PDECKBT_CONTROLLER controller = epContext->Controller;
    PUCHAR buffer;
    ULONG length = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (IoControlCode != (ULONG)IOCTL_INTERNAL_USB_SUBMIT_URB) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    switch (epContext->Address) {
    case DECKBT_EP_EVENT_IN:
        /* Park the reader; DeckBtDrainEvents completes it when an event exists. */
        status = WdfRequestForwardToIoQueue(Request, controller->EventQueue);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(Request, status);
            return;
        }
        DeckBtDrainEvents(controller);
        return;

    case DECKBT_EP_ACL_IN:
        /*
         * With a synthetic stub backend, there is no inbound ACL traffic. Parking the request
         * emulates an idle IN pipe without completing premature zero-length packets.
         */
        status = WdfRequestForwardToIoQueue(Request, controller->AclInQueue);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(Request, status);
        }
        return;

    case DECKBT_EP_ACL_OUT:
        /* Accept and discard: an ACL write to a radio with no links has nowhere to go. */
        status = UdecxUrbRetrieveBuffer(Request, &buffer, &length);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(Request, status);
            return;
        }
        UdecxUrbSetBytesCompleted(Request, length);
        UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
        return;

    case DECKBT_EP_SCO_OUT:
    case DECKBT_EP_SCO_IN:
        /*
         * M2 implements the isochronous data plane. Completing with zero bytes and success here
         * is what lets M1 observe whether isoch URBs reach the client driver at all instead of
         * dying inside ucx01000 - the single most important measurement for M2's design.
         */
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
        return;

    default:
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_STALL_PID);
        return;
    }
}
