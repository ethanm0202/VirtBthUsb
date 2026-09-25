/*
 * deckbtusb.h - DeckBtUsb Bluetooth USB façade.
 *
 * Exposes either the synthetic HCI controller or the Qualcomm QCA2066 UART backend.
 * Backend selection happens at PrepareHardware; both use the same USB endpoint code.
 */

#pragma once

/* Required include order: wdfusb.h needs USB_REQUEST_* / USBD_STATUS from usb.h, and
 * UdeCx.h needs PWDF_USB_CONTROL_SETUP_PACKET from wdfusb.h. */
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <wdfusb.h>
#include <ude/1.1/UdeCx.h>

#include "../include/usb_descriptors.h"
#include "hci_stub.h"
#include "qca_uart.h"
#include "../include/hci_transport.h"
#include "../include/arming.h"
#include "../include/sco_usb.h"

#define DECKBT_CTL_LOG_SLOTS  128u   /* a full BTHUSB bring-up is ~140 transfers */
#define DECKBT_CTL_LOG_STRIDE 12u

/* Outcome codes stored in the trace. */
#define DECKBT_CTL_OK_HCI      1u
#define DECKBT_CTL_STALLED     2u
#define DECKBT_CTL_BADBUFFER   3u
#define DECKBT_CTL_NOTCONTROL  4u

/*
 * SCO voice counters, published as Sco* values under the service Parameters key. They are the only
 * view of the isochronous path on a machine without a kernel debugger.
 */
typedef struct _DECKBT_SCO_STATS {
    ULONG AltSetting;          /* SCO interface setting UdeCx last reported */
    ULONG AltChanges;
    ULONG OutMaxPacket;        /* wMaxPacketSize of the endpoint the latest URB used */
    ULONG InMaxPacket;
    ULONG OutUrbs;
    ULONG InUrbs;
    ULONG OutIsoPackets;
    ULONG InIsoPackets;
    ULONG OutBytes;            /* USB bytes from BTHUSB */
    ULONG InBytes;             /* USB bytes to BTHUSB */
    ULONG OutHciPackets;       /* reassembled packets the bridge accepted */
    ULONG OutRejected;         /* reassembled packets the bridge refused */
    ULONG OutRingDrops;
    ULONG OutResyncSkips;
    ULONG InHciPackets;        /* packets framed for the host */
    ULONG InSourcePackets;     /* controller packets consumed */
    ULONG InSourceRejected;
    ULONG InDroppedBytes;
    ULONG InLastSourceLength;  /* payload length the controller uses on the UART */
    ULONG BadUrbs;             /* not an isochronous transfer, or no buffer */
    ULONG UnpacedUrbs;         /* completed at once: no request context */
    ULONG Flushed;             /* parked requests cancelled by a setting change or reset */
    ULONG FirstOutGeometry;    /* NumberOfPackets << 16 | first packet length */
    ULONG FirstInGeometry;     /* NumberOfPackets << 16 | transfer buffer length */
    ULONG LastTransferFlags;
    ULONG MaxLateUs;           /* worst completion lateness against the frame clock */
    ULONG LastOutHeader;       /* first three bytes of the latest packet sent to the controller */
    ULONG SetupCommands;       /* synchronous-connection commands BTHPORT sent */
} DECKBT_SCO_STATS;

/* Per-URB pacing state for a parked SCO request. */
typedef struct _DECKBT_SCO_REQUEST {
    ULONGLONG   Due;           /* interrupt time at which the transfer completes */
    ULONG       Bytes;         /* OUT: bytes accepted at arrival */
    USBD_STATUS Status;        /* OUT: overall status decided at arrival */
    USHORT      MaxPacketSize; /* IN: packet size of the endpoint it arrived on */
} DECKBT_SCO_REQUEST, *PDECKBT_SCO_REQUEST;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DECKBT_SCO_REQUEST, DeckBtGetScoRequest)

#define DECKBT_SCO_CMD_TRACE_BYTES 64u

/* Root-enumerated controller device (FDO). */
typedef struct _DECKBT_CONTROLLER {
    WDFDEVICE       WdfDevice;
    UDECXUSBDEVICE  UsbDevice;

    /* Endpoint objects, created on demand by EvtUsbDeviceEndpointAdd. */
    UDECXUSBENDPOINT Ep0;
    UDECXUSBENDPOINT EpEventIn;
    UDECXUSBENDPOINT EpAclOut;
    UDECXUSBENDPOINT EpAclIn;
    UDECXUSBENDPOINT EpScoOut;
    UDECXUSBENDPOINT EpScoIn;

    /* Manual queue holding interrupt-IN requests waiting for an HCI event. */
    WDFQUEUE        EventQueue;
    /* Manual queue holding bulk-IN requests waiting for ACL data. */
    WDFQUEUE        AclInQueue;

    /* Backend storage is initialized once with the controller context. */
    HCI_STUB        Hci;
    QCA_UART        Qca;

    /* Transport seam between Windows-facing USB frontend and the selected backend. */
    HCI_TRANSPORT   Transport;

    /* The single lock domain for the selected backend and all transport calls. */
    WDFSPINLOCK     Lock;

    /* Backend whose hardware lifecycle is active for the current prepare/release cycle. */
    ULONG           ActiveBackend;
    BOOLEAN         BackendPrepared;
    BOOLEAN         StubArmClaimed;

    /*
     * Binary gate (SynchronizationEvent, initially signaled) serializing the UART worker's
     * late USB plug-in with ReleaseHardware over BackendPrepared and UsbDevice.
     */
    KEVENT          PlugGate;

    /*
     * A steady UART session that D0Exit stopped for a sleep state (not removal) is started
     * again on D0 entry, and its USB child is replaced so BTHPORT re-initialises the reloaded
     * controller instead of trusting state from before the sleep.
     */
    BOOLEAN         RearmOnD0Entry;
    ULONG           ResumeRearms;
    /* Replacement children plugged in after a re-arm, and when the last re-arm began. */
    ULONG           ResumeReplacements;
    ULONGLONG       ResumeStartedAt;

    /*
     * SCO voice (endpoints 0x03/0x83). Requests park in these manual queues until their frame
     * time on the virtual 1 ms clock; ScoTimer (one-shot, re-armed while anything is parked)
     * releases OUT voice to the bridge and completes due transfers. Everything below is guarded
     * by Lock.
     */
    WDFQUEUE        ScoOutQueue;
    WDFQUEUE        ScoInQueue;
    WDFTIMER        ScoTimer;
    BOOLEAN         ScoTimerArmed;
    SCO_USB_CLOCK   ScoOutClock;
    SCO_USB_CLOCK   ScoInClock;
    SCO_USB_OUT     ScoOut;
    SCO_USB_IN      ScoIn;
    DECKBT_SCO_STATS ScoStats;
    ULONGLONG       ScoPublishedAt;
    /* Latest synchronous-connection command from BTHPORT (Setup/Accept, Write_Voice_Setting). */
    UCHAR           ScoCmdTrace[DECKBT_SCO_CMD_TRACE_BYTES];

    /*
     * Rolling trace of EP0 setup packets. 8 bytes of setup + 4 bytes of outcome per slot,
     * republished to the registry after every control transfer. This exists because BTHUSB's
     * startup failure is otherwise invisible on a machine that cannot be kernel-debugged.
     */
    UCHAR           CtlLog[DECKBT_CTL_LOG_SLOTS][DECKBT_CTL_LOG_STRIDE];
    ULONG           CtlCount;
} DECKBT_CONTROLLER, *PDECKBT_CONTROLLER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DECKBT_CONTROLLER, DeckBtGetController)

/* Endpoint context: lets a queue's callbacks find both the controller and which EP fired. */
typedef struct _DECKBT_ENDPOINT {
    PDECKBT_CONTROLLER Controller;
    UCHAR              Address;
    USHORT             MaxPacketSize;  /* from the endpoint descriptor UdeCx created it with */
} DECKBT_ENDPOINT, *PDECKBT_ENDPOINT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DECKBT_ENDPOINT, DeckBtGetEndpoint)

/* driver.c - progress recorder. With no kernel debugger available on the Deck, each
 * significant step writes its code and NTSTATUS to
 *   HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters
 * (LastAddDeviceStep / LastAddDeviceStatus) so a failure can be located from user mode. */
VOID DeckBtRecordStep(_In_ ULONG Step, _In_ NTSTATUS Status);

#define DECKBT_STEP_PREPARE_HW          20
#define DECKBT_STEP_USB_DEVICE_CREATED  21
#define DECKBT_STEP_PLUGGED_IN          22
#define DECKBT_STEP_DISARMED            23
#define DECKBT_STEP_ARMED               24
#define DECKBT_STEP_BACKEND_STUB        25
#define DECKBT_STEP_BACKEND_UART        26
#define DECKBT_STEP_BACKEND_UNKNOWN     27
#define DECKBT_STEP_ARM_PENDING         28
#define DECKBT_STEP_ARM_CONSUME         29

#define DECKBT_STEP_UDEV_INIT_ALLOC     30
#define DECKBT_STEP_UDEV_DESC_DEVICE    31
#define DECKBT_STEP_UDEV_DESC_CONFIG    32
#define DECKBT_STEP_UDEV_DESC_LANGIDS   33
#define DECKBT_STEP_UDEV_DESC_STRINGS   34
#define DECKBT_STEP_UDEV_CREATE         35
#define DECKBT_STEP_UDEV_PLUGIN         36
#define DECKBT_STEP_EP_DEFAULT_ADD      40
#define DECKBT_STEP_EP_ADD              41
#define DECKBT_STEP_USB_RESET           42

#define DECKBT_STEP_PROBE_MALFORMED     50
#define DECKBT_STEP_PROBE_ENTER         51

/* Reads a DWORD from the service Parameters key (PASSIVE_LEVEL); Default when absent. */
ULONG DeckBtReadParameter(_In_z_ PCWSTR Name, _In_ ULONG Default);
/* UART probe progress recorder */
VOID DeckBtRecordProbeProgress(_In_ const QCA_UART_RECORD *Record);
/* Publishes a steady-state trace: REG_BINARY LogName (Bytes) and DWORD CountName (Count). */
VOID DeckBtRecordTrace(_In_z_ PCWSTR LogName, _In_z_ PCWSTR CountName,
                       _In_reads_bytes_(Bytes) const UCHAR *Data, _In_ ULONG Bytes, _In_ ULONG Count);
/* driver.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           DeckBtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE     DeckBtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     DeckBtEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY            DeckBtEvtControllerD0Entry;
EVT_WDF_DEVICE_D0_EXIT             DeckBtEvtControllerD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL    DeckBtEvtSurpriseRemoval;
EVT_WDF_DRIVER_UNLOAD              DeckBtEvtDriverUnload;
extern EX_RUNDOWN_REF              DeckBtProbeRundown;
NTSTATUS DeckBtCreateProbeThread(_In_ PKSTART_ROUTINE StartRoutine, _In_ PVOID Context);
/*
 * UART backend readiness: plugs in the USB child once the controller answers through the
 * bridge. Called by the UART worker at PASSIVE_LEVEL; a release that has begun wins.
 */
NTSTATUS DeckBtPublishUartDevice(_In_ PQCA_UART Uart);

/* device.c */
NTSTATUS DeckBtCreateUsbDevice(_In_ PDECKBT_CONTROLLER Controller);

EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY    DeckBtEvtQueryUsbCapability;
EVT_UDECX_USB_DEVICE_DEFAULT_ENDPOINT_ADD    DeckBtEvtDefaultEndpointAdd;
EVT_UDECX_USB_DEVICE_ENDPOINT_ADD            DeckBtEvtEndpointAdd;
EVT_UDECX_USB_DEVICE_ENDPOINTS_CONFIGURE     DeckBtEvtEndpointsConfigure;
EVT_UDECX_USB_DEVICE_D0_ENTRY                DeckBtEvtDeviceD0Entry;
EVT_UDECX_USB_DEVICE_D0_EXIT                 DeckBtEvtDeviceD0Exit;
EVT_UDECX_USB_DEVICE_POST_ENUMERATION_RESET  DeckBtEvtDeviceReset;

/* endpoints.c */
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL  DeckBtEvtControlUrb;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL  DeckBtEvtDataUrb;
EVT_UDECX_USB_ENDPOINT_RESET                 DeckBtEvtEndpointReset;

/* Appends one EP0 setup packet plus its outcome to the trace and republishes it. */
VOID DeckBtLogControl(_In_ PDECKBT_CONTROLLER Controller,
                      _In_reads_bytes_(8) const UCHAR *Setup,
                      _In_ ULONG Outcome,
                      _In_ USHORT Opcode,
                      _In_ UCHAR EventLength);

/* Completes as many queued requests on Stream as there are packets ready. */
VOID DeckBtDrainStream(_In_ PDECKBT_CONTROLLER Controller, _In_ HCI_STREAM Stream);

/* Completes as many queued interrupt-IN requests as there are queued HCI events. */
VOID DeckBtDrainEvents(_In_ PDECKBT_CONTROLLER Controller);

/* SCO voice (endpoints.c). Flush cancels parked transfers and restarts framing and clocks. */
EVT_WDF_TIMER DeckBtEvtScoTimer;
VOID DeckBtScoFlush(_In_ PDECKBT_CONTROLLER Controller);
VOID DeckBtScoPublish(_In_ PDECKBT_CONTROLLER Controller, _In_ BOOLEAN Force);

/* Notify callback from the transport seam to wake up stream drains. */
VOID DeckBtTransportNotify(void *NotifyContext, HCI_STREAM Stream);

#define DECKBT_POOL_TAG 'BkcD'  /* "DckB" */
