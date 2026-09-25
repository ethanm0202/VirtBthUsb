/*
 * qca_uart.h - Qualcomm QCA2066 SerCx2 UART backend for DeckBtUsb.
 *
 * Implements the physical SerCx2 UART connection to the Qualcomm QCA2066 Bluetooth
 * controller over ACPI\QCOM2066, providing resource extraction, line configuration,
 * firmware bring-up state machine coordination, an asynchronous read pump feeding
 * the H4 decoder, and non-blocking asynchronous writes beneath the steady-state HCI bridge.
 *
 * Locking and notification rules: HciTransportNotify is never called while holding the
 * controller spin lock. Inbound packets are queued to the bridge under the lock, and
 * HciTransportNotify is invoked after the lock is released.
 *
 * Upstream reference: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137
 * and hci_qca.c (same commit ID) via src/include/qca_protocol.h.
 */

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdf.h>
#include <ntddser.h>
#else
#include <windows.h>
#endif

#include "hci_transport.h"
#include "hci_bridge.h"
#include "h4_codec.h"
#include "qca_protocol.h"
#include "qca_init_fsm.h"
#include "qca_identify.h"
#include "sco_route.h"

/* ---------------------------------------------------------------- Step breadcrumbs
 * Registry breadcrumbs written during bring-up to HKLM\...\Services\DeckBtUsb\Parameters.
 * Range 60..89 is exclusively allocated to QCA UART to prevent collisions.
 */
#define DECKBT_STEP_QCA_PREPARE_HW      60u
#define DECKBT_STEP_QCA_FIND_RES        61u
#define DECKBT_STEP_QCA_TARGET_CREATE   62u
#define DECKBT_STEP_QCA_TARGET_OPEN     63u
#define DECKBT_STEP_QCA_LINE_CONFIG     64u
#define DECKBT_STEP_QCA_BAUD_SET        65u
#define DECKBT_STEP_QCA_FLOW_SET        66u
#define DECKBT_STEP_QCA_FW_LOAD         67u
#define DECKBT_STEP_QCA_FSM_INIT        68u
#define DECKBT_STEP_QCA_READ_PUMP_START 69u
#define DECKBT_STEP_QCA_FSM_PUMP        70u
#define DECKBT_STEP_QCA_PATCH_PROGRESS  71u
#define DECKBT_STEP_QCA_NVM_PROGRESS    72u
#define DECKBT_STEP_QCA_BAUD_SWITCH     73u
#define DECKBT_STEP_QCA_FSM_READY       74u
#define DECKBT_STEP_QCA_BRIDGE_INIT     75u
#define DECKBT_STEP_QCA_BRIDGE_READY    76u
#define DECKBT_STEP_QCA_START_DONE      77u
#define DECKBT_STEP_QCA_STOP            78u
#define DECKBT_STEP_QCA_FSM_FAILED      80u
#define DECKBT_STEP_QCA_TIMEOUT         81u
#define DECKBT_STEP_QCA_FW_NOT_FOUND    82u
#define DECKBT_STEP_QCA_IO_ERROR        83u
#define DECKBT_STEP_QCA_HCI_RESET_SENT  84u
#define DECKBT_STEP_QCA_HCI_RESET_DONE  85u
#define DECKBT_STEP_QCA_PROBE_DONE      86u
#define DECKBT_STEP_QCA_BOARD_ID_REQ    87u
#define DECKBT_STEP_QCA_BOARD_ID_DONE   88u
#define DECKBT_STEP_QCA_NVM_FALLBACK    89u

#define DECKBT_STEP_QCA_IDENTIFY_ENTER    90u
#define DECKBT_STEP_QCA_IDENTIFY_RUNG     91u
#define DECKBT_STEP_QCA_IDENTIFY_REQ_SENT 92u
#define DECKBT_STEP_QCA_IDENTIFY_RESP_RCV 93u
#define DECKBT_STEP_QCA_IDENTIFY_DONE     94u
#define DECKBT_STEP_QCA_IDENTIFY_TIMEOUT  95u

/*
 * Controller wake contract: a sleeping controller holds its RTS (the host's CTS)
 * deasserted. With CTS handshake enabled, host transmits stall in the UART until CTS
 * asserts, and a stalled write will not retire after cancellation. Never transmit without CTS.
 */
#define DECKBT_STEP_QCA_CTS_CHECK         96u
#define DECKBT_STEP_QCA_CTS_WAKE          97u
#define DECKBT_STEP_QCA_CTS_BLOCKED       98u
/* Probe end: return the controller to ROM so the inbox or vendor driver can initialize it. */
#define DECKBT_STEP_QCA_HANDBACK          99u
/* Bring-up start: bring the controller to ROM at 115200 from whatever state it was left in. */
#define DECKBT_STEP_QCA_ENSURE_ROM        100u
/* Steady state: host IBS wake, USB child publication, graceful stop. */
#define DECKBT_STEP_QCA_IBS_WAKE          101u
#define DECKBT_STEP_QCA_USB_PLUG          102u
#define DECKBT_STEP_QCA_SHUTDOWN          103u
#define DECKBT_STEP_QCA_STEADY_READS      104u

typedef enum _QCA_PROBE_MODE {
    QcaProbeModeFull = 0,
    QcaProbeModeIdentify = 1,
    /* Normal operation: full bring-up, then the Phase 2 bridge serves BTHUSB until stopped. */
    QcaProbeModeSteady = 2
} QCA_PROBE_MODE;

#ifndef HCI_MAX_EVENT_SIZE
#define HCI_MAX_EVENT_SIZE              257u
#endif

/* ---------------------------------------------------------------- QCA UART Record */
typedef enum _QCA_UART_COMPLETION {
    QcaUartRecordInProgress = 0,
    QcaUartRecordReleased = 1,   /* Final record; UART ownership has retired (or was never acquired). */
    QcaUartRecordAbandoned = 2   /* Final watchdog/cancel record; UART release is not confirmed. */
} QCA_UART_COMPLETION;

typedef struct _QCA_UART_RECORD {
    ULONG       ProbeRan;
    ULONG       Aborted;
    ULONG       ElapsedMs;
    ULONG       SerialOpened;
    ULONG       BaudInitial;
    ULONG       BaudFinal;
    ULONG       PatchBytesSent;
    ULONG       NvmBytesSent;
    ULONG       TlvSegmentsAcked;
    ULONG       HciResetSent;
    ULONG       HciResetStatus;
    ULONG       HciResetEventLen;
    WCHAR       HciResetEventHex[516];
    ULONG       LastStep;
    ULONG       LastStatus;
    WCHAR       FailurePhase[32];
    ULONG       SocId;
    ULONG       RomVersion;
    ULONG       BoardId;
    ULONG       BoardIdValid;
    WCHAR       NvmSelected[32];
    ULONG       NvmFallback;
    /* Identify-only additions */
    ULONG       IdentifyRan;
    ULONG       IdentifyBaud;
    ULONG       IdentifyAttempts;
    ULONG       ProductId;
    ULONG       PatchVersion;
    WCHAR       IdentifyRawHex[64];
    /* Line-state evidence (IOCTL_SERIAL_GET_MODEMSTATUS). Reads == 0 means never read. */
    ULONG       ModemStatusReads;
    ULONG       ModemStatusFirst;
    ULONG       ModemStatusLast;
    ULONG       WakePulses;
    ULONG       CtsAsserted;
    /*
     * Handback after firmware download (full probe): the rate at which the controller answered a
     * version request after the SoC reset. 115200 = back in ROM; 0 = no answer; 3000000 = the
     * reset did not take. HandbackStatus is the reset/verify status (not the probe verdict).
     */
    ULONG       HandbackBaud;
    ULONG       HandbackStatus;
    /* Bring-up entry state: rate of the first answer (0 = silent) and whether a SoC reset was needed. */
    ULONG       EntryBaud;
    ULONG       EntryReset;
    /*
     * Steady state. SteadyReached: the USB child was plugged in. UsbPlugStatus: its NTSTATUS
     * (STATUS_PENDING until attempted). Ibs*: host wake handshake and in-band sleep traffic.
     * Bridge* / *Errors: runtime counters, refreshed while steady (HCI_BRIDGE_COUNTERS, write slots,
     * reader), the only view of BTHUSB traffic on a machine without a kernel debugger.
     */
    ULONG       SteadyReached;
    ULONG       UsbPlugStatus;
    ULONG       IbsWakeTries;
    ULONG       IbsHostAwake;
    ULONG       IbsWakeIndRx;
    ULONG       IbsWakeAckTx;
    ULONG       IbsSleepIndRx;
    /* Acks not sent: CTS was deasserted (skipped; the controller repeats WAKE_IND) or the write failed. */
    ULONG       IbsAckCtsLow;
    ULONG       IbsAckFailures;
    ULONG       IbsAckLastStatus;
    ULONG       BridgeCommands;
    ULONG       BridgeCommandsFailed;
    ULONG       BridgeEventsReceived;
    ULONG       BridgeEventsQueued;
    ULONG       BridgeAclOut;
    ULONG       BridgeAclIn;
    /* Voice over the UART: packets sent, received, and lost to FIFO overwrite/not-ready/oversize. */
    ULONG       BridgeScoOut;
    ULONG       BridgeScoIn;
    ULONG       BridgeScoLost;
    /*
     * Opt-in SCO loopback self-test (SelfTestScoLoopback=1): HCI local loopback before BTHUSB exists.
     * Statuses are 0x100 + HCI status when the Command Complete arrived, 0 when it never did.
     */
    ULONG       ScoLoopRan;
    ULONG       ScoLoopEnterStatus;
    ULONG       ScoLoopConnections;     /* loopback Connection Complete events */
    ULONG       ScoLoopScoHandle;       /* handle of the SCO loopback link, 0xFFFF if none */
    ULONG       ScoLoopSent;
    ULONG       ScoLoopEchoed;
    ULONG       ScoLoopMatched;         /* echoes byte-identical to a packet sent */
    ULONG       ScoLoopLeaveStatus;
    ULONG       ScoLoopResetStatus;
    /* Voice setups rewritten onto the HCI data path, and answers given back their legacy opcode. */
    ULONG       ScoRouteRewritten;
    ULONG       ScoRouteRestored;
    ULONG       WriteErrors;
    ULONG       ReadErrors;
    /* Reader completions and bytes: bytes per completion shows whether reads return promptly. */
    ULONG       ReadCompletions;
    ULONG       ReadBytes;
    /* LE advertising report events (counted instead of traced in EventLog). */
    ULONG       AdvReports;
    /* Asynchronous HCI writes that completed, and the status of the last completion. */
    ULONG       WriteCompletions;
    ULONG       LastWriteStatus;
    /*
     * Line, sampled once a second while steady (IOCTL_SERIAL_GET_COMMSTATUS, GET_MODEMSTATUS):
     * accumulated SERIAL_ERROR_* bits, last SERIAL_TX_WAITING_* hold reasons (1 = waiting for CTS),
     * bytes still queued for transmit, and how many samples found CTS deasserted.
     */
    ULONG       LineErrors;
    ULONG       LineHoldReasons;
    ULONG       LineOutQueue;
    ULONG       CtsLowSamples;
    ULONG       Completion; /* UartCompletion is committed last; *Ran flags are not completion. */
} QCA_UART_RECORD, *PQCA_UART_RECORD;
/*
 * A serial byte stream has one ordering domain.  Keep exactly one read outstanding so callback
 * scheduling cannot reorder chunks before the stateful H4 decoder consumes them.
 */
#define QCA_UART_READ_REQUESTS          1u
#define QCA_UART_READ_BUF_SIZE          2048u

/*
 * Serial line policy. SERIAL_TIMEOUTS defines ReadIntervalTimeout = MAXULONG with both read
 * totals zero to complete immediately, even with zero bytes, which would spin a re-arming pump.
 * With totals zero and a finite interval, a read pends until the first byte and completes on an
 * inter-byte gap; the interval must exceed the system tick period. Serial line parameters use
 * 1500 ms write timeout, XonLimit=XoffLimit=0x800, and 5 x 5 ms wake pulses.
 */
#define QCA_UART_READ_INTERVAL_MS       20u
#define QCA_UART_WRITE_TIMEOUT_MS       1500u
#define QCA_UART_FLOW_LIMIT             0x800u
#define QCA_UART_WAKE_PULSES            5u
#define QCA_UART_WAKE_PULSE_MS          5u
/* Upstream hci_qca.c (qca_set_baudrate): QCA2066 requires 300 ms settle time before the host switch. */
#define QCA_UART_BAUD_SETTLE_MS         300u
/* 10 ms before the NVM download, as upstream qca_uart_setup waits. */
#define QCA_UART_NVM_SETTLE_MS          10u
/*
 * Controller reset sequence: IBS wake 0xFD three times, 10 ms settle time, SoC reset
 * (0xFC40 without parameters for product 0x13 / ACPI\VEN_QCOM&DEV_2066), then 200 ms
 * settle time before communication resumes at 115,200 baud.
 */
#define QCA_UART_IBS_WAKE_SETTLE_MS     10u
#define QCA_UART_SOC_RESET_SETTLE_MS    200u
/*
 * Steady state. Host wake: WAKE_IND retransmitted every 100 ms until WAKE_ACK (upstream
 * hci_qca.c IBS_WAKE_RETRANS_TIMEOUT_MS), bounded here to one second. Counters refresh once a second.
 * A graceful stop (handback to ROM, ~0.5 s) gets SHUTDOWN_WAIT_MS inside a PnP/power callback
 * before it is escalated to a hard cancel; the monitor allows SHUTDOWN_BUDGET_MS in total.
 */
#define QCA_UART_IBS_WAKE_RETRANS_MS    100u
#define QCA_UART_IBS_WAKE_TRIES         10u
#define QCA_UART_STEADY_PUBLISH_MS      1000u
/* A steady read with nothing to say ends after this (STATUS_TIMEOUT, 0 bytes) and is re-armed. */
#define QCA_UART_STEADY_READ_IDLE_MS    1000u
#define QCA_UART_SHUTDOWN_WAIT_MS       5000u
#define QCA_UART_SHUTDOWN_BUDGET_MS     15000u
#define QCA_UART_EVENT_TRACE_SLOTS      128u
#define QCA_UART_EVENT_TRACE_STRIDE     8u
/* Distinct LE advertisers heard in steady state (first come, first kept). */
#define QCA_UART_ADV_TABLE_SLOTS        48u
/* Synchronous Connection Complete/Changed (0x2C/0x2D) kept whole: 2-byte header + 17 parameters. */
#define QCA_UART_SCO_LINK_BYTES         20u
/* Opt-in SCO loopback self-test (QcaUartScoLoopback): packets sent and their payload size. */
#define QCA_UART_SCO_LOOP_PACKETS       20u
#define QCA_UART_SCO_LOOP_PAYLOAD       48u
#define QCA_UART_OP_WRITE_LOOPBACK      0x1802u
#define QCA_UART_OP_HCI_RESET           0x0C03u

/*
 * One advertiser: address and type as on the wire (address LSB first), the last report's event
 * type (legacy ADV_IND = 0, SCAN_RSP = 4; extended: low byte of the event-type bits), and how
 * many single-report advertising events carried it. 12 bytes, published verbatim as AdvSeen.
 */
typedef struct _QCA_ADV_SEEN {
    UCHAR Address[6];
    UCHAR AddressType;
    UCHAR EventType;
    ULONG Count;
} QCA_ADV_SEEN;

#define QCA_UART_WRITE_SLOTS            4u    /* Asynchronous outbound write slots */
#define QCA_UART_WRITE_BUF_SIZE         2048u /* Maximum framed H4 outbound packet */

/* ---------------------------------------------------------------- Asynchronous write slot */
typedef struct _QCA_UART_WRITE_SLOT {
    WDFREQUEST    Request;
    WDFMEMORY     Memory;
    UCHAR         Buffer[QCA_UART_WRITE_BUF_SIZE];
    BOOLEAN       InUse;
    BOOLEAN       HasBeenSent;
    PVOID         Owner;
} QCA_UART_WRITE_SLOT, *PQCA_UART_WRITE_SLOT;

/* ---------------------------------------------------------------- Main backend struct */
typedef struct _QCA_UART {
    WDFDEVICE            WdfDevice;
    WDFDEVICE            IoOwner;           /* Private non-PnP control device, worker-owned */
    WDFIOTARGET          IoTarget;          /* SerCx2 UART target */
    BOOLEAN              IoTargetOpened;
    WDFIOTARGET          GpioTarget;        /* Optional GPIO target, NULL if not exposed */
    BOOLEAN              GpioExposed;       /* TRUE if GPIO resource found in ACPI */

    /*
     * The front end's controller spin lock, adopted in QcaUartBindTransport - not created or
     * owned by this backend. `Bridge` must have exactly one lock domain: the front end mutates
     * it from SubmitCommand/SubmitAcl/SubmitSco/HasStream/PopStream/Reset under this lock, and
     * the read-completion DPC mutates it from HciBridgeOn{Event,Acl,Sco} under the same one.
     * NULL until bind; QcaUartRunProbe refuses to run without it.
     */
    WDFSPINLOCK          Lock;

    /* Upstream transport bound to this backend */
    HCI_TRANSPORT       *Transport;
    /* Private bridge-facing transport; public Transport is wrapped to add decoder reset. */
    HCI_TRANSPORT        BridgeTransport;

    /* Steady-state bridge */
    HCI_BRIDGE           Bridge;

    /* Inbound H4 receive stream decoder */
    H4_DECODER           Decoder;
    ULONG                PendingNotifyMask;

    /*
     * Lifecycle phase, published with InterlockedExchange:
     *   0 = Uninitialised
     *   1 = Phase 1: Bring-up FSM active (firmware download)
     *   2 = Phase 2: Steady-state HCI bridge active
     *   3 = Stopped / Error
     */
    volatile LONG        Phase;

    /* Bring-up state machine */
    QCA_INIT_FSM         Fsm;

    /* FSM event synchronization */
    KEVENT               FsmEvent;
    BOOLEAN              FsmWaitingForEvent;
    BOOLEAN              FsmEventMatched;

    /* Current operating baud rate */
    ULONG                CurrentBaudRate;

    /* Exactly one outstanding read preserves byte-stream order into Decoder. */
    WDFREQUEST           ReadRequests[QCA_UART_READ_REQUESTS];
    WDFMEMORY            ReadMemories[QCA_UART_READ_REQUESTS];
    UCHAR               *ReadBuffers[QCA_UART_READ_REQUESTS];
    BOOLEAN              ReadRequestSent[QCA_UART_READ_REQUESTS];
    volatile LONG        ReadPumpRunning;

    /* Pre-allocated outbound write slots for non-blocking sends at <= DISPATCH_LEVEL */
    QCA_UART_WRITE_SLOT  WriteSlots[QCA_UART_WRITE_SLOTS];
    WDFSPINLOCK          WriteLock;

    /* Synchronous write request and memory reserved for Phase 1 FSM bring-up */
    WDFREQUEST           FsmWriteRequest;
    WDFMEMORY            FsmWriteMemory;

    /* Observability and diagnostic counters */
    ULONG                TotalBytesRead;
    ULONG                TotalBytesWritten;
    ULONG                ReadCompletions;
    ULONG                WriteCompletions;
    ULONG                ReadErrors;
    ULONG                WriteErrors;
    ULONG                LastIoStatus;

    /* Probe state */
    PQCA_UART_RECORD     ProbeRecord;
    UCHAR                LastHciResetEvent[HCI_MAX_EVENT_SIZE];
    ULONG                LastHciResetEventLen;
    BOOLEAN              HciResetCaptured;
    ULONG                ProbeTlvSegmentsAcked;
    /* Detached probe ownership. Lifecycle callbacks only signal StopRequested. */
    volatile LONG        ProbeActive;
    volatile LONG        StopRequested;
    volatile LONG        ReportingStopped;
    /* TargetLock protects count/event publication; includes the terminal purge borrower. */
    volatile LONG        ActiveIoCount;
    ULONGLONG            ProbeStarted;
    KEVENT               ProbeDone;
    KEVENT               ProbeStop;
    KEVENT               MonitorDone;
    KEVENT               RecordGate;
    KEVENT               IoIdle;
    KSPIN_LOCK           TargetLock;
    WDFIOTARGET          CancelTarget; /* Claimed once under TargetLock; NULL before terminal purge */
    KSPIN_LOCK           RecordLock;
    QCA_UART_RECORD      Record;
    QCA_UART_RECORD      PublishedRecord;
    ULONG                SerialIdLow;
    ULONG                SerialIdHigh;
    BOOLEAN              SerialFound;
    QCA_PROBE_MODE       ProbeMode;
    QCA_IDENTIFY         Identify;
    volatile LONG        IdentifyReadStatus; /* First active read failure, never a silence result */
    UCHAR                LastIdentifyPacket[HCI_MAX_EVENT_SIZE + 1];
    /*
     * Steady state (QcaProbeModeSteady). IbsWakeIndRx/IbsSleepIndRx and IbsTxAwake change under
     * Lock in the read-completion path; a controller WAKE_IND sets IbsAckPending and IbsWorkEvent,
     * and only the steady worker sends the ack (and owns IbsWakeAckTx and IbsAck*).
     * BudgetUnbounded lifts the bring-up deadline while serving BTHUSB. SteadyReady: bridge ready
     * (monitor drops its bring-up watchdog). ShutdownEvent: graceful stop requested.
     * SteadyStopped: the controller was handed back and the UART released.
     */
    volatile LONG        IbsTxAwake;
    volatile LONG        IbsAckPending;
    ULONG                IbsWakeIndRx;
    ULONG                IbsWakeAckTx;
    ULONG                IbsSleepIndRx;
    ULONG                IbsAckCtsLow;
    ULONG                IbsAckFailures;
    ULONG                IbsAckLastStatus;
    ULONG                LastWriteStatus;
    volatile LONG        BudgetUnbounded;
    volatile LONG        ShutdownRequested;
    KEVENT               IbsAckEvent;
    KEVENT               IbsWorkEvent;
    KEVENT               SteadyReady;
    KEVENT               ShutdownEvent;
    KEVENT               SteadyStopped;
    ULONG                LastIdentifyPacketLen;
    /*
     * Inbound HCI event headers in steady state (event code, length, first six parameter bytes:
     * enough for a Command Complete/Status opcode and status, or an LE subevent), captured
     * before bridge filtering. Written under Lock by the read completion; the steady worker
     * copies it under Lock and publishes EventLog/EventCount beside the EP0 ControlLog.
     */
    UCHAR                EventTrace[QCA_UART_EVENT_TRACE_SLOTS][QCA_UART_EVENT_TRACE_STRIDE];
    UCHAR                EventTracePublished[QCA_UART_EVENT_TRACE_SLOTS][QCA_UART_EVENT_TRACE_STRIDE];
    ULONG                EventTraceCount;
    ULONG                AdvReports;
    /* Advertisers heard (same lock and publication path as EventTrace; published as AdvSeen). */
    QCA_ADV_SEEN         AdvSeen[QCA_UART_ADV_TABLE_SLOTS];
    QCA_ADV_SEEN         AdvSeenPublished[QCA_UART_ADV_TABLE_SLOTS];
    ULONG                AdvSeenCount;
    /* Latest synchronous link event, whole (link type, air mode, packet lengths); published ScoLinkEvent. */
    UCHAR                ScoLinkEvent[QCA_UART_SCO_LINK_BYTES];
    UCHAR                ScoLinkPublished[QCA_UART_SCO_LINK_BYTES];
    ULONG                ScoLinkEvents;
    /* Run the opt-in SCO loopback self-test once, before plugging in the USB child. */
    BOOLEAN              ScoLoopbackRequested;
    /* Legacy voice setups rewritten to Enhanced (HCI data path), under Lock (sco_route.h). */
    SCO_ROUTE            ScoRoute;
} QCA_UART, *PQCA_UART;

/* ---------------------------------------------------------------- Public API */
NTSTATUS QcaUartArmProbe(_Inout_ PQCA_UART Uart, _In_ WDFCMRESLIST ResourcesTranslated, _In_ QCA_PROBE_MODE Mode);
/*
 * Resume from system sleep: starts a new steady session on the serial connection the last
 * QcaUartArmProbe found, with a fresh bridge (the controller lost, or may have lost, its firmware
 * and state). PASSIVE_LEVEL; STATUS_DEVICE_BUSY while the previous worker is still retiring.
 */
NTSTATUS QcaUartRearmSteady(_Inout_ PQCA_UART Uart);
VOID QcaUartCancelProbe(_Inout_ PQCA_UART Uart);
VOID QcaUartPublishProbe(_Inout_ PQCA_UART Uart);
/*
 * Stop request from a PnP/power callback (D0Exit, ReleaseHardware), PASSIVE_LEVEL. A steady
 * session that is serving BTHUSB stops gracefully: the bridge closes, the controller is handed
 * back to ROM at 115,200 baud (so the inbox or vendor driver can own it again without a reboot)
 * and the reader retires. Waits at most QCA_UART_SHUTDOWN_WAIT_MS for that, never for the lower
 * stack, then escalates to QcaUartCancelProbe. Every other mode and bring-up in progress cancel at once.
 */
VOID QcaUartRequestStop(_Inout_ PQCA_UART Uart);

/*
 * Initialises the QCA UART backend context.
 * Sets initial phase to 0, zeroes counters and queues.
 */
VOID QcaUartInit(
    _Out_ PQCA_UART Uart,
    _In_ WDFDEVICE Device);

/*
 * Parses the translated resource list, locates the SerCx2 UART connection descriptor,
 * builds the Resource Hub device path (\Device\RESOURCE_HUB\%08x%08x), creates and
 * opens the WDFIOTARGET, and configures line parameters (115200 8N1 hardware flow control).
 *
 * Records whether a GPIO connection descriptor is exposed. Its polarity and required
 * chip-power sequence remain a platform prerequisite; this backend does not invent them.
 *
 * Runs at PASSIVE_LEVEL.
 */
NTSTATUS QcaUartPrepareHardware(_Inout_ PQCA_UART Uart);

/*
 * Worker-only cleanup; cancellation is nonwaiting, followed by bounded polling of completions.
 * If a lower driver ignores cancellation, ownership is retained until it finally completes.
 */
VOID QcaUartReleaseHardware(
    _Inout_ PQCA_UART Uart);

/*
 * Retires readiness and requests cancellation without waiting for the lower stack.
 * Runs at PASSIVE_LEVEL on the detached worker.
 */
VOID QcaUartStop(
    _Inout_ PQCA_UART Uart);

/*
 * Binds this backend to the upstream HCI_TRANSPORT and adopts the front end's lock domain.
 *
 * `ControllerLock` must be the same WDFSPINLOCK the front end holds around every HciTransport*
 * call. The bridge is constructed here (not-ready) and only then are its ops published, so the
 * front end can never reach uninitialised bridge state.
 *
 * Must be called before QcaUartArmProbe.
 */
VOID QcaUartBindTransport(
    _Inout_ HCI_TRANSPORT *Transport,
    _Inout_ PQCA_UART Uart,
    _In_ WDFSPINLOCK ControllerLock);

/*
 * Reprograms the SerCx2 UART to exactly `BaudRate`.  A rejection is fatal: once the controller
 * has accepted its 0xFC48 command, silently choosing a different host-only rate loses the wire.
 */
NTSTATUS QcaUartSetBaudRate(
    _Inout_ PQCA_UART Uart,
    _In_ ULONG BaudRate);

/*
 * Reports whether the controller is currently in Phase 2 (steady-state bridge active).
 */
BOOLEAN QcaUartIsReady(
    _In_ const QCA_UART *Uart);

/*
 * Probe Mode:
 * Runs the QCA bring-up sequence far enough to download the rampatch TLV and NVM TLV,
 * disables SoC logging, retrieves build info, and issues HCI_Reset (0x0C03), then stops: the
 * controller is handed back to ROM and the reader retired. In QcaProbeModeSteady a successful
 * run instead returns with the reader active at the operating rate and Phase 2 (bridge not ready)
 * for QcaUartServeSteady; a failed one is handed back like the probe.
 * Records all progress into ProbeRecord and Parameters registry key.
 * Runs at PASSIVE_LEVEL.
 */
NTSTATUS QcaUartRunProbe(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD ProbeRecord);

/*
 * Identify-Only Mode:
 * Probes baud rates using QCA_IDENTIFY ladder, emitting only the read-only version
 * request (01 00 FC 01 19) and downloading no firmware.
 * Records progress into Record and Parameters registry key.
 * Each baud transition cancels/drains the previous reader before reconfiguration/purge.
 * Failed/short writes and transport errors terminate the probe without another command.
 * STATUS_NOT_FOUND/NoResponse means only an exhausted, otherwise healthy baud ladder.
 * No response metadata may override a transport error or cancellation.
 * Runs at PASSIVE_LEVEL.
 */
NTSTATUS QcaUartRunIdentify(
    _Inout_ PQCA_UART Uart,
    _Inout_ PQCA_UART_RECORD Record);
