# VirtBthUsb

A Windows kernel driver that makes the Steam Deck OLED's Bluetooth controller appear to Windows as a USB Bluetooth radio, so Windows' built-in USB Bluetooth drivers run it. With those drivers, Bluetooth headset microphones (the Hands-Free profile) work, which they do not with the stock driver.

The driver and its service are named `DeckBtUsb`.

> [!WARNING]
> The driver is test-signed. It only loads with Windows test signing turned on, and games with kernel anti-cheat usually refuse to start while test signing is on. `tools\session.ps1 -Uninstall` removes everything and turns test signing off again.

---

## Background

The Steam Deck OLED's Bluetooth controller, a Qualcomm QCA2066, is not a USB device. It sits on a serial line (a UART, ACPI device `QCOM2066`), and Windows drives it through a matching serial transport stack: `qcbtuart.sys`, then `BthMini.sys`. That stack handles call audio only through hardware offload, where voice leaves the controller on a separate audio path instead of passing through Windows. `BthMini.sys` rejects every other voice mode from its transport and fails with `STATUS_DEVICE_CONFIGURATION_ERROR` (`0xC0000182`). On the Deck, Hands-Free devices enumerate in offload mode (`_HCIBYPASS_`), and headset microphones do not work. Music playback (A2DP) is unaffected, because it travels as ordinary data.

Windows has a second Bluetooth transport, `BTHUSB.SYS`, the inbox driver for USB Bluetooth radios. It carries voice in-band, as timed isochronous USB transfers, and needs no offload hardware. It only binds to USB devices, and the Deck's controller is not one.

DeckBtUsb builds that USB device in software. The driver creates a virtual USB host controller (UdeCx) and plugs an emulated Bluetooth radio into it: device and configuration descriptors, a control endpoint for HCI commands, an interrupt endpoint for events, bulk endpoints for data, and isochronous endpoints with seven alternate settings for voice. Windows enumerates it like any USB dongle and loads its own `BTHUSB.SYS` and `BTHPORT.SYS` on top. Nothing in the Windows Bluetooth stack is patched or replaced; it sees an ordinary USB radio.

Behind that device, DeckBtUsb does the stock serial driver's job itself. It takes the UART from the stock driver, wakes the controller, loads its Qualcomm firmware (a 155 KB patch and a board-specific configuration file, at 3,000,000 baud), and translates every packet between USB transfers on one side and H4-framed serial traffic on the other. Voice needs the most care. A real USB radio delivers audio at the pace of the air link, and `BTHUSB.SYS` learns the voice rate only from how fast its transfers complete, so the driver paces each isochronous transfer against a virtual 1 ms USB frame clock and re-cuts the controller's voice packets into the shapes a USB radio would produce. It also rewrites the controller's voice routing so audio comes back over the serial line instead of the offload path. When the session ends, the controller goes back to the stock driver in the state that driver expects, with every pairing intact.

```
┌──────────────────────────────────────────────┐
│ Windows Bluetooth stack (BTHPORT.SYS)        │
└──────────────────────┬───────────────────────┘
┌──────────────────────▼───────────────────────┐
│ Inbox USB transport driver (BTHUSB.SYS)      │
└──────────────────────┬───────────────────────┘
┌──────────────────────▼───────────────────────┐
│ Virtual USB root hub (USBHUB3.SYS)           │
└──────────────────────┬───────────────────────┘
                       │ emulated USB device USB\VID_0CF3&PID_6390
┌──────────────────────▼──────────────────────────────────────────┐
│ DeckBtUsb (KMDF UdeCx virtual host controller)                  │
│  USB frontend: descriptors, EP0 commands, interrupt events,     │
│  bulk ACL, isochronous SCO                                      │
│  HCI bridge: H4 framing, voice routing, SCO pacing              │
└──────────────┬──────────────────────────────────┬───────────────┘
     ┌─────────▼─────────┐              ┌─────────▼──────────────┐
     │ Synthetic HCI stub│              │ Qualcomm QCA2066       │
     │ (development)     │              │ over SerCx2 UART       │
     └───────────────────┘              └────────────────────────┘
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and [docs/QCA2066.md](docs/QCA2066.md) for the controller bring-up.

---

## Status

Headset microphones work. With DeckBtUsb running, Windows lists a Bluetooth headset's Hands-Free microphone as a normal recording device, and calls carry voice in both directions with wideband (mSBC) audio. In testing with AirPods Pro and a Shokz OpenMeet, microphone recordings and Discord calls came through with no voice packets lost.

Everything else the radio is used for keeps working under the same driver: discovery, pairing, mice and other BLE input devices, and music (A2DP). Existing pairings carry over, because the controller keeps its own address. Sleep (S3) works as well: after each resume the driver reloads the controller's firmware and Windows brings the radio back about 7 seconds after wake.

Tested on one Steam Deck OLED running Windows 11 25H2 (build 26200), test signing on, Memory Integrity off. Details and measurements: [docs/VERIFICATION.md](docs/VERIFICATION.md).

- **Headsets paired under the stock driver** may connect with music only. Pairing them again while DeckBtUsb runs makes the microphone available ([docs/INSTALL.md](docs/INSTALL.md)).
- **Not implemented:** loading at boot. DeckBtUsb runs per session, started with `tools\session.ps1 -Start` and stopped with `-Stop`.
- **Not implemented:** recovery from a controller crash.
- **Untested:** narrowband (CVSD) voice, long calls, calls across sleep, hibernate and Fast Startup, battery impact.

---

## Requirements

- Steam Deck OLED with Valve's Windows Bluetooth driver installed. The build copies the controller firmware from that driver's package on your machine. No firmware is included in this repository.
- Windows 11 x64, administrator rights, Secure Boot off (Windows refuses test signing otherwise).
- The Enterprise WDK (EWDK) extracted to `C:\EWDK` to build.
- Python 3 with `numpy` and `sounddevice` for `tools\miccheck.py`; `pycdlib` if using the optional EWDK extraction helper.

## Quick start

From an elevated PowerShell in the repository root:

```powershell
Set-ExecutionPolicy -Scope Process Bypass   # allow the scripts in this window only
tools\build.cmd                    # build and test-sign the driver packages
tools\selftest.cmd                 # host-side tests, no hardware needed
tools\session.ps1 -Prepare         # test signing on, test certificate, baseline; then restart Windows
tools\session.ps1 -Start           # disconnect Bluetooth devices first
tools\session.ps1 -Stop            # hand the radio back to the stock driver
tools\session.ps1 -Uninstall       # remove everything, test signing off; then restart Windows
```

Read [docs/INSTALL.md](docs/INSTALL.md) before running these. It covers what each step changes, headset re-pairing, and recovery.

---

## Repository layout

| Path | Contents | Used by the Bluetooth driver |
|---|---|---|
| `src/driver/` | `deckbtusb.sys`: UdeCx host controller, USB endpoints, UART backend (`qca_uart.c`), synthetic HCI stub | yes |
| `src/common/`, `src/include/` | Kernel/user-mode shared logic: descriptors, H4 codec, HCI bridge, QCA firmware parser and bring-up state machine, SCO framing and routing | yes |
| `src/filter/` | `deckbtflt.sys`: optional lower filter that supplies a USB frame clock (`QueryBusTime`) | no. Voice works without it on the tested build. Used by the isochronous test stack and kept as a diagnostic |
| `src/isotest/`, `tools/isotest/` | Vendor-class UdeCx device and WinUSB harness that measured isochronous transfer support | no. Development instrument |
| `tools/*_selftest.c` | Host-side test suites compiled against the production sources | tests |
| `tools/*.ps1` | Install, session, recovery, diagnostics and test scripts | see [docs/INSTALL.md](docs/INSTALL.md) and [docs/BUILD.md](docs/BUILD.md) |
| `reference/` | Expected descriptors and HCI exchanges of the synthetic stub; isochronous measurement record | tests |

---

## Documentation

- [docs/INSTALL.md](docs/INSTALL.md): install, use, stop, uninstall, recovery
- [docs/BUILD.md](docs/BUILD.md): building and the test suites
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): driver design and Windows constraints
- [docs/QCA2066.md](docs/QCA2066.md): controller bring-up, firmware selection, power handling
- [docs/VERIFICATION.md](docs/VERIFICATION.md): what was tested and the results
- [docs/ROADMAP.md](docs/ROADMAP.md): implemented features and open work

---

## Credits

- **Linux Bluetooth drivers** ([`btqca.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/btqca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137), `btqca.h`, [`hci_qca.c`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/bluetooth/hci_qca.c?id=a5218c6474df97f2e7f3af15dcdfc674bae5c137), GPL-2.0). These are the reference for the Qualcomm vendor commands, the TLV firmware format, the NVM file-selection rule, in-band sleep, and choosing the HCI voice data path on the QCA2066. This project implements those protocols for Windows. It contains no Linux code.
- **[usbip-win2](https://github.com/vadimgrn/usbip-win2)** by vadimgrn (BSD-2-Clause). The optional `deckbtflt.sys` filter follows its UDE filter's approach of substituting a frame clock for the missing `QueryBusTime`.
- **Microsoft Learn**: the UdeCx, SerCx2 and Bluetooth driver documentation.
- **Bluetooth SIG**: Bluetooth Core Specification (HCI, USB transport, synchronous connections) and the Hands-Free Profile (mSBC).
- **Qualcomm and Valve**: the controller firmware, which the build copies from the Windows driver package already installed on the Deck. It is not redistributed.

---

## Disclaimers

- Steam Deck is a trademark of Valve Corporation. Windows is a trademark of Microsoft Corporation. Qualcomm is a trademark of Qualcomm Incorporated. This is an independent project, not affiliated with or endorsed by Valve, Microsoft, or Qualcomm.
- Protocol constants and vendor command values are used for interoperability.
- No firmware or other proprietary binaries are included in this repository.
- This is a kernel driver built and tested on one device. Use it at your own risk.

## License

[MIT](LICENSE)
