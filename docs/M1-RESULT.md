# Milestone M1 — result

**Date:** 2026-09-07
**Machine:** Steam Deck OLED (Galileo), Windows 11 Home build 26200, Secure Boot off, HVCI off
**Outcome:** PASSED, all three criteria

---

## Purpose

Verify that Windows' inbox `BTHUSB.SYS` binds to, starts, and initialises an emulated USB Bluetooth radio device before implementing the physical hardware transport backend.

## What was measured

Device topology Windows built, from `Microsoft-Windows-Kernel-PnP/Configuration`:

ROOT\DEVGEN\DECKBTUSB                      deckbtusb.sys   (UDE host controller)      started
  └─ USB\ROOT_HUB30\1&617241a&0&0          usbhub3.inf     (Windows USB root hub)
       └─ USB\VID_0CF3&PID_6390\DECKBT0001 BTHUSB          started
            stack: BTHUSB / DeckBtFlt / USBHUB3
```

Acceptance:

| # | Criterion | Result |
|---|---|---|
| 1 | Emulated radio enumerates and starts | **PASS** — `Status: Started` |
| 2 | Inbox `BTHUSB.SYS` owns it | **PASS** — `service=BTHUSB`, matched via `USB\VID_0CF3&PID_6390`, rank `0x00FF0001`, outranking `bth.inf`'s `0x00FF2003` |
| 3 | Windows builds its Bluetooth stack on it | **PASS** — 3/3 enumerators: `BTH\MS_BTHBRB` (classic, pairing, profiles), `BTH\MS_BTHLE`, `BTH\MS_RFCOMM` |

HCI initialisation, single clean start, 43 control transfers, no errors and no retry cycle:

```
GET_STATUS · HCI_Reset · Read_BD_ADDR · Read_Local_Supported_Commands · Read_Buffer_Size
Read_Local_Version · Read_Local_Features · Write_Simple_Pairing_Mode
Write_Authentication_Enable · Set_Event_Mask · Read_Inquiry_Rsp_Tx_Power
Write_Page_Timeout · Write_Page_Scan_Activity · Write_Page_Scan_Type
Write_Inquiry_Scan_Activity · Write_Inquiry_Scan_Type · Write_Inquiry_Mode
Write_Class_Of_Device · Write_Extended_Inquiry_Response · Host_Buffer_Size
Write_Local_Name · LE_Read_Local_Features · LE_Read_Supported_States
LE_Read_Buffer_Size · LE_Read_Accept_List_Size · LE_Read_Max_Data_Length
LE_Write_Suggested_Default_Data_Length · Write_LE_Host_Support
LE_Read_Adv_Tx_Power · LE_Set_Event_Mask · Read_Local_Supported_Codecs
Write_Scan_Enable · then SDP service registration (repeated CoD + EIR updates)
```

`BTHUSB` event log after the fixes: no id 3 (command timeout), no id 5 (wrong event size), no
id 31 (LE advertisement filtering), no id 34 (LE state mask). Only id 18 remains, *Information*
level.

## Issues Resolved During Initial Bring-up

| Symptom | Root cause | Fix |
|---|---|---|
| `CM_PROB_FAILED_ADD` `0xC000000D` in `EvtDeviceAdd` | two deviations from the reference UDE client: no `WdfDeviceCreateDeviceInterface(GUID_DEVINTERFACE_USB_HOST_CONTROLLER)`, and `NumberOfUsb30Ports` forced to 0 | publish the interface; leave the port counts at the documented defaults |
| Device reached `Error` and stayed there | `EvtUsbDeviceReset` was not registered, so `GUID_DEVICE_RESET_INTERFACE_STANDARD` and `GUID_REENUMERATE_SELF_INTERFACE_STANDARD` were answered `STATUS_NOT_SUPPORTED` and BTHUSB had no recovery path | implement the reset callback; reinitialise the stub and complete the request |
| Endless reset-and-retry cycle | EP0 handler stalled requests outside of HCI commands, including standard `GET_STATUS` | answer `GET_STATUS`, `CLEAR_FEATURE`, `SET_FEATURE` |
| id 34, LE peripheral role unavailable | `g_LeStates` advertised `0x1FFFFFFFFF`; Windows requires `0x2491F7FFFFF` | advertise all states; regression test asserts the required mask is a subset |
| id 5, id 3 | status-only replies to commands that must return parameters; additionally `0x202F` (`LE_Read_Maximum_Data_Length`) and `0x2023` (`LE_Read_Suggested_Default_Data_Length`) were swapped | ten spec-sized replies added, opcodes corrected |
| id 31 | LE accept/resolving list sizes of 8, below Windows' hardware-filtering threshold | advertise 32 |

## Observations

- *"UdeCx will not provide `USB_BUS_INTERFACE_USBDI` V3."* It does: the filter recorded
  `size=0x60 ver=3 -> STATUS_SUCCESS`. The `QueryBusTime` defect may still bite when isochronous
  traffic actually flows, but the interface handshake itself is fine.
- *"Descriptors or the configuration blob are wrong."* Enumeration never failed once the port
  counts were corrected.

## Operational Constraints

1. **Single Bluetooth Adapter Constraint:** `BTHUSB` logs *"Only one active Bluetooth adapter is supported at a time"* and fails `AddDevice` while another radio is active. The driver must replace the existing transport driver on `ACPI\QCOM2066`; simultaneous operation of multiple adapters is not supported by Windows.
2. **INF Ranking:** `deckbtflt.inf` matching `USB\VID_0CF3&PID_6390` (`0x00FF0001`) takes precedence over `bth.inf` matching `USB\Class_E0&SubClass_01&Prot_01` (`0x00FF2003`), while `Include`/`Needs = BthUsb.NT` retains BTHUSB as the function driver. This allows the filter driver to attach beneath BTHUSB.
3. **Scripted Device Disabling:** `pnputil /disable-device` and `Disable-PnpDevice` are restricted on Windows 11 Home editions; manual disable in Device Manager is used during testing.

## Scope & Limitations

- Isochronous traffic was not exercised.
- Physical UART hardware was not attached.
- The synthetic controller does not perform RF operations (scanning, pairing, or audio streaming).

## Verification

See `docs/BUILD.md`. In short: `tools\build.cmd`, `tools\selftest.cmd`, `m1-install.ps1 -Stage Prepare`,
reboot, disable the physical transport in Device Manager, `m1-install.ps1 -Stage Install`,
`m1-diag.ps1`, re-enable the physical transport.
