/*
 * uart_identify_selftest.c - Deterministic host fault harness for QCA UART identification.
 *
 * Exercises the routines extracted from src/driver/qca_uart.c in user mode
 * with mock WDF and kernel boundaries:
 *   - QcaUartRunIdentify
 *   - QcaUartWriteSynchronous
 *   - QcaUartReadCompletion
 *   - QcaUartStartReadPump
 *   - QcaUartSendTimed
 *   - QcaUartIoCompleted
 *   - QcaUartRequestBudget
 *   - QcaUartOnH4Packet
 *   - QcaUartQuiesceRead (when present)
 *
 * Validates:
 *   1. Clean identification with fragmented responses across multiple reads.
 *   2. True NoResponse contract (STATUS_NOT_FOUND) when healthy ladder is exhausted.
 *   3. Driver stops after failed or short write (no further TX into broken framing).
 *   4. Explicit baud rate and purge failures stop the ladder immediately.
 *   5. Read transport errors latch into IdentifyReadStatus and wake FsmEvent.
 *   6. Cancellation and budget exhaustion preserve Aborted=1 and real failure status.
 *   7. Quiesce/drain cancels previous reader and prevents request reuse while in flight.
 *   8. Drain timeout stops the ladder without changing baud or writing.
 *   9. FsmWaitingForEvent filters identify packet acceptance.
 *  10. The harness accepts another qca_uart.c (-Source); the mutation check uses this to confirm
 *      each scenario fails against the defect it guards.
 *
 * Build: tools\uart-identify-selftest.ps1
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Keep the extracted registry writer independent of the ntdll import library. */
static inline VOID MockRtlInitUnicodeString(PUNICODE_STRING Destination, PCWSTR Source) {
    Destination->Buffer = (PWSTR)Source;
    Destination->Length = Source ? (USHORT)(wcslen(Source) * sizeof(WCHAR)) : 0;
    Destination->MaximumLength = Source ? (USHORT)(Destination->Length + sizeof(WCHAR)) : 0;
}
#define RtlInitUnicodeString MockRtlInitUnicodeString

/* --- Mock WDF & NT types and constants for user-mode compilation --- */

typedef void *WDFOBJECT;
typedef void *WDFDEVICE;
typedef void *WDFIOTARGET;
typedef void *WDFREQUEST;
typedef void *WDFMEMORY;
typedef void *WDFSPINLOCK;
typedef void *WDFKEY;
typedef void *WDFCMRESLIST;
typedef void *WDFCONTEXT;
typedef void *WDFDRIVER;

#ifndef PASSIVE_LEVEL
#define PASSIVE_LEVEL 0
#endif

#ifndef KEY_SET_VALUE
#define KEY_SET_VALUE 0x0002
#endif

#ifndef WDF_NO_OBJECT_ATTRIBUTES
#define WDF_NO_OBJECT_ATTRIBUTES NULL
#endif

static inline ULONG KeGetCurrentIrql(VOID) {
    return PASSIVE_LEVEL;
}

typedef struct _MOCK_REG_ENTRY {
    WCHAR Name[64];
    BOOLEAN IsString;
    ULONG ULongValue;
    WCHAR StringValue[516];
} MOCK_REG_ENTRY;

#define MAX_MOCK_REG_ENTRIES 64
#define MAX_MOCK_REG_WRITES 128

typedef struct _MOCK_REG_WRITE_OP {
    WCHAR Name[64];
    BOOLEAN IsString;
    ULONG ULongValue;
    WCHAR StringValue[64];
    ULONG CurrentCompletionBeforeWrite;
} MOCK_REG_WRITE_OP;

typedef struct _MOCK_REGISTRY {
    MOCK_REG_ENTRY Entries[MAX_MOCK_REG_ENTRIES];
    ULONG EntryCount;
    MOCK_REG_WRITE_OP Writes[MAX_MOCK_REG_WRITES];
    ULONG WriteCount;
    BOOLEAN FailOpenKey;
    BOOLEAN FailWrite;
    WCHAR FailValueName[64];
    NTSTATUS FailureStatus;
} MOCK_REGISTRY;
typedef enum _WDF_IO_TARGET_PURGE_IO_FLAGS {
    WdfIoTargetPurgeIoUndefined = 0,
    WdfIoTargetPurgeIo = 1,
    WdfIoTargetPurgeIoAndWait = 2
} WDF_IO_TARGET_PURGE_IO_FLAGS;

typedef struct _KEVENT {
    LONG State;
} KEVENT, *PKEVENT;

typedef ULONG_PTR KSPIN_LOCK, *PKSPIN_LOCK;
typedef ULONG_PTR KIRQL, *PKIRQL;

#define IO_NO_INCREMENT 0
#define WaitAny 1
#define Executive 0
#define KernelMode 0
#define PAGED_CODE() ((void)0)
#define NonPagedPoolNx 0
#define QCA_UART_POOL_TAG 'aUcq'

#define WDF_REL_TIMEOUT_IN_MS(ms) ((LONGLONG)-(LONGLONG)(ms) * 10000LL)

typedef struct _WDF_REQUEST_SEND_OPTIONS {
    ULONG Size;
    ULONG Flags;
    LONGLONG Timeout;
} WDF_REQUEST_SEND_OPTIONS, *PWDF_REQUEST_SEND_OPTIONS;

#define WDF_REQUEST_SEND_OPTION_SYNCHRONOUS 0x00000001
#define WDF_REQUEST_SEND_OPTION_TIMEOUT     0x00000002

static inline void WDF_REQUEST_SEND_OPTIONS_INIT(PWDF_REQUEST_SEND_OPTIONS Options, ULONG Flags) {
    Options->Size = sizeof(WDF_REQUEST_SEND_OPTIONS);
    Options->Flags = Flags;
    Options->Timeout = 0;
}

static inline void WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(PWDF_REQUEST_SEND_OPTIONS Options, LONGLONG Timeout) {
    Options->Timeout = Timeout;
    Options->Flags |= WDF_REQUEST_SEND_OPTION_TIMEOUT;
}

typedef struct _WDF_MEMORY_DESCRIPTOR {
    ULONG Type;
    PVOID Buffer;
    ULONG BufferLength;
} WDF_MEMORY_DESCRIPTOR, *PWDF_MEMORY_DESCRIPTOR;

static inline void WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(PWDF_MEMORY_DESCRIPTOR Descriptor, PVOID Buffer, ULONG BufferLength) {
    Descriptor->Type = 1;
    Descriptor->Buffer = Buffer;
    Descriptor->BufferLength = BufferLength;
}

typedef struct _WDF_REQUEST_REUSE_PARAMS {
    ULONG Size;
    ULONG Flags;
    NTSTATUS Status;
} WDF_REQUEST_REUSE_PARAMS, *PWDF_REQUEST_REUSE_PARAMS;

#define WDF_REQUEST_REUSE_NO_FLAGS 0

static inline void WDF_REQUEST_REUSE_PARAMS_INIT(PWDF_REQUEST_REUSE_PARAMS Params, ULONG Flags, NTSTATUS Status) {
    Params->Size = sizeof(WDF_REQUEST_REUSE_PARAMS);
    Params->Flags = Flags;
    Params->Status = Status;
}

typedef struct _WDF_REQUEST_COMPLETION_PARAMS {
    ULONG Size;
    IO_STATUS_BLOCK IoStatus;
} WDF_REQUEST_COMPLETION_PARAMS, *PWDF_REQUEST_COMPLETION_PARAMS;

typedef struct _WDF_OBJECT_ATTRIBUTES {
    ULONG Size;
    WDFOBJECT ParentObject;
} WDF_OBJECT_ATTRIBUTES, *PWDF_OBJECT_ATTRIBUTES;

static inline void WDF_OBJECT_ATTRIBUTES_INIT(PWDF_OBJECT_ATTRIBUTES Attributes) {
    Attributes->Size = sizeof(WDF_OBJECT_ATTRIBUTES);
    Attributes->ParentObject = NULL;
}

typedef VOID (*PFN_WDF_REQUEST_COMPLETION_ROUTINE)(
    WDFREQUEST Request,
    WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params,
    WDFCONTEXT Context);

/* Use the SDK's serial contract rather than reproducing its IOCTL definitions. */
#include <devioctl.h>
#include <ntddser.h>

/* NTSTATUS definitions */
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_NOT_FOUND
#define STATUS_NOT_FOUND                ((NTSTATUS)0xC0000225L)
#endif
#ifndef STATUS_CANCELLED
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120L)
#endif
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT                  ((NTSTATUS)0x00000102L)
#endif
#ifndef STATUS_DATA_ERROR
#define STATUS_DATA_ERROR               ((NTSTATUS)0xC000003CL)
#endif
#ifndef STATUS_INVALID_DEVICE_STATE
#define STATUS_INVALID_DEVICE_STATE     ((NTSTATUS)0xC0000184L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_IO_DEVICE_ERROR
#define STATUS_IO_DEVICE_ERROR          ((NTSTATUS)0xC0000185L)
#endif
#ifndef STATUS_IO_TIMEOUT
#define STATUS_IO_TIMEOUT               ((NTSTATUS)0xC00000B5L)
#endif
#ifndef STATUS_DEVICE_HARDWARE_ERROR
#define STATUS_DEVICE_HARDWARE_ERROR    ((NTSTATUS)0xC0000483L)
#endif
#ifndef STATUS_WAIT_0
#define STATUS_WAIT_0                   ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_WAIT_1
#define STATUS_WAIT_1                   ((NTSTATUS)0x00000001L)
#endif
#ifndef STATUS_WAIT_2
#define STATUS_WAIT_2                   ((NTSTATUS)0x00000002L)
#endif
#ifndef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY         ((NTSTATUS)0xC00000A3L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#endif
#ifndef STATUS_INSUFFICIENT_RESOURCES
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#endif
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022L)

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef MAXULONG
#define MAXULONG 0xFFFFFFFFul   /* ntdef.h; absent from the user-mode headers */
#endif
#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif

static inline HRESULT RtlStringCchCopyW(WCHAR *pszDest, size_t cchDest, const WCHAR *pszSrc) {
    if (!pszDest || cchDest == 0) return (HRESULT)0x80070057L;
    wcsncpy_s(pszDest, cchDest, pszSrc, _TRUNCATE);
    return 0;
}

/* Production headers */
#include "hci_transport.h"
#include "hci_bridge.h"
#include "h4_codec.h"
#include "qca_protocol.h"
#include "qca_init_fsm.h"
#include "qca_identify.h"
#include "qca_uart.h"

/* --- Mock State Machine & Tracking --- */

typedef struct _MOCK_REQUEST {
    BOOLEAN InFlight;
    BOOLEAN Cancelled;
    PFN_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine;
    WDFCONTEXT Context;
    UCHAR *Buffer;
    ULONG BufferSize;
} MOCK_REQUEST;

typedef struct _MOCK_UART_STATE {
    /* Fault injection switches */
    BOOLEAN FailBaudRate;
    NTSTATUS BaudRateStatus;
    ULONG BaudFailureRate; /* 0 = fail all rates, nonzero = fail specific rate */

    BOOLEAN FailPurge;
    NTSTATUS PurgeStatus;

    BOOLEAN FailWrite;
    NTSTATUS WriteStatus;
    ULONG WriteBytesToReport;

    BOOLEAN FailReadCompletion;
    NTSTATUS ReadCompletionStatus;
    BOOLEAN ReadErrorOnWait;
    BOOLEAN ReadErrorOnLock;
    BOOLEAN StopOnWait;
    BOOLEAN ReplyDuringWrite;
    BOOLEAN FailReadAfterReply;

    BOOLEAN DelayedReadCancel;
    BOOLEAN ReadCancelPending;

    BOOLEAN IgnoreReadCancel; /* Simulates cancellation ignored by lower stack (drain timeout) */

    /*
     * Controller line state. CTS is asserted unless CtsLow; a CtsLow controller asserts CTS
     * after CtsAssertAfterPulses completed RTS low->high pulses (0 = never). RTS IOCTLs are
     * rejected unless manual RTS control is configured, as with a handshaking serial port.
     */
    BOOLEAN CtsLow;
    ULONG CtsAssertAfterPulses;
    BOOLEAN FailModemStatus;
    NTSTATUS ModemStatusFailure;
    ULONG ModemStatusQueries;
    ULONG RtsPulsesCompleted;
    BOOLEAN RtsLowPending;
    BOOLEAN RtsAsserted;
    ULONG LastControlHandShake;
    ULONG LastFlowReplace;
    BOOLEAN WriteWhileCtsLow;
    ULONGLONG LastBaudSetTime;
    ULONGLONG LastPurgeTime;

    /* Scripted controller responses (can be split across multiple chunks) */
    #define MAX_SCRIPTED_RESPONSES 16
    struct {
        UCHAR Data[256];
        ULONG Length;
        ULONG BaudRate;
    } Responses[MAX_SCRIPTED_RESPONSES];
    ULONG ResponseCount;
    ULONG ResponseIndex;

    /* Trace & Observability */
    ULONG TotalWritesAttempted;
    ULONG TotalWritesCompleted;
    ULONG TotalBytesWritten;
    UCHAR LastWrittenData[64];
    ULONG LastWrittenLength;
    UCHAR WriteLog[16][8];          /* first 8 bytes of each write, in order */
    ULONG WriteLogLength[16];
    ULONG WriteLogBaud[16];         /* host rate in effect for that write */
    ULONG WriteLogCount;
    BOOLEAN SilentUntilReset;       /* controller asleep in IBS: nothing answers before 01 40 FC 00 */
    BOOLEAN ResetSeen;
    ULONG LineRate;                 /* rate the host line actually runs at (last accepted set) */

    ULONG BaudRatesSet[16];
    ULONG BaudRatesSetCount;

    ULONG PurgesExecuted;
    ULONG PurgeMasks[16];
    ULONG PurgeCount;

    /* WDF request contract verification */
    MOCK_REQUEST ReadRequest;
    BOOLEAN RequestReuseViolation;
    BOOLEAN ConfigurationWithPendingRead;

    /* Time simulation */
    ULONGLONG SimulatedTime;

    /* Step recording */
    struct {
        ULONG Step;
        NTSTATUS Status;
    } RecordedSteps[64];
    ULONG RecordedStepCount;

    /* --- Native lifetime & concurrent fault regression state --- */
    PQCA_UART ActiveUart;
    volatile LONG TargetLockHeld;
    volatile DWORD TargetLockOwnerThread;

    /* Scenario 1: Interleaving and callback lifetime tracking */
    volatile LONG CallbackContextActive;
    BOOLEAN TargetClosedDuringCallback;
    BOOLEAN TargetDeletedDuringCallback;
    HANDLE HCallbackStarted;
    HANDLE HCallbackCanFinish;

    /* Scenario 2 & 3: Purge / Borrower / ReleaseHardware */
    volatile LONG PurgeInProgress;
    ULONG TotalPurgesAttempted;
    BOOLEAN TargetClosedDuringPurge;
    BOOLEAN TargetDeletedDuringPurge;
    BOOLEAN PurgeOnClosedTarget;
    BOOLEAN PurgeOnDeletedTarget;
    BOOLEAN ConcurrentPurgeViolation;
    HANDLE HPurgeStarted;
    HANDLE HPurgeRelease;

    /* Object lifetime tracking */
    BOOLEAN IoTargetClosed;
    ULONG IoTargetCloseCount;
    BOOLEAN IoTargetDeleted;
    ULONG IoTargetDeleteCount;
    LONG IoTargetRefCount;

    /* Scenario 4: SendTimed failure */
    BOOLEAN FailSendTimed;
    NTSTATUS SendTimedFailureStatus;

    /* Multi-threaded wait support */
    BOOLEAN MultiThreadedTestActive;

    /* Registry publication tracking (Scenarios 21-23) */
    MOCK_REGISTRY Reg;

    /*
     * Steady state. ShutdownAfterWaits raises the graceful-stop event on that many serving waits.
     * Publish* models DeckBtPublishUartDevice.
     */
    ULONG LastReadSendFlags;
    ULONG ShutdownAfterWaits;
    SERIAL_TIMEOUTS LastTimeouts;   /* last IOCTL_SERIAL_SET_TIMEOUTS */
    ULONG TimeoutsSet;
    UCHAR PublishedEventLog[QCA_UART_EVENT_TRACE_SLOTS * QCA_UART_EVENT_TRACE_STRIDE];
    ULONG PublishedEventCount;
    QCA_ADV_SEEN PublishedAdvSeen[QCA_UART_ADV_TABLE_SLOTS];
    ULONG PublishedAdvSeenCount;
    ULONG PublishCalls;
    BOOLEAN PublishSawReady;
    NTSTATUS PublishStatus;
    ULONG NotifyEvents;
} MOCK_UART_STATE;

static MOCK_UART_STATE g_Mock;
static WDFDRIVER g_DeckBtDriver = (WDFDRIVER)1;
static UCHAR g_ReadBuffer[QCA_UART_READ_BUF_SIZE];

static void CompleteRead(NTSTATUS Status, ULONG Bytes) {
    WDF_REQUEST_COMPLETION_PARAMS params;
    if (!g_Mock.ReadRequest.InFlight) return;
    params.Size = sizeof(params);
    params.IoStatus.Status = Status;
    params.IoStatus.Information = Bytes;
    g_Mock.ReadRequest.InFlight = FALSE;
    g_Mock.ReadRequest.CompletionRoutine(
        (WDFREQUEST)&g_Mock.ReadRequest, NULL, &params, g_Mock.ReadRequest.Context);
}

static BOOLEAN MockCtsAsserted(void) {
    if (!g_Mock.CtsLow) return TRUE;
    return g_Mock.CtsAssertAfterPulses != 0 &&
           g_Mock.RtsPulsesCompleted >= g_Mock.CtsAssertAfterPulses;
}

/* --- Mock Platform & WDF Functions --- */

VOID DeckBtRecordStep(ULONG Step, NTSTATUS Status) {
    if (g_Mock.RecordedStepCount < 64) {
        g_Mock.RecordedSteps[g_Mock.RecordedStepCount].Step = Step;
        g_Mock.RecordedSteps[g_Mock.RecordedStepCount].Status = Status;
        g_Mock.RecordedStepCount++;
    }
}

VOID QcaUartPublishProbe(PQCA_UART Uart) {
    (void)Uart;
}

ULONGLONG KeQueryInterruptTime(VOID) {
    return g_Mock.SimulatedTime;
}

NTSTATUS KeDelayExecutionThread(ULONG WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Interval) {
    (void)WaitMode;
    (void)Alertable;
    if (Interval && Interval->QuadPart < 0) {
        g_Mock.SimulatedTime += (ULONGLONG)(-Interval->QuadPart);
    }
    return STATUS_SUCCESS;
}

LONG KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait) {
    (void)Increment;
    (void)Wait;
    if (Event) {
        Event->State = 1;
    }
    return 0;
}

VOID KeClearEvent(PKEVENT Event) {
    if (Event) {
        Event->State = 0;
    }
}

NTSTATUS KeWaitForSingleObject(
    PVOID Object,
    ULONG WaitReason,
    ULONG WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout)
{
    PKEVENT ev = (PKEVENT)Object;
    (void)WaitReason;
    (void)WaitMode;
    (void)Alertable;

    if (g_Mock.DelayedReadCancel && g_Mock.ReadCancelPending) {
        g_Mock.ReadCancelPending = FALSE;
        if (g_Mock.ReadRequest.InFlight) {
            WDF_REQUEST_COMPLETION_PARAMS params;
            params.Size = sizeof(params);
            params.IoStatus.Status = STATUS_CANCELLED;
            params.IoStatus.Information = 0;
            g_Mock.ReadRequest.InFlight = FALSE;
            if (g_Mock.ReadRequest.CompletionRoutine) {
                g_Mock.ReadRequest.CompletionRoutine(
                    (WDFREQUEST)&g_Mock.ReadRequest, NULL, &params, g_Mock.ReadRequest.Context);
            }
        }
    }

    if (ev && ev->State != 0) {
        return STATUS_SUCCESS;
    }
    if (g_Mock.MultiThreadedTestActive) {
        DWORD waitMs = (Timeout && Timeout->QuadPart < 0) ? (DWORD)((-Timeout->QuadPart) / 10000) : 100;
        DWORD startTick = GetTickCount();
        if (waitMs > 100) waitMs = 100;
        while (ev && ev->State == 0) {
            if (GetTickCount() - startTick >= waitMs) {
                break;
            }
            Sleep(1);
        }
        if (ev && ev->State != 0) {
            return STATUS_SUCCESS;
        }
        return STATUS_TIMEOUT;
    }
    if (Timeout && Timeout->QuadPart < 0) {
        g_Mock.SimulatedTime += (ULONGLONG)-Timeout->QuadPart;
    }
    return STATUS_TIMEOUT;
}

LONG KeReadStateEvent(PKEVENT Event) {
    return Event ? Event->State : 0;
}

NTSTATUS KeWaitForMultipleObjects(
    ULONG Count,
    PVOID Object[],
    ULONG WaitType,
    ULONG WaitReason,
    ULONG WaitMode,
    BOOLEAN Alertable,
    PLARGE_INTEGER Timeout,
    PVOID WaitBlockArray)
{
    PKEVENT ev0 = (PKEVENT)Object[0]; /* FsmEvent */
    PKEVENT ev1 = (PKEVENT)Object[1]; /* ProbeStop */
    (void)Count;
    (void)WaitType;
    (void)WaitReason;
    (void)WaitMode;
    (void)Alertable;
    (void)WaitBlockArray;

    /* The operator's disable arrives after a number of steady serving waits. */
    if (g_Mock.ActiveUart != NULL && ev0 == &g_Mock.ActiveUart->ShutdownEvent &&
        g_Mock.ShutdownAfterWaits != 0 && --g_Mock.ShutdownAfterWaits == 0) {
        ev0->State = 1;
    }

    if (g_Mock.StopOnWait) {
        PQCA_UART uart = (PQCA_UART)g_Mock.ReadRequest.Context;
        g_Mock.StopOnWait = FALSE;
        uart->StopRequested = 1;
        ev1->State = 1;
    }
    if (g_Mock.ReadErrorOnWait) {
        g_Mock.ReadErrorOnWait = FALSE;
        CompleteRead(STATUS_DEVICE_HARDWARE_ERROR, 0);
    }

    if (ev1 && ev1->State != 0) {
        return STATUS_WAIT_1;
    }

    /* Deliver delayed cancellation if pending */
    if (g_Mock.DelayedReadCancel && g_Mock.ReadCancelPending) {
        g_Mock.ReadCancelPending = FALSE;
        if (g_Mock.ReadRequest.InFlight) {
            WDF_REQUEST_COMPLETION_PARAMS params;
            params.Size = sizeof(params);
            params.IoStatus.Status = STATUS_CANCELLED;
            params.IoStatus.Information = 0;
            g_Mock.ReadRequest.InFlight = FALSE;
            if (g_Mock.ReadRequest.CompletionRoutine) {
                g_Mock.ReadRequest.CompletionRoutine(
                    (WDFREQUEST)&g_Mock.ReadRequest, NULL, &params, g_Mock.ReadRequest.Context);
            }
        }
    }

    /* Deliver queued controller responses for current rate */
    if (g_Mock.BaudRatesSetCount > 0 && !(g_Mock.SilentUntilReset && !g_Mock.ResetSeen)) {
        ULONG currentRate = g_Mock.LineRate;
        while (g_Mock.ResponseIndex < g_Mock.ResponseCount &&
               g_Mock.Responses[g_Mock.ResponseIndex].BaudRate == currentRate) {
            ULONG len = g_Mock.Responses[g_Mock.ResponseIndex].Length;
            if (len > sizeof(g_ReadBuffer)) len = sizeof(g_ReadBuffer);
            memcpy(g_ReadBuffer, g_Mock.Responses[g_Mock.ResponseIndex].Data, len);
            g_Mock.ResponseIndex++;

            if (g_Mock.ReadRequest.InFlight) {
                WDF_REQUEST_COMPLETION_PARAMS params;
                params.Size = sizeof(params);
                params.IoStatus.Status = STATUS_SUCCESS;
                params.IoStatus.Information = len;
                g_Mock.ReadRequest.InFlight = FALSE;
                if (g_Mock.ReadRequest.CompletionRoutine) {
                    g_Mock.ReadRequest.CompletionRoutine(
                        (WDFREQUEST)&g_Mock.ReadRequest, NULL, &params, g_Mock.ReadRequest.Context);
                }
            }
            if (ev0 && ev0->State != 0) break;
        }
    }

    if (ev0 && ev0->State != 0) {
        return STATUS_WAIT_0;
    }
    if (ev1 && ev1->State != 0) {
        return STATUS_WAIT_1;
    }
    if (Count > 2 && Object[2] != NULL && ((PKEVENT)Object[2])->State != 0) {
        return STATUS_WAIT_2;
    }

    /* Advance simulated time by timeout */
    if (Timeout) {
        LONGLONG ms = (-Timeout->QuadPart) / 10000LL;
        if (ms > 0) {
            g_Mock.SimulatedTime += (ULONGLONG)ms * 10000ULL;
        }
    }

    return STATUS_TIMEOUT;
}

NTSTATUS WdfRequestCreate(
    PWDF_OBJECT_ATTRIBUTES RequestAttributes,
    WDFIOTARGET IoTarget,
    WDFREQUEST *Request)
{
    (void)RequestAttributes;
    (void)IoTarget;
    *Request = (WDFREQUEST)&g_Mock.ReadRequest;
    g_Mock.ReadRequest.InFlight = FALSE;
    g_Mock.ReadRequest.Cancelled = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS WdfMemoryCreate(
    PWDF_OBJECT_ATTRIBUTES MemoryAttributes,
    ULONG PoolType,
    ULONG Tag,
    size_t BufferSize,
    WDFMEMORY *Memory,
    PVOID *Buffer)
{
    (void)MemoryAttributes;
    (void)PoolType;
    (void)Tag;
    (void)BufferSize;
    *Memory = (WDFMEMORY)g_ReadBuffer;
    *Buffer = (PVOID)g_ReadBuffer;
    g_Mock.ReadRequest.Buffer = g_ReadBuffer;
    g_Mock.ReadRequest.BufferSize = sizeof(g_ReadBuffer);
    return STATUS_SUCCESS;
}

VOID WdfObjectReference(WDFOBJECT Handle) {
    (void)Handle;
    g_Mock.IoTargetRefCount++;
}

NTSTATUS WdfIoTargetFormatRequestForRead(
    WDFIOTARGET IoTarget,
    WDFREQUEST Request,
    WDFMEMORY OutputMemory,
    PVOID OutputMemoryOffset,
    PLONGLONG DeviceOffset)
{
    (void)IoTarget;
    (void)OutputMemory;
    (void)OutputMemoryOffset;
    (void)DeviceOffset;
    if (Request == (WDFREQUEST)&g_Mock.ReadRequest) {
        if (g_Mock.ReadRequest.InFlight) {
            g_Mock.RequestReuseViolation = TRUE;
        }
    }
    return STATUS_SUCCESS;
}

VOID WdfRequestSetCompletionRoutine(
    WDFREQUEST Request,
    PFN_WDF_REQUEST_COMPLETION_ROUTINE CompletionRoutine,
    WDFCONTEXT CompletionContext)
{
    if (Request == (WDFREQUEST)&g_Mock.ReadRequest) {
        g_Mock.ReadRequest.CompletionRoutine = CompletionRoutine;
        g_Mock.ReadRequest.Context = CompletionContext;
    }
}

BOOLEAN WdfRequestSend(
    WDFREQUEST Request,
    WDFIOTARGET Target,
    PWDF_REQUEST_SEND_OPTIONS Options)
{
    (void)Target;
    if (Request == (WDFREQUEST)&g_Mock.ReadRequest && Options != NULL) {
        g_Mock.LastReadSendFlags = Options->Flags;
    }
    if (g_Mock.FailSendTimed) {
        return FALSE;
    }
    if (Request == (WDFREQUEST)&g_Mock.ReadRequest) {
        if (g_Mock.ReadRequest.InFlight) {
            g_Mock.RequestReuseViolation = TRUE;
        }
        g_Mock.ReadRequest.InFlight = TRUE;
        g_Mock.ReadRequest.Cancelled = FALSE;

        if (g_Mock.FailReadAfterReply && g_Mock.ResponseIndex != 0) {
            g_Mock.FailReadAfterReply = FALSE;
            CompleteRead(STATUS_DEVICE_HARDWARE_ERROR, 0);
            return TRUE;
        }
        if (g_Mock.FailReadCompletion) {
            g_Mock.FailReadCompletion = FALSE;
            WDF_REQUEST_COMPLETION_PARAMS params;
            params.Size = sizeof(params);
            params.IoStatus.Status = g_Mock.ReadCompletionStatus;
            params.IoStatus.Information = 0;
            g_Mock.ReadRequest.InFlight = FALSE;
            if (g_Mock.ReadRequest.CompletionRoutine) {
                g_Mock.ReadRequest.CompletionRoutine(
                    Request, Target, &params, g_Mock.ReadRequest.Context);
            }
            return TRUE;
        }
        return TRUE;
    }
    return TRUE;
}

NTSTATUS WdfRequestReuse(
    WDFREQUEST Request,
    PWDF_REQUEST_REUSE_PARAMS ReuseParams)
{
    (void)ReuseParams;
    if (Request == (WDFREQUEST)&g_Mock.ReadRequest) {
        if (g_Mock.ReadRequest.InFlight) {
            g_Mock.RequestReuseViolation = TRUE;
        }
        g_Mock.ReadRequest.InFlight = FALSE;
    }
    return STATUS_SUCCESS;
}

BOOLEAN WdfRequestCancelSentRequest(WDFREQUEST Request) {
    if (Request == (WDFREQUEST)&g_Mock.ReadRequest) {
        if (!g_Mock.ReadRequest.InFlight) {
            return FALSE;
        }
        g_Mock.ReadRequest.Cancelled = TRUE;

        if (g_Mock.IgnoreReadCancel) {
            /* Simulates cancellation ignored by lower stack: reader never completes */
            return FALSE;
        }

        if (g_Mock.DelayedReadCancel) {
            /* Delayed cancellation: reader completes during wait slice */
            g_Mock.ReadCancelPending = TRUE;
            return TRUE;
        } else {
            /* Inline cancellation: completes immediately with STATUS_CANCELLED */
            WDF_REQUEST_COMPLETION_PARAMS params;
            params.Size = sizeof(params);
            params.IoStatus.Status = STATUS_CANCELLED;
            params.IoStatus.Information = 0;
            g_Mock.ReadRequest.InFlight = FALSE;
            if (g_Mock.ReadRequest.CompletionRoutine) {
                g_Mock.ReadRequest.CompletionRoutine(
                    Request, NULL, &params, g_Mock.ReadRequest.Context);
            }
            return TRUE;
        }
    }
    return TRUE;
}

NTSTATUS WdfRequestGetStatus(WDFREQUEST Request) {
    (void)Request;
    if (g_Mock.FailSendTimed) {
        return g_Mock.SendTimedFailureStatus ? g_Mock.SendTimedFailureStatus : STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS WdfIoTargetSendWriteSynchronously(
    WDFIOTARGET IoTarget,
    WDFREQUEST Request,
    PWDF_MEMORY_DESCRIPTOR InputBuffer,
    PVOID InputBufferOffset,
    PWDF_REQUEST_SEND_OPTIONS RequestOptions,
    PULONG_PTR BytesWritten)
{
    (void)IoTarget;
    (void)Request;
    (void)InputBufferOffset;
    (void)RequestOptions;
    g_Mock.TotalWritesAttempted++;
    if (!MockCtsAsserted()) {
        /* Real hardware stalls here with CTS handshake; the driver must never get this far. */
        g_Mock.WriteWhileCtsLow = TRUE;
        if (BytesWritten) *BytesWritten = 0;
        return STATUS_IO_TIMEOUT;
    }

    if (InputBuffer && InputBuffer->Buffer && InputBuffer->BufferLength > 0) {
        ULONG copyLen = InputBuffer->BufferLength;
        if (copyLen > sizeof(g_Mock.LastWrittenData)) copyLen = sizeof(g_Mock.LastWrittenData);
        memcpy(g_Mock.LastWrittenData, InputBuffer->Buffer, copyLen);
        g_Mock.LastWrittenLength = copyLen;
        if (g_Mock.WriteLogCount < 16) {
            ULONG n = copyLen < 8 ? copyLen : 8;
            memcpy(g_Mock.WriteLog[g_Mock.WriteLogCount], InputBuffer->Buffer, n);
            g_Mock.WriteLogLength[g_Mock.WriteLogCount] = InputBuffer->BufferLength;
            g_Mock.WriteLogBaud[g_Mock.WriteLogCount] = g_Mock.ActiveUart ? g_Mock.ActiveUart->CurrentBaudRate : 0;
            g_Mock.WriteLogCount++;
        }
        if (copyLen == 4 && memcmp(InputBuffer->Buffer, "\x01\x40\xFC\x00", 4) == 0) {
            g_Mock.ResetSeen = TRUE;
        }
    }

    if (g_Mock.FailWrite) {
        if (g_Mock.ReplyDuringWrite && g_Mock.ResponseCount != 0) {
            memcpy(g_ReadBuffer, g_Mock.Responses[0].Data, g_Mock.Responses[0].Length);
            CompleteRead(STATUS_SUCCESS, g_Mock.Responses[0].Length);
        }
        if (BytesWritten) *BytesWritten = g_Mock.WriteBytesToReport;
        return g_Mock.WriteStatus;
    }

    if (BytesWritten) {
        *BytesWritten = InputBuffer ? InputBuffer->BufferLength : 0;
    }
    g_Mock.TotalWritesCompleted++;
    g_Mock.TotalBytesWritten += (ULONG)(BytesWritten ? *BytesWritten : 0);
    return STATUS_SUCCESS;
}

NTSTATUS QcaUartSendIoctlSynchronously(
    PQCA_UART Uart,
    ULONG IoctlCode,
    PVOID InBuffer,
    ULONG InBufferSize,
    PVOID OutBuffer,
    ULONG OutBufferSize)
{
    (void)Uart;

    if (IoctlCode == IOCTL_SERIAL_GET_MODEMSTATUS) {
        g_Mock.ModemStatusQueries++;
        if (g_Mock.FailModemStatus) {
            return g_Mock.ModemStatusFailure ? g_Mock.ModemStatusFailure : STATUS_IO_DEVICE_ERROR;
        }
        if (OutBuffer == NULL || OutBufferSize < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)OutBuffer = SERIAL_DSR_STATE | (MockCtsAsserted() ? SERIAL_CTS_STATE : 0);
        return STATUS_SUCCESS;
    }
    if (IoctlCode == IOCTL_SERIAL_SET_HANDFLOW) {
        const SERIAL_HANDFLOW *hf = (const SERIAL_HANDFLOW *)InBuffer;
        if (hf == NULL || InBufferSize < sizeof(*hf)) return (NTSTATUS)STATUS_INVALID_PARAMETER;
        if (g_Mock.ReadRequest.InFlight) g_Mock.ConfigurationWithPendingRead = TRUE;
        g_Mock.LastControlHandShake = hf->ControlHandShake;
        g_Mock.LastFlowReplace = hf->FlowReplace;
        return STATUS_SUCCESS;
    }
    if (IoctlCode == IOCTL_SERIAL_CLR_RTS || IoctlCode == IOCTL_SERIAL_SET_RTS) {
        if ((g_Mock.LastFlowReplace & SERIAL_RTS_MASK) != SERIAL_RTS_CONTROL) {
            return (NTSTATUS)STATUS_INVALID_PARAMETER;
        }
        if (g_Mock.ReadRequest.InFlight) g_Mock.ConfigurationWithPendingRead = TRUE;
        if (IoctlCode == IOCTL_SERIAL_CLR_RTS) {
            g_Mock.RtsAsserted = FALSE;
            g_Mock.RtsLowPending = TRUE;
        } else {
            g_Mock.RtsAsserted = TRUE;
            if (g_Mock.RtsLowPending) {
                g_Mock.RtsLowPending = FALSE;
                g_Mock.RtsPulsesCompleted++;
            }
        }
        return STATUS_SUCCESS;
    }

    if (IoctlCode == IOCTL_SERIAL_SET_TIMEOUTS) {
        if (InBuffer == NULL || InBufferSize < sizeof(SERIAL_TIMEOUTS)) return (NTSTATUS)STATUS_INVALID_PARAMETER;
        if (g_Mock.ReadRequest.InFlight) g_Mock.ConfigurationWithPendingRead = TRUE;
        g_Mock.LastTimeouts = *(const SERIAL_TIMEOUTS *)InBuffer;
        g_Mock.TimeoutsSet++;
        return STATUS_SUCCESS;
    }
    if (IoctlCode == IOCTL_SERIAL_PURGE) {
        g_Mock.PurgesExecuted++;
        if (g_Mock.ReadRequest.InFlight) g_Mock.ConfigurationWithPendingRead = TRUE;
        if (InBuffer && InBufferSize >= sizeof(ULONG)) {
            if (g_Mock.PurgeCount < 16) {
                g_Mock.PurgeMasks[g_Mock.PurgeCount++] = *(ULONG *)InBuffer;
                g_Mock.LastPurgeTime = g_Mock.SimulatedTime;
            }
        }
        if (g_Mock.FailPurge) {
            return g_Mock.PurgeStatus ? g_Mock.PurgeStatus : STATUS_IO_DEVICE_ERROR;
        }
        if (InBuffer && (*(ULONG *)InBuffer & SERIAL_PURGE_RXABORT) != 0) {
            (void)WdfRequestCancelSentRequest((WDFREQUEST)&g_Mock.ReadRequest);
        }
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

NTSTATUS QcaUartSetBaudRate(
    PQCA_UART Uart,
    ULONG BaudRate)
{
    if (Uart->IoTarget == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (g_Mock.ReadRequest.InFlight) g_Mock.ConfigurationWithPendingRead = TRUE;
    if (g_Mock.BaudRatesSetCount < 16) {
        g_Mock.BaudRatesSet[g_Mock.BaudRatesSetCount++] = BaudRate;
        g_Mock.LastBaudSetTime = g_Mock.SimulatedTime;
    }
    if (g_Mock.FailBaudRate) {
        if (g_Mock.BaudFailureRate == 0 || g_Mock.BaudFailureRate == BaudRate) {
            /* Like the driver: a refused rate leaves the line (and CurrentBaudRate) unchanged. */
            return g_Mock.BaudRateStatus ? g_Mock.BaudRateStatus : STATUS_UNSUCCESSFUL;
        }
    }
    Uart->CurrentBaudRate = BaudRate;
    g_Mock.LineRate = BaudRate;
    /* The driver's SetBaudRate finishes by restoring CTS/RTS handshake. */
    g_Mock.LastControlHandShake = SERIAL_CTS_HANDSHAKE;
    g_Mock.LastFlowReplace = SERIAL_RTS_HANDSHAKE;
    return STATUS_SUCCESS;
}

VOID WdfSpinLockAcquire(WDFSPINLOCK SpinLock) {
    (void)SpinLock;
    if (g_Mock.ReadErrorOnLock && g_Mock.ReadRequest.InFlight) {
        g_Mock.ReadErrorOnLock = FALSE;
        CompleteRead(STATUS_DEVICE_HARDWARE_ERROR, 0);
    }
}

VOID WdfSpinLockRelease(WDFSPINLOCK SpinLock) {
    (void)SpinLock;
}

VOID KeAcquireSpinLock(PKSPIN_LOCK SpinLock, PKIRQL OldIrql) {
    if (OldIrql) *OldIrql = 0;
    while (InterlockedCompareExchange((volatile LONG *)SpinLock, 1, 0) != 0) {
        YieldProcessor();
    }
    if (g_Mock.ActiveUart != NULL && SpinLock == &g_Mock.ActiveUart->TargetLock) {
        InterlockedExchange(&g_Mock.TargetLockHeld, 1);
        g_Mock.TargetLockOwnerThread = GetCurrentThreadId();
    }
}

VOID KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql) {
    (void)NewIrql;
    if (g_Mock.ActiveUart != NULL && SpinLock == &g_Mock.ActiveUart->TargetLock) {
        g_Mock.TargetLockOwnerThread = 0;
        InterlockedExchange(&g_Mock.TargetLockHeld, 0);
    }
    InterlockedExchange((volatile LONG *)SpinLock, 0);
}

VOID WdfIoTargetPurge(WDFIOTARGET IoTarget, WDF_IO_TARGET_PURGE_IO_FLAGS PurgeFlags) {
    (void)PurgeFlags;
    if (IoTarget == NULL) {
        return;
    }
    if (g_Mock.IoTargetClosed) {
        g_Mock.PurgeOnClosedTarget = TRUE;
    }
    if (g_Mock.IoTargetDeleted) {
        g_Mock.PurgeOnDeletedTarget = TRUE;
    }
    if (InterlockedCompareExchange(&g_Mock.PurgeInProgress, 1, 0) != 0) {
        g_Mock.ConcurrentPurgeViolation = TRUE;
    }
    g_Mock.TotalPurgesAttempted++;

    if (g_Mock.HPurgeStarted != NULL) {
        SetEvent(g_Mock.HPurgeStarted);
    }
    if (g_Mock.HPurgeRelease != NULL) {
        WaitForSingleObject(g_Mock.HPurgeRelease, 2000);
    }

    if (g_Mock.ReadRequest.InFlight) {
        WDF_REQUEST_COMPLETION_PARAMS params;
        params.Size = sizeof(params);
        params.IoStatus.Status = STATUS_CANCELLED;
        params.IoStatus.Information = 0;
        g_Mock.ReadRequest.InFlight = FALSE;
        if (g_Mock.ReadRequest.CompletionRoutine) {
            g_Mock.ReadRequest.CompletionRoutine(
                (WDFREQUEST)&g_Mock.ReadRequest, NULL, &params, g_Mock.ReadRequest.Context);
        }
    }

    InterlockedExchange(&g_Mock.PurgeInProgress, 0);
}

VOID WdfIoTargetClose(WDFIOTARGET IoTarget) {
    (void)IoTarget;
    if (g_Mock.PurgeInProgress) {
        g_Mock.TargetClosedDuringPurge = TRUE;
    }
    if (g_Mock.CallbackContextActive) {
        g_Mock.TargetClosedDuringCallback = TRUE;
    }
    g_Mock.IoTargetCloseCount++;
    g_Mock.IoTargetClosed = TRUE;
}

VOID WdfObjectDelete(WDFOBJECT Handle) {
    if (Handle == (g_Mock.ActiveUart ? g_Mock.ActiveUart->IoTarget : (WDFIOTARGET)0x1)) {
        if (g_Mock.PurgeInProgress) {
            g_Mock.TargetDeletedDuringPurge = TRUE;
        }
        if (g_Mock.CallbackContextActive) {
            g_Mock.TargetDeletedDuringCallback = TRUE;
        }
        g_Mock.IoTargetDeleteCount++;
        g_Mock.IoTargetDeleted = TRUE;
    }
}

VOID WdfObjectDereference(WDFOBJECT Handle) {
    (void)Handle;
    g_Mock.IoTargetRefCount--;
}

/* --- Mock Registry APIs for DeckBtRecordProbeProgress --- */

static WDFKEY g_MockParametersKey = (WDFKEY)0x10;

NTSTATUS WdfDriverOpenParametersRegistryKey(
    WDFDRIVER Driver,
    ACCESS_MASK DesiredAccess,
    PWDF_OBJECT_ATTRIBUTES KeyAttributes,
    WDFKEY *Key)
{
    (void)Driver;
    (void)DesiredAccess;
    (void)KeyAttributes;
    if (g_Mock.Reg.FailOpenKey) {
        return STATUS_UNSUCCESSFUL;
    }
    *Key = g_MockParametersKey;
    return STATUS_SUCCESS;
}

VOID WdfRegistryClose(WDFKEY Key) {
    (void)Key;
}

static ULONG MockRegistryGetULong(const WCHAR *Name, ULONG DefaultValue) {
    ULONG i;
    for (i = 0; i < g_Mock.Reg.EntryCount; i++) {
        if (wcscmp(g_Mock.Reg.Entries[i].Name, Name) == 0) {
            return g_Mock.Reg.Entries[i].ULongValue;
        }
    }
    return DefaultValue;
}

static BOOLEAN MockRegistryGetString(const WCHAR *Name, WCHAR *Out, size_t MaxChars) {
    ULONG i;
    for (i = 0; i < g_Mock.Reg.EntryCount; i++) {
        if (wcscmp(g_Mock.Reg.Entries[i].Name, Name) == 0) {
            wcsncpy_s(Out, MaxChars, g_Mock.Reg.Entries[i].StringValue, _TRUNCATE);
            return TRUE;
        }
    }
    return FALSE;
}

NTSTATUS WdfRegistryAssignULong(
    WDFKEY Key,
    PCUNICODE_STRING ValueName,
    ULONG Value)
{
    ULONG i;
    ULONG curComp;
    (void)Key;
    if (!ValueName || !ValueName->Buffer) return STATUS_INVALID_PARAMETER;

    if (g_Mock.Reg.FailWrite) {
        if (g_Mock.Reg.FailValueName[0] == L'\0' || wcscmp(ValueName->Buffer, g_Mock.Reg.FailValueName) == 0) {
            return g_Mock.Reg.FailureStatus ? g_Mock.Reg.FailureStatus : STATUS_UNSUCCESSFUL;
        }
    }

    curComp = MockRegistryGetULong(L"UartCompletion", 0);
    if (g_Mock.Reg.WriteCount < MAX_MOCK_REG_WRITES) {
        wcsncpy_s(g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].Name, 64, ValueName->Buffer, _TRUNCATE);
        g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].IsString = FALSE;
        g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].ULongValue = Value;
        g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].CurrentCompletionBeforeWrite = curComp;
        g_Mock.Reg.WriteCount++;
    }

    for (i = 0; i < g_Mock.Reg.EntryCount; i++) {
        if (wcscmp(g_Mock.Reg.Entries[i].Name, ValueName->Buffer) == 0) {
            g_Mock.Reg.Entries[i].IsString = FALSE;
            g_Mock.Reg.Entries[i].ULongValue = Value;
            return STATUS_SUCCESS;
        }
    }
    if (g_Mock.Reg.EntryCount < MAX_MOCK_REG_ENTRIES) {
        wcsncpy_s(g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].Name, 64, ValueName->Buffer, _TRUNCATE);
        g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].IsString = FALSE;
        g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].ULongValue = Value;
        g_Mock.Reg.EntryCount++;
    }
    return STATUS_SUCCESS;
}

NTSTATUS WdfRegistryAssignUnicodeString(
    WDFKEY Key,
    PCUNICODE_STRING ValueName,
    PCUNICODE_STRING Value)
{
    ULONG i;
    ULONG curComp;
    (void)Key;
    if (!ValueName || !ValueName->Buffer) return STATUS_INVALID_PARAMETER;

    if (g_Mock.Reg.FailWrite) {
        if (g_Mock.Reg.FailValueName[0] == L'\0' || wcscmp(ValueName->Buffer, g_Mock.Reg.FailValueName) == 0) {
            return g_Mock.Reg.FailureStatus ? g_Mock.Reg.FailureStatus : STATUS_UNSUCCESSFUL;
        }
    }

    curComp = MockRegistryGetULong(L"UartCompletion", 0);
    if (g_Mock.Reg.WriteCount < MAX_MOCK_REG_WRITES) {
        wcsncpy_s(g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].Name, 64, ValueName->Buffer, _TRUNCATE);
        g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].IsString = TRUE;
        if (Value && Value->Buffer) {
            wcsncpy_s(g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].StringValue, 64, Value->Buffer, _TRUNCATE);
        } else {
            g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].StringValue[0] = L'\0';
        }
        g_Mock.Reg.Writes[g_Mock.Reg.WriteCount].CurrentCompletionBeforeWrite = curComp;
        g_Mock.Reg.WriteCount++;
    }

    for (i = 0; i < g_Mock.Reg.EntryCount; i++) {
        if (wcscmp(g_Mock.Reg.Entries[i].Name, ValueName->Buffer) == 0) {
            g_Mock.Reg.Entries[i].IsString = TRUE;
            if (Value && Value->Buffer) {
                wcsncpy_s(g_Mock.Reg.Entries[i].StringValue, 516, Value->Buffer, _TRUNCATE);
            } else {
                g_Mock.Reg.Entries[i].StringValue[0] = L'\0';
            }
            return STATUS_SUCCESS;
        }
    }
    if (g_Mock.Reg.EntryCount < MAX_MOCK_REG_ENTRIES) {
        wcsncpy_s(g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].Name, 64, ValueName->Buffer, _TRUNCATE);
        g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].IsString = TRUE;
        if (Value && Value->Buffer) {
            wcsncpy_s(g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].StringValue, 516, Value->Buffer, _TRUNCATE);
        } else {
            g_Mock.Reg.Entries[g_Mock.Reg.EntryCount].StringValue[0] = L'\0';
        }
        g_Mock.Reg.EntryCount++;
    }
    return STATUS_SUCCESS;
}

/* Registry publication of the steady traces: keeps the last published copy of each. */
VOID DeckBtRecordTrace(PCWSTR LogName, PCWSTR CountName, const UCHAR *Data, ULONG Bytes, ULONG Count) {
    (void)CountName;
    if (wcscmp(LogName, L"EventLog") == 0) {
        if (Bytes > sizeof(g_Mock.PublishedEventLog)) Bytes = sizeof(g_Mock.PublishedEventLog);
        memcpy(g_Mock.PublishedEventLog, Data, Bytes);
        g_Mock.PublishedEventCount = Count;
    } else if (wcscmp(LogName, L"AdvSeen") == 0) {
        if (Bytes > sizeof(g_Mock.PublishedAdvSeen)) Bytes = sizeof(g_Mock.PublishedAdvSeen);
        memcpy(g_Mock.PublishedAdvSeen, Data, Bytes);
        g_Mock.PublishedAdvSeenCount = Count;
    }
}

/* Front end's late plug-in: records whether the bridge was answering when it was asked. */
NTSTATUS DeckBtPublishUartDevice(PQCA_UART Uart) {
    g_Mock.PublishCalls++;
    g_Mock.PublishSawReady = Uart->Bridge.Ready != 0 && Uart->Phase == 2;
    return g_Mock.PublishStatus;
}

VOID DeckBtRecordProbeProgress(_In_ const QCA_UART_RECORD *Record);

/* --- Include extracted driver functions from generated build output --- */
#include "qca_uart_extracted.c"

/* --- Test Harness Helpers --- */

static void ResetMockState(void) {
    memset(&g_Mock, 0, sizeof(g_Mock));
    g_Mock.SimulatedTime = 1000000ULL; /* Start at 100ms */
}

static void InitMockUart(PQCA_UART Uart, PQCA_UART_RECORD Record) {
    memset(Uart, 0, sizeof(*Uart));
    memset(Record, 0, sizeof(*Record));

    Uart->IoTarget = (WDFIOTARGET)0x1;
    Uart->IoTargetOpened = TRUE;
    Uart->CancelTarget = Uart->IoTarget;
    Uart->Lock = (WDFSPINLOCK)0x2;
    Uart->WriteLock = (WDFSPINLOCK)0x4;
    Uart->Transport = (HCI_TRANSPORT *)0x3;
    Uart->ProbeRecord = Record;
    Uart->ProbeMode = QcaProbeModeIdentify;
    Uart->ProbeActive = 1;
    Uart->ProbeStarted = g_Mock.SimulatedTime;

    g_Mock.ActiveUart = Uart;

    QcaIdentifyInit(&Uart->Identify);
    H4DecoderInit(&Uart->Decoder);
}

/* Builds valid 17-byte EDL version event matching QcaParseVersionEvent */
static ULONG BuildVersionEvent(UCHAR *Out, ULONG ProductId, USHORT PatchVer, USHORT RomVerField, ULONG SocId) {
    ULONG i;
    UCHAR payload[12];
    payload[0] = (UCHAR)(ProductId & 0xFFu);
    payload[1] = (UCHAR)((ProductId >> 8) & 0xFFu);
    payload[2] = (UCHAR)((ProductId >> 16) & 0xFFu);
    payload[3] = (UCHAR)((ProductId >> 24) & 0xFFu);
    payload[4] = (UCHAR)(PatchVer & 0xFFu);
    payload[5] = (UCHAR)((PatchVer >> 8) & 0xFFu);
    payload[6] = (UCHAR)(RomVerField & 0xFFu);
    payload[7] = (UCHAR)((RomVerField >> 8) & 0xFFu);
    payload[8] = (UCHAR)(SocId & 0xFFu);
    payload[9] = (UCHAR)((SocId >> 8) & 0xFFu);
    payload[10] = (UCHAR)((SocId >> 16) & 0xFFu);
    payload[11] = (UCHAR)((SocId >> 24) & 0xFFu);

    Out[0] = H4_PKT_EVENT; /* 0x04 */
    Out[1] = 0xFFu;        /* HCI_EV_VENDOR */
    Out[2] = 14u;          /* Parameter length */
    Out[3] = 0x00u;        /* EDL_CMD_REQ_RES_EVT */
    Out[4] = 0x19u;        /* EDL_PATCH_VER_RES_EVT */
    for (i = 0; i < 12; i++) {
        Out[5 + i] = payload[i];
    }
    return 17u;
}

/* --- Test Cases --- */

/*
 * Test 1: Normal Identification at Later Baud with Fragmented Response (Scenario 9)
 * - 115200 is silent.
 * - 3000000 answers with valid EDL version packet split across 2 read chunks.
 * - Verifies H4 decoder reassembly across reads, correct IdentifyBaud, and zero reuse violations.
 */
static BOOLEAN TestValidFragmentedResponseLaterBaud(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    UCHAR verPacket[32];
    ULONG verLen;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    verLen = BuildVersionEvent(verPacket, 0x00000042, 0x0100, 0x0200, 0x40070200);

    /* Queue fragmented reply at 3,000,000 baud: chunk 1 (7 bytes), chunk 2 (10 bytes) */
    g_Mock.Responses[0].BaudRate = 3000000ul;
    g_Mock.Responses[0].Length = 7;
    memcpy(g_Mock.Responses[0].Data, verPacket, 7);

    g_Mock.Responses[1].BaudRate = 3000000ul;
    g_Mock.Responses[1].Length = verLen - 7;
    memcpy(g_Mock.Responses[1].Data, &verPacket[7], verLen - 7);

    g_Mock.ResponseCount = 2;

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_SUCCESS) return FALSE;
    if (record.IdentifyRan != 1) return FALSE;
    if (record.IdentifyBaud != 3000000ul) return FALSE;
    if (record.SocId != 0x40070200) return FALSE;
    if (record.RomVersion == 0) return FALSE;
    if (record.Aborted != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_SUCCESS) return FALSE;
    if (wcscmp(record.FailurePhase, L"Answered") != 0) return FALSE;
    if (g_Mock.RequestReuseViolation) return FALSE;
    if (g_Mock.ConfigurationWithPendingRead) return FALSE;

    return TRUE;
}

/*
 * Test 2: True NoResponse Across Exhausted Healthy Ladder (Scenario 8)
 * - All 3 rates silent without transport error.
 * - Must return STATUS_NOT_FOUND (0xC0000225), IdentifyBaud=0, Aborted=0, FailurePhase=NoResponse.
 */
static BOOLEAN TestTrueNoResponseReturnsNotFound(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_NOT_FOUND) return FALSE;
    if (record.IdentifyRan != 1) return FALSE;
    if (record.IdentifyBaud != 0) return FALSE;
    if (record.Aborted != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_NOT_FOUND) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") != 0) return FALSE;
    if (g_Mock.BaudRatesSetCount != 3) return FALSE;
    if (g_Mock.TotalWritesAttempted != 3) return FALSE;
    if (g_Mock.ConfigurationWithPendingRead) return FALSE;

    return TRUE;
}

/*
 * Test 3: Write Failure Stops Ladder with No Further TX (Scenario 3a)
 * - Rung 1 write fails with STATUS_IO_DEVICE_ERROR.
 * - Must NOT advance to 3000000 or 3200000; must not emit more writes.
 */
static BOOLEAN TestWriteFailureStopsLadder(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.FailWrite = TRUE;
    g_Mock.WriteStatus = STATUS_IO_DEVICE_ERROR;

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (g_Mock.TotalWritesAttempted != 1) return FALSE; /* No further TX */
    if (g_Mock.BaudRatesSetCount != 1) return FALSE;   /* No further baud */
    if (record.IdentifyBaud != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_IO_DEVICE_ERROR) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;

    return TRUE;
}

/*
 * Test 4: Partial / Short Write Stops Ladder (Scenario 3b)
 * - Write helper reports 2 bytes written instead of 5, producing STATUS_DATA_ERROR.
 * - Must stop ladder immediately and preserve STATUS_DATA_ERROR.
 */
static BOOLEAN TestPartialWriteStopsLadder(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.FailWrite = TRUE;
    g_Mock.WriteStatus = STATUS_SUCCESS;
    g_Mock.WriteBytesToReport = 2; /* 2 != 5 -> STATUS_DATA_ERROR */

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (g_Mock.TotalWritesAttempted != 1) return FALSE;
    if (g_Mock.BaudRatesSetCount != 1) return FALSE;
    if (record.IdentifyBaud != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_DATA_ERROR) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;

    return TRUE;
}

/*
 * Test 5: Explicit Baud Rate Failure Stops Ladder (Scenario 4a)
 * - Setting 115200 baud fails with STATUS_UNSUCCESSFUL.
 * - Must NOT attempt write; must not advance to 3000000.
 */
static BOOLEAN TestBaudFailureStopsLadder(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.FailBaudRate = TRUE;
    g_Mock.BaudRateStatus = STATUS_UNSUCCESSFUL;

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (g_Mock.TotalWritesAttempted != 0) return FALSE; /* No write if baud failed */
    if (g_Mock.BaudRatesSetCount != 1) return FALSE;   /* Stopped at rung 1 */
    if (record.IdentifyBaud != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_UNSUCCESSFUL) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;

    return TRUE;
}

/*
 * Test 6: Explicit Purge Failure Stops Ladder (Scenario 4b)
 * - Purge IOCTL fails with STATUS_IO_DEVICE_ERROR.
 * - Must NOT emit write; must stop ladder.
 */
static BOOLEAN TestPurgeFailureStopsLadder(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.FailPurge = TRUE;
    g_Mock.PurgeStatus = STATUS_IO_DEVICE_ERROR;

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (g_Mock.TotalWritesAttempted != 0) return FALSE; /* No write if purge failed */
    if (record.IdentifyBaud != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_IO_DEVICE_ERROR) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;

    return TRUE;
}

/*
 * Test 7: Read Transport Error While Running Wakes FsmEvent (Scenario 5)
 * - Reader completes with STATUS_DEVICE_HARDWARE_ERROR.
 * - Must latch into IdentifyReadStatus and wake FsmEvent so ladder does not hang 1.5s.
 */
static BOOLEAN TestReadErrorSignalsFsmEvent(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.FailReadCompletion = TRUE;
    g_Mock.ReadCompletionStatus = STATUS_DEVICE_HARDWARE_ERROR;

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (record.IdentifyBaud != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_DEVICE_HARDWARE_ERROR) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;

    return TRUE;
}

/*
 * Test 8: Cancellation Preserves Aborted and Error Status (Scenario 6)
 * - StopRequested set while waiting on rung 1.
 * - Must set Aborted=1, return STATUS_CANCELLED, not NoResponse.
 */
static BOOLEAN TestCancellationPreservesAbortedAndStatus(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.StopOnWait = TRUE;

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (record.Aborted != 1) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_CANCELLED) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;
    if (g_Mock.TotalWritesAttempted != 1 || g_Mock.BaudRatesSetCount != 1) return FALSE;

    return TRUE;
}

/*
 * Test 9: Budget Exhaustion Preserves Aborted and Status (Scenario 7)
 * - Simulated probe elapsed time exceeds 60s.
 * - Must set Aborted=1, return STATUS_CANCELLED, FailurePhase=BudgetExhausted.
 */
static BOOLEAN TestBudgetExhaustionPreservesAbortedAndStatus(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    /* Advance time past 60s budget */
    g_Mock.SimulatedTime = uart.ProbeStarted + (65000ULL * 10000ULL);

    status = QcaUartRunIdentify(&uart, &record);

    if (NT_SUCCESS(status)) return FALSE;
    if (record.Aborted != 1) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_CANCELLED) return FALSE;
    if (wcscmp(record.FailurePhase, L"NoResponse") == 0) return FALSE;

    return TRUE;
}

/*
 * Test 10: Delayed Read Cancellation Retires Cleanly Before Next Rung (Scenario 1 & 2)
 * - Rung 1 times out. Cancellation completes during drain wait.
 * - Rung 2 answers at 3000000.
 * - Request must NOT be reused while in flight (RequestReuseViolation == FALSE).
 */
static BOOLEAN TestDelayedCancellationDrainsCleanly(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    UCHAR verPacket[32];
    ULONG verLen;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    verLen = BuildVersionEvent(verPacket, 0x00000042, 0x0100, 0x0200, 0x40070200);

    /* Enable delayed cancellation simulation */
    g_Mock.DelayedReadCancel = TRUE;

    /* Script response only at 3000000 baud */
    g_Mock.Responses[0].BaudRate = 3000000ul;
    g_Mock.Responses[0].Length = verLen;
    memcpy(g_Mock.Responses[0].Data, verPacket, verLen);
    g_Mock.ResponseCount = 1;

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_SUCCESS) return FALSE;
    if (record.IdentifyBaud != 3000000ul) return FALSE;
    if (g_Mock.RequestReuseViolation) return FALSE;

    return TRUE;
}

/*
 * Test 11: Ignored Read Cancellation / Drain Timeout Stops Ladder
 * - Lower stack ignores cancellation: ActiveIoCount remains nonzero.
 * - Drain wait times out. Ladder must halt without changing to 3000000 or writing.
 */
static BOOLEAN TestDrainTimeoutStopsLadder(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.IgnoreReadCancel = TRUE;

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_IO_TIMEOUT || record.Aborted != 1) return FALSE;
    if (record.IdentifyBaud != 0) return FALSE;
    /* Must not have advanced past rung 1 */
    if (g_Mock.TotalWritesAttempted != 1 || g_Mock.BaudRatesSetCount != 1) return FALSE;
    if (g_Mock.SimulatedTime - uart.ProbeStarted > 5500ULL * 10000ULL) return FALSE;
    if (g_Mock.RequestReuseViolation) return FALSE;

    return TRUE;
}

/*
 * Test 12: FsmWaitingForEvent Filters Identify Packet Acceptance
 * - When FsmWaitingForEvent == FALSE, stray incoming packet must be discarded.
 * - When FsmWaitingForEvent == TRUE, packet is accepted and signals FsmEvent.
 */
static BOOLEAN TestFsmWaitingForEventFiltersPacket(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    UCHAR verPacket[32];
    ULONG verLen;

    ResetMockState();
    InitMockUart(&uart, &record);

    verLen = BuildVersionEvent(verPacket, 0x00000042, 0x0100, 0x0200, 0x40070200);

    /* Case A: FsmWaitingForEvent == FALSE (e.g. during drain / before write) */
    uart.FsmWaitingForEvent = FALSE;
    uart.Identify.Attempts = 1;
    KeClearEvent(&uart.FsmEvent);
    uart.FsmEventMatched = FALSE;
    QcaUartOnH4Packet(&uart, verPacket[0], &verPacket[1], verLen - 1);

    if (uart.FsmEvent.State != 0) return FALSE;
    if (uart.FsmEventMatched) return FALSE;
    if (uart.Identify.Answered) return FALSE;

    /* Case B: FsmWaitingForEvent == TRUE */
    uart.FsmWaitingForEvent = TRUE;
    uart.Identify.Attempts = 1; /* Match rung 1 */
    QcaUartOnH4Packet(&uart, verPacket[0], &verPacket[1], verLen - 1);

    if (uart.FsmEvent.State == 0) return FALSE;
    if (!uart.FsmEventMatched) return FALSE;
    if (!uart.Identify.Answered) return FALSE;

    return TRUE;
}


static BOOLEAN TestReadErrorDuringWait(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.ReadErrorOnWait = TRUE;
    if (QcaUartRunIdentify(&uart, &record) != STATUS_DEVICE_HARDWARE_ERROR) return FALSE;
    return record.IdentifyBaud == 0 && g_Mock.TotalWritesAttempted == 1 &&
        g_Mock.BaudRatesSetCount == 1 && g_Mock.SimulatedTime == uart.ProbeStarted;
}

static BOOLEAN TestReadErrorBeforeWriteDoesNotLoseWake(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.ReadErrorOnLock = TRUE;
    if (QcaUartRunIdentify(&uart, &record) != STATUS_DEVICE_HARDWARE_ERROR) return FALSE;
    return record.IdentifyBaud == 0 && g_Mock.TotalWritesAttempted == 0 &&
        g_Mock.BaudRatesSetCount == 1 && g_Mock.SimulatedTime == uart.ProbeStarted;
}

static BOOLEAN TestReplyCannotHideWriteError(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.Responses[0].Length = BuildVersionEvent(
        g_Mock.Responses[0].Data, 0x42, 0x0100, 0x0200, 0x40070200);
    g_Mock.ResponseCount = 1;
    g_Mock.ReplyDuringWrite = TRUE;
    g_Mock.FailWrite = TRUE;
    g_Mock.WriteStatus = STATUS_IO_DEVICE_ERROR;
    if (QcaUartRunIdentify(&uart, &record) != STATUS_IO_DEVICE_ERROR) return FALSE;
    return record.IdentifyBaud == 0 && record.IdentifyRawHex[0] == L'\0' &&
        g_Mock.TotalWritesAttempted == 1 && g_Mock.BaudRatesSetCount == 1;
}

static BOOLEAN TestReplyCannotHideReadError(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.Responses[0].Length = BuildVersionEvent(
        g_Mock.Responses[0].Data, 0x42, 0x0100, 0x0200, 0x40070200);
    g_Mock.Responses[0].BaudRate = 115200;
    g_Mock.ResponseCount = 1;
    g_Mock.FailReadAfterReply = TRUE;
    if (QcaUartRunIdentify(&uart, &record) != STATUS_DEVICE_HARDWARE_ERROR) return FALSE;
    return record.IdentifyBaud == 0 && record.IdentifyRawHex[0] == L'\0' &&
        g_Mock.TotalWritesAttempted == 1 && g_Mock.BaudRatesSetCount == 1;
}

/* --- Native Lifetime Regression Scenarios --- */

typedef struct _TEST_THREAD_CONTEXT {
    PQCA_UART Uart;
} TEST_THREAD_CONTEXT;

static DWORD WINAPI Test17CallbackThread(LPVOID Param) {
    TEST_THREAD_CONTEXT *ctx = (TEST_THREAD_CONTEXT *)Param;
    g_Mock.CallbackContextActive = 1;
    if (g_Mock.HCallbackStarted != NULL) {
        SetEvent(g_Mock.HCallbackStarted);
    }
    if (g_Mock.HCallbackCanFinish != NULL) {
        WaitForSingleObject(g_Mock.HCallbackCanFinish, 500);
    }
    QcaUartIoCompleted(ctx->Uart);
    g_Mock.CallbackContextActive = 0;
    return 0;
}

static DWORD WINAPI TestCancelThreadProc(LPVOID Param) {
    TEST_THREAD_CONTEXT *ctx = (TEST_THREAD_CONTEXT *)Param;
    QcaUartCancelProbe(ctx->Uart);
    return 0;
}

static DWORD WINAPI TestReleaseThreadProc(LPVOID Param) {
    TEST_THREAD_CONTEXT *ctx = (TEST_THREAD_CONTEXT *)Param;
    QcaUartReleaseHardware(ctx->Uart);
    return 0;
}

/*
 * Test 17: Completion Count Reaches Zero While IoIdle Signal In-Flight (Scenario 1)
 * - Completion count transitions to zero while callback context access is still in flight.
 * - ReleaseHardware drain loop must not close or delete target while callback is active.
 * - Eventual cleanup must succeed cleanly only after callback retirement.
 */
static BOOLEAN TestIoIdlePublicationSerializedUnderTargetLock(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    TEST_THREAD_CONTEXT ctx;
    HANDLE hCallbackThread;
    HANDLE hReleaseThread;
    HANDLE hStarted;
    HANDLE hCanFinish;

    ResetMockState();
    InitMockUart(&uart, &record);

    hStarted = CreateEvent(NULL, FALSE, FALSE, NULL);
    hCanFinish = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_Mock.HCallbackStarted = hStarted;
    g_Mock.HCallbackCanFinish = hCanFinish;
    g_Mock.MultiThreadedTestActive = TRUE;

    /* Simulate 1 active I/O count before completion */
    uart.ActiveIoCount = 1;
    uart.IoIdle.State = 0;

    ctx.Uart = &uart;

    hCallbackThread = CreateThread(NULL, 0, Test17CallbackThread, &ctx, 0, NULL);
    if (hCallbackThread == NULL) {
        CloseHandle(hStarted);
        CloseHandle(hCanFinish);
        return FALSE;
    }

    if (WaitForSingleObject(hStarted, 2000) != WAIT_OBJECT_0) {
        SetEvent(hCanFinish);
        WaitForSingleObject(hCallbackThread, 2000);
        CloseHandle(hCallbackThread);
        CloseHandle(hStarted);
        CloseHandle(hCanFinish);
        return FALSE;
    }

    /* ReleaseHardware runs on worker thread while callback is still active */
    hReleaseThread = CreateThread(NULL, 0, TestReleaseThreadProc, &ctx, 0, NULL);
    if (hReleaseThread == NULL) {
        SetEvent(hCanFinish);
        WaitForSingleObject(hCallbackThread, 2000);
        CloseHandle(hCallbackThread);
        CloseHandle(hStarted);
        CloseHandle(hCanFinish);
        return FALSE;
    }

    /* Give release thread time to enter drain loop */
    Sleep(50);

    /* Observable assertion: target must not be closed or deleted while callback is active */
    if (g_Mock.TargetClosedDuringCallback || g_Mock.TargetDeletedDuringCallback) {
        SetEvent(hCanFinish);
        WaitForSingleObject(hCallbackThread, 2000);
        WaitForSingleObject(hReleaseThread, 2000);
        CloseHandle(hCallbackThread);
        CloseHandle(hReleaseThread);
        CloseHandle(hStarted);
        CloseHandle(hCanFinish);
        return FALSE;
    }

    /* Allow callback to finish */
    SetEvent(hCanFinish);

    WaitForSingleObject(hCallbackThread, 3000);
    WaitForSingleObject(hReleaseThread, 3000);

    CloseHandle(hCallbackThread);
    CloseHandle(hReleaseThread);
    CloseHandle(hStarted);
    CloseHandle(hCanFinish);
    g_Mock.MultiThreadedTestActive = FALSE;

    if (g_Mock.TargetClosedDuringCallback) return FALSE;
    if (g_Mock.TargetDeletedDuringCallback) return FALSE;
    if (!g_Mock.IoTargetClosed) return FALSE;
    if (!g_Mock.IoTargetDeleted) return FALSE;
    if (uart.IoTarget != NULL) return FALSE;

    return TRUE;
}

/*
 * Test 18: Cancel Borrower Retains Target Across Completed Requests (Scenario 2)
 * - Requests have already completed (ActiveIoCount was 0).
 * - QcaUartCancelProbe borrows CancelTarget and is inside nonwaiting WdfIoTargetPurge.
 * - ReleaseHardware must wait for cancellation borrower to retire before close/delete.
 */
static BOOLEAN TestCancelBorrowerPreventsPrematureTargetRelease(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    TEST_THREAD_CONTEXT ctx;
    HANDLE hCancelThread;
    HANDLE hReleaseThread;
    HANDLE hPurgeStarted;
    HANDLE hPurgeRelease;

    ResetMockState();
    InitMockUart(&uart, &record);

    hPurgeStarted = CreateEvent(NULL, FALSE, FALSE, NULL);
    hPurgeRelease = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_Mock.HPurgeStarted = hPurgeStarted;
    g_Mock.HPurgeRelease = hPurgeRelease;
    g_Mock.MultiThreadedTestActive = TRUE;

    ctx.Uart = &uart;

    /* Start cancel thread which will enter WdfIoTargetPurge and block on hPurgeRelease */
    hCancelThread = CreateThread(NULL, 0, TestCancelThreadProc, &ctx, 0, NULL);
    if (hCancelThread == NULL) {
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* Wait until purge has started */
    if (WaitForSingleObject(hPurgeStarted, 2000) != WAIT_OBJECT_0) {
        SetEvent(hPurgeRelease);
        WaitForSingleObject(hCancelThread, 2000);
        CloseHandle(hCancelThread);
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* Target is currently in purge. Now call ReleaseHardware on a worker thread. */
    hReleaseThread = CreateThread(NULL, 0, TestReleaseThreadProc, &ctx, 0, NULL);
    if (hReleaseThread == NULL) {
        SetEvent(hPurgeRelease);
        WaitForSingleObject(hCancelThread, 2000);
        CloseHandle(hCancelThread);
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* Give release thread time to enter drain loop */
    Sleep(50);

    /* Verify that while purge is active, target has not been closed or deleted */
    if (g_Mock.IoTargetClosed || g_Mock.IoTargetDeleted || g_Mock.TargetClosedDuringPurge) {
        SetEvent(hPurgeRelease);
        WaitForSingleObject(hCancelThread, 2000);
        WaitForSingleObject(hReleaseThread, 2000);
        CloseHandle(hCancelThread);
        CloseHandle(hReleaseThread);
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* Release purge so cancel thread can finish and decrement borrower count */
    SetEvent(hPurgeRelease);

    WaitForSingleObject(hCancelThread, 3000);
    WaitForSingleObject(hReleaseThread, 3000);

    CloseHandle(hCancelThread);
    CloseHandle(hReleaseThread);
    CloseHandle(hPurgeStarted);
    CloseHandle(hPurgeRelease);
    g_Mock.MultiThreadedTestActive = FALSE;

    if (g_Mock.TargetClosedDuringPurge) return FALSE;
    if (g_Mock.TargetDeletedDuringPurge) return FALSE;
    if (!g_Mock.IoTargetClosed) return FALSE;
    if (!g_Mock.IoTargetDeleted) return FALSE;
    if (uart.IoTarget != NULL) return FALSE;

    return TRUE;
}

/*
 * Test 19: Repeated or Detached Cancellation Never Calls Into Closed Target (Scenario 3)
 * - While first cancellation purge is active, repeated cancellation/worker stop must not purge.
 * - After hardware release, detached cancellation must not access closed/deleted target.
 */
static BOOLEAN TestRepeatedOrDetachedCancellationNeverReachesClosedTarget(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    TEST_THREAD_CONTEXT ctx;
    HANDLE hCancelThread;
    HANDLE hPurgeStarted;
    HANDLE hPurgeRelease;

    ResetMockState();
    InitMockUart(&uart, &record);

    hPurgeStarted = CreateEvent(NULL, FALSE, FALSE, NULL);
    hPurgeRelease = CreateEvent(NULL, FALSE, FALSE, NULL);
    g_Mock.HPurgeStarted = hPurgeStarted;
    g_Mock.HPurgeRelease = hPurgeRelease;
    g_Mock.MultiThreadedTestActive = TRUE;

    ctx.Uart = &uart;

    /* Phase 1: Start initial cancellation and block inside purge */
    hCancelThread = CreateThread(NULL, 0, TestCancelThreadProc, &ctx, 0, NULL);
    if (hCancelThread == NULL) {
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    if (WaitForSingleObject(hPurgeStarted, 2000) != WAIT_OBJECT_0) {
        SetEvent(hPurgeRelease);
        WaitForSingleObject(hCancelThread, 2000);
        CloseHandle(hCancelThread);
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* While first purge is in progress, issue repeated cancellations and worker stop */
    QcaUartCancelProbe(&uart);
    QcaUartStop(&uart);
    QcaUartStopReadPump(&uart);
    QcaUartCancelProbe(&uart);

    if (g_Mock.ConcurrentPurgeViolation) {
        SetEvent(hPurgeRelease);
        WaitForSingleObject(hCancelThread, 2000);
        CloseHandle(hCancelThread);
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    if (g_Mock.TotalPurgesAttempted != 1) {
        SetEvent(hPurgeRelease);
        WaitForSingleObject(hCancelThread, 2000);
        CloseHandle(hCancelThread);
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* Phase 2: Unblock first purge and let cancellation complete */
    SetEvent(hPurgeRelease);
    WaitForSingleObject(hCancelThread, 3000);
    CloseHandle(hCancelThread);

    /* Phase 3: Release hardware and close target */
    QcaUartReleaseHardware(&uart);

    if (!g_Mock.IoTargetClosed || !g_Mock.IoTargetDeleted) {
        CloseHandle(hPurgeStarted);
        CloseHandle(hPurgeRelease);
        return FALSE;
    }

    /* Phase 4: Detached cancellation calls after target is closed/deleted */
    QcaUartCancelProbe(&uart);
    QcaUartStop(&uart);
    QcaUartStopReadPump(&uart);

    CloseHandle(hPurgeStarted);
    CloseHandle(hPurgeRelease);
    g_Mock.MultiThreadedTestActive = FALSE;

    if (g_Mock.PurgeOnClosedTarget) return FALSE;
    if (g_Mock.PurgeOnDeletedTarget) return FALSE;
    if (g_Mock.ConcurrentPurgeViolation) return FALSE;
    if (g_Mock.TotalPurgesAttempted != 1) return FALSE;

    return TRUE;
}

/* A failed WdfRequestSend has a failing request status (the WDF API contract).
 * Preserve that error and allow hardware release without an outstanding request. */
static BOOLEAN TestSendFailurePreservesStatusWithBalancedLifetime(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);

    g_Mock.FailSendTimed = TRUE;
    g_Mock.SendTimedFailureStatus = STATUS_DEVICE_HARDWARE_ERROR;

    status = QcaUartSendTimed(&uart, (WDFREQUEST)&g_Mock.ReadRequest, TRUE);

    if (status != STATUS_DEVICE_HARDWARE_ERROR) return FALSE;
    /* The failed send must not leave quiesce or release waiting on a callback. */
    status = QcaUartQuiesceRead(&uart);
    if (status != STATUS_SUCCESS) return FALSE;

    QcaUartReleaseHardware(&uart);
    if (!g_Mock.IoTargetClosed || uart.IoTarget != NULL) return FALSE;

    return TRUE;
}

/* --- Publication & Registry Contract Scenarios --- */

/*
 * Test 21: Terminal Observer Never Sees Incomplete Payload
 * - When UartCompletion transitions to terminal (1 or 2), all payload fields
 *   must be already committed and coherent.
 * - During all intermediate payload field writes, UartCompletion must be 0.
 */
static BOOLEAN TestTerminalObserverNeverSeesIncompletePayload(void) {
    QCA_UART_RECORD record;
    ULONG i;

    ResetMockState();
    memset(&record, 0, sizeof(record));

    record.ProbeRan = 1;
    record.IdentifyRan = 1;
    record.IdentifyBaud = 3000000;
    record.IdentifyAttempts = 2;
    record.ProductId = 0x00000042;
    record.PatchVersion = 0x0100;
    record.SocId = 0x40070200;
    record.RomVersion = 0x0200;
    record.LastStep = 94;
    record.LastStatus = STATUS_SUCCESS;
    (void)RtlStringCchCopyW(record.FailurePhase, ARRAYSIZE(record.FailurePhase), L"Answered");
    (void)RtlStringCchCopyW(record.IdentifyRawHex, ARRAYSIZE(record.IdentifyRawHex), L"04FF0E0019420000000001000200020740");
    record.Completion = QcaUartRecordReleased; /* 1 */

    DeckBtRecordProbeProgress(&record);

    /* UartCompletion must have been written */
    if (MockRegistryGetULong(L"UartCompletion", 999) != QcaUartRecordReleased) {
        return FALSE;
    }

    /* Terminal observer verification: every write before the final completion update
     * must have observed UartCompletion == 0 */
    if (g_Mock.Reg.WriteCount == 0) return FALSE;

    for (i = 0; i < g_Mock.Reg.WriteCount; i++) {
        if (wcscmp(g_Mock.Reg.Writes[i].Name, L"UartCompletion") == 0) {
            if (i == 0) {
                /* First write must be UartCompletion = 0 */
                if (g_Mock.Reg.Writes[i].ULongValue != QcaUartRecordInProgress) return FALSE;
            } else if (i == g_Mock.Reg.WriteCount - 1) {
                /* Last write is terminal UartCompletion = 1 */
                if (g_Mock.Reg.Writes[i].ULongValue != QcaUartRecordReleased) return FALSE;
            }
        } else {
            /* Any payload field write MUST have UartCompletion == 0 */
            if (g_Mock.Reg.Writes[i].CurrentCompletionBeforeWrite != QcaUartRecordInProgress) {
                return FALSE;
            }
        }
    }

    /* Coherent payload committed */
    if (MockRegistryGetULong(L"UartProbeRan", 0) != 1) return FALSE;
    if (MockRegistryGetULong(L"UartIdentifyBaud", 0) != 3000000) return FALSE;
    if (MockRegistryGetULong(L"UartProductId", 0) != 0x42) return FALSE;
    if (MockRegistryGetULong(L"UartSocId", 0) != 0x40070200) return FALSE;

    return TRUE;
}

/*
 * Test 22: Progress Publication Never Terminal Despite Ran 1
 * - When in-progress updates are published, UartCompletion must remain 0.
 * - Even with ProbeRan=1 or IdentifyRan=1, consumer never observes 1 or 2.
 */
static BOOLEAN TestProgressPublicationNeverTerminal(void) {
    QCA_UART_RECORD record;
    ULONG i;

    ResetMockState();
    memset(&record, 0, sizeof(record));

    record.ProbeRan = 1;
    record.IdentifyRan = 1;
    record.IdentifyBaud = 115200;
    record.IdentifyAttempts = 1;
    record.LastStep = 91;
    (void)RtlStringCchCopyW(record.FailurePhase, ARRAYSIZE(record.FailurePhase), L"IdentifyRung");
    record.Completion = QcaUartRecordInProgress; /* 0 */

    DeckBtRecordProbeProgress(&record);

    /* UartCompletion must be 0 */
    if (MockRegistryGetULong(L"UartCompletion", 999) != QcaUartRecordInProgress) {
        return FALSE;
    }

    /* Must never have written 1 or 2 at any point during progress publication */
    for (i = 0; i < g_Mock.Reg.WriteCount; i++) {
        if (wcscmp(g_Mock.Reg.Writes[i].Name, L"UartCompletion") == 0) {
            if (g_Mock.Reg.Writes[i].ULongValue != QcaUartRecordInProgress) {
                return FALSE;
            }
        }
    }

    return TRUE;
}

/*
 * Test 23: Injected Payload Write Error Invalidates Previous Terminal and Leaves Zero
 * - Injected failure on payload field write must invalidate any previous terminal state
 *   BEFORE metadata is updated, leaving UartCompletion == 0.
 */
static BOOLEAN TestPayloadWriteErrorInvalidatesPreviousTerminalAndLeavesZero(void) {
    QCA_UART_RECORD record;

    ResetMockState();

    /* Simulate registry previously having terminal state from prior completed run */
    g_Mock.Reg.Entries[0].IsString = FALSE;
    (void)RtlStringCchCopyW(g_Mock.Reg.Entries[0].Name, 64, L"UartCompletion");
    g_Mock.Reg.Entries[0].ULongValue = QcaUartRecordReleased; /* 1 */
    g_Mock.Reg.EntryCount = 1;

    memset(&record, 0, sizeof(record));
    record.ProbeRan = 1;
    record.IdentifyRan = 1;
    record.IdentifyBaud = 3000000;
    record.Completion = QcaUartRecordReleased; /* 1 */

    /* Inject write failure on UartElapsedMs */
    g_Mock.Reg.FailWrite = TRUE;
    (void)RtlStringCchCopyW(g_Mock.Reg.FailValueName, 64, L"UartElapsedMs");
    g_Mock.Reg.FailureStatus = STATUS_ACCESS_DENIED;

    DeckBtRecordProbeProgress(&record);

    /* First write MUST have been UartCompletion = 0 to invalidate previous terminal */
    if (g_Mock.Reg.WriteCount == 0) return FALSE;
    if (wcscmp(g_Mock.Reg.Writes[0].Name, L"UartCompletion") != 0) return FALSE;
    if (g_Mock.Reg.Writes[0].ULongValue != QcaUartRecordInProgress) return FALSE;

    /* Final registry state must be UartCompletion == 0 */
    if (MockRegistryGetULong(L"UartCompletion", 999) != QcaUartRecordInProgress) {
        return FALSE;
    }

    return TRUE;
}

/* --- Test Execution & Main --- */

typedef struct _TEST_ENTRY {
    const char *Name;
    BOOLEAN (*Fn)(void);
} TEST_ENTRY;

/*
 * Test 24: Controller never asserts CTS
 * - Wake pulses RTS five times; CTS stays low.
 * - Must never reach a write, must fail with CtsNotAsserted, and must leave RTS asserted under
 *   handshake control.
 */
static BOOLEAN TestCtsNeverAssertsBlocksEveryWrite(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.CtsLow = TRUE;
    g_Mock.CtsAssertAfterPulses = 0;

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_DEVICE_NOT_READY) return FALSE;
    if (g_Mock.TotalWritesAttempted != 0 || g_Mock.WriteWhileCtsLow) return FALSE;
    if (wcscmp(record.FailurePhase, L"CtsNotAsserted") != 0) return FALSE;
    if (record.LastStep != DECKBT_STEP_QCA_CTS_BLOCKED) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_DEVICE_NOT_READY) return FALSE;
    if (record.CtsAsserted != 0 || record.WakePulses != QCA_UART_WAKE_PULSES) return FALSE;
    if (record.ModemStatusReads != 1 + QCA_UART_WAKE_PULSES) return FALSE;
    if ((record.ModemStatusFirst & SERIAL_CTS_STATE) != 0) return FALSE;
    if (!g_Mock.RtsAsserted) return FALSE;
    if (g_Mock.LastFlowReplace != SERIAL_RTS_HANDSHAKE) return FALSE;
    if (g_Mock.LastControlHandShake != SERIAL_CTS_HANDSHAKE) return FALSE;
    if (g_Mock.BaudRatesSetCount != 1) return FALSE; /* never advanced the ladder */
    if (g_Mock.ConfigurationWithPendingRead) return FALSE;
    return TRUE;
}

/*
 * Test 25: Sleeping controller wakes on the vendor RTS pulse, then identifies at 115200
 * - CTS asserts after two pulses; the reply arrives at 115200.
 * - Exactly two pulses, one write, handshake restored, reply accepted.
 */
static BOOLEAN TestRtsPulseWakesControllerBeforeWrite(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    UCHAR verPacket[32];
    ULONG verLen;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.CtsLow = TRUE;
    g_Mock.CtsAssertAfterPulses = 2;
    verLen = BuildVersionEvent(verPacket, 0x00000010, 0x0000, 0x0200, 0x40070200);
    g_Mock.Responses[0].BaudRate = 115200ul;
    g_Mock.Responses[0].Length = verLen;
    memcpy(g_Mock.Responses[0].Data, verPacket, verLen);
    g_Mock.ResponseCount = 1;

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_SUCCESS) return FALSE;
    if (record.IdentifyBaud != 115200ul) return FALSE;
    if (wcscmp(record.FailurePhase, L"Answered") != 0) return FALSE;
    if (g_Mock.WriteWhileCtsLow || g_Mock.TotalWritesAttempted != 1) return FALSE;
    if (record.WakePulses != 2 || record.CtsAsserted != 1) return FALSE;
    if ((record.ModemStatusFirst & SERIAL_CTS_STATE) != 0) return FALSE;
    if ((record.ModemStatusLast & SERIAL_CTS_STATE) == 0) return FALSE;
    if (g_Mock.LastFlowReplace != SERIAL_RTS_HANDSHAKE || !g_Mock.RtsAsserted) return FALSE;
    if (g_Mock.ConfigurationWithPendingRead) return FALSE;
    return TRUE;
}

/*
 * Test 26: Unreadable modem status fails closed
 * - GET_MODEMSTATUS fails; CTS cannot be verified, so no byte may be transmitted.
 */
static BOOLEAN TestModemStatusFailureFailsClosed(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.FailModemStatus = TRUE;
    g_Mock.ModemStatusFailure = STATUS_IO_DEVICE_ERROR;

    status = QcaUartRunIdentify(&uart, &record);

    if (status != STATUS_IO_DEVICE_ERROR) return FALSE;
    if (g_Mock.TotalWritesAttempted != 0) return FALSE;
    if (wcscmp(record.FailurePhase, L"ModemStatus") != 0) return FALSE;
    if (record.LastStep != DECKBT_STEP_QCA_CTS_CHECK) return FALSE;
    if (record.ModemStatusReads != 0 || record.CtsAsserted != 0) return FALSE;
    return TRUE;
}

/*
 * Test 27: Full-probe baud switch never decodes across mismatched rates
 * - A reader is in flight at 115200 when 0xFC48 has drained.
 * - The reader retires before any line change; the host switches only after the 300 ms settle;
 *   the purge follows the switch; a fresh reader then runs at the new rate.
 */
static BOOLEAN TestBaudSwitchNeverDecodesAcrossRates(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;
    ULONGLONG started;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeFull;
    if (QcaUartStartReadPump(&uart) != STATUS_SUCCESS || !g_Mock.ReadRequest.InFlight) return FALSE;
    started = g_Mock.SimulatedTime;

    status = QcaUartSwitchBaud(&uart, 3000000ul);

    if (status != STATUS_SUCCESS) return FALSE;
    if (g_Mock.ConfigurationWithPendingRead) return FALSE;
    if (g_Mock.BaudRatesSetCount != 1 || g_Mock.BaudRatesSet[0] != 3000000ul) return FALSE;
    if (g_Mock.LastBaudSetTime - started < (ULONGLONG)QCA_UART_BAUD_SETTLE_MS * 10000ULL) return FALSE;
    if (g_Mock.PurgeCount != 1 || g_Mock.LastPurgeTime < g_Mock.LastBaudSetTime) return FALSE;
    if ((g_Mock.PurgeMasks[0] & (SERIAL_PURGE_RXABORT | SERIAL_PURGE_RXCLEAR)) !=
        (SERIAL_PURGE_RXABORT | SERIAL_PURGE_RXCLEAR)) return FALSE;
    if (!g_Mock.ReadRequest.InFlight || uart.ReadPumpRunning != 1) return FALSE;
    if (g_Mock.RequestReuseViolation) return FALSE;
    return TRUE;
}

/*
 * Test 28: A reader that will not retire blocks the baud switch
 * - Changing the rate under an active read would decode garbage; nothing may be reconfigured.
 */
static BOOLEAN TestBaudSwitchRefusesActiveReader(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    NTSTATUS status;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeFull;
    if (QcaUartStartReadPump(&uart) != STATUS_SUCCESS) return FALSE;
    g_Mock.IgnoreReadCancel = TRUE;

    status = QcaUartSwitchBaud(&uart, 3000000ul);

    if (NT_SUCCESS(status)) return FALSE;
    if (g_Mock.BaudRatesSetCount != 0 || g_Mock.PurgeCount != 0) return FALSE;
    return TRUE;
}

/* Post-firmware state as the full probe leaves it: 3 Mbaud, FSM reader running, verdict recorded. */
static void InitPostFirmwareUart(PQCA_UART Uart, PQCA_UART_RECORD Record) {
    InitMockUart(Uart, Record);
    Uart->ProbeMode = QcaProbeModeFull;
    Uart->CurrentBaudRate = 3000000ul;
    Record->LastStep = DECKBT_STEP_QCA_HCI_RESET_DONE;
    wcscpy_s(Record->FailurePhase, ARRAYSIZE(Record->FailurePhase), L"None");
    (void)QcaUartStartReadPump(Uart);
}

static BOOLEAN WriteIs(ULONG Index, const UCHAR *Bytes, ULONG Length, ULONG Baud) {
    return Index < g_Mock.WriteLogCount && g_Mock.WriteLogLength[Index] == Length &&
           memcmp(g_Mock.WriteLog[Index], Bytes, Length < 8 ? Length : 8) == 0 &&
           g_Mock.WriteLogBaud[Index] == Baud;
}

/*
 * Test 29: Handback returns the controller to ROM
 * - At 3 Mbaud: IBS wake FD FD FD, then SoC reset 01 40 FC 00; then the identify ladder answers
 *   at 115200. The probe's own LastStep/FailurePhase are preserved.
 */
static BOOLEAN TestHandbackReturnsControllerToRom(void) {
    static const UCHAR wake[] = { 0xFD, 0xFD, 0xFD };
    static const UCHAR reset[] = { 0x01, 0x40, 0xFC, 0x00 };
    static const UCHAR version[] = { 0x01, 0x00, 0xFC, 0x01, 0x19 };
    QCA_UART uart;
    QCA_UART_RECORD record;
    UCHAR verPacket[32];
    ULONG verLen;

    ResetMockState();
    InitPostFirmwareUart(&uart, &record);
    verLen = BuildVersionEvent(verPacket, 0x13, 0x38E6, 0x0201, 0x400C1211);
    g_Mock.Responses[0].BaudRate = 115200ul;
    g_Mock.Responses[0].Length = verLen;
    memcpy(g_Mock.Responses[0].Data, verPacket, verLen);
    g_Mock.ResponseCount = 1;

    QcaUartHandback(&uart, &record);

    if (record.HandbackBaud != 115200ul || record.HandbackStatus != (ULONG)STATUS_SUCCESS) return FALSE;
    if (!WriteIs(0, wake, sizeof(wake), 3000000ul)) return FALSE;
    if (!WriteIs(1, reset, sizeof(reset), 3000000ul)) return FALSE;
    if (!WriteIs(2, version, sizeof(version), 115200ul)) return FALSE;
    if (record.LastStep != DECKBT_STEP_QCA_HCI_RESET_DONE) return FALSE;
    if (wcscmp(record.FailurePhase, L"None") != 0) return FALSE;
    if (g_Mock.WriteWhileCtsLow || g_Mock.ConfigurationWithPendingRead) return FALSE;
    return TRUE;
}

/*
 * Test 30: A reset that does not take is reported, not hidden
 * - The controller still answers only at 3 Mbaud: HandbackBaud records 3000000.
 */
static BOOLEAN TestHandbackReportsResetNotTaken(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    UCHAR verPacket[32];
    ULONG verLen;

    ResetMockState();
    InitPostFirmwareUart(&uart, &record);
    verLen = BuildVersionEvent(verPacket, 0x13, 0x3A98, 0x0201, 0x400C1211);
    g_Mock.Responses[0].BaudRate = 3000000ul;
    g_Mock.Responses[0].Length = verLen;
    memcpy(g_Mock.Responses[0].Data, verPacket, verLen);
    g_Mock.ResponseCount = 1;

    QcaUartHandback(&uart, &record);

    if (record.HandbackBaud != 3000000ul) return FALSE;
    if (record.LastStep != DECKBT_STEP_QCA_HCI_RESET_DONE) return FALSE;
    return TRUE;
}

/*
 * Test 31: Handback never transmits without CTS
 * - CTS never asserts: no byte is written, the failure is recorded, the verdict is untouched.
 */
static BOOLEAN TestHandbackRespectsCtsGuard(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;

    ResetMockState();
    InitPostFirmwareUart(&uart, &record);
    g_Mock.CtsLow = TRUE;

    QcaUartHandback(&uart, &record);

    if (g_Mock.TotalWritesAttempted != 0 || g_Mock.WriteWhileCtsLow) return FALSE;
    if (record.HandbackStatus != (ULONG)STATUS_DEVICE_NOT_READY || record.HandbackBaud != 0) return FALSE;
    if (record.LastStep != DECKBT_STEP_QCA_HCI_RESET_DONE) return FALSE;
    if (wcscmp(record.FailurePhase, L"None") != 0) return FALSE;
    return TRUE;
}

static void QueueVersion(ULONG Index, ULONG Baud, USHORT PatchVer) {
    UCHAR packet[32];
    ULONG len = BuildVersionEvent(packet, 0x13, PatchVer, 0x0201, 0x400C1211);
    g_Mock.Responses[Index].BaudRate = Baud;
    g_Mock.Responses[Index].Length = len;
    memcpy(g_Mock.Responses[Index].Data, packet, len);
    if (g_Mock.ResponseCount < Index + 1) g_Mock.ResponseCount = Index + 1;
}

static ULONG CountResets(void) {
    ULONG i, n = 0;
    for (i = 0; i < g_Mock.WriteLogCount; i++) {
        if (g_Mock.WriteLogLength[i] == 4 && memcmp(g_Mock.WriteLog[i], "\x01\x40\xFC\x00", 4) == 0) n++;
    }
    return n;
}

/*
 * Test 32: Bring-up from ROM needs no reset
 */
static BOOLEAN TestEnsureRomAcceptsRom(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeFull;
    QueueVersion(0, 115200ul, 0x38E6);

    if (QcaUartEnsureRom(&uart, &record) != STATUS_SUCCESS) return FALSE;
    if (record.EntryBaud != 115200ul || record.EntryReset != 0 || CountResets() != 0) return FALSE;
    if (uart.CurrentBaudRate != 115200ul || uart.ProbeMode != QcaProbeModeFull) return FALSE;
    return TRUE;
}

/*
 * Test 33: A patch left running at 3 Mbaud is reset there, then answers at 115200
 */
static BOOLEAN TestEnsureRomResetsAwakePatch(void) {
    static const UCHAR wake[] = { 0xFD, 0xFD, 0xFD };
    static const UCHAR reset[] = { 0x01, 0x40, 0xFC, 0x00 };
    QCA_UART uart;
    QCA_UART_RECORD record;
    ULONG i, resetAt = 0;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeFull;
    QueueVersion(0, 3000000ul, 0x3A98);
    QueueVersion(1, 115200ul, 0x38E6);

    if (QcaUartEnsureRom(&uart, &record) != STATUS_SUCCESS) return FALSE;
    if (record.EntryBaud != 3000000ul || record.EntryReset != 1 || CountResets() != 1) return FALSE;
    for (i = 0; i < g_Mock.WriteLogCount; i++) {
        if (WriteIs(i, reset, sizeof(reset), 3000000ul)) resetAt = i;
    }
    if (resetAt == 0 || !WriteIs(resetAt - 1, wake, sizeof(wake), 3000000ul)) return FALSE;
    if (uart.CurrentBaudRate != 115200ul || g_Mock.ConfigurationWithPendingRead) return FALSE;
    return TRUE;
}

/*
 * Test 34: A controller asleep in IBS (silent at every rate) is reset at the operating rate.
 * Silent at 115200 and 3000000, host UART refuses 3200000.
 */
static BOOLEAN TestEnsureRomResetsSilentController(void) {
    static const UCHAR reset[] = { 0x01, 0x40, 0xFC, 0x00 };
    QCA_UART uart;
    QCA_UART_RECORD record;
    ULONG i;
    BOOLEAN resetAtOper = FALSE;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeFull;
    g_Mock.SilentUntilReset = TRUE;
    g_Mock.FailBaudRate = TRUE;
    g_Mock.BaudFailureRate = 3200000ul;
    g_Mock.BaudRateStatus = STATUS_INVALID_PARAMETER;
    QueueVersion(0, 115200ul, 0x38E6);

    if (QcaUartEnsureRom(&uart, &record) != STATUS_SUCCESS) return FALSE;
    if (record.EntryBaud != 0 || record.EntryReset != 1 || CountResets() != 1) return FALSE;
    for (i = 0; i < g_Mock.WriteLogCount; i++) {
        if (WriteIs(i, reset, sizeof(reset), QCA_OPER_BAUD_RATE)) resetAtOper = TRUE;
    }
    return resetAtOper && uart.CurrentBaudRate == 115200ul;
}

/*
 * Test 36: A rate the host UART refuses is skipped, not fatal, and nothing is sent at it
 */
static BOOLEAN TestHostRejectedRateSkipsRung(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;
    ULONG i;

    ResetMockState();
    InitMockUart(&uart, &record);
    g_Mock.FailBaudRate = TRUE;
    g_Mock.BaudFailureRate = 3000000ul;
    g_Mock.BaudRateStatus = STATUS_INVALID_PARAMETER;
    QueueVersion(0, 3200000ul, 0x38E6);

    if (QcaUartRunIdentify(&uart, &record) != STATUS_SUCCESS) return FALSE;
    if (record.IdentifyBaud != 3200000ul || g_Mock.TotalWritesAttempted != 2) return FALSE;
    for (i = 0; i < g_Mock.WriteLogCount; i++) {
        if (g_Mock.WriteLogBaud[i] == 3000000ul) return FALSE;
    }
    return TRUE;
}

/*
 * Test 35: A reset that does not take fails bring-up before any firmware is sent
 */
static BOOLEAN TestEnsureRomFailsWhenResetDoesNotTake(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeFull;
    QueueVersion(0, 3000000ul, 0x3A98);
    QueueVersion(1, 3000000ul, 0x3A98);

    if (QcaUartEnsureRom(&uart, &record) != STATUS_INVALID_DEVICE_STATE) return FALSE;
    if (wcscmp(record.FailurePhase, L"EnsureRom") != 0) return FALSE;
    if (record.LastStatus != (ULONG)STATUS_INVALID_DEVICE_STATE) return FALSE;
    return TRUE;
}

/* ---------------------------------------------------------------- Steady state */

static unsigned char MockWireSend(void *Context, const unsigned char *Packet, unsigned long Length) {
    (void)Context;
    (void)Packet;
    (void)Length;
    return 1;
}

static const HCI_BRIDGE_WIRE_OPS g_MockWireOps = { MockWireSend, MockWireSend, MockWireSend };

static void MockNotify(void *Context, HCI_STREAM Stream) {
    (void)Context;
    if (Stream == HciStreamEvent) g_Mock.NotifyEvents++;
}

static void QueueBytes(ULONG Index, ULONG Baud, const UCHAR *Data, ULONG Length) {
    g_Mock.Responses[Index].BaudRate = Baud;
    g_Mock.Responses[Index].Length = Length;
    memcpy(g_Mock.Responses[Index].Data, Data, Length);
    if (g_Mock.ResponseCount < Index + 1) g_Mock.ResponseCount = Index + 1;
}

/*
 * State at the end of a successful steady bring-up (QcaUartRunProbe steady exit): Phase 2,
 * bridge not ready, reader active at 3,000,000, IBS slot allocated, a front end listening.
 */
static void InitSteadyUart(PQCA_UART Uart, HCI_TRANSPORT *Upstream) {
    HCI_BRIDGE_WIRE wire;

    ResetMockState();
    InitMockUart(Uart, &Uart->Record);
    Uart->ProbeMode = QcaProbeModeSteady;
    memset(Upstream, 0, sizeof(*Upstream));
    Upstream->Notify = MockNotify;
    Uart->Transport = Upstream;
    wire.Ops = &g_MockWireOps;
    wire.Context = NULL;
    HciBridgeInit(&Uart->Bridge, &wire);
    HciBridgeBindTransport(&Uart->BridgeTransport, &Uart->Bridge);
    (void)QcaUartSetBaudRate(Uart, 3000000ul);
    Uart->Phase = 2;
    (void)QcaUartStartReadPump(Uart);
}

/*
 * Test 37: The controller's wake is recognized between packets and acknowledged by the worker
 * - FD (wake) + Read_BD_ADDR Command Complete whose address bytes are FD FE FC + FE (sleep).
 * - The read completion writes nothing but leaves one pending ack; the event reaches the host intact; nothing is desync.
 * - The worker then sends exactly one WAKE_ACK at the operating rate.
 */
static BOOLEAN TestSteadyAcksControllerWakeBetweenPackets(void) {
    static const UCHAR rx[] = {
        0xFD,
        0x04, 0x0E, 0x0A, 0x01, 0x09, 0x10, 0x00, 0xFD, 0xFE, 0xFC, 0x65, 0x20, 0x50,
        0xFE
    };
    static const UCHAR wakeAck[] = { 0xFC };
    QCA_UART uart;
    HCI_TRANSPORT upstream;
    UCHAR evt[HCI_BRIDGE_MAX_EVENT_SIZE];
    unsigned long written = 0;

    InitSteadyUart(&uart, &upstream);
    HciBridgeSetReady(&uart.Bridge, 1);
    memcpy(g_ReadBuffer, rx, sizeof(rx));
    CompleteRead(STATUS_SUCCESS, sizeof(rx));

    if (g_Mock.TotalWritesAttempted != 0) return FALSE;
    if (uart.IbsAckPending != 1 || uart.IbsWorkEvent.State == 0) return FALSE;
    if (uart.IbsWakeIndRx != 1 || uart.IbsSleepIndRx != 1) return FALSE;
    if (uart.Decoder.Desynchronised != 0 || g_Mock.NotifyEvents != 1) return FALSE;
    if (!HciTransportPopStream(&uart.BridgeTransport, HciStreamEvent, evt, sizeof(evt), &written)) return FALSE;
    /* 0E 0A | 01 09 10 | 00 | BD_ADDR: FD FE FC 65 20 50 */
    if (written != 12 || evt[6] != 0xFD || evt[7] != 0xFE || evt[8] != 0xFC) return FALSE;
    if (!g_Mock.ReadRequest.InFlight) return FALSE;   /* the reader keeps listening */

    QcaUartServiceIbs(&uart, &uart.Record);
    if (!WriteIs(0, wakeAck, sizeof(wakeAck), 3000000ul) || uart.IbsWakeAckTx != 1) return FALSE;
    if (uart.IbsAckPending != 0 || uart.IbsWorkEvent.State != 0) return FALSE;
    QcaUartServiceIbs(&uart, &uart.Record);   /* nothing pending: nothing more written */
    return g_Mock.TotalWritesAttempted == 1;
}

/*
 * Test 43: The controller's rejection of Write_LE_Host_Support reaches BTHPORT as success, as the
 * vendor driver does it (0E 04 01 6D 0C 11, then no LE scan) - and only
 * that completion: the same 0x11 on another opcode is passed through untouched.
 */
static BOOLEAN TestSteadyLeHostSupportRejectionReportedAsSuccess(void) {
    static const UCHAR rx[] = {
        0x04, 0x0E, 0x04, 0x01, 0x6D, 0x0C, 0x11,
        0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x11
    };
    QCA_UART uart;
    HCI_TRANSPORT upstream;
    UCHAR evt[HCI_BRIDGE_MAX_EVENT_SIZE];
    unsigned long written = 0;

    InitSteadyUart(&uart, &upstream);
    HciBridgeSetReady(&uart.Bridge, 1);
    memcpy(g_ReadBuffer, rx, sizeof(rx));
    CompleteRead(STATUS_SUCCESS, sizeof(rx));

    if (!HciTransportPopStream(&uart.BridgeTransport, HciStreamEvent, evt, sizeof(evt), &written)) return FALSE;
    if (written != 6 || evt[3] != 0x6D || evt[4] != 0x0C || evt[5] != 0x00) return FALSE;
    if (!HciTransportPopStream(&uart.BridgeTransport, HciStreamEvent, evt, sizeof(evt), &written)) return FALSE;
    if (written != 6 || evt[3] != 0x03 || evt[4] != 0x0C || evt[5] != 0x11) return FALSE;
    /* The trace keeps what the controller actually said. */
    return uart.EventTraceCount == 2 && uart.EventTrace[0][5] == 0x11;
}

/*
 * Test 44: A scan's advertising reports are counted and delivered but not traced, so the
 * connection events stay in the 128-slot trace.
 */
static BOOLEAN TestSteadyAdvReportsCountedNotTraced(void) {
    /* Legacy report x2 (11:22:33:44:55:66), extended report (CA:FE:00:11:22:33, random), LE Connection Complete. */
    static const UCHAR rx[] = {
        0x04, 0x3E, 0x0C, 0x02, 0x01, 0x00, 0x00, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xC5,
        0x04, 0x3E, 0x0C, 0x02, 0x01, 0x00, 0x00, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xC5,
        0x04, 0x3E, 0x1A, 0x0D, 0x01, 0x13, 0x00, 0x01, 0x33, 0x22, 0x11, 0x00, 0xFE, 0xCA, 0x01,
        0x00, 0xFF, 0x7F, 0xC5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x3E, 0x13, 0x01, 0x00, 0x40, 0x00, 0x00, 0x00, 0x33, 0x22, 0x11, 0x00, 0xFE, 0xCA,
        0x18, 0x00, 0x00, 0x00, 0xC8, 0x00, 0x00
    };
    static const UCHAR phone[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
    static const UCHAR mouse[6] = { 0x33, 0x22, 0x11, 0x00, 0xFE, 0xCA };
    QCA_UART uart;
    HCI_TRANSPORT upstream;
    UCHAR evt[HCI_BRIDGE_MAX_EVENT_SIZE];
    unsigned long written = 0;
    ULONG delivered = 0;

    InitSteadyUart(&uart, &upstream);
    HciBridgeSetReady(&uart.Bridge, 1);
    memcpy(g_ReadBuffer, rx, sizeof(rx));
    CompleteRead(STATUS_SUCCESS, sizeof(rx));

    while (HciTransportPopStream(&uart.BridgeTransport, HciStreamEvent, evt, sizeof(evt), &written)) {
        delivered++;
    }
    if (delivered != 4 || uart.AdvReports != 3) return FALSE;
    /* Every advertiser is listed once, with its own type and report count. */
    if (uart.AdvSeenCount != 2) return FALSE;
    if (memcmp(uart.AdvSeen[0].Address, phone, 6) != 0 || uart.AdvSeen[0].AddressType != 0 ||
        uart.AdvSeen[0].Count != 2) return FALSE;
    if (memcmp(uart.AdvSeen[1].Address, mouse, 6) != 0 || uart.AdvSeen[1].AddressType != 1 ||
        uart.AdvSeen[1].EventType != 0x13 || uart.AdvSeen[1].Count != 1) return FALSE;
    return uart.EventTraceCount == 1 && uart.EventTrace[0][0] == 0x3E && uart.EventTrace[0][2] == 0x01;
}

/*
 * Test 42: A wake ack is never written into a deasserted CTS
 * - CTS low when the ack is due: nothing written (a write stalled on CTS never retires), the
 *   skip counted, the pending ack consumed; the controller's repeated WAKE_IND is then answered.
 */
static BOOLEAN TestSteadyAckNeverWritesIntoLowCts(void) {
    static const UCHAR wakeInd[] = { 0xFD };
    static const UCHAR wakeAck[] = { 0xFC };
    QCA_UART uart;
    HCI_TRANSPORT upstream;

    InitSteadyUart(&uart, &upstream);
    memcpy(g_ReadBuffer, wakeInd, sizeof(wakeInd));
    CompleteRead(STATUS_SUCCESS, sizeof(wakeInd));
    g_Mock.CtsLow = TRUE;
    QcaUartServiceIbs(&uart, &uart.Record);
    if (g_Mock.TotalWritesAttempted != 0 || g_Mock.WriteWhileCtsLow) return FALSE;
    if (uart.IbsAckCtsLow != 1 || uart.IbsWakeAckTx != 0 || uart.IbsAckPending != 0) return FALSE;

    g_Mock.CtsLow = FALSE;
    memcpy(g_ReadBuffer, wakeInd, sizeof(wakeInd));
    CompleteRead(STATUS_SUCCESS, sizeof(wakeInd));
    QcaUartServiceIbs(&uart, &uart.Record);
    return WriteIs(0, wakeAck, sizeof(wakeAck), 3000000ul) && uart.IbsWakeAckTx == 1;
}

/*
 * Test 38: Steady session order: host wake, bridge open, USB child, serve, hand back
 * - WAKE_IND goes out first at the operating rate and is acknowledged.
 * - The USB child is requested exactly once, with the bridge already answering.
 * - Traffic while serving reaches the record; a graceful stop closes the bridge and returns the
 *   controller to ROM (reset at 3 Mbaud, answers at 115200) for the vendor driver.
 */
static BOOLEAN TestSteadyServesAfterHostWakeAndHandsBack(void) {
    static const UCHAR wakeAck[] = { 0xFC };
    static const UCHAR wakeInd[] = { 0xFD };
    static const UCHAR reset[] = { 0x01, 0x40, 0xFC, 0x00 };
    static const UCHAR talks[] = { 0xFD, 0x04, 0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00 };
    QCA_UART uart;
    HCI_TRANSPORT upstream;
    ULONG i;
    ULONG ackAt = 0;
    ULONG resetAt = 0;

    InitSteadyUart(&uart, &upstream);
    QueueBytes(0, 3000000ul, wakeAck, sizeof(wakeAck));
    QueueBytes(1, 3000000ul, talks, sizeof(talks));
    QueueVersion(2, 115200ul, 0x38E6);
    g_Mock.ShutdownAfterWaits = 3;

    if (QcaUartServeSteady(&uart) != STATUS_SUCCESS) return FALSE;
    if (!WriteIs(0, wakeInd, sizeof(wakeInd), 3000000ul)) return FALSE;
    /* Steady reads return on the first byte, set with no read in flight (SERIAL_TIMEOUTS remarks). */
    if (g_Mock.TimeoutsSet != 1 || g_Mock.ConfigurationWithPendingRead) return FALSE;
    if (g_Mock.LastTimeouts.ReadIntervalTimeout != MAXULONG ||
        g_Mock.LastTimeouts.ReadTotalTimeoutMultiplier != MAXULONG ||
        g_Mock.LastTimeouts.ReadTotalTimeoutConstant == 0 ||
        g_Mock.LastTimeouts.ReadTotalTimeoutConstant == MAXULONG) return FALSE;
    if (uart.Record.IbsWakeTries != 1 || uart.Record.IbsHostAwake != 1) return FALSE;
    if (g_Mock.PublishCalls != 1 || !g_Mock.PublishSawReady) return FALSE;
    if (uart.Record.SteadyReached != 1 || uart.Record.UsbPlugStatus != 0) return FALSE;
    if (uart.Record.BridgeEventsQueued != 1 || uart.Record.IbsWakeAckTx != 1) return FALSE;
    /* The operator sees that event's header: HCI_Reset Command Complete, status 0. */
    if (g_Mock.PublishedEventCount != 1 || g_Mock.PublishedEventLog[0] != 0x0E ||
        g_Mock.PublishedEventLog[3] != 0x03 || g_Mock.PublishedEventLog[4] != 0x0C ||
        g_Mock.PublishedEventLog[5] != 0x00) return FALSE;
    for (i = 0; i < g_Mock.WriteLogCount; i++) {
        if (ackAt == 0 && WriteIs(i, wakeAck, sizeof(wakeAck), 3000000ul)) ackAt = i;
        if (WriteIs(i, reset, sizeof(reset), 3000000ul)) resetAt = i;
    }
    /* The controller's wake was answered while serving, before the handback reset. */
    if (ackAt == 0 || resetAt <= ackAt || uart.Record.HandbackBaud != 115200ul) return FALSE;
    if (uart.Bridge.Ready != 0 || uart.Phase != 3 || uart.BudgetUnbounded != 0) return FALSE;
    if (wcscmp(uart.Record.FailurePhase, L"Stopped") != 0) return FALSE;
    return TRUE;
}

/*
 * Test 39: A USB plug-in failure is reported and the controller is still handed back
 */
static BOOLEAN TestSteadyPlugFailureStillHandsBack(void) {
    static const UCHAR wakeAck[] = { 0xFC };
    QCA_UART uart;
    HCI_TRANSPORT upstream;

    InitSteadyUart(&uart, &upstream);
    QueueBytes(0, 3000000ul, wakeAck, sizeof(wakeAck));
    QueueVersion(1, 115200ul, 0x38E6);
    g_Mock.PublishStatus = STATUS_INSUFFICIENT_RESOURCES;

    if (QcaUartServeSteady(&uart) != STATUS_INSUFFICIENT_RESOURCES) return FALSE;
    if (uart.Record.SteadyReached != 0) return FALSE;
    if (uart.Record.UsbPlugStatus != (ULONG)STATUS_INSUFFICIENT_RESOURCES) return FALSE;
    if (wcscmp(uart.Record.FailurePhase, L"UsbPlugIn") != 0) return FALSE;
    if (uart.Record.HandbackBaud != 115200ul || uart.Bridge.Ready != 0) return FALSE;
    return TRUE;
}

/*
 * Test 40: The steady reader survives an idle line and the bring-up deadline
 * - The read carried over from bring-up times out (cancelled): it is re-armed, untimed.
 * - Two minutes into a session the 60 s bring-up budget does not stop it.
 */
static BOOLEAN TestSteadyReaderSurvivesIdleLine(void) {
    QCA_UART uart;
    HCI_TRANSPORT upstream;

    InitSteadyUart(&uart, &upstream);
    if ((g_Mock.LastReadSendFlags & WDF_REQUEST_SEND_OPTION_TIMEOUT) == 0) return FALSE;
    uart.BudgetUnbounded = 1;
    CompleteRead(STATUS_CANCELLED, 0);
    if (!g_Mock.ReadRequest.InFlight || uart.ReadPumpRunning == 0) return FALSE;
    if ((g_Mock.LastReadSendFlags & WDF_REQUEST_SEND_OPTION_TIMEOUT) != 0) return FALSE;
    g_Mock.SimulatedTime += 120ull * 1000ull * 10000ull;
    if (QcaUartRequestBudget(&uart, 5000) != 5000 || uart.StopRequested != 0) return FALSE;
    return TRUE;
}

/*
 * Test 41: Stop requests: graceful only for a session serving BTHUSB, bounded
 * - Bring-up in progress: hard cancel at once (nothing to hand back yet).
 * - Serving: graceful stop requested and the worker's handback awaited, not cancelled.
 * - Handback overrunning its bound: escalated to the hard cancel.
 */
static BOOLEAN TestRequestStopGracefulOnlyWhenServing(void) {
    QCA_UART uart;
    QCA_UART_RECORD record;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeSteady;
    QcaUartRequestStop(&uart);
    if (uart.StopRequested != 1 || uart.ShutdownEvent.State != 0) return FALSE;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeSteady;
    uart.SteadyReady.State = 1;
    uart.SteadyStopped.State = 1;
    QcaUartRequestStop(&uart);
    if (uart.StopRequested != 0 || uart.ShutdownEvent.State != 1 || uart.ShutdownRequested != 1) return FALSE;

    ResetMockState();
    InitMockUart(&uart, &record);
    uart.ProbeMode = QcaProbeModeSteady;
    uart.SteadyReady.State = 1;
    QcaUartRequestStop(&uart);
    if (uart.StopRequested != 1) return FALSE;
    return TRUE;
}

static const TEST_ENTRY g_Tests[] = {
    { "TestValidFragmentedResponseLaterBaud", TestValidFragmentedResponseLaterBaud },
    { "TestTrueNoResponseReturnsNotFound", TestTrueNoResponseReturnsNotFound },
    { "TestWriteFailureStopsLadder", TestWriteFailureStopsLadder },
    { "TestPartialWriteStopsLadder", TestPartialWriteStopsLadder },
    { "TestBaudFailureStopsLadder", TestBaudFailureStopsLadder },
    { "TestHostRejectedRateSkipsRung", TestHostRejectedRateSkipsRung },
    { "TestPurgeFailureStopsLadder", TestPurgeFailureStopsLadder },
    { "TestReadErrorSignalsFsmEvent", TestReadErrorSignalsFsmEvent },
    { "TestCancellationPreservesAbortedAndStatus", TestCancellationPreservesAbortedAndStatus },
    { "TestBudgetExhaustionPreservesAbortedAndStatus", TestBudgetExhaustionPreservesAbortedAndStatus },
    { "TestDelayedCancellationDrainsCleanly", TestDelayedCancellationDrainsCleanly },
    { "TestDrainTimeoutStopsLadder", TestDrainTimeoutStopsLadder },
    { "TestFsmWaitingForEventFiltersPacket", TestFsmWaitingForEventFiltersPacket },
    { "TestReadErrorDuringWait", TestReadErrorDuringWait },
    { "TestReadErrorBeforeWriteDoesNotLoseWake", TestReadErrorBeforeWriteDoesNotLoseWake },
    { "TestReplyCannotHideWriteError", TestReplyCannotHideWriteError },
    { "TestReplyCannotHideReadError", TestReplyCannotHideReadError },
    { "TestIoIdlePublicationSerializedUnderTargetLock", TestIoIdlePublicationSerializedUnderTargetLock },
    { "TestCancelBorrowerPreventsPrematureTargetRelease", TestCancelBorrowerPreventsPrematureTargetRelease },
    { "TestRepeatedOrDetachedCancellationNeverReachesClosedTarget", TestRepeatedOrDetachedCancellationNeverReachesClosedTarget },
    { "TestSendFailurePreservesStatusWithBalancedLifetime", TestSendFailurePreservesStatusWithBalancedLifetime },
    { "TestTerminalObserverNeverSeesIncompletePayload", TestTerminalObserverNeverSeesIncompletePayload },
    { "TestProgressPublicationNeverTerminal", TestProgressPublicationNeverTerminal },
    { "TestPayloadWriteErrorInvalidatesPreviousTerminalAndLeavesZero", TestPayloadWriteErrorInvalidatesPreviousTerminalAndLeavesZero },
    { "TestCtsNeverAssertsBlocksEveryWrite", TestCtsNeverAssertsBlocksEveryWrite },
    { "TestRtsPulseWakesControllerBeforeWrite", TestRtsPulseWakesControllerBeforeWrite },
    { "TestModemStatusFailureFailsClosed", TestModemStatusFailureFailsClosed },
    { "TestBaudSwitchNeverDecodesAcrossRates", TestBaudSwitchNeverDecodesAcrossRates },
    { "TestBaudSwitchRefusesActiveReader", TestBaudSwitchRefusesActiveReader },
    { "TestHandbackReturnsControllerToRom", TestHandbackReturnsControllerToRom },
    { "TestHandbackReportsResetNotTaken", TestHandbackReportsResetNotTaken },
    { "TestHandbackRespectsCtsGuard", TestHandbackRespectsCtsGuard },
    { "TestEnsureRomAcceptsRom", TestEnsureRomAcceptsRom },
    { "TestEnsureRomResetsAwakePatch", TestEnsureRomResetsAwakePatch },
    { "TestEnsureRomResetsSilentController", TestEnsureRomResetsSilentController },
    { "TestEnsureRomFailsWhenResetDoesNotTake", TestEnsureRomFailsWhenResetDoesNotTake },
    { "TestSteadyAcksControllerWakeBetweenPackets", TestSteadyAcksControllerWakeBetweenPackets },
    { "TestSteadyServesAfterHostWakeAndHandsBack", TestSteadyServesAfterHostWakeAndHandsBack },
    { "TestSteadyPlugFailureStillHandsBack", TestSteadyPlugFailureStillHandsBack },
    { "TestSteadyReaderSurvivesIdleLine", TestSteadyReaderSurvivesIdleLine },
    { "TestRequestStopGracefulOnlyWhenServing", TestRequestStopGracefulOnlyWhenServing },
    { "TestSteadyAckNeverWritesIntoLowCts", TestSteadyAckNeverWritesIntoLowCts },
    { "TestSteadyLeHostSupportRejectionReportedAsSuccess", TestSteadyLeHostSupportRejectionReportedAsSuccess },
    { "TestSteadyAdvReportsCountedNotTraced", TestSteadyAdvReportsCountedNotTraced },
};


int main(void) {
    int total = (int)ARRAYSIZE(g_Tests);
    int passed = 0;
    int i;
    for (i = 0; i < total; i++) {
        BOOLEAN result = g_Tests[i].Fn();
        if (result) passed++;
        printf("  [%s] %s\n", result ? "pass" : "FAIL", g_Tests[i].Name);
        fflush(stdout);
    }
    printf("UART IDENTIFY: %d/%d scenarios passed\n", passed, total);
    return passed == total ? 0 : 1;
}
