# Cytech T501 Linux Driver

> [!WARNING]
> **v1.0.4 transport fix candidate.** v1.0.1 used the generic HID receive path for vendor report `0x06`; this tablet omits that report from its HID descriptor, so the kernel could bind without actually delivering pen packets. v1.0.4 now reads interface 1's 64-byte interrupt endpoint (`0x83`) directly and sends the full-area feature sequence with direct USB control transfers, matching the known-working userspace implementation. Keep this release experimental until it has broader hardware testing.

Native Linux HID driver research for the SZ PING-IT / Gotop **T501** graphics tablet with USB ID `08f2:6811` and product string **`[T501] Driver Inside Tablet`**.

This driver was reverse-engineered from the tablet's bundled Windows driver and the vendor's 2018 macOS `MyTabletDaemon`. The goal is to expose the device as a real Linux tablet instead of translating it into a relative mouse.

## Current status

- Protocol decoding: working
- PC/full-area initialization: working
- Absolute coordinates / pressure model: working
- Kernel HID device creation: working
- Kernel USB/HID report transport: **v1.0.4 direct-URB fix installed and under hardware verification**
- Direct feature-report control path: working on the tested device
- Userspace absolute-tablet path: retained as a fallback during development

## Intended features

- Native absolute pen coordinates
- Pressure reporting (`0..2047`)
- Pen tip / proximity
- Two stylus side buttons
- Tablet shortcut buttons
- PC/full-area mode initialization
- Keeps the built-in driver CD / mass-storage interface untouched
- DKMS support for automatic rebuilds after kernel updates

## Tested hardware

```text
USB: 08f2:6811
Manufacturer: SZ PING-IT INC.
Product: [T501] Driver Inside Tablet
```

`08f2:6811` is reused by other tablet-family devices, so the driver also checks the product name and is intended for this T501 variant.

## Build (experimental kernel module)

Requirements on Arch/CachyOS:

```bash
sudo pacman -S --needed base-devel linux-headers dkms clang llvm
```

Build against the running kernel:

```bash
make LLVM=1
```

For development/testing only:

```bash
sudo insmod hid-cytech-t501.ko
```

v1.0.4 bypasses the broken generic-HID receive path for the vendor packet stream. It still uses the Linux HID layer for device binding/hidraw, but owns a direct interrupt URB for endpoint `0x83`.

## DKMS install (experimental)

The DKMS path installs the current v1.0.4 transport fix. This is still an experimental hardware driver; keep a fallback input device available while testing.

```bash
sudo mkdir -p /usr/src/cytech-t501-1.0.4
sudo cp hid-cytech-t501.c Makefile dkms.conf /usr/src/cytech-t501-1.0.4/
sudo dkms add -m cytech-t501 -v 1.0.4
sudo dkms build -m cytech-t501 -v 1.0.4
sudo dkms install -m cytech-t501 -v 1.0.4
sudo modprobe hid-cytech-t501
```

When the kernel path works, Linux exposes input devices similar to:

```text
Cytech T501 Pen
Cytech T501 Pad Buttons
```

## Wayland / compositor mapping

The driver intentionally does not know about monitors. Output mapping belongs to the compositor.

For **labwc**, for example:

```xml
<tablet rotate="0" mouseEmulation="no">
  <mapToOutput>HDMI-A-1</mapToOutput>
  <area top="6.75" left="0.0" width="256.0" height="144.0" />
  <map button="Stylus" to="Right" />
  <map button="Stylus2" to="Middle" />
</tablet>
<tabletTool motion="absolute" minPressure="0.0" maxPressure="1.0" />
```

Adjust the output name and active area for your display/tablet setup.

## Debugging

Enable sampled packet logging:

```bash
sudo modprobe hid-cytech-t501 debug_packets=1
sudo journalctl -kf
```

Pressure thresholds can be changed with module parameters:

```text
pressure_hover
pressure_full
pressure_press
pressure_release
```

## Reverse engineering notes

See [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md) for packet format, feature reports, and evidence recovered from the vendor macOS driver.

## License

GPL-2.0-only. See [`LICENSE`](LICENSE).
