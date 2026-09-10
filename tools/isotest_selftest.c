#include <stdio.h>
#include <string.h>
#include "../src/isotest/isotest_descriptors.h"
#include "../src/include/usb_descriptors.h"

#define USB_DT_CONFIG 2u
#define USB_DT_INTERFACE 4u
#define USB_DT_ENDPOINT 5u
#define USB_XFER_ISOCH 1u
#define USB_XFER_BULK 2u
static int failures;
static void check(const char *name, int condition)
{
    if (condition) {
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
        failures++;
    }
}

#define CHECK(name, condition) check((name), (condition))

typedef struct _GEOMETRY {
    unsigned altCount;
    unsigned seen[ISOTEST_ALT_COUNT];
    unsigned endpointCount[ISOTEST_ALT_COUNT];
    unsigned outSize[ISOTEST_ALT_COUNT];
    unsigned inSize[ISOTEST_ALT_COUNT];
    unsigned outInterval[ISOTEST_ALT_COUNT];
    unsigned inInterval[ISOTEST_ALT_COUNT];
    unsigned bulkOutSize;
    unsigned bulkInSize;
    int valid;
} GEOMETRY;

static unsigned word(const UCHAR *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

static GEOMETRY parse(const UCHAR *blob, ULONG size, UCHAR iface, UCHAR outEp, UCHAR inEp,
                      UCHAR bulkOutEp, UCHAR bulkInEp)
{
    GEOMETRY g;
    ULONG offset = 0;
    int currentAlt = -1;
    memset(&g, 0, sizeof(g));
    g.valid = size >= 9 && blob[0] >= 9 && blob[1] == USB_DT_CONFIG && word(blob + 2) == size;
    while (g.valid && offset < size) {
        unsigned length = blob[offset];
        unsigned type;
        if (length < 2 || length > size - offset) { g.valid = 0; break; }
        type = blob[offset + 1];
        if (type == USB_DT_INTERFACE && length >= 9) {
            currentAlt = -1;
            if (blob[offset + 2] == iface) {
                unsigned alt = blob[offset + 3];
                if (alt >= ISOTEST_ALT_COUNT || g.seen[alt]) { g.valid = 0; break; }
                currentAlt = (int)alt;
                g.seen[alt] = 1;
                g.altCount++;
                if (blob[offset + 4] != 2) g.valid = 0;
            }
        } else if (type == USB_DT_ENDPOINT && length >= 7) {
            unsigned address = blob[offset + 2];
            unsigned transfer = blob[offset + 3] & 3u;
            unsigned packet = word(blob + offset + 4) & 0x7ffu;
            if (currentAlt >= 0) {
                unsigned alt = (unsigned)currentAlt;
                if (transfer != USB_XFER_ISOCH) g.valid = 0;
                if (address == outEp) { g.outSize[alt] = packet; g.outInterval[alt] = blob[offset + 6]; }
                else if (address == inEp) { g.inSize[alt] = packet; g.inInterval[alt] = blob[offset + 6]; }
                else g.valid = 0;
                g.endpointCount[alt]++;
            } else if (transfer == USB_XFER_BULK) {
                if (address == bulkOutEp) g.bulkOutSize = packet;
                if (address == bulkInEp) g.bulkInSize = packet;
            }
        }
        offset += length;
    }
    if (offset != size) g.valid = 0;
    return g;
}

int main(void)
{
    GEOMETRY instrument = parse(IsoTestConfigDescriptor, IsoTestConfigDescriptorSize,
        ISOTEST_IFACE_ISOCH, ISOTEST_EP_ISOCH_OUT, ISOTEST_EP_ISOCH_IN,
        ISOTEST_EP_BULK_OUT, ISOTEST_EP_BULK_IN);
    GEOMETRY production = parse(DeckBtConfigDescriptor, DeckBtConfigDescriptorSize,
        DECKBT_IFACE_SCO, DECKBT_EP_SCO_OUT, DECKBT_EP_SCO_IN,
        DECKBT_EP_ACL_OUT, DECKBT_EP_ACL_IN);
    unsigned alt;

    printf("isotest descriptor contract\n");
    CHECK("instrument descriptor chain reaches declared wTotalLength", instrument.valid);
    CHECK("production descriptor chain reaches declared wTotalLength", production.valid);
    CHECK("declared alternate counts agree", DECKBT_SCO_ALT_COUNT == ISOTEST_ALT_COUNT);
    CHECK("descriptor alternate counts agree with both contracts",
          instrument.altCount == ISOTEST_ALT_COUNT && production.altCount == DECKBT_SCO_ALT_COUNT);
    CHECK("instrument bulk OUT matches production packet capacity",
          instrument.bulkOutSize == production.bulkOutSize && instrument.bulkOutSize == ISOTEST_EP_BULK_MAXPACKET);
    CHECK("instrument bulk IN matches production packet capacity",
          instrument.bulkInSize == production.bulkInSize && instrument.bulkInSize == ISOTEST_EP_BULK_MAXPACKET);

    for (alt = 0; alt < ISOTEST_ALT_COUNT && alt < DECKBT_SCO_ALT_COUNT; alt++) {
        char label[96];
        sprintf_s(label, sizeof(label), "alt %u has one interface and exactly two endpoints", alt);
        CHECK(label, instrument.seen[alt] && production.seen[alt] &&
                     instrument.endpointCount[alt] == 2 && production.endpointCount[alt] == 2);
        sprintf_s(label, sizeof(label), "alt %u packet sizes match production extern table and blob", alt);
        CHECK(label, instrument.outSize[alt] == DeckBtScoAltPacketSize[alt] &&
                     instrument.inSize[alt] == DeckBtScoAltPacketSize[alt] &&
                     production.outSize[alt] == DeckBtScoAltPacketSize[alt] &&
                     production.inSize[alt] == DeckBtScoAltPacketSize[alt]);
        sprintf_s(label, sizeof(label), "alt %u both intervals satisfy UDE and match production", alt);
        CHECK(label, instrument.outInterval[alt] >= 4 && instrument.inInterval[alt] >= 4 &&
                     instrument.outInterval[alt] == production.outInterval[alt] &&
                     instrument.inInterval[alt] == production.inInterval[alt]);
    }

    CHECK("frozen alt 0 keeps two zero-packet-size endpoints",
          instrument.endpointCount[0] == 2 && instrument.outSize[0] == 0 && instrument.inSize[0] == 0);
    if (failures) { printf("isotest_selftest: %d failure(s)\n", failures); return 1; }
    printf("isotest_selftest: all checks passed\n");
    return 0;
}
