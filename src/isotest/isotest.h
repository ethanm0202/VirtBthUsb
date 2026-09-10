/*
 * isotest.h - Kernel-mode declarations for DeckBtIsoTest UDE driver (Stage 2).
 *
 * This driver creates a throwaway UDE host controller and plugs in a high-speed,
 * non-composite, vendor-class USB device with the exact same isochronous endpoint
 * geometry as the DeckBtUsb virtual Bluetooth radio (Interface 1 alt settings 0..6,
 * packet sizes 0, 9, 17, 25, 33, 49, 63, interval 4).
 *
 * It binds to WinUSB so user-mode test harness can drive isochronous traffic
 * directly without involving BTHUSB or any Bluetooth semantics.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <wdfusb.h>
#include <usbiodef.h>
#include <UdeCx.h>
#include "isotest_descriptors.h"

/* Step codes written to registry breadcrumbs */
#define ISOTEST_STEP_ENTER               1
#define ISOTEST_STEP_UDECX_INIT          2
#define ISOTEST_STEP_DEVICE_CREATE       3
#define ISOTEST_STEP_DEVICE_INTERFACE    4
#define ISOTEST_STEP_SPINLOCK            5
#define ISOTEST_STEP_QUEUE_CREATE        6
#define ISOTEST_STEP_ADD_EMULATION       7
#define ISOTEST_STEP_ADD_DONE            8
#define ISOTEST_STEP_PREPARE_HW         20
#define ISOTEST_STEP_UDEV_INIT_ALLOC    21
#define ISOTEST_STEP_UDEV_DESC          22
#define ISOTEST_STEP_UDEV_CREATE        23
#define ISOTEST_STEP_UDEV_PLUGIN        24
#define ISOTEST_STEP_ALT_CHANGED        30
#define ISOTEST_STEP_URB_ISOCH_OUT      40
#define ISOTEST_STEP_URB_ISOCH_IN       41
#define ISOTEST_STEP_URB_HELD           42
#define ISOTEST_STEP_URB_CANCELLED      43
#define ISOTEST_STEP_URB_RELEASED       44

/* Vendor control requests on EP0 */
#define ISOTEST_BMREQUEST_VENDOR_OUT    0x40u
#define ISOTEST_BMREQUEST_VENDOR_IN     0xC0u
#define ISOTEST_VENDOR_REQ_HOLD_NEXT    0xAAu
#define ISOTEST_VENDOR_REQ_RELEASE_HELD 0xABu

/* History record for registry breadcrumbs */
#define ISOTEST_URB_LOG_SLOTS           16u

typedef struct _ISOTEST_ISOCH_RECORD {
    ULONG Sequence;
    UCHAR Endpoint;
    UCHAR Direction;     /* 0 = OUT, 1 = IN */
    UCHAR AltSetting;
    UCHAR Reserved;
    ULONG NumberOfPackets;
    ULONG TransferBufferLength;
    ULONG PacketLength0;
    ULONG UsbdStatus;
    ULONG BytesCompleted;
    ULONG Flags;
    ULONG EndpointMaxPacketSize;
    ULONG Reserved2[7];  /* pads to 64 bytes (0x40 fixed stride) */
} ISOTEST_ISOCH_RECORD, *PISOTEST_ISOCH_RECORD;

struct _ISOTEST_CONTROLLER;

typedef struct _ISOTEST_ENDPOINT {
    struct _ISOTEST_CONTROLLER *Controller;
    ULONG MaxPacketSize;
    UCHAR Address;
    UCHAR Reserved[3];
} ISOTEST_ENDPOINT, *PISOTEST_ENDPOINT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ISOTEST_ENDPOINT, IsoTestGetEndpoint)

typedef enum _ISOTEST_HOLD_STATE {
    IsoTestHoldIdle = 0,
    IsoTestHoldArmed,
    IsoTestHoldActive
} ISOTEST_HOLD_STATE;

typedef struct _ISOTEST_CONTROLLER {
    WDFDEVICE WdfDevice;
    UDECXUSBDEVICE UsbDevice;
    WDFSPINLOCK Lock;

    UDECXUSBENDPOINT Ep0;
    UDECXUSBENDPOINT EpEventIn;
    UDECXUSBENDPOINT EpBulkOut;
    UDECXUSBENDPOINT EpBulkIn;
    UDECXUSBENDPOINT EpIsochOut;
    UDECXUSBENDPOINT EpIsochIn;
    WDFQUEUE PendingIsochQueue;
    ISOTEST_HOLD_STATE HoldState;
    BOOLEAN PendingCancelled;
    ULONG HoldCommandCount;
    ULONG HeldTransferCount;
    ULONG CancelCallbackCount;
    ULONG ReleaseCommandCount;
    UCHAR CurrentAltSetting;
    ULONG EndpointsConfigureCount;
    ULONG LastConfigureType;
    UCHAR LastConfigureInterface;
    UCHAR LastConfigureSetting;

    ULONG IsochUrbCount;
    UCHAR LastIsochEndpoint;
    ULONG LastIsochNumPackets;
    ULONG LastIsochBufferLength;
    ULONG LastIsochPacketLength0;
    ULONG LastIsochUsbdStatus;
    ULONG LastIsochMaxPacketSize;

    /*
     * Whole-run aggregate counters.
     * The retained UrbLog window is only 16 entries (ISOTEST_URB_LOG_SLOTS),
     * so tail records can never substantiate a claim about a whole run.
     * These counters accumulate over every isochronous URB processed.
     */
    ULONG IsochUrbCountIn;
    ULONG IsochUrbCountOut;
    ULONG IsochNonSuccessCount;
    ULONG IsochShortCompletionCount;
    ULONG IsochPacketSizeMismatchCount;
    ULONG IsochBytesRequestedIn;
    ULONG IsochBytesCompletedIn;
    ULONG IsochBytesRequestedOut;
    ULONG IsochBytesCompletedOut;

    ISOTEST_ISOCH_RECORD UrbLog[ISOTEST_URB_LOG_SLOTS];
} ISOTEST_CONTROLLER, *PISOTEST_CONTROLLER;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(ISOTEST_CONTROLLER, IsoTestGetController)


/* Lifecycle & device callbacks */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD IsoTestEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE IsoTestEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE IsoTestEvtDeviceReleaseHardware;

EVT_UDECX_WDF_DEVICE_QUERY_USB_CAPABILITY IsoTestEvtQueryUsbCapability;
EVT_UDECX_USB_DEVICE_D0_ENTRY IsoTestEvtDeviceD0Entry;
EVT_UDECX_USB_DEVICE_D0_EXIT IsoTestEvtDeviceD0Exit;
EVT_UDECX_USB_DEVICE_POST_ENUMERATION_RESET IsoTestEvtDeviceReset;
EVT_UDECX_USB_DEVICE_DEFAULT_ENDPOINT_ADD IsoTestEvtDefaultEndpointAdd;
EVT_UDECX_USB_DEVICE_ENDPOINT_ADD IsoTestEvtEndpointAdd;
EVT_UDECX_USB_DEVICE_ENDPOINTS_CONFIGURE IsoTestEvtEndpointsConfigure;
EVT_UDECX_USB_ENDPOINT_RESET IsoTestEvtEndpointReset;

EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL IsoTestEvtControlUrb;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL IsoTestEvtDataUrb;

NTSTATUS IsoTestCreateUsbDevice(_In_ PISOTEST_CONTROLLER Controller);
VOID IsoTestRecordStep(_In_ ULONG Step, _In_ NTSTATUS Status);
VOID IsoTestLogConfigure(_In_ PISOTEST_CONTROLLER Controller, _In_ ULONG Type,
                         _In_ UCHAR Iface, _In_ UCHAR Setting);
EVT_WDF_IO_QUEUE_IO_CANCELED_ON_QUEUE IsoTestEvtHeldRequestCanceled;
VOID IsoTestLogIsoch(_In_ PISOTEST_CONTROLLER Controller, _In_ UCHAR Endpoint, _In_ UCHAR Direction,
                     _In_ ULONG NumPackets, _In_ ULONG BufferLen, _In_ ULONG PacketLen0,
                     _In_ ULONG UsbdStatus, _In_ ULONG BytesDone, _In_ ULONG Flags,
                     _In_ ULONG EndpointMaxPacketSize);
VOID IsoTestResetAggregates(_In_ PISOTEST_CONTROLLER Controller);
VOID IsoTestReleaseHeldRequest(_In_ PISOTEST_CONTROLLER Controller, _In_ BOOLEAN Cancel);
VOID IsoTestLogHoldTelemetry(_In_ PISOTEST_CONTROLLER Controller);
