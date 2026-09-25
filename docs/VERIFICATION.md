# Verification

Hardware results from a Steam Deck OLED running Windows 11 Home 25H2 (build 26200), with Secure Boot and Memory Integrity off and test signing on.

## Host-side tests

`tools\selftest.cmd` tests the firmware parser, controller state machine, USB/HCI transport and recovery scripts without installing a driver. [BUILD.md](BUILD.md#tests) lists the suites and explains how to run them, including checks that deliberately inject faults to verify failure detection.

## Session and peripheral checks

The Release x64 packages built and signed successfully. The installed driver was tested with the session scripts in [INSTALL.md](INSTALL.md):

- `Prepare → Start` and `Prepare → Stop` passed.
- Discovery found 17 unpaired devices. All five existing pairings were preserved.
- `Stop` returned the radio to the stock driver. Full uninstall and `verify-clean.ps1` passed; the test-signing configuration change was checked without rebooting.
- Uninstall removed only the project signing certificate. Unrelated certificates and the original recovery files were preserved.
- Sleep/resume: the radio returned 8 seconds after waking.
- Bluetooth mouse: movement, clicks and scrolling worked; 3,155 input reports in 30 seconds.
- Shokz OpenMeet: microphone and playback worked at 16 kHz using mSBC/eSCO. The SCO lost-packet counter remained zero.
- No driver command failures.

## USB enumeration

Backend: synthetic HCI stub on a root-enumerated device, with the stock radio disabled.

```
ROOT\DEVGEN\DECKBTUSB                      deckbtusb.sys   started
  └─ USB\ROOT_HUB30\...                    usbhub3.inf
       └─ USB\VID_0CF3&PID_6390\DECKBT0001 BTHUSB          started
```

| Criterion | Result |
|---|---|
| Emulated radio enumerates and starts | pass: `Status: Started` |
| Inbox `BTHUSB.SYS` owns it | pass: `service=BTHUSB` |
| Windows builds its Bluetooth stack on it | pass: `BTH\MS_BTHBRB`, `BTH\MS_BTHLE`, `BTH\MS_RFCOMM` |

`BTHPORT` initialised the stub with 43 control transfers, with no command timeouts (event 3), wrong-size events (event 5), or LE state/filtering warnings (events 31, 34). Enumeration requirements:

| Symptom | Cause | Fix |
|---|---|---|
| `CM_PROB_FAILED_ADD`, `0xC000000D` in `EvtDeviceAdd` | no `GUID_DEVINTERFACE_USB_HOST_CONTROLLER` interface; USB 3 port count forced to 0 | publish the interface; keep the default port counts |
| Device stuck in `Error` | no `EvtUsbDeviceReset`, so `BTHUSB` had no reset path | implement the reset callback |
| Endless reset and retry | EP0 stalled standard requests such as `GET_STATUS` | answer `GET_STATUS`, `CLEAR_FEATURE`, `SET_FEATURE` |
| Event 34, LE peripheral role unavailable | supported-states mask too small (Windows requires `0x2491F7FFFFF`) | advertise all states |
| Events 5 and 3 | status-only replies to commands that return parameters; two LE opcodes swapped | spec-sized replies, opcodes corrected |
| Event 31 | LE accept/resolving list size 8, below Windows' hardware-filtering threshold | report 32 |

The stub's descriptors and command/event pairs are recorded in `reference/VIRTUAL-HCI-REFERENCE.txt`. `tools\check-reference.cmd` regenerates them from the sources and compares.

## Isochronous transport

Instrument: the vendor-class `isotest` device with WinUSB, driven through 432 transfer cells (alternate settings 1-6, packet counts 1-64, both directions, legal and deliberately illegal geometries). Full record: [`reference/isochronous/`](../reference/isochronous/SUMMARY.md).

- Without a frame clock every transfer failed: UdeCx answers `QueryBusTime` with `STATUS_NOT_SUPPORTED`.
- With the substituted clock (`deckbtflt.sys` mode 2): 216 legal cells accepted, 0 legal cells rejected, 216 illegal cells rejected as expected. Across 235 URBs, IN and OUT byte counts matched exactly, with 0 errors. Cancellation drained cleanly.

These measurements validate SCO packet geometry under UdeCx. The Bluetooth voice tests below used `BTHUSB` without the frame-clock filter.

## Controller bring-up

`tools\session.ps1` modes `-Identify`, `-Probe`, `-Bridge`.

| Check | Result |
|---|---|
| Identify at 115200 | CTS asserted, answer in 72 ms, reply byte-identical to the vendor driver's (SoC `0x400C1211`) |
| Firmware | rampatch 155,044 bytes, board `0x0309`, NVM `hpnv21g.309` 6,716 bytes, `HCI_Reset` complete `0E 04 01 03 0C 00`, 3.7 s |
| Bring-up without a reboot | controller left at 3,000,000 baud: reset, ROM at 115200, full firmware, 7.3 s |
| Hand-back | controller answers at 115200. The vendor driver rebinds healthy with every pairing, no reboot |
| `BTHPORT` initialisation over the real controller | `BTH\MS_BTHBRB` and `BTH\MS_BTHLE` OK, 44 commands |
| Discovery | 17 unpaired LE advertisers listed by Windows |
| Pairing | a BLE mouse paired through DeckBtUsb: LE Connection Complete status 0, Encryption Change enabled |
| Input | 3,749 HID input reports from that mouse in a 300 s hold |

The controller reports the same Bluetooth address to DeckBtUsb as to the vendor driver (no address is written), so pairings made under either driver apply to both.

## Sleep and resume

`tools\s3-cycle.ps1` puts the Deck into S3 with a wake timer and checks the radio after each resume.

- 62 resumes with DeckBtUsb bound, including 50 consecutive cycles.
- Every resume: firmware reloaded, replacement USB device plugged in 4.2-4.7 s after D0 entry on the first attempt, radio and both Bluetooth enumerators up 7-8 s after resume, 61-66 `BTHPORT` commands, every pairing present again.
- No hangs and no bugchecks.

## Voice

Tested with AirPods Pro and Shokz OpenMeet. `tools\miccheck.py` records from the headset microphone through WASAPI while playing a tone through its playback endpoint.

- Through DeckBtUsb, Windows exposes the Hands-Free service as an ordinary device (`BthHFEnum`/`BthHFAud`) and a 16 kHz capture endpoint. The AirPods, paired under the stock driver, connected with music only until they were removed and paired again through DeckBtUsb.
- `BTHPORT` negotiated eSCO with transparent air mode (mSBC), 7.5 ms interval, 60-byte packets. `BTHUSB` used alternate setting 6.
- Without the voice-routing rewrite the link came up on air, but no voice crossed the UART (the controller's default route is the offload path). With the rewrite, voice flows over HCI in both directions.
- Both directions at the voice rate: 2,530 host-to-headset packets sent, 2,530 delivered; 2,607 headset-to-host packets received, 2,607 delivered. 0 lost.
- `miccheck.py` passed on both headsets: at least 95 % of expected samples arrived, with at least 97 % of the energy in the speech band.
- Voice calls in Discord: 23 voice links, 23,919 frames in and 19,509 out, none lost.
- Recorded word endings decayed in about 50 ms on the AirPods and 115 ms on the Shokz. The cause of the difference is unknown.

## Not yet tested

- Narrowband voice (CVSD, alternate settings 1-5)
- Long calls, and calls across sleep
- Hibernate and Fast Startup
- Controller crash recovery
- Battery impact
- Other Steam Deck OLED units and other Windows builds
- Memory Integrity (HVCI) on
