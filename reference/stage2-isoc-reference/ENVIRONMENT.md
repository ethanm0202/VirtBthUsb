# Stage 2 Isochronous Reference Environment

## Measured Framework Environment

```
OS_Caption = Microsoft Windows 11 Home
OS_Version = 10.0.26200
OS_Build = 26100.1.amd64fre.ge_release.240331-1435
OS_DisplayVersion = 25H2
ucx01000.sys = 10.0.26100.8972  sha256[0:16]=DDA7856FE987AA94
udecx.sys    = 10.0.26100.8972  sha256[0:16]=6DA8B54B88E22938
winusb.sys   = 10.0.26100.8972  sha256[0:16]=AC22B89F159699BD
usbhub3.sys  = 10.0.26100.1     sha256[0:16]=A0A985E2D6D93B6A
usbccgp.sys  = 10.0.26100.8972  sha256[0:16]=51820D18B2320217
bthusb.sys   = 10.0.26100.8655  sha256[0:16]=F0CCDBF9FD4B2D7D
bthport.sys  = 10.0.26100.8655  sha256[0:16]=B937E4D95C2FD8BA
winusb.dll   = 10.0.26100.1150
```

## Harness and Driver Timestamps

- **Harness Build Stamp:** `Sep  8 2026 15:39:09` (`isotest.exe` last write: `2026-09-08 15:39:10` local / `2026-09-08 20:39:10 UTC`, size: 175616 bytes)
- **Isotest Driver Package Timestamps:**
  - `isotest.inf` DriverVer: `09/08/2026,15.39.5.288`
  - `isotest.sys` last write: `2026-09-08 15:39:07` local / `2026-09-08 20:39:07 UTC` (size: 23392 bytes)
  - `isoflt.sys` last write: `2026-09-08 15:39:07` local / `2026-09-08 20:39:07 UTC` (size: 23152 bytes)

These versions are the exact framework binaries the result was measured against and that a different `udecx.sys`/`ucx01000.sys`/`winusb.sys` invalidates nothing but must be re-measured.
