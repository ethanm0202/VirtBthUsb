# VirtBthUsb Project Roadmap

This document outlines the milestones for VirtBthUsb, bridging non-USB Bluetooth controllers (specifically the Qualcomm WCN6855 on the Steam Deck OLED) into Windows' native USB Bluetooth stack.

---

## Milestone 1: Virtual USB Controller & BTHUSB Binding
Status: Complete
- [x] KMDF UdeCx virtual host controller (`deckbtusb.sys`)
- [x] High-Speed USB descriptor tables conforming to UdeCx constraints (`usb_descriptors.c`)
- [x] Synthetic HCI controller stub answering mandatory 43-command initialization (`hci_stub.c`)
- [x] Lower filter driver capturing bus-interface queries (`deckbtflt.c`)
- [x] Verified: Windows brings up `USB\ROOT_HUB30`, binds `BTHUSB.SYS`, and creates `MS_BTHBRB`, `MS_BTHLE`, and `MS_RFCOMM` enumerators.
- [x] Baseline USB descriptors and expected HCI responses captured (`reference/VIRTUAL-HCI-REFERENCE.txt`)

---

## Milestone 2: Isochronous Data Plane & QueryBusTime Filter
Status: Complete
- [x] Isochronous endpoint handling on Interface 1 alternate settings 0–6
- [x] Measured and recorded packet geometry support matrix under `ucx01000` (432 transfer cells)
- [x] QueryBusTime / QueryBusTimeEx synthesis implemented in `deckbtflt.sys` / `isoflt.sys`
- [x] Verified clean transfer teardown and cancellation
- [x] Geometry matrix and baseline measurements recorded (`reference/stage2-isoc-reference/`)
---

## Milestone 3: Qualcomm WCN6855 UART Transport Bridge
Status: In progress
- [ ] FDO binding to `ACPI\QCOM2066` SerCx2 resource-hub UART connection
- [ ] Firmware download FSM streaming rampatch (`hpbtfw21.tlv`) and NVM (`hpnv21.bin`) at 3,000,000 baud
- [ ] Switchable backend selector (`REG_DWORD` in hardware key: `0 = stub`, `1 = UART`) to allow instant diffing against the synthetic stub baseline
- [ ] Bidirectional ACL routing and event dispatch
- [ ] Acceptance criteria: RF HID input (mouse / gamepad) reaches Windows

---

## Milestone 4: Power Management & System Sleep
Status: Planned
- [ ] D0 / D3 power transitions
- [ ] S0ix Modern Standby suspend / resume cycles
- [ ] Fast firmware re-download on resume (~0.5s)
- [ ] Pairing retention across sleep cycles and reboots

---

## Milestone 5: SCO / eSCO In-Band Voice Audio
Status: Planned
- [ ] Fixed-latency jitter buffer between USB isochronous endpoints and UART SCO packets
- [ ] Bluetooth HFP voice profile negotiation
- [ ] In-band audio capture (microphone) and render (speaker)
- [ ] Acceptance criteria: Functional Bluetooth headset microphone in Windows Sound Settings
