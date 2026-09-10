/*
 * deckbtusb.h - VirtBthUsb Virtual Bluetooth Host Controller driver (KMDF / UdeCx).
 */

#pragma once

/* Include order is load-bearing: wdfusb.h needs USB_REQUEST_* / USBD_STATUS from usb.h, and
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

#define DECKBT_CTL_LOG_SLOTS  128u   /* a full BTHUSB bring-up is ~140 transfers */
#define DECKBT_CTL_LOG_STRIDE 12u

/* Outcome codes stored in the trace. */
#define DECKBT_CTL_OK_HCI      1u
#define DECKBT_CTL_STALLED     2u
#define DECKBT_CTL_BADBUFFER   3u
#define DECKBT_CTL_NOTCONTROL  4u
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

    /* Synthetic controller state, including the pending-event FIFO. */
    HCI_STUB        Hci;

    /* Guards Hci and the pend/complete handoff. */
    WDFSPINLOCK     Lock;

    /* Current alternate setting selected on the SCO interface (0..6). */
    UCHAR           ScoAltSetting;

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
} DECKBT_ENDPOINT, *PDECKBT_ENDPOINT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DECKBT_ENDPOINT, DeckBtGetEndpoint)

/* driver.c - progress recorder. With no kernel debugger available on the Deck, each
 * significant step writes its code and NTSTATUS to
 *   HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters
 * (LastAddDeviceStep / LastAddDeviceStatus) so a failure can be located from user mode. */
VOID DeckBtRecordStep(_In_ ULONG Step, _In_ NTSTATUS Status);

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

/* driver.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD           DeckBtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE     DeckBtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE     DeckBtEvtDeviceReleaseHardware;

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

/* Completes as many queued interrupt-IN requests as there are queued HCI events. */
VOID DeckBtDrainEvents(_In_ PDECKBT_CONTROLLER Controller);

#define DECKBT_POOL_TAG 'BkcD'  /* "DckB" */
