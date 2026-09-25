# Installing and using DeckBtUsb

DeckBtUsb replaces the stock Bluetooth driver on the Steam Deck OLED for one session at a time. Everything below runs from an **elevated PowerShell** in the repository root, after `Set-ExecutionPolicy -Scope Process Bypass` (which allows the scripts in that window only). Each step prints a result line and exits non-zero on failure. Session logs are written to `tools\_build\sessions\`.

> [!IMPORTANT]
> - The driver is test-signed. Test signing must be on, which requires Secure Boot off. Games with kernel anti-cheat usually refuse to start while test signing is on.
> - Windows runs one Bluetooth radio at a time. While DeckBtUsb runs, the stock driver is disabled.
> - Disconnect or switch off every Bluetooth device before `-Start` and `-Stop`. Connected peripherals can make a driver change require a restart; `-Start` refuses connected devices before disabling the stock radio.
> - Tested on one device only. Read [VERIFICATION.md](VERIFICATION.md) for what has and has not been tested.

## 1. Build

```powershell
tools\build.cmd
```

See [BUILD.md](BUILD.md). The build copies the controller firmware from the stock driver package on this machine.

## 2. Prepare (once)

```powershell
tools\session.ps1 -Prepare
```

- Records the current Bluetooth state and backs up the stock driver package to `recovery\baseline\`. Restore and uninstall use this backup.
- Exports the build's test certificate to `recovery\DeckBtUsbTestCert.cer`.
- Exports the boot configuration to `recovery\` and turns test signing on.
- Disables the stock Bluetooth radio. Bluetooth stays off until `-Start` or `-Stop`.

Restart Windows afterwards (not needed if test signing was already on in this boot). The `recovery\` directory is specific to this machine and is ignored by git.

## 3. Start

```powershell
tools\session.ps1 -Start
```

Trusts the test certificate, installs the driver package on the controller (`ACPI\QCOM2066`), loads the firmware and starts the virtual USB radio. Windows then shows the Bluetooth radio again with the existing pairings.

**Headsets:** a headset paired under the stock driver may connect with music only. Remove it in Settings and pair it again while DeckBtUsb runs, and its microphone becomes available.

**Sleep:** the radio returns about 7–8 seconds after S3 resume.

**Restart:** DeckBtUsb does not start the radio at boot. After a restart the driver is still installed but idle, and Windows has no Bluetooth radio. Run `-Stop` to return to the stock driver, and `-Start` again to use DeckBtUsb.

## 4. Check voice (optional)

With a headset connected:

```powershell
pip install numpy sounddevice
python tools\miccheck.py
```

It records 6 seconds (`--seconds` changes that) from the headset microphone while playing a quiet tone into the headset. Speak during the recording. The result is PASS, FAIL or NODEV (no Hands-Free link), and the recording is saved as a WAV file under `tools\_build\miccheck\`. `tools\sco-status.ps1` shows the driver's voice counters.

## 5. Stop

```powershell
tools\session.ps1 -Stop
```

Removes the DeckBtUsb package from the controller, waits until the driver has handed the controller back, and verifies that the stock driver and its Bluetooth radio are healthy. If Windows still needs to finish the removal at the next boot, the script says so. After that restart the stock driver owns the radio.

## 6. Uninstall

```powershell
tools\session.ps1 -Uninstall
```

Removes project packages, services and devices, verifies that the stock radio is healthy, then removes trust for the saved project signing certificate and turns test signing off. If restoration or package removal fails, trust and test signing are left intact for recovery. If the saved certificate cannot be identified, automatic certificate removal is refused. Removing its trust also affects any other packages signed with that same certificate. Restart Windows afterwards. `tools\verify-clean.ps1` checks the resulting state; a failed or unreadable check is not reported as clean.

## Recovery

| Situation | Action |
|---|---|
| No Bluetooth radio after a restart | `tools\session.ps1 -Stop` |
| `-Stop` reports that a restart is needed | save work, restart Windows, then run `-Stop` again to verify restoration |
| A timeout or unconfirmed driver retirement is reported | stop issuing device, service, uninstall, or rollback commands; preserve the session log and follow the recovery warning below |
| Another step fails without a timeout or retirement warning | resolve the reported failure before retrying; do not chain recovery commands |

**Timeout or unconfirmed retirement:** a responsive desktop does not prove the UART has been released. Save work and attempt a normal Windows restart rather than issuing more driver commands. If Windows cannot complete the restart, a forced power-off may be necessary and can lose unsaved data. Only after Windows has booted successfully should you run `tools\session.ps1 -Stop`. If it still fails, preserve the new log and report the failure instead of continuing with uninstall.

## Other modes

These modes install the driver, run one check, and restore the stock driver. They are for development and diagnosis:

| Mode | Does |
|---|---|
| `-Identify` | reads the controller's version at 115200 baud; no firmware |
| `-Probe` | full firmware bring-up and `HCI_Reset`, then hand-back |
| `-Bridge [-HoldSeconds n]` | as `-Start`, checks that Windows initialised the radio, optionally holds it for `n` seconds, then restores the stock driver |

`tools\diag.ps1` decodes the driver's recorded values. `tools\vendor-log.ps1` enables the stock driver's own debug log (see [QCA2066.md](QCA2066.md#vendor-log)).
