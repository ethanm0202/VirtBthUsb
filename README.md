# VirtBthUsb

A Windows kernel driver that makes the Steam Deck OLED's Bluetooth controller appear to Windows as a USB Bluetooth radio, so Windows' built-in USB Bluetooth drivers run it. With those drivers, Bluetooth headset microphones (the Hands-Free profile) work, which they do not with the stock driver.

The driver and its service are named `DeckBtUsb`.

> [!WARNING]
> The driver is test-signed. It only loads with Windows test signing turned on, and games with kernel anti-cheat usually refuse to start while test signing is on. `tools\session.ps1 -Uninstall` removes everything and turns test signing off again.

---

## Background

The Steam Deck OLED's Bluetooth controller is a Qualcomm QCA2066 connected over a UART (ACPI device `QCOM2066`). Windows drives it through the UART transport stack: `qcbtuart.sys`, then `BthMini.sys`. That stack carries voice only over a hardware offload path. `BthMini.sys` accepts no other SCO mode from its transport and fails with `STATUS_DEVICE_CONFIGURATION_ERROR` (`0xC0000182`) otherwise. On the Deck, Hands-Free devices enumerate in offload mode (`_HCIBYPASS_`), and the headset microphone does not work. Music playback (A2DP) is unaffected.

Windows' USB Bluetooth transport (`BTHUSB.SYS`) carries voice in-band, over isochronous USB endpoints. DeckBtUsb takes the UART controller from the stock driver, loads its firmware, and presents it through a virtual USB host controller (UdeCx) as a standard USB Bluetooth device. Windows loads `BTHUSB.SYS` and `BTHPORT.SYS` on that device, and voice travels over HCI like on any USB dongle.

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

Tested on one Steam Deck OLED running Windows 11 25H2 (build 26200), test signing on, Memory Integrity off. Details and measurements: [docs/VERIFICATION.md](docs/VERIFICATION.md).

- **Working:** firmware bring-up, discovery, pairing and encryption, BLE input devices, Classic headsets, music (A2DP), and Hands-Free voice in both directions with wideband (mSBC) audio. Existing pairings keep working because the controller reports its own address.
- **Working:** sleep (S3). After each resume the driver reloads the firmware and Windows reinitialises the radio, about 7 seconds after wake.
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
| `src/filter/` | `deckbtflt.sys`: optional lower filter that supplies a USB frame clock (`QueryBusTime`) | no. BTHUSB does not need it. Kept for the isochronous test stack and as a fallback |
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
