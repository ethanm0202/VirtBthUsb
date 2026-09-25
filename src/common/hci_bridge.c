/*
 * hci_bridge.c - transport-agnostic bridge between the Windows-facing USB front end
 * and a real Bluetooth controller reached over a wire.
 *
 * See src/include/hci_bridge.h for architecture, locking rules, and policy documentation.
 */

#include <string.h>
#include "../include/hci_bridge.h"

/* Forward declarations for HCI_TRANSPORT_OPS */
static unsigned char HciBridgeSubmitCommand(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length);

static unsigned char HciBridgeSubmitAcl(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length);

static unsigned char HciBridgeSubmitSco(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length);

static unsigned char HciBridgeHasStream(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream);

static unsigned char HciBridgePopStream(
    HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned char *Buffer,
    unsigned long Capacity,
    unsigned long *Written);

static unsigned long HciBridgeLastEventLength(
    const HCI_TRANSPORT *Transport);

static void HciBridgeReset(
    HCI_TRANSPORT *Transport);

/* Static vtable exposed to the front end */
static const HCI_TRANSPORT_OPS g_HciBridgeTransportOps = {
    HciBridgeSubmitCommand,
    HciBridgeSubmitAcl,
    HciBridgeSubmitSco,
    HciBridgeHasStream,
    HciBridgePopStream,
    HciBridgeLastEventLength,
    HciBridgeReset
};

/* Internal helper: compute sum of Outstanding over all InUse handle entries */
static unsigned short HciBridgeTotalInFlight(const HCI_BRIDGE *Bridge)
{
    unsigned int i;
    unsigned short inFlight = 0;
    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse) {
            inFlight = (unsigned short)(inFlight + Bridge->Handles[i].Outstanding);
        }
    }
    return inFlight;
}

/* Internal helper: clamp AvailableAclCredits against true ceiling (TotalAclBuffers - inFlight) */
static void HciBridgeClampCredits(HCI_BRIDGE *Bridge)
{
    unsigned short inFlight;
    unsigned short ceiling;

    if (Bridge->TotalAclBuffers == 0) {
        Bridge->AvailableAclCredits = 0;
        return;
    }

    inFlight = HciBridgeTotalInFlight(Bridge);
    ceiling = (Bridge->TotalAclBuffers > inFlight)
            ? (unsigned short)(Bridge->TotalAclBuffers - inFlight)
            : 0;

    if (Bridge->AvailableAclCredits > ceiling) {
        Bridge->AvailableAclCredits = ceiling;
    }
}

/* Internal helper: reserve a slot for Handle if not already present; returns slot index or -1 if full */
static int HciBridgeReserveHandleSlot(HCI_BRIDGE *Bridge, unsigned short Handle)
{
    unsigned int i;
    int firstFree = -1;

    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse) {
            if (Bridge->Handles[i].Handle == Handle) {
                return (int)i;
            }
        } else if (firstFree < 0) {
            firstFree = (int)i;
        }
    }

    if (firstFree >= 0) {
        Bridge->Handles[firstFree].InUse = 1;
        Bridge->Handles[firstFree].Handle = Handle;
        Bridge->Handles[firstFree].Outstanding = 0;
        return firstFree;
    }

    return -1; /* Table full (all 8 slots occupied by different active handles) */
}

/* Internal helper: record completed ACL packets for Handle */
static void HciBridgeRecordAclCompleted(
    HCI_BRIDGE *Bridge,
    unsigned short Handle,
    unsigned short Completed)
{
    unsigned int i;
    int matched = 0;

    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse && Bridge->Handles[i].Handle == Handle) {
            unsigned short actual = (Bridge->Handles[i].Outstanding >= Completed)
                                  ? Completed
                                  : Bridge->Handles[i].Outstanding;
            Bridge->Handles[i].Outstanding = (unsigned short)(Bridge->Handles[i].Outstanding - actual);
            Bridge->AvailableAclCredits = (unsigned short)(Bridge->AvailableAclCredits + actual);
            matched = 1;
            break;
        }
    }

    /* If no handle matched, do NOT credit (ignores stale / rogue reports) */
    if (!matched) {
        return;
    }

    /* Clamp against true ceiling: TotalAclBuffers - inFlight */
    HciBridgeClampCredits(Bridge);
}

/* Internal helper: handle Disconnection_Complete (event 0x05) to restore outstanding credits */
static void HciBridgeRecordDisconnection(HCI_BRIDGE *Bridge, unsigned short Handle)
{
    unsigned int i;
    for (i = 0; i < HCI_BRIDGE_MAX_HANDLES; i++) {
        if (Bridge->Handles[i].InUse && Bridge->Handles[i].Handle == Handle) {
            unsigned short lostCredits = Bridge->Handles[i].Outstanding;
            Bridge->Handles[i].Outstanding = 0;
            Bridge->Handles[i].InUse = 0;
            if (lostCredits > 0) {
                Bridge->AvailableAclCredits = (unsigned short)(Bridge->AvailableAclCredits + lostCredits);
            }
            HciBridgeClampCredits(Bridge);
            return;
        }
    }
}


/* ---------------------------------------------------------------- Lifecycle */

void HciBridgeInit(
    _Out_ HCI_BRIDGE *Bridge,
    _In_ const HCI_BRIDGE_WIRE *Wire)
{
    if (!Bridge) {
        return;
    }
    memset(Bridge, 0, sizeof(*Bridge));
    if (Wire) {
        Bridge->Wire = *Wire;
    }
}

void HciBridgeSetReady(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ unsigned char Ready)
{
    unsigned char wasReady;
    if (!Bridge) {
        return;
    }

    wasReady = Bridge->Ready;
    Bridge->Ready = Ready;

    /* On transition from 0 -> 1, replay held command immediately (hold-then-replay) */
    if (!wasReady && Ready) {
        if (Bridge->HeldCommandPending) {
            Bridge->HeldCommandPending = 0;
            Bridge->Counters.CommandsReplayed++;
            if (Bridge->Wire.Ops && Bridge->Wire.Ops->SendCommand) {
                unsigned char ok = Bridge->Wire.Ops->SendCommand(
                    Bridge->Wire.Context,
                    Bridge->HeldCommand,
                    Bridge->HeldCommandLength);
                if (ok) {
                    Bridge->Counters.CommandsSentToWire++;
                } else {
                    Bridge->Counters.CommandsFailedWire++;
                }
            }
        }
        /* Drain any pending outbound SCO packets */
        HciBridgeDrainOutboundSco(Bridge);
    }
}

void HciBridgeBindTransport(
    _Inout_ HCI_TRANSPORT *Transport,
    _Inout_ HCI_BRIDGE *Bridge)
{
    if (!Transport || !Bridge) {
        return;
    }
    Transport->Ops = &g_HciBridgeTransportOps;
    Transport->Context = Bridge;
    Transport->Backend = HCI_BACKEND_UART;
    Bridge->Transport = Transport;
}

/* ------------------------------------------------------ HCI_TRANSPORT_OPS */

static unsigned char HciBridgeSubmitCommand(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context || !Packet) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Validate packet framing: opcode (2) + plen (1) + parameters */
    if (Length < 3 || Length != (3u + (unsigned long)Packet[2]) ||
        Length > HCI_BRIDGE_MAX_COMMAND_SIZE) {
        bridge->Counters.CommandsDropped++;
        return 0;
    }

    bridge->Counters.CommandsSubmitted++;

    /*
     * Readiness gating: hold-then-replay.
     *
     * Returning 0 here makes the front end stall EP0, which during enumeration puts
     * BTHUSB into an endless reset-and-retry cycle with no usable diagnostic. Accepting
     * and holding converts enumeration failure into a command timeout, leaving the device
     * enumerated and diagnostic counters readable.
     *
     * The slot is depth 1 because BTHUSB is stop-and-wait on EP0. If a second command
     * arrives before replay, the earlier held command is overwritten.
     */
    if (!bridge->Ready) {
        if (bridge->HeldCommandPending) {
            bridge->Counters.CommandsHeldOverwritten++;
        }
        memcpy(bridge->HeldCommand, Packet, Length);
        bridge->HeldCommandLength = Length;
        bridge->HeldCommandPending = 1;
        bridge->Counters.CommandsHeld++;
        /* TRUE: BTHUSB's EP0 transfer succeeds and it waits for the reply. */
        return 1;
    }

    /* Ready: forward directly to wire */
    if (bridge->Wire.Ops && bridge->Wire.Ops->SendCommand) {
        unsigned char ok = bridge->Wire.Ops->SendCommand(
            bridge->Wire.Context, Packet, Length);
        if (ok) {
            bridge->Counters.CommandsSentToWire++;
            return 1;
        } else {
            bridge->Counters.CommandsFailedWire++;
            return 0;
        }
    }

    return 0;
}

static unsigned char HciBridgeSubmitAcl(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    HCI_BRIDGE *bridge;
    unsigned short handle;
    if (!Transport || !Transport->Context || !Packet) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Validate packet framing: handle/flags (2) + length (2) + payload */
    if (Length < 4 || Length != (4u + (unsigned long)Packet[2] + ((unsigned long)Packet[3] << 8)) ||
        Length > HCI_BRIDGE_MAX_ACL_SIZE) {
        return 0;
    }

    bridge->Counters.AclSubmitted++;

    if (!bridge->Ready) {
        bridge->Counters.AclOutDroppedNotReady++;
        return 0;
    }

    /* Credit check */
    if (bridge->AvailableAclCredits == 0) {
        bridge->Counters.AclDroppedNoCredit++;
        return 0;
    }
    handle = (unsigned short)(((unsigned short)Packet[0] | ((unsigned short)Packet[1] << 8)) & 0x0FFFu);

    /* Reserve handle slot BEFORE consuming credit or sending to wire */
    {
        int slotIdx = HciBridgeReserveHandleSlot(bridge, handle);
        if (slotIdx < 0) {
            bridge->Counters.AclDroppedNoHandleSlot++;
            return 0;
        }

        if (bridge->Wire.Ops && bridge->Wire.Ops->SendAcl) {
            unsigned char ok = bridge->Wire.Ops->SendAcl(
                bridge->Wire.Context, Packet, Length);
            if (ok) {
                bridge->AvailableAclCredits--;
                bridge->Counters.AclSentToWire++;
                bridge->Handles[slotIdx].Outstanding++;
                return 1;
            } else {
                bridge->Counters.AclDroppedWireFailed++;
                return 0;
            }
        }
    }
    return 0;
}

static unsigned char HciBridgeSubmitSco(
    HCI_TRANSPORT *Transport,
    const unsigned char *Packet,
    unsigned long Length)
{
    HCI_BRIDGE *bridge;
    HCI_BRIDGE_SCO_SLOT *slot;
    if (!Transport || !Transport->Context || !Packet) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Validate packet framing: handle/flags (2) + length (1) + payload */
    if (Length < 3 || Length != (3u + (unsigned long)Packet[2]) ||
        Length > HCI_BRIDGE_MAX_SCO_SIZE) {
        return 0;
    }

    bridge->Counters.ScoSubmitted++;

    if (!bridge->Ready) {
        bridge->Counters.ScoOutDroppedNotReady++;
        return 0;
    }

    /* Synchronous FIFO: overwrite-oldest policy */
    if (bridge->OutboundScoCount >= HCI_BRIDGE_SCO_OUT_FIFO_DEPTH) {
        bridge->OutboundScoHead = (bridge->OutboundScoHead + 1) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
        bridge->OutboundScoCount--;
        bridge->Counters.ScoOutboundOverwrites++;
    }

    slot = &bridge->OutboundScoFifo[bridge->OutboundScoTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    bridge->OutboundScoTail = (bridge->OutboundScoTail + 1) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
    bridge->OutboundScoCount++;

    /* Attempt to drain to wire immediately */
    HciBridgeDrainOutboundSco(bridge);
    return 1;
}

void HciBridgeDrainOutboundSco(HCI_BRIDGE *Bridge)
{
    if (!Bridge || !Bridge->Wire.Ops || !Bridge->Wire.Ops->SendSco) {
        return;
    }

    while (Bridge->OutboundScoCount > 0) {
        HCI_BRIDGE_SCO_SLOT *slot = &Bridge->OutboundScoFifo[Bridge->OutboundScoHead];
        unsigned char ok = Bridge->Wire.Ops->SendSco(
            Bridge->Wire.Context, slot->Data, slot->Length);
        if (!ok) {
            /* Wire is busy or buffer is full; leave queued */
            break;
        }
        Bridge->Counters.ScoSentToWire++;
        Bridge->OutboundScoHead = (Bridge->OutboundScoHead + 1) % HCI_BRIDGE_SCO_OUT_FIFO_DEPTH;
        Bridge->OutboundScoCount--;
    }
}

static unsigned char HciBridgeHasStream(
    const HCI_TRANSPORT *Transport,
    HCI_STREAM Stream)
{
    const HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context) {
        return 0;
    }
    bridge = (const HCI_BRIDGE *)Transport->Context;

    switch (Stream) {
    case HciStreamEvent: return (bridge->EventCount > 0) ? (unsigned char)1 : (unsigned char)0;
    case HciStreamAcl:   return (bridge->AclInCount > 0) ? (unsigned char)1 : (unsigned char)0;
    case HciStreamSco:   return (bridge->ScoInCount > 0) ? (unsigned char)1 : (unsigned char)0;
    default:             return (unsigned char)0;
    }
}

static unsigned char HciBridgePopStream(
    HCI_TRANSPORT *Transport,
    HCI_STREAM Stream,
    unsigned char *Buffer,
    unsigned long Capacity,
    unsigned long *Written)
{
    HCI_BRIDGE *bridge;
    if (!Written) {
        return 0;
    }
    *Written = 0;
    if (!Transport || !Transport->Context || !Buffer) {
        return 0;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    switch (Stream) {
    case HciStreamEvent: {
        HCI_BRIDGE_EVENT_SLOT *slot;
        if (bridge->EventCount == 0) {
            return 0;
        }
        slot = &bridge->EventFifo[bridge->EventHead];
        if (slot->Length > Capacity) {
            /* Too small: never partially fill, leave packet queued */
            return 0;
        }
        memcpy(Buffer, slot->Data, slot->Length);
        *Written = slot->Length;
        bridge->EventHead = (bridge->EventHead + 1) % HCI_BRIDGE_EVENT_FIFO_DEPTH;
        bridge->EventCount--;
        return 1;
    }

    case HciStreamAcl: {
        HCI_BRIDGE_ACL_SLOT *slot;
        if (bridge->AclInCount == 0) {
            return 0;
        }
        slot = &bridge->AclInFifo[bridge->AclInHead];
        if (slot->Length > Capacity) {
            return 0;
        }
        memcpy(Buffer, slot->Data, slot->Length);
        *Written = slot->Length;
        bridge->AclInHead = (bridge->AclInHead + 1) % HCI_BRIDGE_ACL_IN_FIFO_DEPTH;
        bridge->AclInCount--;
        return 1;
    }

    case HciStreamSco: {
        HCI_BRIDGE_SCO_SLOT *slot;
        if (bridge->ScoInCount == 0) {
            return 0;
        }
        slot = &bridge->ScoInFifo[bridge->ScoInHead];
        if (slot->Length > Capacity) {
            /*
             * Exception for HciStreamSco per hci_transport.h:
             * Synchronous packet that does not fit Capacity is DISCARDED, not left queued,
             * and PopStream returns FALSE. Isoch IN capacity is fixed by whichever SCO
             * alternate setting Windows selected, so an oversize packet left queued would
             * be re-read forever and wedge the synchronous stream. Stale audio is worthless.
             */
            bridge->ScoInHead = (bridge->ScoInHead + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
            bridge->ScoInCount--;
            bridge->Counters.ScoInboundDiscardedOversize++;
            return 0;
        }
        memcpy(Buffer, slot->Data, slot->Length);
        *Written = slot->Length;
        bridge->ScoInHead = (bridge->ScoInHead + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
        bridge->ScoInCount--;
        return 1;
    }

    default:
        return 0;
    }
}

/*
 * hci_transport.h defines LastEventLength only for HCI_BACKEND_STUB. This bridge is the
 * asynchronous UART backend: events arrive from the wire on a DPC, so any remembered length is
 * already stale by the time the front end reads it. A breadcrumb that lies is worse than one
 * that is absent, and breadcrumbs are the only debugging channel this hardware has. The front
 * end gets the true length from PopStream's *Written.
 */
static unsigned long HciBridgeLastEventLength(const HCI_TRANSPORT *Transport)
{
    UNREFERENCED_PARAMETER(Transport);
    return 0;
}

static void HciBridgeReset(HCI_TRANSPORT *Transport)
{
    HCI_BRIDGE *bridge;
    if (!Transport || !Transport->Context) {
        return;
    }
    bridge = (HCI_BRIDGE *)Transport->Context;

    /* Drop all queued packets and partial state */
    bridge->EventHead = bridge->EventTail = bridge->EventCount = 0;
    bridge->AclInHead = bridge->AclInTail = bridge->AclInCount = 0;
    bridge->ScoInHead = bridge->ScoInTail = bridge->ScoInCount = 0;
    bridge->OutboundScoHead = bridge->OutboundScoTail = bridge->OutboundScoCount = 0;

    bridge->HeldCommandPending = 0;
    bridge->HeldCommandLength = 0;

    memset(bridge->Handles, 0, sizeof(bridge->Handles));
    bridge->AvailableAclCredits = bridge->TotalAclBuffers;

    bridge->Counters.Resets++;
}

/* ------------------------------------------------ Inbound Wire Feeds */

unsigned char HciBridgeOnEvent(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    HCI_BRIDGE_EVENT_SLOT *slot;
    if (!Bridge || !Packet) {
        return 0;
    }

    /* Validate packet framing: event code (1) + plen (1) + payload */
    if (Length < 2 || Length != (2u + (unsigned long)Packet[1]) ||
        Length > HCI_BRIDGE_MAX_EVENT_SIZE) {
        return 0;
    }

    Bridge->Counters.EventsReceived++;

    /* Boot-chatter suppression: unconditionally drop vendor events (0xFF) */
    if (Packet[0] == 0xFFu) {
        Bridge->Counters.EventsSuppressedVendor++;
        return 0;
    }

    /* Snoop for Read_Buffer_Size Command_Complete (opcode 0x1005) */
    if (Packet[0] == 0x0Eu && Length >= 11) {
        unsigned short opcode = (unsigned short)((unsigned short)Packet[3] | ((unsigned short)Packet[4] << 8));
        unsigned char status = Packet[5];
        if (opcode == 0x1005u && status == 0x00u && Length >= 13) {
            unsigned short totalAcl = (unsigned short)((unsigned short)Packet[9] | ((unsigned short)Packet[10] << 8));
            if (Bridge->TotalAclBuffers == 0) {
                memset(Bridge->Handles, 0, sizeof(Bridge->Handles));
                Bridge->AvailableAclCredits = totalAcl;
            }
            Bridge->TotalAclBuffers = totalAcl;
            HciBridgeClampCredits(Bridge);
        }
    }

    /* Snoop for Number_Of_Completed_Packets (event 0x13) */
    if (Packet[0] == 0x13u && Length >= 3) {
        unsigned char numHandles = Packet[2];
        if (Length >= (unsigned long)(3 + numHandles * 4)) {
            unsigned char i;
            for (i = 0; i < numHandles; i++) {
                unsigned long off = 3 + (unsigned long)i * 4;
                unsigned short handle = (unsigned short)(((unsigned short)Packet[off] | ((unsigned short)Packet[off + 1] << 8)) & 0x0FFFu);
                unsigned short completed = (unsigned short)((unsigned short)Packet[off + 2] | ((unsigned short)Packet[off + 3] << 8));
                HciBridgeRecordAclCompleted(Bridge, handle, completed);
            }
        }
    }
    /* Snoop for Disconnection_Complete (event 0x05) */
    if (Packet[0] == 0x05u && Length >= 6) {
        unsigned char status = Packet[2];
        if (status == 0x00u) {
            unsigned short handle = (unsigned short)(((unsigned short)Packet[3] | ((unsigned short)Packet[4] << 8)) & 0x0FFFu);
            HciBridgeRecordDisconnection(Bridge, handle);
        }
    }
    /*
     * Boot-chatter suppression:
     * Unconditionally drop Command_Complete (0x0E) or Command_Status (0x0F)
     * with vendor opcodes (OGF 0x3F, e.g. 0xFC00, 0xFC17, 0xFC48).
     * Late vendor completions (such as the 0xFC48 baud-switch reply) arrive after the
     * host has progressed and must be dropped so BTHPORT does not see a completion for
     * an unissued command (compare upstream Linux hci_qca.c:
     * https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137).
     */
    if (Packet[0] == 0x0Eu && Length >= 5) {
        unsigned short opcode = (unsigned short)((unsigned short)Packet[3] | ((unsigned short)Packet[4] << 8));
        if ((opcode >> 10) == 0x3Fu) {
            Bridge->Counters.EventsSuppressedVendor++;
            return 0;
        }
    }

    if (Packet[0] == 0x0Fu && Length >= 6) {
        unsigned short opcode = (unsigned short)((unsigned short)Packet[4] | ((unsigned short)Packet[5] << 8));
        if ((opcode >> 10) == 0x3Fu) {
            Bridge->Counters.EventsSuppressedVendor++;
            return 0;
        }
    }



    if (!Bridge->Ready) {
        Bridge->Counters.EventsDroppedNotReady++;
        return 0;
    }

    if (Bridge->EventCount >= HCI_BRIDGE_EVENT_FIFO_DEPTH) {
        Bridge->Counters.EventsDroppedFifoFull++;
        return 0;
    }

    slot = &Bridge->EventFifo[Bridge->EventTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    Bridge->EventTail = (Bridge->EventTail + 1) % HCI_BRIDGE_EVENT_FIFO_DEPTH;
    Bridge->EventCount++;
    Bridge->Counters.EventsQueued++;
    return 1;
}

unsigned char HciBridgeOnAcl(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    HCI_BRIDGE_ACL_SLOT *slot;
    if (!Bridge || !Packet) {
        return 0;
    }

    /* Validate packet framing: handle/flags (2) + len (2) + payload */
    if (Length < 4 || Length != (4u + (unsigned long)Packet[2] + ((unsigned long)Packet[3] << 8)) ||
        Length > HCI_BRIDGE_MAX_ACL_SIZE) {
        return 0;
    }

    Bridge->Counters.AclReceived++;

    if (!Bridge->Ready) {
        Bridge->Counters.AclInDroppedNotReady++;
        return 0;
    }

    if (Bridge->AclInCount >= HCI_BRIDGE_ACL_IN_FIFO_DEPTH) {
        Bridge->Counters.AclDroppedFifoFull++;
        return 0;
    }

    slot = &Bridge->AclInFifo[Bridge->AclInTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    Bridge->AclInTail = (Bridge->AclInTail + 1) % HCI_BRIDGE_ACL_IN_FIFO_DEPTH;
    Bridge->AclInCount++;
    Bridge->Counters.AclQueued++;
    return 1;
}

unsigned char HciBridgeOnSco(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    HCI_BRIDGE_SCO_SLOT *slot;
    if (!Bridge || !Packet) {
        return 0;
    }

    /* Validate packet framing: handle/flags (2) + len (1) + payload */
    if (Length < 3 || Length != (3u + (unsigned long)Packet[2]) ||
        Length > HCI_BRIDGE_MAX_SCO_SIZE) {
        return 0;
    }

    Bridge->Counters.ScoReceived++;

    if (!Bridge->Ready) {
        Bridge->Counters.ScoDroppedNotReady++;
        return 0;
    }

    /* Synchronous FIFO: overwrite-oldest policy */
    if (Bridge->ScoInCount >= HCI_BRIDGE_SCO_FIFO_DEPTH) {
        Bridge->ScoInHead = (Bridge->ScoInHead + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
        Bridge->ScoInCount--;
        Bridge->Counters.ScoInboundOverwrites++;
    }

    slot = &Bridge->ScoInFifo[Bridge->ScoInTail];
    memcpy(slot->Data, Packet, Length);
    slot->Length = Length;
    Bridge->ScoInTail = (Bridge->ScoInTail + 1) % HCI_BRIDGE_SCO_FIFO_DEPTH;
    Bridge->ScoInCount++;
    Bridge->Counters.ScoQueued++;
    return 1;
}

unsigned char HciBridgeOnInbound(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ HCI_STREAM Stream,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length)
{
    switch (Stream) {
    case HciStreamEvent:
        return HciBridgeOnEvent(Bridge, Packet, Length);
    case HciStreamAcl:
        return HciBridgeOnAcl(Bridge, Packet, Length);
    case HciStreamSco:
        return HciBridgeOnSco(Bridge, Packet, Length);
    default:
        return 0;
    }
}
