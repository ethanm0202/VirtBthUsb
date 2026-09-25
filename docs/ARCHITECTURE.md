# Architecture

DeckBtUsb is a KMDF driver built on the USB Device Emulation class extension (UdeCx). It creates a virtual USB host controller with one emulated USB Bluetooth device. Windows binds its inbox USB Bluetooth driver (`BTHUSB.SYS`) and Bluetooth stack (`BTHPORT.SYS`) to that device. DeckBtUsb carries the HCI traffic between the emulated device and a backend: either the real controller or a synthetic stub.

## Why a virtual USB device

On the Steam Deck OLED the Bluetooth controller, a Qualcomm QCA2066, is attached to an AMD UART (`ACPI\AMDI0020`, driver `amduart.sys`, a SerCx2 client). Its ACPI node `ACPI\QCOM2066` is normally owned by Qualcomm's `qcbtuart.sys`, which exposes the radio through the BthX transport miniport `BthMini.sys`.

- BthX expects synchronous voice (SCO/eSCO) to leave the HCI transport through a hardware path ("HCI bypass"). `BthMini.sys` accepts no other SCO mode from its transport and fails with `STATUS_DEVICE_CONFIGURATION_ERROR` (`0xC0000182`) otherwise.
- With the stock driver, Hands-Free devices enumerate in offload mode (`BTHENUM\{0000111E-...}_HCIBYPASS_...`), and the headset microphone does not work.
- `BTHUSB.SYS` carries voice in-band over isochronous endpoints. Through it the Hands-Free devices enumerate as ordinary `{0000111E-...}` devices with `BthHFEnum`/`BthHFAud`, and voice travels over HCI.

Windows allows one active Bluetooth radio at a time. With a second radio active, `BTHUSB` logs event 6 ("Only one active Bluetooth adapter is supported at a time") and fails `AddDevice`. DeckBtUsb therefore replaces `qcbtuart.sys` on `ACPI\QCOM2066` instead of running beside it.

## Components

```
BTHPORT.SYS
  │
BTHUSB.SYS ── USB\VID_0CF3&PID_6390 (emulated child, class E0/01/01)
  │
USBHUB3.SYS (virtual root hub)
  │
deckbtusb.sys
  ├─ USB frontend       src/driver/device.c, endpoints.c, src/common/usb_descriptors.c
  ├─ HCI transport      src/include/hci_transport.h (stream FIFOs between frontend and backend)
  ├─ backend: stub      src/driver/hci_stub.c        (root\DeckBtUsb, development)
  └─ backend: UART      src/driver/qca_uart.c        (ACPI\QCOM2066)
        ├─ bring-up     src/common/qca_init_fsm.c, qca_tlv.c, qca_identify.c
        ├─ H4 framing   src/common/h4_codec.c
        ├─ HCI bridge   src/common/hci_bridge.c, sco_route.c
        └─ SCO framing  src/common/sco_usb.c
```

The INF (`src/driver/deckbtusb.inx`) binds two hardware IDs:

- `root\DeckBtUsb`: a root-enumerated development device that uses the synthetic HCI stub. It never touches the radio.
- `ACPI\QCOM2066`: the real controller. The INF writes `Backend = 1` to the device's hardware key, which selects the UART backend. It also copies the settings of the vendor INF's hardware section (device security, out-of-band host wake, directed power transitions).

A backend starts only when the service value `HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters\Enabled` is exactly `REG_DWORD 1` (`src/include/arming.h`). Any other state leaves a healthy but idle device. The stub consumes the value, so it runs once per start. The UART backend does not consume it, but the session scripts set it back to `0` after starting, so the radio does not start by itself at the next boot.

## USB frontend

The emulated device is a non-composite Bluetooth device (`bDeviceClass 0xE0`, subclass `0x01`, protocol `0x01`) with the standard layout:

| Endpoint | Type | Carries |
|---|---|---|
| EP0 | control | HCI commands (class request) |
| 0x81 | interrupt IN | HCI events |
| 0x02 / 0x82 | bulk OUT / IN | ACL data |
| 0x03 / 0x83 | isochronous OUT / IN, interface 1 alternate settings 0-6 | SCO voice, 0/9/17/25/33/49/63 bytes per packet |

UdeCx and `ucx01000.sys` impose constraints that a real Full Speed dongle's descriptors violate:

1. **High Speed.** The configuration descriptor is validated against High Speed rules whatever speed is declared. A Full-Speed-shaped descriptor fails with `CONFIGURATION_DESCRIPTOR_VALIDATION_FAILURE` (`0xC000000D`). The device declares `UdecxUsbHighSpeed`.
2. **Bulk `wMaxPacketSize` 512.** Required under High Speed rules (a Full Speed dongle reports 64).
3. **Isochronous `bInterval` 4.** `bInterval` is interpreted in 125 µs microframes: $2^{4-1} \times 125\,\mu\text{s} = 1\,\text{ms}$, the Bluetooth SCO frame. Smaller values fail URB submission with `USBD_STATUS_INVALID_PARAMETER`.
4. **Dynamic endpoints.** `UdecxEndpointTypeSimple` forbids alternate settings, and SCO needs alternate settings 0-6. The device uses `UdecxEndpointTypeDynamic`.
5. **No interface association.** A composite descriptor makes Windows insert `usbccgp.sys` and split the interfaces into separate devnodes. `BTHUSB.SYS` must own the whole device.
6. **Reset callback.** Without `EvtUsbDeviceReset`, `BTHUSB` has no recovery path after an initialisation failure and the device stays in an error state.

The emulated device uses the hardware ID `USB\VID_0CF3&PID_6390`. Inbox `bth.inf` matches its compatible ID `USB\Class_E0&SubClass_01&Prot_01`, so `BTHUSB.SYS` is the function driver without any INF of this project.

## UART backend

`qca_uart.c` owns the controller's serial connection (a SerCx2 resource-hub connection from the ACPI `_CRS`). Blocking UART I/O runs on a dedicated system thread; PnP and power callbacks may wait for a bounded hand-back from that worker. Firmware buffers are nonpaged because parsing and segment copies also run under the controller spin lock at `DISPATCH_LEVEL`. [QCA2066.md](QCA2066.md) describes the controller-specific steps. In outline:

1. **Take over.** Open the UART, configure 8N1 with RTS/CTS flow control, and wake the controller if it holds CTS low.
2. **Reach ROM state.** Identify at 115200 baud. A controller left running at the operating rate by another driver is reset first.
3. **Bring-up.** Switch to 3,000,000 baud, download the rampatch and the board-specific NVM (with one byte rewritten, as the vendor driver does), then send `HCI_Reset`.
4. **Publish.** Plug in the emulated USB device. `BTHPORT` then initialises the controller through `BTHUSB` like any USB radio.
5. **Bridge.** Move HCI packets between the USB endpoints and the UART in H4 framing until the device stops.
6. **Hand back.** On stop, reset the controller to ROM at 115200 baud so the vendor driver can own it again.

Progress and results are recorded as `Uart*` values under the service `Parameters` key (for example `UartFailurePhase`, `UartHandbackBaud`, `UartBridgeScoIn`). The session scripts and `tools/diag.ps1` read them.

### HCI bridge

`hci_bridge.c` moves commands, events, ACL and SCO between the frontend's stream FIFOs and the wire layer in `qca_uart.c`, which adds H4 framing. Along the way:

- **Readiness.** A host command that arrives during firmware bring-up is held and sent once the controller is ready. Stalling EP0 instead would make `BTHUSB` fail initialisation and restart the device.
- **Vendor events.** Qualcomm vendor events (event code `0xFF`) are dropped. `BTHPORT` never requested them.
- **ACL credits.** The bridge learns the controller's ACL buffer count from `Read_Buffer_Size`, tracks packets in flight per connection handle, and credits them back from `Number_Of_Completed_Packets` and disconnections.
- **In-band sleep bytes.** The controller uses Qualcomm in-band sleep (IBS): single bytes `0xFE` (sleep), `0xFD` (wake indication) and `0xFC` (wake acknowledgement) between H4 packets. The H4 decoder removes them from the HCI stream. Each wake indication must be acknowledged, or the controller keeps waiting and HCI traffic stops. Acknowledgements are sent from a passive-level worker, as the vendor driver does, and steady-state reads complete on the first byte so that no indication waits behind a read interval.

`qca_uart.c` edits two kinds of HCI traffic, both following what the stock stack or upstream Linux does:

- **`Write_LE_Host_Support`.** `BTHPORT` sets `Simultaneous_LE_Host = 1`, which this controller rejects with status `0x11`. The completion is reported to `BTHPORT` as success. The vendor driver makes the same change.
- **Voice routing** (`sco_route.c`). `BTHPORT` sets up voice links with the legacy `Setup_/Accept_Synchronous_Connection` commands (`0x0428`/`0x0429`), which have no data-path field. This controller's default voice route is the offload path, so a legacy setup produces a working link on air that carries no voice over HCI. The commands are rewritten to `Enhanced_Setup_/Enhanced_Accept_Synchronous_Connection` (`0x043D`/`0x043E`) with input and output data path `0x00` (HCI). The codec fields come from `BTHPORT`'s voice setting: transparent air mode carries mSBC frames, and CVSD carries 16-bit linear PCM on the host side. The controller's answers get `BTHPORT`'s legacy opcode back. Upstream Linux selects the HCI or offload path for this chip the same way.

### Voice over the isochronous endpoints

`sco_usb.c` converts between USB isochronous packets and HCI SCO packets and provides the time base a real bus would provide:

- **Pacing.** SCO has no host flow control, and `BTHUSB` learns the voice rate only from how fast isochronous transfers complete. A transfer therefore completes only once the air time it represents has passed. For alternate settings 1-5 a packet represents one 1 ms frame. At alternate setting 6 (63 bytes, mSBC) each non-empty OUT packet is one whole 60-byte mSBC frame, which is 7.5 ms of voice. A pacing timer in `endpoints.c` completes parked transfers when they are due.
- **Host to controller.** OUT packet bytes are queued with their due time and released at that time. HCI SCO packets are reassembled from the stream. When the queue is full the oldest frame is dropped, and reassembly resynchronises on the connection handle.
- **Controller to host.** The controller picks its own SCO packet size on the UART. The IN framer re-cuts the payload into the geometry a USB controller produces for the selected setting. For settings 1-5, one HCI packet of `3 * wMaxPacketSize - 3` payload bytes spans three isochronous packets. At setting 6, one 60-byte packet fills one isochronous packet. Frames with no data carry zero bytes, so gaps are never filled with invented audio.

Voice counters are published as `Sco*` values under the service `Parameters` key (`tools/sco-status.ps1`).

## Power

The Steam Deck OLED uses S3 sleep under Windows, and the controller loses its firmware across it.

- **D0 exit** (`DeckBtEvtControllerD0Exit`): the session stops and the controller is handed back. When the target is a sleep state (not removal), the device is marked for re-arm.
- **D0 entry** (`DeckBtEvtControllerD0Entry`): the worker repeats the bring-up. D0 entry never fails for this. A failed re-arm leaves the radio absent and is recorded in `ResumeLastStatus`.
- **Child replacement** (`DeckBtPublishUartDevice`): once the controller answers, the emulated USB device from before the sleep is unplugged (surprise removal, as if a dongle were pulled out) and a new one is plugged in. `BTHPORT` then reinitialises the controller from `HCI_Reset`. Timing is recorded as `ResumeReadyMs`, `ResumeReplacements`, `ResumePlugAttempts` and `ResumePlugStatus`.

## Optional lower filter (`deckbtflt.sys`)

UdeCx does not implement `QueryBusTime`/`QueryBusTimeEx` in the USB bus interface (`STATUS_NOT_SUPPORTED`), and WinUSB's isochronous API depends on it. `deckbtflt.sys` is a lower filter that can substitute a monotonic frame clock derived from `KeQueryPerformanceCounter` (1 ms frames, 125 µs microframes). The approach follows the UDE filter in [usbip-win2](https://github.com/vadimgrn/usbip-win2).

`BTHUSB.SYS` does not need it. It does not import `USBD_QueryBusTime`, and voice works without the filter. The filter is used by the isochronous test stack (`src/isotest/`) and kept as a fallback (`tools/bus-time-filter.ps1`). `IsochClockMode` in its service `Parameters` selects 0 (pass-through, default), 1 (observe and log) or 2 (substitute on failure).
