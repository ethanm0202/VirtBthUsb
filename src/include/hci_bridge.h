/*
 * hci_bridge.h - transport-agnostic bridge between the Windows-facing USB front end
 * and a real Bluetooth controller reached over a wire.
 *
 * ARCHITECTURAL ROLE
 * ------------------
 * DeckBtUsb presents a virtual USB Bluetooth radio to Windows, so the OS loads inbox
 * BTHUSB + BTHPORT. This bridge routes traffic between the USB front end and the physical
 * Qualcomm QCA2066 controller reached over SerCx2 UART.
 *
 * This module is pure logic with no OS dependency (no WDF, no NTSTATUS, no dynamic memory
 * allocation). It compiles in both kernel mode (the driver) and user mode (host-side self-tests).
 * It sits between:
 *   - Above:  HCI_TRANSPORT_OPS vtable consumed by the USB front end (src/driver/endpoints.c).
 *   - Below:  HCI_BRIDGE_WIRE injected function pointers (implemented by src/driver/qca_uart.c
 *             in the driver, or by a mock controller in the host self-test).
 *
 * Buffers crossing this interface carry no H4 packet-type prefix. USB separates streams by
 * endpoint, while UART uses H4. Framing and H4 prefixing belong entirely in the wire layer below.
 *
 * READINESS GATING POLICY: HOLD-THEN-REPLAY
 * -----------------------------------------
 * When the driver starts or resumes, the Qualcomm controller requires a firmware download
 * (qca_init_fsm.c: 639 segments of hpbtfw21.tlv + 28 segments of hpnv21.bin, taking ~0.5 s).
 * During this bring-up, the chip cannot answer host HCI commands.
 *
 * Policy choice: HOLD-THEN-REPLAY (chosen over REFUSE).
 * Justification:
 *   1. BTHUSB uses a stop-and-wait protocol on EP0 (one control transfer at a time; it never
 *      pipelines commands). If SubmitCommand returns FALSE (0), the front end stalls EP0.
 *      An EP0 stall during enumeration causes BTHUSB to fail immediately and enter an endless
 *      reset-and-retry cycle.
 *   2. Conversely, BTHUSB's command timeout is multi-second (typically 2 to 5 seconds; event 3
 *      only fires after a prolonged hang). Firmware download takes ~500 ms.
 *   3. By holding the first host command (typically HCI_Reset, opcode 0x0C03) and returning
 *      success on SubmitCommand, BTHUSB waits comfortably. As soon as firmware download completes
 *      and HciBridgeSetReady(Bridge, 1) is called, the bridge immediately transmits the held
 *      command to the controller. The controller's Command_Complete is returned to BTHUSB well
 *      within its timeout window, achieving clean, glitch-free initialisation.
 *
 * BOOT-CHATTER SUPPRESSION
 * ------------------------
 * Real Qualcomm controllers emit vendor events (event code 0xFF), EDL responses, firmware logs,
 * and baud-switch confirmations during and immediately after firmware download.
 * If any vendor event reached BTHUSB on the interrupt endpoint, BTHUSB would log an error or
 * drop the radio because it received an event it never requested.
 * The bridge unconditionally intercepts and drops all vendor events (0xFF), incrementing the
 * EventsSuppressedVendor diagnostic counter.
 *
 * ACL CREDIT BOOKKEEPING
 * ----------------------
 * With real silicon, the controller has a finite ACL buffer pool. To prevent buffer overflow
 * and hardware lockup:
 *   1. The bridge snoops the Command_Complete event for Read_Buffer_Size (opcode 0x1005) to
 *      learn HC_Total_Num_ACL_Data_Packets. AvailableAclCredits is initialised to this count.
 *   2. SubmitAcl decrements AvailableAclCredits. If credits are exhausted (AvailableAclCredits == 0),
 *      SubmitAcl returns 0 (FALSE), dropping the packet and incrementing AclDroppedNoCredit.
 *   3. The bridge tracks in-flight packets across multiple connection handles (HCI_BRIDGE_MAX_HANDLES).
 *   4. Inbound Number_Of_Completed_Packets events (code 0x13) report completed packets per handle.
 *      The bridge credits back the completed buffers, clamping AvailableAclCredits to TotalAclBuffers
 *      so that boot chatter (unexpected initial credits) cannot artificially inflate the credit pool.
 *   5. The event itself is also queued to HciStreamEvent so BTHPORT receives its credit updates.
 *   6. Disconnection handling: on Disconnection_Complete (event 0x05, status 0x00), any outstanding
 *      in-flight credits for that handle are immediately restored to AvailableAclCredits (clamped
 *      to TotalAclBuffers) and the handle entry is freed, preventing credit leaks on disconnect.
 *
 * POPSTREAM SCO EXCEPTION (OVERSIZED PACKETS DISCARDED)
 * ----------------------------------------------------
 * Per hci_transport.h:
 * For HciStreamEvent and HciStreamAcl, if Capacity is too small to fit the queued packet, PopStream
 * returns 0, writes *Written = 0, and leaves the packet queued.
 * For HciStreamSco, isoch IN capacity is fixed by whichever SCO alternate setting Windows selected
 * (alt 1 = 9 bytes/frame ... alt 5 = 63 bytes). An oversize synchronous packet left queued would
 * be re-read forever, permanently wedging the synchronous stream. Therefore, an oversize SCO packet
 * is DISCARDED, ScoInboundDiscardedOversize is incremented, and PopStream returns 0 (*Written = 0).
 *
 * SYNCHRONOUS (SCO) FIFO WITH OVERWRITE-OLDEST
 * --------------------------------------------
 * Per the Bluetooth USB Transport Specification (Core Spec Vol 4, Part B, Section 2.1.2):
 * Synchronous voice audio (CVSD / mSBC) is strictly latency-critical; queuing delays create
 * permanent audio lag.
 * Therefore, when either the inbound or outbound synchronous FIFO is full, new data MUST overwrite
 * the oldest queued packet. The oldest packet is dropped, the FIFO pointers advance, and the
 * overwrite counter is incremented.
 *
 * LOCKING AND NOTIFY RULES
 * ------------------------
 * All functions in HCI_TRANSPORT_OPS (SubmitCommand, SubmitAcl, SubmitSco, HasStream, PopStream,
 * LastEventLength, Reset) are called with the controller spin lock HELD at <= DISPATCH_LEVEL.
 * They MUST NOT block, allocate, or call HciTransportNotify.
 *
 * The inbound feed functions (HciBridgeOnEvent, HciBridgeOnAcl, HciBridgeOnSco) MUST also be called
 * with the controller spin lock HELD, protecting internal queue integrity.
 * To respect the rule that HciTransportNotify is called with the lock NOT held:
 *   - HciBridgeOnEvent/Acl/Sco return 1 (TRUE) if a packet was successfully queued for the host.
 *   - If the packet was suppressed, dropped, or unhandled, they return 0 (FALSE).
 *   - The caller (e.g. qca_uart.c read completion) MUST release the spin lock first, and then call
 *     HciTransportNotify(Transport, Stream) if and only if the return value was 1.
 * This guarantees zero lock inversion and avoids holding the spin lock across front-end callbacks.
 */

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#include "hci_transport.h"

/* Sizing constants - all statically allocated, zero heap usage */

/* Maximum HCI Command: 2-byte opcode + 1-byte plen + 255-byte parameter payload */
#define HCI_BRIDGE_MAX_COMMAND_SIZE       258u

/* Maximum HCI Event: 1-byte code + 1-byte plen + 255-byte parameter payload (padded to 260) */
#define HCI_BRIDGE_MAX_EVENT_SIZE         260u

/* Maximum HCI ACL: 2-byte handle/flags + 2-byte len + 1024-byte payload (QCA2066 max ACL is 1021) */
#define HCI_BRIDGE_MAX_ACL_SIZE           1028u

/* Maximum HCI SCO: 2-byte handle/flags + 1-byte len + 255-byte payload */
#define HCI_BRIDGE_MAX_SCO_SIZE           258u

/*
 * FIFO depths:
 * - Event FIFO: 16 slots (~4 KB), matching hci_stub depth; absorbs bursts of controller events.
 * - Inbound ACL FIFO: 8 slots (~8 KB), matches controller ACL buffer pool capacity.
 * - SCO FIFOs: 8 slots (~2 KB each). At 7.5 ms / 10 ms frame interval, 8 slots represent
 *   60-80 ms of jitter buffer. Sized strictly to bound audio latency.
 */
#define HCI_BRIDGE_EVENT_FIFO_DEPTH       16u
#define HCI_BRIDGE_ACL_IN_FIFO_DEPTH      8u
#define HCI_BRIDGE_SCO_FIFO_DEPTH         8u
#define HCI_BRIDGE_SCO_OUT_FIFO_DEPTH     8u

/* Maximum concurrent ACL connection handles tracked for credit accounting */
#define HCI_BRIDGE_MAX_HANDLES            8u

/* Slots for internal FIFO queues */
typedef struct _HCI_BRIDGE_EVENT_SLOT {
    unsigned char Data[HCI_BRIDGE_MAX_EVENT_SIZE];
    unsigned long Length;
} HCI_BRIDGE_EVENT_SLOT;

typedef struct _HCI_BRIDGE_ACL_SLOT {
    unsigned char Data[HCI_BRIDGE_MAX_ACL_SIZE];
    unsigned long Length;
} HCI_BRIDGE_ACL_SLOT;

typedef struct _HCI_BRIDGE_SCO_SLOT {
    unsigned char Data[HCI_BRIDGE_MAX_SCO_SIZE];
    unsigned long Length;
} HCI_BRIDGE_SCO_SLOT;

/* Per-handle credit tracking entry */
typedef struct _HCI_BRIDGE_HANDLE_ENTRY {
    unsigned short Handle;      /* 12-bit connection handle */
    unsigned short Outstanding; /* ACL packets sent to controller awaiting completion */
    unsigned char  InUse;
} HCI_BRIDGE_HANDLE_ENTRY;

/* Comprehensive diagnostic counters - the sole diagnostic window on this hardware */
typedef struct _HCI_BRIDGE_COUNTERS {
    unsigned long CommandsSubmitted;      /* Total host commands submitted via SubmitCommand */
    unsigned long CommandsHeld;           /* Commands held while not ready (gating) */
    /*
     * A second command arriving while a command is already held. The hold slot is depth 1
     * because BTHUSB is stop-and-wait on EP0 and never pipelines commands. If non-zero,
     * a host command was overwritten before replay.
     */
    unsigned long CommandsHeldOverwritten;
    unsigned long CommandsReplayed;       /* Held commands replayed upon becoming ready */
    unsigned long CommandsSentToWire;     /* Commands forwarded to wire */
    unsigned long CommandsFailedWire;     /* Commands wire refused */
    unsigned long CommandsDropped;        /* Commands dropped (malformed) */

    unsigned long AclSubmitted;           /* Total host ACL packets submitted via SubmitAcl */
    unsigned long AclSentToWire;          /* ACL packets forwarded to wire */
    unsigned long AclOutDroppedNotReady;  /* Outbound ACL dropped because bridge not ready */
    unsigned long AclDroppedNoCredit;     /* ACL dropped due to credit exhaustion (CreditStalls) */
    unsigned long AclDroppedWireFailed;   /* ACL dropped because wire refused */
    unsigned long AclDroppedNoHandleSlot; /* ACL dropped because handle table full (8 active handles) */

    unsigned long ScoSubmitted;           /* Total host SCO packets submitted via SubmitSco */
    unsigned long ScoSentToWire;          /* SCO packets forwarded to wire */
    unsigned long ScoOutboundOverwrites;  /* Outbound SCO dropped due to FIFO full (overwrite-oldest) */
    unsigned long ScoInboundOverwrites;   /* Inbound SCO dropped due to FIFO full (overwrite-oldest) */
    unsigned long ScoOutDroppedNotReady;  /* Outbound SCO dropped because bridge not ready */

    unsigned long EventsReceived;         /* Inbound events received from wire */
    unsigned long EventsQueued;           /* Inbound events queued for host */
    unsigned long EventsSuppressedVendor; /* Vendor events (0xFF) suppressed */
    unsigned long EventsDroppedNotReady;  /* Events dropped because bridge not ready */
    unsigned long EventsDroppedFifoFull;  /* Events dropped because event FIFO full */

    unsigned long AclReceived;            /* Inbound ACL packets received from wire */
    unsigned long AclQueued;              /* Inbound ACL packets queued for host */
    unsigned long AclInDroppedNotReady;   /* Inbound ACL dropped because bridge not ready */
    unsigned long AclDroppedFifoFull;     /* Inbound ACL dropped because inbound FIFO full */

    unsigned long ScoReceived;            /* Inbound SCO packets received from wire */
    unsigned long ScoQueued;              /* Inbound SCO packets queued for host */
    unsigned long ScoDroppedNotReady;     /* Inbound SCO dropped because bridge not ready */
    unsigned long ScoInboundDiscardedOversize; /* Inbound SCO discarded because capacity too small */
    unsigned long Resets;                 /* Number of Reset calls */
} HCI_BRIDGE_COUNTERS;

/*
 * Injected wire operations.
 * The wire layer below (SerCx2 UART driver in production, or mock chip in self-test)
 * provides these function pointers.
 * Packets carry no H4 prefix (wire adds H4 byte 0x01/0x02/0x03 if writing to UART).
 * Return 1 on success, 0 on failure/busy.
 */
typedef struct _HCI_BRIDGE_WIRE_OPS {
    unsigned char (*SendCommand)(void *Context, const unsigned char *Packet, unsigned long Length);
    unsigned char (*SendAcl)(void *Context, const unsigned char *Packet, unsigned long Length);
    unsigned char (*SendSco)(void *Context, const unsigned char *Packet, unsigned long Length);
} HCI_BRIDGE_WIRE_OPS;

typedef struct _HCI_BRIDGE_WIRE {
    const HCI_BRIDGE_WIRE_OPS *Ops;
    void                      *Context;
} HCI_BRIDGE_WIRE;

typedef struct _HCI_BRIDGE {
    /* Wire interface beneath the bridge */
    HCI_BRIDGE_WIRE       Wire;

    /* Upstream transport reference */
    HCI_TRANSPORT        *Transport;

    /* Readiness state */
    unsigned char         Ready;

    /* Held command buffer for readiness gating (hold-then-replay) */
    unsigned char         HeldCommand[HCI_BRIDGE_MAX_COMMAND_SIZE];
    unsigned long         HeldCommandLength;
    unsigned char         HeldCommandPending;

    /* Inbound Event FIFO */
    HCI_BRIDGE_EVENT_SLOT EventFifo[HCI_BRIDGE_EVENT_FIFO_DEPTH];
    unsigned int          EventHead;
    unsigned int          EventTail;
    unsigned int          EventCount;

    /* Inbound ACL FIFO */
    HCI_BRIDGE_ACL_SLOT   AclInFifo[HCI_BRIDGE_ACL_IN_FIFO_DEPTH];
    unsigned int          AclInHead;
    unsigned int          AclInTail;
    unsigned int          AclInCount;

    /* Inbound SCO FIFO (overwrite-oldest) */
    HCI_BRIDGE_SCO_SLOT   ScoInFifo[HCI_BRIDGE_SCO_FIFO_DEPTH];
    unsigned int          ScoInHead;
    unsigned int          ScoInTail;
    unsigned int          ScoInCount;

    /* Outbound SCO FIFO (overwrite-oldest) */
    HCI_BRIDGE_SCO_SLOT   OutboundScoFifo[HCI_BRIDGE_SCO_OUT_FIFO_DEPTH];
    unsigned int          OutboundScoHead;
    unsigned int          OutboundScoTail;
    unsigned int          OutboundScoCount;

    /* ACL Credit Accounting */
    unsigned short        TotalAclBuffers;
    unsigned short        AvailableAclCredits;
    HCI_BRIDGE_HANDLE_ENTRY Handles[HCI_BRIDGE_MAX_HANDLES];

    /* Diagnostic counters */
    HCI_BRIDGE_COUNTERS   Counters;
} HCI_BRIDGE;

/* API */

/*
 * Initialises the bridge with the supplied wire interface.
 * Sets Ready to 0 and zeroes all queues, state, and counters.
 */
void HciBridgeInit(
    _Out_ HCI_BRIDGE *Bridge,
    _In_ const HCI_BRIDGE_WIRE *Wire);

/*
 * Sets the bridge readiness state.
 * When transition from 0 -> 1 occurs, any held command is replayed to the wire immediately.
 */
void HciBridgeSetReady(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ unsigned char Ready);

/*
 * Binds the bridge to the front-end HCI_TRANSPORT struct, setting Ops and Context.
 * Mirrors how the stub backend is bound.
 */
void HciBridgeBindTransport(
    _Inout_ HCI_TRANSPORT *Transport,
    _Inout_ HCI_BRIDGE *Bridge);

/*
 * Inbound feed functions called by the wire layer when a complete packet is received.
 * Packets carry no H4 prefix (wire stripped H4 byte).
 *
 * LOCKING: MUST be called with controller spin lock HELD.
 * RETURN:  1 (TRUE) if packet was queued to the matching stream;
 *          0 (FALSE) if packet was suppressed (e.g. vendor event 0xFF), dropped, or unhandled.
 *
 * The caller MUST release the spin lock before calling HciTransportNotify(Transport, Stream)
 * when 1 is returned.
 */
unsigned char HciBridgeOnEvent(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length);

unsigned char HciBridgeOnAcl(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length);

unsigned char HciBridgeOnSco(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length);

/* Generic stream dispatcher: routes to HciBridgeOnEvent/Acl/Sco by Stream index */
unsigned char HciBridgeOnInbound(
    _Inout_ HCI_BRIDGE *Bridge,
    _In_ HCI_STREAM Stream,
    _In_reads_bytes_(Length) const unsigned char *Packet,
    _In_ unsigned long Length);

/* Attempts to drain any queued outbound SCO packets to the wire */
void HciBridgeDrainOutboundSco(
    _Inout_ HCI_BRIDGE *Bridge);
