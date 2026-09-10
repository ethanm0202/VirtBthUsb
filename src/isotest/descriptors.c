/*
 * descriptors.c - USB descriptors for DeckBtIsoTest UDE driver (Stage 2).
 *
 * Implements vendor-class device with the exact isochronous geometry from
 * reference/VIRTUAL-HCI-REFERENCE.txt (Interface 1, alternate settings 0..6,
 * packet sizes 0, 9, 17, 25, 33, 49, 63, interval 4).
 */

#include "isotest_descriptors.h"

#define LO(x) ((UCHAR)((x) & 0xFFu))
#define HI(x) ((UCHAR)(((x) >> 8) & 0xFFu))

const USHORT IsoTestScoAltPacketSize[ISOTEST_ALT_COUNT] = { 0, 9, 17, 25, 33, 49, 63 };

/* ---------------------------------------------------------------- device */

const UCHAR IsoTestDeviceDescriptor[] = {
    18,                             /* bLength                                    */
    0x01,                           /* bDescriptorType = DEVICE                   */
    0x00, 0x02,                     /* bcdUSB = 2.00 (High-Speed capable)         */
    ISOTEST_CLASS_VENDOR,           /* bDeviceClass = 0xFF (Vendor Specific)      */
    ISOTEST_SUBCLASS_VENDOR,        /* bDeviceSubClass = 0x00                     */
    ISOTEST_PROTOCOL_VENDOR,        /* bDeviceProtocol = 0x00                     */
    64,                             /* bMaxPacketSize0                            */
    LO(ISOTEST_VENDOR_ID),  HI(ISOTEST_VENDOR_ID),
    LO(ISOTEST_PRODUCT_ID), HI(ISOTEST_PRODUCT_ID),
    LO(ISOTEST_DEVICE_BCD), HI(ISOTEST_DEVICE_BCD),
    ISOTEST_ISTRING_MANUFACTURER,
    ISOTEST_ISTRING_PRODUCT,
    ISOTEST_ISTRING_SERIAL,
    1                               /* bNumConfigurations                         */
};
const ULONG IsoTestDeviceDescriptorSize = sizeof(IsoTestDeviceDescriptor);

const UCHAR IsoTestDeviceQualifier[] = {
    10,                             /* bLength                                    */
    0x06,                           /* bDescriptorType = DEVICE_QUALIFIER         */
    0x00, 0x02,                     /* bcdUSB = 2.00                              */
    ISOTEST_CLASS_VENDOR,
    ISOTEST_SUBCLASS_VENDOR,
    ISOTEST_PROTOCOL_VENDOR,
    64,                             /* bMaxPacketSize0                            */
    1,                              /* bNumConfigurations (other speed)           */
    0                               /* bReserved                                  */
};
const ULONG IsoTestDeviceQualifierSize = sizeof(IsoTestDeviceQualifier);

/* ---------------------------------------------------------------- configuration */

/*
 * Total length arithmetic:
 *   Configuration descriptor:             9 bytes
 *   Interface 0 (control/bulk/event):     9 bytes
 *     Endpoint 0x81 (Interrupt IN):       7 bytes
 *     Endpoint 0x02 (Bulk OUT):           7 bytes
 *     Endpoint 0x82 (Bulk IN):            7 bytes
 *   Interface 1 (isochronous alt 0..6):
 *     7 settings * (9 + 7 + 7) =        161 bytes
 *   Total:                              200 bytes (matches VIRTUAL-HCI-REFERENCE.txt)
 */
#define ISOTEST_CONFIG_TOTAL_LENGTH 200u

#define ISOTEST_ISOCH_ALT(alt, size)                                             \
    9, 0x04, ISOTEST_IFACE_ISOCH, (alt), 2,                                      \
        ISOTEST_CLASS_VENDOR, ISOTEST_SUBCLASS_VENDOR, ISOTEST_PROTOCOL_VENDOR, 0,\
    7, 0x05, ISOTEST_EP_ISOCH_OUT, 0x01, LO(size), HI(size), ISOTEST_EP_ISOCH_INTERVAL,\
    7, 0x05, ISOTEST_EP_ISOCH_IN,  0x01, LO(size), HI(size), ISOTEST_EP_ISOCH_INTERVAL

const UCHAR IsoTestConfigDescriptor[] = {
    /* --- configuration ------------------------------------------------------- */
    9,                              /* bLength                                    */
    0x02,                           /* bDescriptorType = CONFIGURATION            */
    LO(ISOTEST_CONFIG_TOTAL_LENGTH), HI(ISOTEST_CONFIG_TOTAL_LENGTH),
    2,                              /* bNumInterfaces                             */
    1,                              /* bConfigurationValue                        */
    0,                              /* iConfiguration                             */
    0xA0,                           /* bmAttributes: bus powered + remote wakeup  */
    50,                             /* bMaxPower = 100 mA                         */

    /* --- interface 0: command/bulk/event ------------------------------------- */
    9, 0x04, ISOTEST_IFACE_CONTROL, 0, 3,
        ISOTEST_CLASS_VENDOR, ISOTEST_SUBCLASS_VENDOR, ISOTEST_PROTOCOL_VENDOR, 0,

    /* interrupt IN */
    7, 0x05, ISOTEST_EP_EVENT_IN, 0x03,
        LO(ISOTEST_EP_EVENT_MAXPACKET), HI(ISOTEST_EP_EVENT_MAXPACKET),
        ISOTEST_EP_EVENT_INTERVAL,
    /* bulk OUT - HS bulk min 512 */
    7, 0x05, ISOTEST_EP_BULK_OUT, 0x02,
        LO(ISOTEST_EP_BULK_MAXPACKET), HI(ISOTEST_EP_BULK_MAXPACKET), 0,
    /* bulk IN - HS bulk min 512 */
    7, 0x05, ISOTEST_EP_BULK_IN, 0x02,
        LO(ISOTEST_EP_BULK_MAXPACKET), HI(ISOTEST_EP_BULK_MAXPACKET), 0,

    /* --- interface 1: isochronous alternate settings 0..6 -------------------- */
    ISOTEST_ISOCH_ALT(0, 0),
    ISOTEST_ISOCH_ALT(1, 9),
    ISOTEST_ISOCH_ALT(2, 17),
    ISOTEST_ISOCH_ALT(3, 25),
    ISOTEST_ISOCH_ALT(4, 33),
    ISOTEST_ISOCH_ALT(5, 49),
    ISOTEST_ISOCH_ALT(6, 63)
};
const ULONG IsoTestConfigDescriptorSize = sizeof(IsoTestConfigDescriptor);

/* ---------------------------------------------------------------- strings */

const UCHAR IsoTestStringLangIds[] = {
    4, 0x03, LO(ISOTEST_LANGID_EN_US), HI(ISOTEST_LANGID_EN_US)
};

/* "DeckBtUsb" */
const UCHAR IsoTestStringManufacturer[] = {
    20, 0x03,
    'D',0, 'e',0, 'c',0, 'k',0, 'B',0, 't',0, 'U',0, 's',0, 'b',0
};

/* "IsoTest Virtual Device" */
const UCHAR IsoTestStringProduct[] = {
    46, 0x03,
    'I',0, 's',0, 'o',0, 'T',0, 'e',0, 's',0, 't',0, ' ',0,
    'V',0, 'i',0, 'r',0, 't',0, 'u',0, 'a',0, 'l',0, ' ',0,
    'D',0, 'e',0, 'v',0, 'i',0, 'c',0, 'e',0
};

/* "ISOTEST0001" */
const UCHAR IsoTestStringSerial[] = {
    24, 0x03,
    'I',0, 'S',0, 'O',0, 'T',0, 'E',0, 'S',0, 'T',0, '0',0, '0',0, '0',0, '1',0
};

const UCHAR *IsoTestGetStringDescriptor(_In_ UCHAR Index, _Out_ ULONG *Length)
{
    switch (Index) {
    case ISOTEST_ISTRING_LANGIDS:
        *Length = sizeof(IsoTestStringLangIds);
        return IsoTestStringLangIds;
    case ISOTEST_ISTRING_MANUFACTURER:
        *Length = sizeof(IsoTestStringManufacturer);
        return IsoTestStringManufacturer;
    case ISOTEST_ISTRING_PRODUCT:
        *Length = sizeof(IsoTestStringProduct);
        return IsoTestStringProduct;
    case ISOTEST_ISTRING_SERIAL:
        *Length = sizeof(IsoTestStringSerial);
        return IsoTestStringSerial;
    default:
        *Length = 0;
        return NULL;
    }
}
