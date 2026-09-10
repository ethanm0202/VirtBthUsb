# VirtBthUsb

A KMDF UdeCx driver that exposes a virtual USB Bluetooth controller to Windows.

Targeted at enabling Bluetooth voice audio (microphone) on the Steam Deck OLED under Windows dual-boot by routing around the Windows `BthX` driver stack.

---

## Background

On the Steam Deck OLED, the Qualcomm WCN6855 Wi-Fi/Bluetooth controller connects over UART (`ACPI\QCOM2066`). Under Windows:

1. Windows drives UART-attached Bluetooth through the legacy `BthX` stack (`qcbtuart.sys` -> `bthmini.sys`).
2. `BthX` requires voice audio (SCO/eSCO) to bypass the host CPU over a dedicated hardware sideband (I2S/PCM direct to audio hardware). The Steam Deck APU does not wire this sideband.
3. Windows `bthmini.sys` explicitly rejects in-band SCO voice data over the transport (`0xC0000182` / `STATUS_INVALID_PARAMETER_MIX`).
4. As a result, Bluetooth audio playback (A2DP) works, but the Bluetooth microphone is unsupported.

---

## Architecture

Windows' inbox USB Bluetooth transport driver (`BTHUSB.SYS`) supports in-band isochronous SCO voice over USB endpoints.

This driver emulates a USB Bluetooth device (`USB\VID_0CF3&PID_6390`, Wireless class `0xE0`/`0x01`/`0x01`) using the KMDF USB Device Emulation framework (`UdeCx`). Windows attaches `BTHUSB.SYS` to the emulated device, which then routes HCI traffic to the backend.

```
┌──────────────────────────────────────────────┐
│ Windows Bluetooth Stack (BTHPORT.SYS)        │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│ Inbox USB Transport Driver (BTHUSB.SYS)       │
└──────────────────────┬───────────────────────┘
                       │
┌──────────────────────▼───────────────────────┐
│ Virtual USB Root Hub (USBHUB3.SYS)           │
└──────────────────────┬───────────────────────┘
                       │ (Emulated USB Device)
┌──────────────────────▼──────────────────────────────────────────────┐
│ VirtBthUsb (KMDF UdeCx Virtual Host Controller)                     │
├─────────────────────────────────────────────────────────────────────┤
│ Frontend: Descriptors, EP0 Control, Interrupt, Bulk, Isochronous    │
├─────────────────────────────────────────────────────────────────────┤
│ Backend Bridge                                                      │
└──────────────┬───────────────────────────────────────┬──────────────┘
               │                                       │
     ┌─────────▼─────────┐                   ┌─────────▼─────────┐
     │  Synthetic Stub   │                   │ Qualcomm WCN6855  │
     │   (HCI Testing)   │                   │    SerCx2 UART    │
     └───────────────────┘                   └───────────────────┘
```

---

## Status

- **Virtual USB Controller & BTHUSB Binding:** Complete. Windows creates a root hub (`USB\ROOT_HUB30`), loads `BTHUSB.SYS`, and initializes the standard Bluetooth enumerators (`MS_BTHBRB`, `MS_BTHLE`, `MS_RFCOMM`) against the synthetic stub.
- **Isochronous Endpoints & Bus Filter:** Complete. Tested across 432 transfer cells (alternate settings 1–6, packet counts 1–64); QueryBusTime clock synthesis unblocks UdeCx isochronous transfers under ucx01000.
- **Qualcomm UART Bridge:** In progress. TLV firmware parser and 673-command bring-up state machine verified against mock hardware.
- **Power Management (S0ix):** Planned.
- **In-Band SCO Voice:** Planned.

---

## Repository Layout

```
VirtBthUsb/
├── LICENSE                    <- MIT License
├── README.md                  <- This document
├── docs/
│   ├── ARCHITECTURE.md        <- UdeCx constraints, descriptors, and Windows behavior
│   ├── BUILD.md               <- Build and self-test instructions
│   ├── M1-RESULT.md           <- Initial measurement log
│   └── ROADMAP.md             <- Implementation milestones
├── reference/
│   ├── VIRTUAL-HCI-REFERENCE.txt <- Baseline USB descriptors and expected HCI responses
│   └── stage2-isoc-reference/    <- Stage 2 isochronous geometry matrix and measurement baseline
├── src/
│   ├── include/               <- Shared contracts: usb_descriptors.h, qca_protocol.h, qca_init_fsm.h
│   ├── common/                <- Host-testable logic: usb_descriptors.c, qca_tlv.c, qca_init_fsm.c
│   ├── driver/                <- UdeCx virtual host controller (deckbtusb.sys)
│   ├── filter/                <- Bus-interface lower filter (deckbtflt.sys, isoflt.sys)
│   └── isotest/               <- UdeCx isochronous endpoint test driver
└── tools/
    ├── build.cmd              <- Compiles driver packages via EWDK or MSVC
    ├── selftest.cmd           <- Compiles and runs all five host-side unit test suites
    ├── isotest/               <- User-mode WinUSB isochronous geometry measurement tool
    ├── m2-iso.ps1             <- Isochronous test automation script
    ├── *_selftest.c           <- Unit test sources
    ├── m1-install.ps1         <- Driver test staging script
    └── m1-diag.ps1            <- User-mode decoder for in-kernel diagnostic breadcrumbs
```

---

## Building and Testing

### Build
Requires Visual Studio 2022 with the WDK, or the standalone Enterprise WDK (EWDK):

```cmd
tools\build.cmd
```

### Self-Tests
The core logic compiles into host-side test executables without requiring driver installation or hardware:

```cmd
tools\selftest.cmd
```

Validates descriptor geometry, isochronous alternate settings, HCI response framing, and firmware parsing against user-mode mocks.

---

## Technical Constraints

Emulating a Bluetooth controller under UdeCx requires adhering to specific kernel constraints:

1. **HighSpeed Emulation:** UDE validates configuration descriptors against High Speed rules regardless of declared device speed. Full-Speed descriptors fail validation.
2. **Bulk Packet Size:** Bulk endpoints must declare a packet size of 512 bytes.
3. **Isochronous Interval:** `ucx01000` interprets `bInterval` as $125\,\mu\text{s}$ microframes. An interval of 4 yields $2^{(4-1)} \times 125\,\mu\text{s} = 1\text{ ms}$, preserving standard Bluetooth frame rates.
4. **Dynamic Endpoints:** `UdecxEndpointTypeDynamic` is required for alternate interface settings (SCO alt 0–6).
5. **Non-Composite Device:** Must report class `0xE0` at the device level without an Interface Association Descriptor (IAD) so `BTHUSB.SYS` owns the entire devnode.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for technical notes.

---

## Disclaimers

- Steam Deck is a registered trademark of Valve Corporation. Windows is a registered trademark of Microsoft Corporation. Qualcomm is a registered trademark of Qualcomm Incorporated. All trademarks belong to their respective owners. This is an independent open-source project and is not affiliated with or endorsed by Valve, Microsoft, or Qualcomm.
- Protocol constants are used for technical interoperability.
- No proprietary firmware binaries are included in this repository.

---

## License

[MIT](LICENSE)
