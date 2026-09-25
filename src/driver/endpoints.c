/*
 * endpoints.c - URB handling for DeckBtUsb.
 *
 * Over the USB Bluetooth transport:
 *   EP0 class OUT data stage -> HCI command packet
 *   EP 0x81 interrupt IN     -> HCI events
 *   EP 0x02 / 0x82 bulk      -> HCI ACL data
 *   EP 0x03 / 0x83 isoch     -> SCO voice, paced on a virtual 1 ms frame clock (sco_usb.h)
 * There is no H4 one-byte packet-type prefix on USB: the endpoints separate the streams.
 */

#include "deckbtusb.h"

/* USB class-request recipient/type bits for an HCI command: host-to-device, class, device. */
#define DECKBT_BMREQUEST_HCI_COMMAND 0x20u

/* HCI commands that define a synchronous link; their parameters are kept for diagnosis. */
#define DECKBT_OP_SETUP_SYNC          0x0428u
#define DECKBT_OP_ACCEPT_SYNC         0x0429u
#define DECKBT_OP_ENH_SETUP_SYNC      0x043Du
#define DECKBT_OP_ENH_ACCEPT_SYNC     0x043Eu
#define DECKBT_OP_WRITE_VOICE_SETTING 0x0C26u

/* ------------------------------------------------------------------ SCO voice */

/*
 * The frame clock. KeQueryInterruptTime advances only on the clock tick (15.6 ms unless raised),
 * coarser than the 1 ms frames it has to measure; the precise variant reads the hardware counter.
 */
static ULONGLONG
DeckBtScoNow(VOID)
{
    ULONG64 qpc;

    return KeQueryInterruptTimePrecise(&qpc);
}

/* ScoUsbOutRelease emit target, under controller->Lock: one reassembled packet to the bridge. */
static void
DeckBtScoEmit(void *Context, const unsigned char *Packet, unsigned long Length)
{
    PDECKBT_CONTROLLER controller = (PDECKBT_CONTROLLER)Context;

    if (HciTransportSubmitSco(&controller->Transport, Packet, Length)) {
        controller->ScoStats.OutHciPackets++;
    } else {
        controller->ScoStats.OutRejected++;
    }
    controller->ScoStats.LastOutHeader = (ULONG)Packet[0] | ((ULONG)Packet[1] << 8) | ((ULONG)Packet[2] << 16);
}

/* ScoUsbInFill source, under controller->Lock: one controller SCO packet from the bridge. */
static unsigned char
DeckBtScoPull(void *Context, unsigned char *Packet, unsigned long Capacity, unsigned long *Written)
{
    PDECKBT_CONTROLLER controller = (PDECKBT_CONTROLLER)Context;

    return HciTransportPopStream(&controller->Transport, HciStreamSco, Packet, Capacity, Written);
}

static struct _URB_ISOCH_TRANSFER *
DeckBtScoUrb(_In_ WDFREQUEST Request, _Out_ PUCHAR *Buffer, _Out_ PULONG BufferLength)
{
    WDF_REQUEST_PARAMETERS params;
    PURB urb;

    *Buffer = NULL;
    *BufferLength = 0;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    urb = (PURB)params.Parameters.Others.Arg1;
    if (urb == NULL || urb->UrbHeader.Function != URB_FUNCTION_ISOCH_TRANSFER ||
        urb->UrbIsochronousTransfer.NumberOfPackets == 0 ||
        !NT_SUCCESS(UdecxUrbRetrieveBuffer(Request, Buffer, BufferLength)) || *Buffer == NULL) {
        return NULL;
    }
    return &urb->UrbIsochronousTransfer;
}

/* Completes an IN transfer with every packet empty: no voice for these frames. */
static VOID
DeckBtScoCompleteEmptyIn(_In_ WDFREQUEST Request, _Inout_ struct _URB_ISOCH_TRANSFER *Isoch)
{
    for (ULONG i = 0; i < Isoch->NumberOfPackets; i++) {
        Isoch->IsoPacket[i].Length = 0;
        Isoch->IsoPacket[i].Status = USBD_STATUS_SUCCESS;
    }
    Isoch->Hdr.Status = USBD_STATUS_SUCCESS;
    Isoch->ErrorCount = 0;
    UdecxUrbSetBytesCompleted(Request, 0);
    UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
}

/* Arms the pacing timer unless it is already running. */
static VOID
DeckBtScoKick(_In_ PDECKBT_CONTROLLER Controller)
{
    BOOLEAN start = FALSE;

    WdfSpinLockAcquire(Controller->Lock);
    if (!Controller->ScoTimerArmed) {
        Controller->ScoTimerArmed = TRUE;
        start = TRUE;
    }
    WdfSpinLockRelease(Controller->Lock);
    if (start) {
        (void)WdfTimerStart(Controller->ScoTimer, WDF_REL_TIMEOUT_IN_MS(1));
    }
}

/*
 * A SCO transfer from BTHUSB, PASSIVE_LEVEL. It is given its slot on the endpoint's frame clock and
 * parked until that slot ends. OUT bytes are copied out now, each isochronous packet tagged with the
 * end of its own frame, so the timer hands voice to the controller at the air rate.
 */
static VOID
DeckBtScoAccept(_In_ PDECKBT_CONTROLLER Controller, _In_ PDECKBT_ENDPOINT Endpoint, _In_ WDFREQUEST Request)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PDECKBT_SCO_REQUEST context = NULL;
    struct _URB_ISOCH_TRANSFER *isoch;
    PUCHAR buffer;
    ULONG bufferLength;
    BOOLEAN out = (BOOLEAN)(Endpoint->Address == DECKBT_EP_SCO_OUT);
    ULONG packets;
    ULONG total = 0;
    ULONG errors = 0;
    USBD_STATUS overall = USBD_STATUS_SUCCESS;
    ULONGLONG due;
    NTSTATUS status;

    isoch = DeckBtScoUrb(Request, &buffer, &bufferLength);
    if (isoch == NULL) {
        WdfSpinLockAcquire(Controller->Lock);
        Controller->ScoStats.BadUrbs++;
        WdfSpinLockRelease(Controller->Lock);
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_INVALID_PARAMETER);
        return;
    }
    packets = isoch->NumberOfPackets;

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DECKBT_SCO_REQUEST);
    status = WdfObjectAllocateContext(Request, &attributes, (PVOID *)&context);
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_EXISTS) {
        context = NULL;
    }

    WdfSpinLockAcquire(Controller->Lock);
    if (out) {
        ULONGLONG span = 0;
        ULONGLONG at;

        for (ULONG i = 0; i < packets; i++) {
            ULONG offset = isoch->IsoPacket[i].Offset;
            ULONG length = isoch->IsoPacket[i].Length;

            if (offset > bufferLength || length > bufferLength - offset) {
                isoch->IsoPacket[i].Status = USBD_STATUS_DATA_OVERRUN;
                overall = USBD_STATUS_DATA_OVERRUN;
                errors++;
                span += SCO_USB_FRAME_100NS;
            } else {
                isoch->IsoPacket[i].Status = USBD_STATUS_SUCCESS;
                total += length;
                span += ScoUsbOutPacketSpan(Endpoint->MaxPacketSize, length);
            }
        }
        due = ScoUsbSchedule(&Controller->ScoOutClock, DeckBtScoNow(), span);
        /* Each packet's bytes are released when the voice before it has had its air time. */
        at = due - span;
        for (ULONG i = 0; i < packets; i++) {
            if (isoch->IsoPacket[i].Status == USBD_STATUS_SUCCESS) {
                at += ScoUsbOutPacketSpan(Endpoint->MaxPacketSize, isoch->IsoPacket[i].Length);
                ScoUsbOutPush(&Controller->ScoOut, at, buffer + isoch->IsoPacket[i].Offset,
                              isoch->IsoPacket[i].Length);
            } else {
                at += SCO_USB_FRAME_100NS;
            }
        }
        if (Controller->ScoStats.OutUrbs == 0) {
            Controller->ScoStats.FirstOutGeometry = (packets << 16) | (isoch->IsoPacket[0].Length & 0xFFFFu);
        }
        Controller->ScoStats.OutUrbs++;
        Controller->ScoStats.OutIsoPackets += packets;
        Controller->ScoStats.OutBytes += total;
        Controller->ScoStats.OutMaxPacket = Endpoint->MaxPacketSize;
        isoch->Hdr.Status = overall;
        isoch->ErrorCount = errors;
    } else {
        due = ScoUsbSchedule(&Controller->ScoInClock, DeckBtScoNow(), (ULONGLONG)packets * SCO_USB_FRAME_100NS);
        if (Controller->ScoStats.InUrbs == 0) {
            Controller->ScoStats.FirstInGeometry = (packets << 16) | (bufferLength & 0xFFFFu);
        }
        Controller->ScoStats.InUrbs++;
        Controller->ScoStats.InIsoPackets += packets;
        Controller->ScoStats.InMaxPacket = Endpoint->MaxPacketSize;
    }
    Controller->ScoStats.LastTransferFlags = isoch->TransferFlags;
    if (context == NULL) {
        Controller->ScoStats.UnpacedUrbs++;
    }
    WdfSpinLockRelease(Controller->Lock);

    if (context != NULL) {
        context->Due = due;
        context->Bytes = total;
        context->Status = overall;
        context->MaxPacketSize = Endpoint->MaxPacketSize;
        status = WdfRequestForwardToIoQueue(Request, out ? Controller->ScoOutQueue : Controller->ScoInQueue);
    }
    if (context == NULL || !NT_SUCCESS(status)) {
        /* Cannot park it: complete now rather than hold BTHUSB's stream hostage. */
        if (out) {
            UdecxUrbSetBytesCompleted(Request, total);
            UdecxUrbComplete(Request, overall);
        } else {
            DeckBtScoCompleteEmptyIn(Request, isoch);
        }
    }
    DeckBtScoKick(Controller);
    DeckBtScoPublish(Controller, FALSE);
}

/* Fills a due IN transfer from the controller's voice, one isochronous packet per frame. */
static VOID
DeckBtScoFillIn(_In_ PDECKBT_CONTROLLER Controller, _In_ WDFREQUEST Request, _In_ const DECKBT_SCO_REQUEST *Context)
{
    struct _URB_ISOCH_TRANSFER *isoch;
    PUCHAR buffer;
    ULONG bufferLength;
    ULONG total = 0;
    ULONG errors = 0;
    USBD_STATUS overall = USBD_STATUS_SUCCESS;

    isoch = DeckBtScoUrb(Request, &buffer, &bufferLength);
    if (isoch == NULL) {
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_INVALID_PARAMETER);
        return;
    }
    WdfSpinLockAcquire(Controller->Lock);
    for (ULONG i = 0; i < isoch->NumberOfPackets; i++) {
        ULONG offset = isoch->IsoPacket[i].Offset;
        ULONG got = 0;

        if (offset <= bufferLength) {
            ULONG capacity = min((ULONG)Context->MaxPacketSize, bufferLength - offset);
            got = ScoUsbInFill(&Controller->ScoIn, Context->MaxPacketSize, buffer + offset, capacity,
                               DeckBtScoPull, Controller);
            isoch->IsoPacket[i].Status = USBD_STATUS_SUCCESS;
        } else {
            isoch->IsoPacket[i].Status = USBD_STATUS_DATA_OVERRUN;
            overall = USBD_STATUS_DATA_OVERRUN;
            errors++;
        }
        isoch->IsoPacket[i].Length = got;
        total += got;
    }
    Controller->ScoStats.InBytes += total;
    WdfSpinLockRelease(Controller->Lock);

    isoch->Hdr.Status = overall;
    isoch->ErrorCount = errors;
    UdecxUrbSetBytesCompleted(Request, total);
    UdecxUrbComplete(Request, overall);
}

/* Completes every parked transfer on Queue whose frame slot has ended by Now. */
static VOID
DeckBtScoCompleteDue(_In_ PDECKBT_CONTROLLER Controller, _In_ WDFQUEUE Queue, _In_ ULONGLONG Now, _In_ BOOLEAN In)
{
    for (;;) {
        WDFREQUEST found = NULL;
        WDFREQUEST request = NULL;
        PDECKBT_SCO_REQUEST context;
        ULONGLONG lateUs;
        NTSTATUS status;

        status = WdfIoQueueFindRequest(Queue, NULL, NULL, NULL, &found);
        if (!NT_SUCCESS(status)) {
            return;
        }
        context = DeckBtGetScoRequest(found);
        if (context->Due > Now) {
            WdfObjectDereference(found);
            return;
        }
        status = WdfIoQueueRetrieveFoundRequest(Queue, found, &request);
        WdfObjectDereference(found);
        if (status == STATUS_NOT_FOUND) {
            continue;   /* cancelled concurrently */
        }
        if (!NT_SUCCESS(status)) {
            return;
        }
        lateUs = (Now - context->Due) / 10u;
        WdfSpinLockAcquire(Controller->Lock);
        if (lateUs > Controller->ScoStats.MaxLateUs) {
            Controller->ScoStats.MaxLateUs = (ULONG)min(lateUs, 0xFFFFFFFFull);
        }
        WdfSpinLockRelease(Controller->Lock);
        if (In) {
            DeckBtScoFillIn(Controller, request, context);
        } else {
            UdecxUrbSetBytesCompleted(request, context->Bytes);
            UdecxUrbComplete(request, context->Status);
        }
    }
}

/*
 * Pacing tick, DISPATCH_LEVEL. Releases OUT voice whose frame has ended, completes due transfers,
 * and re-arms while anything is parked. The parked-work check runs under Lock, the same lock
 * DeckBtScoKick takes after parking, so a transfer parked during a tick is never stranded.
 */
VOID
DeckBtEvtScoTimer(_In_ WDFTIMER Timer)
{
    PDECKBT_CONTROLLER controller = DeckBtGetController((WDFDEVICE)WdfTimerGetParentObject(Timer));
    ULONGLONG now = DeckBtScoNow();
    ULONG outParked = 0;
    ULONG inParked = 0;
    BOOLEAN pending;

    WdfSpinLockAcquire(controller->Lock);
    (void)ScoUsbOutRelease(&controller->ScoOut, now, DeckBtScoEmit, controller);
    WdfSpinLockRelease(controller->Lock);

    DeckBtScoCompleteDue(controller, controller->ScoOutQueue, now, FALSE);
    DeckBtScoCompleteDue(controller, controller->ScoInQueue, now, TRUE);

    WdfSpinLockAcquire(controller->Lock);
    (void)WdfIoQueueGetState(controller->ScoOutQueue, &outParked, NULL);
    (void)WdfIoQueueGetState(controller->ScoInQueue, &inParked, NULL);
    pending = (BOOLEAN)(controller->ScoOut.Count != 0 || outParked != 0 || inParked != 0);
    if (!pending) {
        controller->ScoTimerArmed = FALSE;
    }
    WdfSpinLockRelease(controller->Lock);
    if (pending) {
        (void)WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(1));
    }
}

/* Ends the current voice stream: cancels parked transfers, drops queued voice, restarts clocks. */
VOID
DeckBtScoFlush(_In_ PDECKBT_CONTROLLER Controller)
{
    WDFQUEUE queues[2] = { Controller->ScoOutQueue, Controller->ScoInQueue };
    ULONG flushed = 0;

    for (ULONG q = 0; q < ARRAYSIZE(queues); q++) {
        WDFREQUEST request;

        if (queues[q] == NULL) {
            continue;
        }
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(queues[q], &request))) {
            UdecxUrbCompleteWithNtStatus(request, STATUS_CANCELLED);
            flushed++;
        }
    }
    WdfSpinLockAcquire(Controller->Lock);
    /* The framers restart per stream; their totals are kept for the session. */
    Controller->ScoStats.OutRingDrops += Controller->ScoOut.RingDrops;
    Controller->ScoStats.OutResyncSkips += Controller->ScoOut.ResyncSkips;
    Controller->ScoStats.InHciPackets += Controller->ScoIn.FramedPackets;
    Controller->ScoStats.InSourcePackets += Controller->ScoIn.SourcePackets;
    Controller->ScoStats.InSourceRejected += Controller->ScoIn.SourceRejected;
    Controller->ScoStats.InDroppedBytes += Controller->ScoIn.DroppedBytes;
    if (Controller->ScoIn.LastSourceLength != 0) {
        Controller->ScoStats.InLastSourceLength = Controller->ScoIn.LastSourceLength;
    }
    ScoUsbOutReset(&Controller->ScoOut);
    ScoUsbInReset(&Controller->ScoIn);
    Controller->ScoOutClock.NextFree = 0;
    Controller->ScoInClock.NextFree = 0;
    Controller->ScoStats.Flushed += flushed;
    WdfSpinLockRelease(Controller->Lock);
}

/* Publishes the Sco* counters, PASSIVE_LEVEL, at most every 500 ms unless forced. */
VOID
DeckBtScoPublish(_In_ PDECKBT_CONTROLLER Controller, _In_ BOOLEAN Force)
{
#define DECKBT_SCO_FIELD(Name) { L"Sco" #Name, (ULONG)FIELD_OFFSET(DECKBT_SCO_STATS, Name) }
    static const struct {
        PCWSTR Name;
        ULONG  Offset;
    } fields[] = {
        DECKBT_SCO_FIELD(AltSetting),       DECKBT_SCO_FIELD(AltChanges),
        DECKBT_SCO_FIELD(OutMaxPacket),     DECKBT_SCO_FIELD(InMaxPacket),
        DECKBT_SCO_FIELD(OutUrbs),          DECKBT_SCO_FIELD(InUrbs),
        DECKBT_SCO_FIELD(OutIsoPackets),    DECKBT_SCO_FIELD(InIsoPackets),
        DECKBT_SCO_FIELD(OutBytes),         DECKBT_SCO_FIELD(InBytes),
        DECKBT_SCO_FIELD(OutHciPackets),    DECKBT_SCO_FIELD(OutRejected),
        DECKBT_SCO_FIELD(OutRingDrops),     DECKBT_SCO_FIELD(OutResyncSkips),
        DECKBT_SCO_FIELD(InHciPackets),     DECKBT_SCO_FIELD(InSourcePackets),
        DECKBT_SCO_FIELD(InSourceRejected), DECKBT_SCO_FIELD(InDroppedBytes),
        DECKBT_SCO_FIELD(InLastSourceLength), DECKBT_SCO_FIELD(BadUrbs),
        DECKBT_SCO_FIELD(UnpacedUrbs),      DECKBT_SCO_FIELD(Flushed),
        DECKBT_SCO_FIELD(FirstOutGeometry), DECKBT_SCO_FIELD(FirstInGeometry),
        DECKBT_SCO_FIELD(LastTransferFlags), DECKBT_SCO_FIELD(MaxLateUs),
        DECKBT_SCO_FIELD(LastOutHeader),    DECKBT_SCO_FIELD(SetupCommands),
    };
#undef DECKBT_SCO_FIELD
    DECKBT_SCO_STATS snapshot;
    UCHAR command[DECKBT_SCO_CMD_TRACE_BYTES];
    ULONGLONG now = KeQueryInterruptTime();
    WDFKEY key = NULL;
    UNICODE_STRING name;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    WdfSpinLockAcquire(Controller->Lock);
    if (!Force && now - Controller->ScoPublishedAt < 500ull * 10000ull) {
        WdfSpinLockRelease(Controller->Lock);
        return;
    }
    Controller->ScoPublishedAt = now;
    snapshot = Controller->ScoStats;
    snapshot.OutRingDrops += Controller->ScoOut.RingDrops;
    snapshot.OutResyncSkips += Controller->ScoOut.ResyncSkips;
    snapshot.InHciPackets += Controller->ScoIn.FramedPackets;
    snapshot.InSourcePackets += Controller->ScoIn.SourcePackets;
    snapshot.InSourceRejected += Controller->ScoIn.SourceRejected;
    snapshot.InDroppedBytes += Controller->ScoIn.DroppedBytes;
    if (Controller->ScoIn.LastSourceLength != 0) {
        snapshot.InLastSourceLength = Controller->ScoIn.LastSourceLength;
    }
    RtlCopyMemory(command, Controller->ScoCmdTrace, sizeof(command));
    WdfSpinLockRelease(Controller->Lock);

    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    for (ULONG i = 0; i < ARRAYSIZE(fields); i++) {
        RtlInitUnicodeString(&name, fields[i].Name);
        (void)WdfRegistryAssignULong(key, &name, *(const ULONG *)((const UCHAR *)&snapshot + fields[i].Offset));
    }
    RtlInitUnicodeString(&name, L"ScoSetupCommand");
    (void)WdfRegistryAssignValue(key, &name, REG_BINARY, sizeof(command), command);
    WdfRegistryClose(key);
}

VOID
DeckBtEvtEndpointReset(_In_ UDECXUSBENDPOINT UdecxUsbEndpoint, _In_ WDFREQUEST Request)
{
    UNREFERENCED_PARAMETER(UdecxUsbEndpoint);

    /*
     * Complete endpoint reset successfully so BTHUSB does not abort the pipe.
     */
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
DeckBtDrainStream(_In_ PDECKBT_CONTROLLER Controller, _In_ HCI_STREAM Stream)
{
    WDFQUEUE queue = NULL;

    switch (Stream) {
    case HciStreamEvent:
        queue = Controller->EventQueue;
        break;
    case HciStreamAcl:
        queue = Controller->AclInQueue;
        break;
    default:
        return;
    }

    if (queue == NULL) {
        return;
    }

    for (;;) {
        NTSTATUS status;
        WDFREQUEST request;
        PUCHAR buffer;
        ULONG capacity = 0;
        ULONG written = 0;
        UCHAR popped;

        WdfSpinLockAcquire(Controller->Lock);
        if (!HciTransportHasStream(&Controller->Transport, Stream)) {
            WdfSpinLockRelease(Controller->Lock);
            return;
        }
        WdfSpinLockRelease(Controller->Lock);

        status = WdfIoQueueRetrieveNextRequest(queue, &request);
        if (!NT_SUCCESS(status)) {
            return;   /* no reader posted; the packet stays queued until one is */
        }

        status = UdecxUrbRetrieveBuffer(request, &buffer, &capacity);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(request, status);
            continue;
        }

        WdfSpinLockAcquire(Controller->Lock);
        popped = HciTransportPopStream(&Controller->Transport, Stream,
                                       buffer, capacity, &written);
        WdfSpinLockRelease(Controller->Lock);

        if (!popped) {
            /* Raced with another drain, or the packet did not fit the posted buffer. */
            UdecxUrbSetBytesCompleted(request, 0);
            UdecxUrbComplete(request, USBD_STATUS_SUCCESS);
            continue;
        }

        UdecxUrbSetBytesCompleted(request, written);
        UdecxUrbComplete(request, USBD_STATUS_SUCCESS);
    }
}

VOID
DeckBtDrainEvents(_In_ PDECKBT_CONTROLLER Controller)
{
    DeckBtDrainStream(Controller, HciStreamEvent);
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
     * UdeCx answers GET_DESCRIPTOR, SET_CONFIGURATION, and SET_INTERFACE from the registered
     * descriptors, but forwards other standard requests here. Handle GET_STATUS
     * (bmRequestType 0x80, bRequest 0x00) so BTHUSB does not stall or reset the controller.
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

    /* Record the opcode and the size of the reply the backend generated, so the event log's
     * "wrong event size" / "command timed out" complaints can be tied to a specific command. */
    {
        USHORT opcode = (length >= 2u)
                      ? (USHORT)(buffer[0] | ((USHORT)buffer[1] << 8))
                      : (USHORT)0;
        UCHAR  evtLen = 0;
        BOOLEAN sync = (BOOLEAN)(opcode == DECKBT_OP_SETUP_SYNC || opcode == DECKBT_OP_ACCEPT_SYNC ||
                                 opcode == DECKBT_OP_ENH_SETUP_SYNC || opcode == DECKBT_OP_ENH_ACCEPT_SYNC ||
                                 opcode == DECKBT_OP_WRITE_VOICE_SETTING);

        WdfSpinLockAcquire(controller->Lock);
        accepted = (BOOLEAN)HciTransportSubmitCommand(&controller->Transport, buffer, length);
        if (accepted) {
            evtLen = (UCHAR)min(HciTransportLastEventLength(&controller->Transport), 255u);
        }
        if (sync) {
            /* How Windows asked for the voice link (codec, air mode, packet types) is not in any trace. */
            RtlZeroMemory(controller->ScoCmdTrace, sizeof(controller->ScoCmdTrace));
            RtlCopyMemory(controller->ScoCmdTrace, buffer, min(length, (ULONG)sizeof(controller->ScoCmdTrace)));
            controller->ScoStats.SetupCommands++;
        }
        WdfSpinLockRelease(controller->Lock);

        DeckBtLogControl(controller, (const UCHAR *)&setup,
                         accepted ? DECKBT_CTL_OK_HCI : DECKBT_CTL_STALLED, opcode, evtLen);
        if (sync) {
            DeckBtScoPublish(controller, TRUE);
        }
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
        /* Park the reader; DeckBtDrainStream completes it when ACL data exists. */
        status = WdfRequestForwardToIoQueue(Request, controller->AclInQueue);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(Request, status);
            return;
        }
        DeckBtDrainStream(controller, HciStreamAcl);
        return;

    case DECKBT_EP_ACL_OUT: {
        BOOLEAN acceptedAcl;
        status = UdecxUrbRetrieveBuffer(Request, &buffer, &length);
        if (!NT_SUCCESS(status)) {
            UdecxUrbCompleteWithNtStatus(Request, status);
            return;
        }
        WdfSpinLockAcquire(controller->Lock);
        acceptedAcl = (BOOLEAN)HciTransportSubmitAcl(&controller->Transport, buffer, length);
        WdfSpinLockRelease(controller->Lock);
        if (!acceptedAcl) {
            UdecxUrbSetBytesCompleted(Request, 0);
            UdecxUrbComplete(Request, USBD_STATUS_STALL_PID);
            return;
        }
        UdecxUrbSetBytesCompleted(Request, length);
        UdecxUrbComplete(Request, USBD_STATUS_SUCCESS);
        return;
    }

    case DECKBT_EP_SCO_OUT:
    case DECKBT_EP_SCO_IN:
        DeckBtScoAccept(controller, epContext, Request);
        return;

    default:
        UdecxUrbSetBytesCompleted(Request, 0);
        UdecxUrbComplete(Request, USBD_STATUS_STALL_PID);
        return;
    }
}
