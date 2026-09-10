# Stage 2 Isochronous Geometry Reference

This document records the validated isochronous geometry matrix, packet counts, transfer categories, legal combinations, and negative control contracts for the synthetic USB substrate.

## 1. SCO Interface Topology & Alternate Settings

In Bluetooth USB controllers (and the frozen descriptor specification in `reference/VIRTUAL-HCI-REFERENCE.txt`), Interface 1 carries synchronous voice (SCO/eSCO) traffic using isochronous endpoints:
- Endpoint `0x03`: Isochronous OUT
- Endpoint `0x83`: Isochronous IN
- Endpoint Interval: `bInterval = 4` (High-Speed: $2^{4-1} = 8$ microframes = 1 ms per interval)

### Alternate Setting 0: Zero-Bandwidth
- **Endpoints:** EP `0x03` and EP `0x83` with `wMaxPacketSize = 0`
- **Behavior:** Reserved for inactive SCO state. It allocates no bus bandwidth and carries **no data traffic**. WinUSB exposes no usable pipe handles for zero-bandwidth endpoints. The test harness verifies alt setting 0 interface selection during initialization but submits no transfer cells against it.

### Alternate Settings 1 through 6: Active SCO Modes
Each alternate setting provides a specific nominal byte payload per 1 ms interval:

| Alternate Setting | Nominal Bytes / Interval (`wMaxPacketSize`) | Typical Bluetooth SCO / eSCO Use Case |
| :---: | :---: | :--- |
| **Alt 1** | **9 bytes** | 8 kHz 16-bit mono voice (e.g. CVSD 64 kbps, 8 bytes/ms + 1 framing byte) |
| **Alt 2** | **17 bytes** | 16 kHz 16-bit wideband voice (e.g. mSBC 16-byte frames + framing) |
| **Alt 3** | **25 bytes** | Extended SCO (eSCO) 2-EV3 / 3-EV3 packet geometries |
| **Alt 4** | **33 bytes** | Extended SCO (eSCO) wideband configurations |
| **Alt 5** | **49 bytes** | High-bandwidth eSCO transparent data |
| **Alt 6** | **63 bytes** | Maximum-rate SCO/eSCO alternate setting |

## 2. Packet Counts Exercised

For each alternate setting and direction, 9 distinct packet counts are exercised:
```
1, 2, 4, 8, 10, 16, 20, 32, 64
```
At 1 ms per interval, these correspond to transfer durations of 1 ms, 2 ms, 4 ms, 8 ms, 10 ms, 16 ms, 20 ms, 32 ms, and 64 ms.

## 3. Transfer Categories

Each cell tests one of four transfer length categories:

1. **`exact`:** `PacketSize = NominalBytesPerInterval`.
   `TotalLength = Packets * NominalBytesPerInterval`.
2. **`sub`:** `PacketSize = NominalBytesPerInterval / 2` (integer division: 4, 8, 12, 16, 24, 31 bytes).
   `TotalLength = Packets * PacketSize`.
3. **`oversize`:** `PacketSize = NominalBytesPerInterval + 1` (10, 18, 26, 34, 50, 64 bytes).
   `TotalLength = Packets * PacketSize`.
4. **`zero`:** `PacketSize = 0`, `TotalLength = 0`.

## 4. Total Matrix Space

$$\text{Total Cells} = 6 \text{ (Alts 1--6)} \times 2 \text{ (Directions OUT/IN)} \times 9 \text{ (Packet Counts)} \times 4 \text{ (Categories)} = 432 \text{ cells}$$

## 5. Legal Combinations vs. Deliberate Negative Controls

The test matrix is split exactly 50/50 between expected-accepted transfers and deliberate negative controls:

### Accepted Combinations (216 cells)
- **All `exact` OUT (54 cells):** Validated across all 6 alts and all 9 packet counts.
- **All `sub` OUT (54 cells):** WinUSB allows submitting OUT buffers smaller than the endpoint maximum packet size.
- **All `oversize` OUT (54 cells):** WinUSB allows multi-interval OUT buffers that exceed a single packet size.
- **All `exact` IN (54 cells):** Validated across all 6 alts and all 9 packet counts. WinUSB receives exact packet allocations and reports observed transfer byte counts matching $Packets \times NominalBytesPerInterval$.
- **Total Accepted:** $54 + 54 + 54 + 54 = 216$ cells. Zero exact cells are rejected.

### Deliberate Negative Controls (216 cells)
The remaining 216 cells test boundary failure enforcement. Each negative control category must produce an exact, reproducible Win32 error code at an exact lifecycle stage:

| Category | Direction | Count | Failure Stage | Expected Win32 Error | Constant & Description |
| :--- | :---: | :---: | :--- | :---: | :--- |
| **`zero`** | OUT | 54 | `SUBMIT_TRANSFER` | `0x000006F8` | `ERROR_INVALID_USER_BUFFER` (1784): WinUSB rejects 0-byte isochronous OUT buffer submissions. |
| **`zero`** | IN | 54 | `SUBMIT_TRANSFER` | `0x00000057` | `ERROR_INVALID_PARAMETER` (87): WinUSB rejects 0-byte isochronous IN buffer submissions. |
| **`sub`** | IN | 54 | `COMPLETION` | `0x0000007A` | `ERROR_INSUFFICIENT_BUFFER` (122): Buffer passes submission but fails at completion because the registered buffer cannot accommodate incoming packets. |
| **`oversize`** | IN | 54 | `SUBMIT_TRANSFER` | `0x000006F8` | `ERROR_INVALID_USER_BUFFER` (1784): WinUSB validates buffer registration against $Packets \times wMaxPacketSize$; exceeding registered bounds fails at submission. |

- **Total Rejected:** $54 + 54 + 54 + 54 = 216$ cells.

## 6. Regression Contract Summary

Any future regression run against this reference must demonstrate:
1. Exactly 432 total cells.
2. Exactly 216 accepted cells (54 IN, 162 OUT).
3. Exactly 0 exact cells rejected.
4. Exactly 216 rejected cells, partitioned identically:
   - `zero`: 108 (54 OUT with `0x6F8` at `SUBMIT_TRANSFER`, 54 IN with `0x57` at `SUBMIT_TRANSFER`)
   - `sub`: 54 (all IN with `0x7A` at `COMPLETION`)
   - `oversize`: 54 (all IN with `0x6F8` at `SUBMIT_TRANSFER`)
5. Total IN bytes transferred on accepted cells: 30,772 bytes.
