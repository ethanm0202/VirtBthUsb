/* Portable descriptor contract shared by the kernel instrument and host regression suite. */
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#endif

#define ISOTEST_VENDOR_ID          0xCAFEu
#define ISOTEST_PRODUCT_ID         0x4001u
#define ISOTEST_DEVICE_BCD         0x0001u

#define ISOTEST_CLASS_VENDOR       0xFFu
#define ISOTEST_SUBCLASS_VENDOR    0x00u
#define ISOTEST_PROTOCOL_VENDOR    0x00u

#define ISOTEST_EP_EVENT_IN        0x81u
#define ISOTEST_EP_BULK_OUT        0x02u
#define ISOTEST_EP_BULK_IN         0x82u
#define ISOTEST_EP_ISOCH_OUT       0x03u
#define ISOTEST_EP_ISOCH_IN        0x83u

#define ISOTEST_EP_EVENT_MAXPACKET 16u
#define ISOTEST_EP_BULK_MAXPACKET  512u
#define ISOTEST_EP_EVENT_INTERVAL  4u
#define ISOTEST_EP_ISOCH_INTERVAL  4u

#define ISOTEST_IFACE_CONTROL      0u
#define ISOTEST_IFACE_ISOCH        1u
#define ISOTEST_ALT_COUNT          7u
#define ISOTEST_ALT_MAX            6u

#define ISOTEST_ISTRING_LANGIDS      0u
#define ISOTEST_ISTRING_MANUFACTURER 1u
#define ISOTEST_ISTRING_PRODUCT      2u
#define ISOTEST_ISTRING_SERIAL       3u
#define ISOTEST_ISTRING_COUNT        4u
#define ISOTEST_LANGID_EN_US         0x0409u

extern const USHORT IsoTestScoAltPacketSize[ISOTEST_ALT_COUNT];
extern const UCHAR IsoTestDeviceDescriptor[];
extern const ULONG IsoTestDeviceDescriptorSize;
extern const UCHAR IsoTestDeviceQualifier[];
extern const ULONG IsoTestDeviceQualifierSize;
extern const UCHAR IsoTestConfigDescriptor[];
extern const ULONG IsoTestConfigDescriptorSize;
extern const UCHAR IsoTestStringLangIds[];
extern const UCHAR IsoTestStringManufacturer[];
extern const UCHAR IsoTestStringProduct[];
extern const UCHAR IsoTestStringSerial[];

_Success_(return != NULL)
const UCHAR *IsoTestGetStringDescriptor(_In_ UCHAR Index, _Out_ ULONG *Length);
