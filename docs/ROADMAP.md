# Roadmap

Implemented features and remaining work. Test results are in [VERIFICATION.md](VERIFICATION.md).

## Virtual USB radio

- [x] UdeCx virtual host controller with an emulated USB Bluetooth device
- [x] Descriptors that satisfy UdeCx's High Speed and isochronous rules
- [x] Synthetic HCI stub answering Windows' initialisation
- [x] `BTHUSB`/`BTHPORT` bind and create the Bluetooth enumerators

## Isochronous transport

- [x] Alternate settings 0-6 on the SCO interface
- [x] Packet geometry measured under `ucx01000` (432 transfer cells)
- [x] `QueryBusTime` substitution filter for the test stack

## QCA2066 UART transport

- [x] Take the controller from the stock driver through its SerCx2 connection, with the CTS wake handshake
- [x] Bring-up from ROM or a controller left running vendor firmware: identify, 3,000,000 baud, rampatch, NVM, `HCI_Reset`
- [x] Bridge to `BTHUSB`: commands, events, ACL, in-band sleep
- [x] Discovery, pairing, encryption, BLE input
- [x] Hand the controller back to the stock driver without a reboot

## Sleep

- [x] Re-initialise the controller after S3 and replace the emulated USB device
- [x] Pairings kept across sleep

## Voice

- [x] SCO/eSCO over the isochronous endpoints with pacing
- [x] Voice links placed on the HCI data path (enhanced synchronous connection setup)
- [x] Wideband (mSBC) microphone and speaker through Windows' Hands-Free driver

## Open work

- **Signing.** A Microsoft attestation signature, which would remove the need for test signing. This is the main obstacle to normal use.
- **Start at boot**, with automatic fallback to the stock driver if bring-up fails.
- **Controller crash recovery** (the controller resetting itself while in use).
- **Narrowband voice** (CVSD, alternate settings 1-5): implemented, not yet tested.
- **Testing:** long calls, calls across sleep, hibernate and Fast Startup, battery impact, Memory Integrity, other units.
- **Microphone start latency:** opening the microphone takes about 0.9 s before audio flows.
