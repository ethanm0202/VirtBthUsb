# Building and testing

## Toolchain

The scripts use the Enterprise WDK (EWDK), a self-contained build environment (MSVC, Windows SDK, WDK, MSBuild) that needs no installation.

1. Download the Windows 11 EWDK ISO (build 26100 or newer) from [Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk).
2. Extract it to `C:\EWDK`, or mount it and set `EWDK` to the mounted drive. The optional Python extractor runs without mounting or elevation:

   ```powershell
   python -m pip install pycdlib
   python tools\extract_ewdk.py <iso> C:\EWDK
   ```

   A non-zero exit means extraction failed; do not use an incomplete toolchain.

Visual Studio 2022 with the WDK also builds the projects (`src\driver\deckbtusb.vcxproj`, `src\filter\deckbtflt.vcxproj`, `src\isotest\isotest.vcxproj`), but the scripts expect the EWDK.

## Driver packages

```cmd
rem Release
tools\build.cmd
rem Debug
tools\build.cmd Debug
```

`build.cmd` first runs `tools\stage-firmware.ps1`, which copies the five controller firmware files from the installed Valve/Qualcomm package (`qcbtuart.inf_amd64_*` in the DriverStore) into `src\driver\`. It then builds and test-signs:

- `src\driver\x64\<cfg>\deckbtusb\`: `deckbtusb.sys`, `.inf`, `.cat`, firmware
- `src\filter\x64\<cfg>\deckbtflt\`: `deckbtflt.sys`, `.inf`, `.cat`

The build checks that the package contains every file its INF lists. Test signing uses the WDK's per-user test certificate (`WDKTestCert <user>`), which MSBuild creates on first use. Firmware files and build outputs are ignored by git.

`tools\build-isotest.cmd` builds the isochronous test stack (`isotest.sys`, `isoflt.sys`, `isotest.exe`). The Bluetooth driver does not need it.

## Tests

```cmd
tools\selftest.cmd
```

No elevation, no driver installation, no hardware. Each C suite compiles production source files into a user-mode program:

| Suite | Sources under test | Covers |
|---|---|---|
| `descriptor_selftest` | `usb_descriptors.c` | configuration descriptor bytes, UdeCx High Speed and isochronous rules |
| `hci_selftest` | `hci_stub.c` | stub replies (43-command initialisation), LE state mask, FIFO limits |
| `qca_selftest` | `qca_tlv.c` | TLV parsing and segmentation against the real firmware files |
| `tlv_segment_selftest` | `qca_tlv.c` | segment boundaries for the firmware sizes, parameter lengths, acknowledgement rules |
| `qca_fsm_selftest` | `qca_init_fsm.c` | complete bring-up against a mock controller |
| `nvm_selftest` | `qca_init_fsm.c`, `qca_tlv.c` | NVM selection, the HCI rate-byte rewrite |
| `identify_selftest` | `qca_identify.c` | version-response parsing, including replies recorded from the controller |
| `h4_selftest` | `h4_codec.c` | H4 framing, split and malformed input, in-band sleep bytes |
| `bridge_selftest` | `hci_bridge.c` | readiness hold, vendor-event filtering, ACL credits |
| `sco_usb_selftest` | `sco_usb.c` | SCO pacing, OUT reassembly and resynchronisation, IN re-framing |
| `sco_route_selftest` | `sco_route.c` | enhanced synchronous-connection rewrite and opcode restore |
| `isotest_selftest` | `isotest/descriptors.c` | test-device alternate settings |

It then runs these script suites:

| Row | Script | Covers |
|---|---|---|
| `isoc_reference` | `check-isoc-reference.cmd` | the isochronous measurement record is unchanged (SHA-256 manifest) |
| `operator` | `operator-selftest.ps1` | session and recovery scripts, with device, service and registry access mocked |
| `service` | `service-selftest.ps1` | service-record reads, leftover cleanup, and the vendor-radio health guard |
| `recovery` | `recovery-selftest.ps1` | failed handback, certificate scope, package-query failures, and recovery snapshot preservation with machine operations mocked |
| `uart_identify` | `uart-identify-selftest.ps1` | the real UART identify routines from `qca_uart.c` against simulated WDF and serial faults |

`qca_selftest`, `qca_fsm_selftest` and `nvm_selftest` read the firmware from the newest installed `qcbtuart.inf_amd64_*` package in the DriverStore. Set `QCA_FW_DIR` to another directory containing `hpbtfw21.tlv` and the `hpnv21*` files to override that. `EWDK` overrides the toolchain location (default `C:\EWDK`) for the build and test scripts.

### Mutation checks

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\qca-mutation-check.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\operator-mutation-check.ps1
```

These scripts deliberately introduce faults into temporary source copies, such as accepting an invalid firmware response or continuing recovery after package removal fails. The corresponding test must detect each fault. A check fails if the altered code still passes, or if the source changed and the fault could not be applied. Neither script installs a driver or operates the radio.

### Reference check

```cmd
tools\check-reference.cmd
```

Regenerates the stub's descriptors and HCI exchanges with `tools\refdump.c` and compares them with `reference\VIRTUAL-HCI-REFERENCE.txt`. A difference means the USB frontend changed. If the change is intended, run `tools\refdump.cmd` and commit the new reference.

## Development without the radio

`tools\stub-install.ps1` installs the driver on a root-enumerated device with the synthetic HCI stub. Windows can then enumerate and initialise the virtual radio without the Qualcomm controller. It requires the stock radio to be disabled (Windows allows one radio). Stages, run from an elevated PowerShell:

```powershell
tools\stub-install.ps1 -Stage Prepare     # test certificate, test signing; restart afterwards
tools\stub-install.ps1 -Stage Install     # stage the package, create the root device (disarmed)
tools\stub-install.ps1 -Stage Arm         # start the virtual radio once
tools\stub-install.ps1 -Stage Disarm      # stop it
tools\stub-install.ps1 -Stage Uninstall   # remove the root device and package
tools\stub-install.ps1 -Stage RestoreRadio  # re-enable the stock radio
tools\diag.ps1                            # decode the driver's registry records
```
