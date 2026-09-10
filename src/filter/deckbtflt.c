/*
 * deckbtflt.c - Lower filter driver attached beneath BTHUSB.SYS.
 *
 * Intercepts and logs IRP_MN_QUERY_INTERFACE requests for the USB bus interface,
 * and provides a hook for QueryBusTime handling if required by the isochronous data plane.
 *
 * Pass-through only in this initial build.
 */

#include <initguid.h>
#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <usbbusif.h>

#define DECKBTFLT_LOG_SLOTS  32u
#define DECKBTFLT_POOL_TAG   'tFkD'

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
    WDFDEVICE       Device;
    DECKBTFLT_ENTRY Log[DECKBTFLT_LOG_SLOTS];
    ULONG           Count;
    ULONG           PnpIrps;
} DECKBTFLT_CONTEXT, *PDECKBTFLT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DECKBTFLT_CONTEXT, DeckBtFltGetContext)

static WDFDRIVER g_Driver = NULL;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD DeckBtFltEvtDeviceAdd;

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

    RtlInitUnicodeString(&nameBlob, L"QueryInterfaceLog");
    RtlInitUnicodeString(&nameCount, L"QueryInterfaceCount");
    RtlInitUnicodeString(&namePnp, L"PnpIrpCount");
    (void)WdfRegistryAssignValue(key, &nameBlob, REG_BINARY, sizeof(Context->Log), Context->Log);
    (void)WdfRegistryAssignULong(key, &nameCount, Context->Count);
    (void)WdfRegistryAssignULong(key, &namePnp, Context->PnpIrps);

    WdfRegistryClose(key);
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

    DeckBtFltPublishLog(Context);
}

/*
 * Completion routine for IRP_MN_QUERY_INTERFACE: the status is only known after the stack
 * below has answered, so the entry is recorded here rather than on the way down.
 */
typedef struct _DECKBTFLT_QI {
    PDECKBTFLT_CONTEXT Context;
    GUID   InterfaceType;
    USHORT Size;
    USHORT Version;
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
        DeckBtFltRecord(qi->Context, &qi->InterfaceType, qi->Size, qi->Version,
                        Irp->IoStatus.Status, IRP_MN_QUERY_INTERFACE);
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
            qi->Context = context;
            RtlCopyMemory(&qi->InterfaceType, stack->Parameters.QueryInterface.InterfaceType,
                          sizeof(GUID));
            qi->Size    = stack->Parameters.QueryInterface.Size;
            qi->Version = stack->Parameters.QueryInterface.Version;

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

NTSTATUS
DeckBtFltEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PDECKBTFLT_CONTEXT context;
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
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = DeckBtFltGetContext(device);
    RtlZeroMemory(context, sizeof(*context));
    context->Device = device;

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
