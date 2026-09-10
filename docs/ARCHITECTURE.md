# VirtBthUsb Architecture

## Overview

VirtBthUsb provides a virtual USB Bluetooth Host Controller on Windows using KMDF and the USB Device Emulation Class Extension (`UdeCx`).

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

### The Problem: BthX vs BTHUSB
On Windows, non-USB Bluetooth controllers (UART, SDIO, PCIe) are driven through the `BthX` transport miniport architecture (`qcbtuart.sys` -> `bthmini.sys`).
- `BthX` was architected under the assumption that voice audio (SCO/eSCO) bypasses the host CPU through a hardware sideband (I2S/PCM direct from the Bluetooth chip to the audio DSP/codec).
- `bthmini.sys` explicitly checks `ScoSupport` and rejects in-band SCO voice audio (`cmp dword [rdx+0xc], 2` returning `STATUS_INVALID_PARAMETER_MIX` / `0xC0000182`).
- On hardware where the OEM connected Bluetooth over UART without wiring hardware sideband audio routing (such as the Steam Deck OLED's Qualcomm WCN6855), Windows audio output (A2DP) works, but the Bluetooth microphone (HFP/HSP) is completely unsupported.

### The Solution
Windows' inbox USB Bluetooth driver (`BTHUSB.SYS`) has natively supported in-band isochronous SCO voice over USB endpoints for over two decades. By emulating a standard USB Bluetooth dongle in software, Windows loads `BTHUSB.SYS` and `BTHPORT.SYS`, bypassing `BthX` entirely.

---

## Critical UdeCx / USB Constraints

Emulating a Bluetooth radio under UdeCx requires adhering to non-obvious constraints enforced by `ucx01000.sys` and `usbhub3.sys`:

1. **Declare `UdecxUsbHighSpeed`, Not FullSpeed:**
   - Real Bluetooth USB dongles report Full Speed (12 Mbps).
   - However, UDE/USBHUB3 validates the configuration descriptor against High Speed rules regardless of declared speed. A Full-Speed-shaped descriptor fails with `CONFIGURATION_DESCRIPTOR_VALIDATION_FAILURE` (`0xC000000D`).
2. **Bulk Endpoint `wMaxPacketSize` Must Be 512:**
   - Under High Speed rules, USB bulk endpoints must declare a packet size of 512 bytes (not the 64 bytes reported by Full Speed dongles).
3. **Isochronous `bInterval` Must Be $\ge 4$:**
   - `ucx01000` interprets `bInterval` as $125\,\mu\text{s}$ microframe units regardless of declared speed. Smaller values fail URB submission with `USBD_STATUS_INVALID_PARAMETER`.
   - Setting $bInterval = 4 \implies 2^{(4-1)} = 8\text{ microframes} = 1\text{ ms}$. This preserves standard Bluetooth 1 ms frame pacing and byte rates for alternate settings 0–6.
4. **Endpoint Type Must Be `UdecxEndpointTypeDynamic`:**
   - `UdecxEndpointTypeSimple` forbids alternate interface settings. Bluetooth voice audio requires alternate settings 0–6 on Interface 1 to adjust bandwidth.
5. **Non-Composite Device Architecture:**
   - The device descriptor must declare `bDeviceClass = 0xE0` (Wireless Controller), `bDeviceSubClass = 0x01` (RF), and `bDeviceProtocol = 0x01` (Bluetooth) at the device level without an Interface Association Descriptor (IAD).
   - If declared composite, Windows inserts `usbccgp.sys`, which splits interface 0 and interface 1 into separate devnodes. `BTHUSB.SYS` requires ownership of the entire device.

---

## Windows Bluetooth Subsystem Constraints

1. **Single-Adapter Policy:**
   - Windows permits only one active Bluetooth radio at a time. If another Bluetooth radio is active, `BTHUSB.SYS` logs:
     `"Only one active Bluetooth adapter is supported at a time"` (Event ID 6)
     and fails `AddDevice` with `STATUS_UNSUCCESSFUL` (`CM_PROB_FAILED_ADD`).
   - To test or run the virtual radio, any existing physical Bluetooth controller must be disabled or replaced as the function driver.
2. **INF Ranking:**
   - `deckbtflt.inf` matches the exact hardware ID `USB\VID_0CF3&PID_6390` with rank `0x00FF0001`.
   - This outranks `bth.inf`'s compatible ID match (`USB\Class_E0&SubClass_01&Prot_01`, rank `0x00FF2003`), allowing `deckbtflt.sys` to install as a lower filter while deferring function driver duties to `BTHUSB.SYS` via `Include`/`Needs = BthUsb.NT`.
   - `BTHUSB.SYS` queries for `GUID_DEVICE_RESET_INTERFACE_STANDARD` during initialization failure. Providing `EvtUsbDeviceReset` allows the stack to reset and recover cleanly without wedging in `CM_PROB_FAILED_ADD`.
