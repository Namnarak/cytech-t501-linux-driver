# Cytech T501 Linux Driver

Native Linux HID driver for the SZ PING-IT / Gotop **T501** graphics tablet with USB ID `08f2:6811` and product string **`[T501] Driver Inside Tablet`**.

This driver was reverse-engineered from the tablet's bundled Windows driver and the vendor's 2018 macOS `MyTabletDaemon`. It exposes the device as a real Linux tablet instead of translating it into a relative mouse.

## Features

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

## Build

Requirements on Arch/CachyOS:

```bash
sudo pacman -S --needed base-devel linux-headers dkms clang llvm
```

Build against the running kernel:

```bash
make LLVM=1
```

Load manually for testing:

```bash
sudo insmod hid-cytech-t501.ko
```

## DKMS install

```bash
sudo mkdir -p /usr/src/cytech-t501-1.0.1
sudo cp hid-cytech-t501.c Makefile dkms.conf /usr/src/cytech-t501-1.0.1/
sudo dkms add -m cytech-t501 -v 1.0.1
sudo dkms build -m cytech-t501 -v 1.0.1
sudo dkms install -m cytech-t501 -v 1.0.1
sudo modprobe hid-cytech-t501
```

After loading, reconnect the tablet. Linux should expose input devices similar to:

```text
Cytech T501 Pen
Cytech T501 Pad Buttons
```

## Wayland / compositor mapping

The kernel driver intentionally does not know about monitors. Output mapping belongs to the compositor.

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
