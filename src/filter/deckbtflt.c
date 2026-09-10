/*
 * deckbtflt.c - Lower filter driver attached beneath BTHUSB.SYS or WinUSB.
 *
 * Intercepts and logs IRP_MN_QUERY_INTERFACE requests for USB bus interfaces,
 * and provides optional QueryBusTime synthesis for UdeCx isochronous support.
 *
 * Isochronous clock modes (configured via IsochClockMode REG_DWORD in service Parameters):
 * - Mode 0 (default): Pass-through only. Forwards requests unmodified.
 * - Mode 1: Probe and log. Hooks QueryBusTime/QueryBusTimeEx to call the underlying stack
 *   and records call statistics to the registry.
 * - Mode 2: Synthesize. Intercepts QueryBusTime/QueryBusTimeEx and provides a monotonic frame
 *   clock derived from KeQueryPerformanceCounter when the underlying call returns an error.
 */

#include <initguid.h>
#include <ntddk.h>
#include <stddef.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbbusif.h>

#define DECKBTFLT_LOG_SLOTS    32u
#define DECKBTFLT_POOL_TAG     'tFkD'
#define DECKBTFLT_MAX_DEVICES  4

/* One recorded QUERY_INTERFACE, 32 bytes, fixed layout so user mode can decode it. */
typedef struct _DECKBTFLT_ENTRY {
    UCHAR  Guid[16];      /* InterfaceType                                    */
    USHORT Size;          /* requested Size                                   */
    USHORT Version;       /* requested Version                                */
    ULONG  Status;        /* NTSTATUS returned by the lower driver stack      */
    ULONG  Minor;         /* IRP_MN_* (QUERY_INTERFACE = 0x08)                */
    ULONG  Sequence;      /* Monotonic sequence number                        */
} DECKBTFLT_ENTRY;

typedef struct _DECKBTFLT_CONTEXT {
    WDFDEVICE                   Device;
    WDFWORKITEM                 PublishWorkItem;
    DECKBTFLT_ENTRY             Log[DECKBTFLT_LOG_SLOTS];
    ULONG                       Count;
    ULONG                       PnpIrps;

    /* Mode configuration and active state */
    ULONG                       IsochClockMode;
    ULONG                       IsochClockModeActive;
    ULONG                       IsochHookInstalled;
    ULONG                       IsochHookSynthesizing;

    /* USBDI query observation */
    ULONG                       UsbdiQueryCount;
    ULONG                       UsbdiLastRequestedSize;
    ULONG                       UsbdiLastRequestedVersion;
    ULONG                       UsbdiLastStatus;

    /* Bus time call counters and status */
    ULONG                       QueryBusTimeCalls;
    ULONG                       QueryBusTimeExCalls;
    ULONG                       QueryBusTimeLastStatus;
    ULONG                       QueryBusTimeLastFrame;
    ULONG                       QueryBusTimeUnderlyingStatus;

    /* Bus time probe results */
    ULONG                       QueryBusTimeProbeStatus;
    ULONG                       QueryBusTimeExProbeStatus;
    LONG                        FallbackFrameCounter;
    /* Saved lower stack interface members */
    PVOID                       OriginalBusContext;
    PUSB_BUSIFFN_QUERY_BUS_TIME OriginalQueryBusTime;
    PUSB_BUSIFFN_QUERY_BUS_TIME_EX OriginalQueryBusTimeEx;
} DECKBTFLT_CONTEXT, *PDECKBTFLT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DECKBTFLT_CONTEXT, DeckBtFltGetContext)

static WDFDRIVER g_Driver = NULL;
static PDECKBTFLT_CONTEXT g_FilterDevices[DECKBTFLT_MAX_DEVICES];
static LONG g_FilterDeviceCount = 0;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DeckBtFltEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP DeckBtFltEvtDeviceCleanup;
EVT_WDF_WORKITEM DeckBtFltEvtWorkItem;

static VOID
DeckBtFltRegisterDevice(_In_ PDECKBTFLT_CONTEXT Context)
{
    LONG i;
    for (i = 0; i < DECKBTFLT_MAX_DEVICES; i++) {
        if (InterlockedCompareExchangePointer((PVOID volatile *)&g_FilterDevices[i], Context, NULL) == NULL) {
            InterlockedIncrement(&g_FilterDeviceCount);
            break;
        }
    }
}

static VOID
DeckBtFltUnregisterDevice(_In_ PDECKBTFLT_CONTEXT Context)
{
    LONG i;
    for (i = 0; i < DECKBTFLT_MAX_DEVICES; i++) {
        if (InterlockedCompareExchangePointer((PVOID volatile *)&g_FilterDevices[i], NULL, Context) == Context) {
            InterlockedDecrement(&g_FilterDeviceCount);
            break;
        }
    }
}

static PDECKBTFLT_CONTEXT
DeckBtFltFindContext(_In_opt_ PVOID BusContext)
{
    LONG i;
    if (BusContext != NULL) {
        /* Check if BusContext directly matches a device context */
        for (i = 0; i < DECKBTFLT_MAX_DEVICES; i++) {
            PDECKBTFLT_CONTEXT ctx = g_FilterDevices[i];
            if (ctx != NULL && (PVOID)ctx == BusContext) {
                return ctx;
            }
        }
        /* Check if BusContext matches the saved lower OriginalBusContext */
        for (i = 0; i < DECKBTFLT_MAX_DEVICES; i++) {
            PDECKBTFLT_CONTEXT ctx = g_FilterDevices[i];
            if (ctx != NULL && ctx->OriginalBusContext == BusContext) {
                return ctx;
            }
        }
    }
    /* Fallback: return the first active registered device */
    for (i = 0; i < DECKBTFLT_MAX_DEVICES; i++) {
        PDECKBTFLT_CONTEXT ctx = g_FilterDevices[i];
        if (ctx != NULL) {
            return ctx;
        }
    }
    return NULL;
}

static ULONG
DeckBtFltReadIsochClockMode(_In_ WDFDRIVER Driver)
{
    WDFKEY key = NULL;
    NTSTATUS status;
    ULONG mode = 0;
    DECLARE_CONST_UNICODE_STRING(valName, L"IsochClockMode");

    if (Driver == NULL) {
        return 0;
    }

    status = WdfDriverOpenParametersRegistryKey(Driver, KEY_READ,
                                                WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return 0;
    }

    status = WdfRegistryQueryULong(key, &valName, &mode);
    if (!NT_SUCCESS(status)) {
        mode = 0;
    }

    WdfRegistryClose(key);
    return mode;
}

static VOID
DeckBtFltPublishLog(_In_ PDECKBTFLT_CONTEXT Context)
{
    WDFKEY key = NULL;
    UNICODE_STRING nameBlob;
    UNICODE_STRING nameCount;
    UNICODE_STRING namePnp;

    if (g_Driver == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(g_Driver, KEY_SET_VALUE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    /* Fixed layout entries for existing tools and PowerShell decoders */
    RtlInitUnicodeString(&nameBlob, L"QueryInterfaceLog");
    RtlInitUnicodeString(&nameCount, L"QueryInterfaceCount");
    RtlInitUnicodeString(&namePnp, L"PnpIrpCount");
    (void)WdfRegistryAssignValue(key, &nameBlob, REG_BINARY, sizeof(Context->Log), Context->Log);
    (void)WdfRegistryAssignULong(key, &nameCount, Context->Count);
    (void)WdfRegistryAssignULong(key, &namePnp, Context->PnpIrps);

    /* Publish extended diagnostic counters when IsochClockMode is active (mode 1 or 2) */
    if (Context->IsochClockMode != 0) {
        UNICODE_STRING nameModeActive;
        UNICODE_STRING nameHookInstalled;
        UNICODE_STRING nameHookSynthesizing;
        UNICODE_STRING nameUsbdiQueryCount;
        UNICODE_STRING nameUsbdiLastReqSize;
        UNICODE_STRING nameUsbdiLastReqVer;
        UNICODE_STRING nameUsbdiLastStatus;
        UNICODE_STRING nameQbtCalls;
        UNICODE_STRING nameQbtExCalls;
        UNICODE_STRING nameQbtLastStatus;
        UNICODE_STRING nameQbtLastFrame;
        UNICODE_STRING nameQbtUnderlyingStatus;
        UNICODE_STRING nameQbtProbeStatus;
        UNICODE_STRING nameQbtExProbeStatus;

        RtlInitUnicodeString(&nameModeActive, L"IsochClockModeActive");
        RtlInitUnicodeString(&nameHookInstalled, L"IsochHookInstalled");
        RtlInitUnicodeString(&nameHookSynthesizing, L"IsochHookSynthesizing");
        RtlInitUnicodeString(&nameUsbdiQueryCount, L"UsbdiQueryCount");
        RtlInitUnicodeString(&nameUsbdiLastReqSize, L"UsbdiLastRequestedSize");
        RtlInitUnicodeString(&nameUsbdiLastReqVer, L"UsbdiLastRequestedVersion");
        RtlInitUnicodeString(&nameUsbdiLastStatus, L"UsbdiLastStatus");
        RtlInitUnicodeString(&nameQbtCalls, L"QueryBusTimeCalls");
        RtlInitUnicodeString(&nameQbtExCalls, L"QueryBusTimeExCalls");
        RtlInitUnicodeString(&nameQbtLastStatus, L"QueryBusTimeLastStatus");
        RtlInitUnicodeString(&nameQbtLastFrame, L"QueryBusTimeLastFrame");
        RtlInitUnicodeString(&nameQbtUnderlyingStatus, L"QueryBusTimeUnderlyingStatus");
        RtlInitUnicodeString(&nameQbtProbeStatus, L"QueryBusTimeProbeStatus");
        RtlInitUnicodeString(&nameQbtExProbeStatus, L"QueryBusTimeExProbeStatus");

        (void)WdfRegistryAssignULong(key, &nameModeActive, Context->IsochClockModeActive);
        (void)WdfRegistryAssignULong(key, &nameHookInstalled, Context->IsochHookInstalled);
        (void)WdfRegistryAssignULong(key, &nameHookSynthesizing, Context->IsochHookSynthesizing);
        (void)WdfRegistryAssignULong(key, &nameUsbdiQueryCount, Context->UsbdiQueryCount);
        (void)WdfRegistryAssignULong(key, &nameUsbdiLastReqSize, Context->UsbdiLastRequestedSize);
        (void)WdfRegistryAssignULong(key, &nameUsbdiLastReqVer, Context->UsbdiLastRequestedVersion);
        (void)WdfRegistryAssignULong(key, &nameUsbdiLastStatus, Context->UsbdiLastStatus);
        (void)WdfRegistryAssignULong(key, &nameQbtCalls, Context->QueryBusTimeCalls);
        (void)WdfRegistryAssignULong(key, &nameQbtExCalls, Context->QueryBusTimeExCalls);
        (void)WdfRegistryAssignULong(key, &nameQbtLastStatus, Context->QueryBusTimeLastStatus);
        (void)WdfRegistryAssignULong(key, &nameQbtLastFrame, Context->QueryBusTimeLastFrame);
        (void)WdfRegistryAssignULong(key, &nameQbtUnderlyingStatus, Context->QueryBusTimeUnderlyingStatus);
        (void)WdfRegistryAssignULong(key, &nameQbtProbeStatus, Context->QueryBusTimeProbeStatus);
        (void)WdfRegistryAssignULong(key, &nameQbtExProbeStatus, Context->QueryBusTimeExProbeStatus);
    }

    WdfRegistryClose(key);
}

VOID
DeckBtFltEvtWorkItem(_In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PDECKBTFLT_CONTEXT context = DeckBtFltGetContext(device);
    DeckBtFltPublishLog(context);
}

static VOID
DeckBtFltRequestPublish(_In_ PDECKBTFLT_CONTEXT Context)
{
    if (Context == NULL) {
        return;
    }
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        DeckBtFltPublishLog(Context);
    } else if (Context->PublishWorkItem != NULL && KeGetCurrentIrql() <= DISPATCH_LEVEL) {
        WdfWorkItemEnqueue(Context->PublishWorkItem);
    }
}

static VOID
DeckBtFltRecord(
    _In_ PDECKBTFLT_CONTEXT Context,
    _In_ const GUID *InterfaceType,
    _In_ USHORT Size,
    _In_ USHORT Version,
    _In_ NTSTATUS Status,
    _In_ ULONG Minor)
{
    ULONG slot = Context->Count % DECKBTFLT_LOG_SLOTS;
    DECKBTFLT_ENTRY *e = &Context->Log[slot];

    RtlCopyMemory(e->Guid, InterfaceType, sizeof(e->Guid));
    e->Size     = Size;
    e->Version  = Version;
    e->Status   = (ULONG)Status;
    e->Minor    = Minor;
    e->Sequence = Context->Count;
    Context->Count++;

    DeckBtFltRequestPublish(Context);
}

/*
 * Synthetic clock derived from KeQueryPerformanceCounter().
 *
 * QueryBusTime returns a 1 kHz frame counter (1 ms units).
 * QueryBusTimeEx returns an 8 kHz microframe counter (125 microsecond units, frame << 3).
 */
static ULONG
DeckBtFltGetSyntheticBusFrame(_Inout_ PDECKBTFLT_CONTEXT Context)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    counter = KeQueryPerformanceCounter(&frequency);
    if (frequency.QuadPart > 0 && counter.QuadPart >= 0) {
        return (ULONG)((ULONG64)counter.QuadPart * 1000ULL / (ULONG64)frequency.QuadPart);
    }

    /* Fallback monotonic counter if reported frequency is non-positive */
    return (ULONG)InterlockedIncrement(&Context->FallbackFrameCounter);
}

/*
 * Thunks over QueryBusTime and QueryBusTimeEx
 */

static NTSTATUS USB_BUSIFFN
DeckBtFltHookQueryBusTime(
    _In_opt_ PVOID BusContext,
    _Out_opt_ PULONG CurrentUsbFrame)
{
    PDECKBTFLT_CONTEXT context = DeckBtFltFindContext(BusContext);
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    ULONG frame = 0;

    if (context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    InterlockedIncrement((PLONG)&context->QueryBusTimeCalls);

    /* Call underlying implementation if available */
    if (context->OriginalQueryBusTime != NULL) {
        status = context->OriginalQueryBusTime(context->OriginalBusContext, &frame);
    }
    InterlockedExchange((PLONG)&context->QueryBusTimeUnderlyingStatus, (LONG)status);

    if (context->IsochClockMode == 2 &&
        (context->OriginalQueryBusTime == NULL || NT_ERROR(status))) {
        frame = DeckBtFltGetSyntheticBusFrame(context);
        status = STATUS_SUCCESS;
        InterlockedExchange((PLONG)&context->IsochHookSynthesizing, 1);
    }

    if (CurrentUsbFrame != NULL) {
        *CurrentUsbFrame = frame;
    }

    InterlockedExchange((PLONG)&context->QueryBusTimeLastFrame, (LONG)frame);
    InterlockedExchange((PLONG)&context->QueryBusTimeLastStatus, (LONG)status);

    DeckBtFltRequestPublish(context);
    return status;
}

static NTSTATUS USB_BUSIFFN
DeckBtFltHookQueryBusTimeEx(
    _In_opt_ PVOID BusContext,
    _Out_opt_ PULONG HighSpeedFrameCounter)
{
    PDECKBTFLT_CONTEXT context = DeckBtFltFindContext(BusContext);
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    ULONG microframe = 0;

    if (context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    InterlockedIncrement((PLONG)&context->QueryBusTimeExCalls);

    /* Call underlying implementation if available */
    if (context->OriginalQueryBusTimeEx != NULL) {
        status = context->OriginalQueryBusTimeEx(context->OriginalBusContext, &microframe);
    }
    InterlockedExchange((PLONG)&context->QueryBusTimeUnderlyingStatus, (LONG)status);

    if (context->IsochClockMode == 2 &&
        (context->OriginalQueryBusTimeEx == NULL || NT_ERROR(status))) {
        microframe = DeckBtFltGetSyntheticBusFrame(context) << 3;
        status = STATUS_SUCCESS;
        InterlockedExchange((PLONG)&context->IsochHookSynthesizing, 1);
    }

    if (HighSpeedFrameCounter != NULL) {
        *HighSpeedFrameCounter = microframe;
    }

    InterlockedExchange((PLONG)&context->QueryBusTimeLastFrame, (LONG)microframe);
    InterlockedExchange((PLONG)&context->QueryBusTimeLastStatus, (LONG)status);

    DeckBtFltRequestPublish(context);
    return status;
}

/*
 * Synthetic interface callbacks used only when the underlying stack completely fails
 * the IRP_MN_QUERY_INTERFACE request for USB_BUS_INTERFACE_USBDI in Mode 2.
 */

static VOID
DeckBtFltSyntheticInterfaceReference(_In_opt_ PVOID BusContext)
{
    PDECKBTFLT_CONTEXT context = DeckBtFltFindContext(BusContext);
    if (context != NULL && context->Device != NULL) {
        WdfObjectReference(context->Device);
    }
}

static VOID
DeckBtFltSyntheticInterfaceDereference(_In_opt_ PVOID BusContext)
{
    PDECKBTFLT_CONTEXT context = DeckBtFltFindContext(BusContext);
    if (context != NULL && context->Device != NULL) {
        WdfObjectDereference(context->Device);
    }
}

static VOID USB_BUSIFFN
DeckBtFltSyntheticGetUSBDIVersion(
    _In_opt_ PVOID BusContext,
    _Out_opt_ PUSBD_VERSION_INFORMATION VersionInformation,
    _Out_opt_ PULONG HcdCapabilities)
{
    UNREFERENCED_PARAMETER(BusContext);

    if (VersionInformation != NULL) {
        VersionInformation->USBDI_Version = 0x00000500; /* Usbport */
        VersionInformation->Supported_USB_Version = 0x0200; /* USB 2.0 */
    }
    if (HcdCapabilities != NULL) {
        *HcdCapabilities = USB_HCD_CAPS_SUPPORTS_RT_THREADS;
    }
}

static NTSTATUS USB_BUSIFFN
DeckBtFltSyntheticSubmitIsoOutUrb(
    _In_ PVOID BusContext,
    _In_ PURB Urb)
{
    /* Documented standard refusal: USBPORT_SubmitIsoOutUrb returns STATUS_NOT_SUPPORTED */
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(Urb);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS USB_BUSIFFN
DeckBtFltSyntheticQueryBusInformation(
    _In_ PVOID BusContext,
    _In_ ULONG Level,
    _Inout_ PVOID BusInformationBuffer,
    _Out_ PULONG BusInformationBufferLength,
    _Out_opt_ PULONG BusInformationActualLength)
{
    /* Documented refusal: bandwidth reporting not simulated */
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(BusInformationBuffer);
    UNREFERENCED_PARAMETER(BusInformationBufferLength);
    UNREFERENCED_PARAMETER(BusInformationActualLength);
    return STATUS_NOT_SUPPORTED;
}

static BOOLEAN USB_BUSIFFN
DeckBtFltSyntheticIsDeviceHighSpeed(_In_opt_ PVOID BusContext)
{
    UNREFERENCED_PARAMETER(BusContext);
    return TRUE;
}

static NTSTATUS USB_BUSIFFN
DeckBtFltSyntheticEnumLogEntry(
    _In_ PVOID BusContext,
    _In_ ULONG DriverTag,
    _In_ ULONG EnumTag,
    _In_ ULONG P1,
    _In_ ULONG P2)
{
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(DriverTag);
    UNREFERENCED_PARAMETER(EnumTag);
    UNREFERENCED_PARAMETER(P1);
    UNREFERENCED_PARAMETER(P2);
    return STATUS_SUCCESS;
}

static NTSTATUS USB_BUSIFFN
DeckBtFltSyntheticQueryControllerType(
    _In_opt_ PVOID BusContext,
    _Out_opt_ PULONG HcdiOptionFlags,
    _Out_opt_ PUSHORT PciVendorId,
    _Out_opt_ PUSHORT PciDeviceId,
    _Out_opt_ PUCHAR PciClass,
    _Out_opt_ PUCHAR PciSubClass,
    _Out_opt_ PUCHAR PciRevisionId,
    _Out_opt_ PUCHAR PciProgIf)
{
    /* Documented refusal: virtual controller has no physical PCI identifiers */
    UNREFERENCED_PARAMETER(BusContext);
    UNREFERENCED_PARAMETER(HcdiOptionFlags);
    UNREFERENCED_PARAMETER(PciVendorId);
    UNREFERENCED_PARAMETER(PciDeviceId);
    UNREFERENCED_PARAMETER(PciClass);
    UNREFERENCED_PARAMETER(PciSubClass);
    UNREFERENCED_PARAMETER(PciRevisionId);
    UNREFERENCED_PARAMETER(PciProgIf);
    return STATUS_NOT_SUPPORTED;
}

/*
 * Hooks the lower stack's USBDI interface for QueryBusTime and QueryBusTimeEx.
 *
 * Probes the lower stack implementation; on error, substitutes a synthetic
 * monotonic clock (1 kHz frame for QueryBusTime, 8 kHz microframe for QueryBusTimeEx).
 */
static VOID
DeckBtFltHookInterface(
    _Inout_ PDECKBTFLT_CONTEXT Context,
    _Inout_ PINTERFACE Interface,
    _In_ USHORT Size,
    _In_ USHORT Version)
{
    PUSB_BUS_INTERFACE_USBDI_V0 busIf = (PUSB_BUS_INTERFACE_USBDI_V0)Interface;
    const size_t qbtRequiredSize = offsetof(USB_BUS_INTERFACE_USBDI_V0, QueryBusTime) +
                                   sizeof(busIf->QueryBusTime);

    if ((size_t)Size < qbtRequiredSize) {
        return;
    }

    /* Save lower stack's BusContext */
    Context->OriginalBusContext = busIf->BusContext;

    if (busIf->QueryBusTime != NULL) {
        ULONG dummy = 0;
        NTSTATUS probeStatus;

        /* Save lower stack's QueryBusTime */
        Context->OriginalQueryBusTime = busIf->QueryBusTime;

        /* Probe the lower stack's QueryBusTime with a dummy ULONG before installing any thunk */
        probeStatus = busIf->QueryBusTime(busIf->BusContext, &dummy);
        Context->QueryBusTimeProbeStatus = (ULONG)probeStatus;

        if (Context->IsochClockMode == 2 && NT_ERROR(probeStatus)) {
            InterlockedExchange((PLONG)&Context->IsochHookSynthesizing, 1);
        }

        /* Install hook */
        busIf->QueryBusTime = DeckBtFltHookQueryBusTime;
        InterlockedExchange((PLONG)&Context->IsochHookInstalled, 1);
    } else if (Context->IsochClockMode == 2) {
        /* Lower stack returned NULL pointer: in Mode 2, substitute synthetic clock without probing */
        InterlockedExchange((PLONG)&Context->IsochHookSynthesizing, 1);
        busIf->QueryBusTime = DeckBtFltHookQueryBusTime;
        InterlockedExchange((PLONG)&Context->IsochHookInstalled, 1);
    }

    /* If version is V3 or higher, check size and hook QueryBusTimeEx */
    if (Version >= USB_BUSIF_USBDI_VERSION_3) {
        PUSB_BUS_INTERFACE_USBDI_V3 busIfV3 = (PUSB_BUS_INTERFACE_USBDI_V3)Interface;
        const size_t qbtExRequiredSize = offsetof(USB_BUS_INTERFACE_USBDI_V3, QueryBusTimeEx) +
                                         sizeof(busIfV3->QueryBusTimeEx);

        if ((size_t)Size >= qbtExRequiredSize) {
            if (busIfV3->QueryBusTimeEx != NULL) {
                ULONG dummyEx = 0;
                NTSTATUS probeStatusEx;

                /* Save lower stack's QueryBusTimeEx */
                Context->OriginalQueryBusTimeEx = busIfV3->QueryBusTimeEx;

                /* Probe the lower stack's QueryBusTimeEx with a dummy ULONG before installing any thunk */
                probeStatusEx = busIfV3->QueryBusTimeEx(busIfV3->BusContext, &dummyEx);
                Context->QueryBusTimeExProbeStatus = (ULONG)probeStatusEx;

                if (Context->IsochClockMode == 2 && NT_ERROR(probeStatusEx)) {
                    InterlockedExchange((PLONG)&Context->IsochHookSynthesizing, 1);
                }

                /* Install hook */
                busIfV3->QueryBusTimeEx = DeckBtFltHookQueryBusTimeEx;
            } else if (Context->IsochClockMode == 2) {
                /* Lower stack returned NULL pointer: in Mode 2, substitute synthetic clock without probing */
                InterlockedExchange((PLONG)&Context->IsochHookSynthesizing, 1);
                busIfV3->QueryBusTimeEx = DeckBtFltHookQueryBusTimeEx;
            }
        }
    }
}

static VOID
DeckBtFltSynthesizeInterface(
    _Inout_ PDECKBTFLT_CONTEXT Context,
    _Inout_ PINTERFACE Interface,
    _In_ USHORT Size,
    _In_ USHORT Version)
{
    PUSB_BUS_INTERFACE_USBDI_V3 busIf = (PUSB_BUS_INTERFACE_USBDI_V3)Interface;

    RtlZeroMemory(Interface, Size);

    busIf->Size = Size;
    busIf->Version = Version;
    busIf->BusContext = (PVOID)Context;

    /* Interface reference/dereference manages device object lifetime */
    busIf->InterfaceReference = DeckBtFltSyntheticInterfaceReference;
    busIf->InterfaceDereference = DeckBtFltSyntheticInterfaceDereference;

    /* V0 members */
    if (Size >= sizeof(USB_BUS_INTERFACE_USBDI_V0)) {
        busIf->GetUSBDIVersion = DeckBtFltSyntheticGetUSBDIVersion;
        busIf->QueryBusTime = DeckBtFltHookQueryBusTime;
        busIf->SubmitIsoOutUrb = DeckBtFltSyntheticSubmitIsoOutUrb;
        busIf->QueryBusInformation = DeckBtFltSyntheticQueryBusInformation;
    }

    /* V1 members */
    if (Version >= USB_BUSIF_USBDI_VERSION_1 && Size >= sizeof(USB_BUS_INTERFACE_USBDI_V1)) {
        busIf->IsDeviceHighSpeed = DeckBtFltSyntheticIsDeviceHighSpeed;
    }

    /* V2 members */
    if (Version >= USB_BUSIF_USBDI_VERSION_2 && Size >= sizeof(USB_BUS_INTERFACE_USBDI_V2)) {
        busIf->EnumLogEntry = DeckBtFltSyntheticEnumLogEntry;
    }

    /* V3 members */
    if (Version >= USB_BUSIF_USBDI_VERSION_3 && Size >= sizeof(USB_BUS_INTERFACE_USBDI_V3)) {
        busIf->QueryBusTimeEx = DeckBtFltHookQueryBusTimeEx;
        busIf->QueryControllerType = DeckBtFltSyntheticQueryControllerType;
    }

    InterlockedExchange((PLONG)&Context->IsochHookInstalled, 1);
    InterlockedExchange((PLONG)&Context->IsochHookSynthesizing, 1);
}

/*
 * Completion routine for IRP_MN_QUERY_INTERFACE: the status is only known after the stack
 * below has answered, so the entry is recorded and thunks installed here.
 */
typedef struct _DECKBTFLT_QI {
    PDECKBTFLT_CONTEXT Context;
    GUID               InterfaceType;
    USHORT             Size;
    USHORT             Version;
    PINTERFACE         Interface;
} DECKBTFLT_QI;

static IO_COMPLETION_ROUTINE DeckBtFltQueryInterfaceComplete;

static NTSTATUS
DeckBtFltQueryInterfaceComplete(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_reads_opt_(_Inexpressible_("varies")) PVOID Context)
{
    DECKBTFLT_QI *qi = (DECKBTFLT_QI *)Context;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (qi != NULL) {
        PDECKBTFLT_CONTEXT context = qi->Context;
        NTSTATUS status = Irp->IoStatus.Status;
        BOOLEAN isUsbdi = IsEqualGUID(&qi->InterfaceType, &USB_BUS_INTERFACE_USBDI_GUID);

        if (isUsbdi) {
            InterlockedIncrement((PLONG)&context->UsbdiQueryCount);
            context->UsbdiLastRequestedSize = qi->Size;
            context->UsbdiLastRequestedVersion = qi->Version;
            context->UsbdiLastStatus = (ULONG)status;

            if (context->IsochClockMode != 0) {
                if (NT_SUCCESS(status) && qi->Interface != NULL) {
                    /* Lower stack succeeded the interface query: hook QueryBusTime / QueryBusTimeEx */
                    DeckBtFltHookInterface(context, qi->Interface, qi->Size, qi->Version);
                } else if (context->IsochClockMode == 2 && qi->Interface != NULL) {
                    /* Mode 2: lower stack failed the interface query, synthesize the interface */
                    DeckBtFltSynthesizeInterface(context, qi->Interface, qi->Size, qi->Version);
                    Irp->IoStatus.Status = STATUS_SUCCESS;
                    Irp->IoStatus.Information = 0;
                    status = STATUS_SUCCESS;
                    context->UsbdiLastStatus = (ULONG)STATUS_SUCCESS;
                }
            }
        }

        DeckBtFltRecord(context, &qi->InterfaceType, qi->Size, qi->Version,
                        status, IRP_MN_QUERY_INTERFACE);
        ExFreePoolWithTag(qi, DECKBTFLT_POOL_TAG);
    }

    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    return STATUS_SUCCESS;
}

/*
 * Preprocesses IRP_MJ_PNP before KMDF handles it. Only QUERY_INTERFACE is inspected;
 * other PnP IRPs are forwarded unmodified.
 */
static NTSTATUS
DeckBtFltDispatchPnp(_In_ WDFDEVICE Device, _Inout_ PIRP Irp)
{
    PDECKBTFLT_CONTEXT context = DeckBtFltGetContext(Device);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    context->PnpIrps++;

    if (stack->MinorFunction == IRP_MN_QUERY_INTERFACE &&
        stack->Parameters.QueryInterface.InterfaceType != NULL) {

        DECKBTFLT_QI *qi = (DECKBTFLT_QI *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                           sizeof(DECKBTFLT_QI),
                                                           DECKBTFLT_POOL_TAG);
        if (qi != NULL) {
            qi->Context   = context;
            RtlCopyMemory(&qi->InterfaceType, stack->Parameters.QueryInterface.InterfaceType,
                          sizeof(GUID));
            qi->Size      = stack->Parameters.QueryInterface.Size;
            qi->Version   = stack->Parameters.QueryInterface.Version;
            qi->Interface = stack->Parameters.QueryInterface.Interface;

            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, DeckBtFltQueryInterfaceComplete, qi, TRUE, TRUE, TRUE);
            return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
        }

        /* Out of memory: record the attempt without a status and forward untouched. */
        DeckBtFltRecord(context, stack->Parameters.QueryInterface.InterfaceType,
                        stack->Parameters.QueryInterface.Size,
                        stack->Parameters.QueryInterface.Version,
                        STATUS_INSUFFICIENT_RESOURCES, IRP_MN_QUERY_INTERFACE);
    }

    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

VOID
DeckBtFltEvtDeviceCleanup(_In_ WDFOBJECT Object)
{
    WDFDEVICE device = (WDFDEVICE)Object;
    PDECKBTFLT_CONTEXT context = DeckBtFltGetContext(device);
    DeckBtFltUnregisterDevice(context);
}

NTSTATUS
DeckBtFltEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PDECKBTFLT_CONTEXT context;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_OBJECT_ATTRIBUTES workItemAttributes;
    UCHAR minors[1] = { IRP_MN_QUERY_INTERFACE };

    UNREFERENCED_PARAMETER(Driver);

    WdfFdoInitSetFilter(DeviceInit);

    /* Register pre-process callback for IRP_MN_QUERY_INTERFACE. */
    status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, DeckBtFltDispatchPnp,
                                                         IRP_MJ_PNP, minors,
                                                         RTL_NUMBER_OF(minors));
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DECKBTFLT_CONTEXT);
    attributes.EvtCleanupCallback = DeckBtFltEvtDeviceCleanup;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = DeckBtFltGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device = device;

    /* Read mode configuration from driver Parameters key */
    context->IsochClockMode = DeckBtFltReadIsochClockMode(Driver);
    context->IsochClockModeActive = context->IsochClockMode;
    context->QueryBusTimeProbeStatus = (ULONG)STATUS_PENDING;
    context->QueryBusTimeExProbeStatus = (ULONG)STATUS_PENDING;

    /* Create work item for PASSIVE_LEVEL registry publication */
    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, DeckBtFltEvtWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&workItemAttributes);
    workItemAttributes.ParentObject = device;
    status = WdfWorkItemCreate(&workItemConfig, &workItemAttributes, &context->PublishWorkItem);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DeckBtFltRegisterDevice(context);

    /* If mode != 0, publish initial state to registry */
    if (context->IsochClockMode != 0) {
        DeckBtFltPublishLog(context);
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, DeckBtFltEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           &g_Driver);
}
