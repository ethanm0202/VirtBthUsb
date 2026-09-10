# Stage 2: Isochronous Endpoint Geometry Verification

## Purpose

Evaluate whether a virtual USB controller under KMDF UdeCx can transport isochronous traffic matching Bluetooth SCO alternate-setting geometries, and identify any required kernel bus-interface support.

---

## Results

Isochronous transfers succeed across alternate settings 1 through 6 and packet counts 1 through 64 when `QueryBusTime` synthesis is enabled in the companion filter driver (`deckbtflt.sys` / `isoflt.sys`).

### Test Matrix Summary

Evaluated across 432 test cells (6 alternate settings $\times$ 2 directions $\times$ 9 packet counts $\times$ 4 transfer sizes):

- **Accepted Transfers: 216 cells**
  - IN (54 cells): Exact geometry across alternate settings 1–6 and packet counts 1–64.
  - OUT (162 cells): Accepted across exact, sub-sized, and oversized buffers.
- **Rejected Transfers (Negative Controls): 216 cells**
  - Zero-length buffers: 108 (54 OUT rejected at submission with `ERROR_INVALID_USER_BUFFER`, 54 IN rejected with `ERROR_INVALID_PARAMETER`).
  - Sub-sized IN buffers: 54 (rejected at completion with `ERROR_INSUFFICIENT_BUFFER`).
  - Oversized IN buffers: 54 (rejected at submission with `ERROR_INVALID_USER_BUFFER`).
- **Transfer Volume:** 30,772 bytes IN / 77,437 bytes OUT completed with zero URB errors or packet-size mismatches.
- **Cancellation:** Pending transfers drained cleanly on cancellation without wedging the endpoint queue.

---

## Control Baseline (Without QueryBusTime Synthesis)

When tested without filter assistance (standard UdeCx behavior):
- **Accepted Transfers:** 0 of 432 cells.
- **Rejected Transfers:** 432 of 432 cells (failed with `ERROR_NOT_SUPPORTED` / `0xC00000BB`).
- All `QueryBusTime` queries returned `STATUS_NOT_SUPPORTED`.

This confirms that UdeCx requires bus-time synthesis in the filter driver to support isochronous endpoints under `ucx01000.sys`.

---

## Scope & Limitations

This test was performed using a vendor-class test device (`USB\VID_CAFE&PID_4001`) driven by WinUSB to validate raw endpoint and bus-timing behavior:
- Bluetooth SCO baseband link negotiation was not exercised.
- Audio codecs (CVSD, mSBC) and audio capture/render pipelines were not tested.
