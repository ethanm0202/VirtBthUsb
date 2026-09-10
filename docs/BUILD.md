# Building & Testing VirtBthUsb

## Prerequisites

- **Windows 11** (x64)
- **Visual Studio 2022** with Windows Driver Kit (WDK) **OR** the standalone Enterprise WDK (EWDK) ISO
- For kernel driver installation testing:
  - Secure Boot **disabled** (required for test-signed drivers)
  - Memory Integrity (HVCI) **disabled**
  - Test signing enabled (`bcdedit /set testsigning on`)

---

## 1. Toolchain Setup

### Option A: Standalone EWDK (Zero-Install)
Microsoft provides a self-contained command-line build environment containing MSVC, Windows SDK, WDK, and MSBuild.

1. Download the Windows 11 EWDK ISO (Build 26100 or newer) from Microsoft's hardware dev center.
2. Mount or extract the ISO to `C:\EWDK`.
3. The build script automatically detects `C:\EWDK\BuildEnv\SetupBuildEnv.cmd`.

### Option B: Visual Studio 2022 + WDK
If you have Visual Studio 2022 with the "Desktop development with C++" workload and the Windows 11 WDK installed, open a **Developer Command Prompt for VS 2022**.

---

## 2. Compiling the Drivers

Run the build script from the repository root:

```cmd
tools\build.cmd            :: Release build (default)
tools\build.cmd Debug      :: Debug build
```

This compiles and packages both drivers:
- `src\driver\x64\Release\deckbtusb\`: `deckbtusb.sys` + `.inf` + `.cat` (Virtual UdeCx Host Controller)
- `src\filter\x64\Release\deckbtflt\`: `deckbtflt.sys` + `.inf` + `.cat` (Bus Interface Lower Filter)

---

## 3. Host-Side Verification (Self-Tests)

The repository includes four unit test suites that compile the production source files directly into user-mode executables, allowing verification without deploying a kernel driver:

```cmd
tools\selftest.cmd
```

| Test Suite | Source File | Validations |
|---|---|---|
| `descriptor_selftest` | `src\common\usb_descriptors.c` | Verifies full 200-byte configuration set and UdeCx High-Speed / isoch constraints |
| `hci_selftest` | `src\driver\hci_stub.c` | Verifies 43 HCI return parameter lengths, LE state masks, FIFO boundaries |
| `qca_selftest` | `src\common\qca_tlv.c` | Verifies TLV header parser and 243-byte segmentation against real firmware |
| `qca_fsm_selftest` | `src\common\qca_init_fsm.c` | Runs full 673-command bring-up FSM against a mock chip |

*Note: For `qca_selftest` and `qca_fsm_selftest`, point the `QCA_FW_DIR` environment variable to a directory containing `hpbtfw21.tlv` and `hpnv21.bin` if testing outside a system with Qualcomm drivers installed.*

---

## 4. Test Installation

To test the driver locally on a machine with test signing enabled:

```powershell
# Open an elevated PowerShell prompt in tools/
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\m1-install.ps1 -Stage Prepare    # Installs test cert, configures testsigning (requires reboot)
# ---- REBOOT ----
.\m1-install.ps1 -Stage Install    # Stages driver package and creates root\DeckBtUsb devnode
.\m1-diag.ps1                      # Displays diagnostic registry breadcrumbs and EP0 transfer log
.\m1-install.ps1 -Stage Uninstall  # Clean rollback
```
