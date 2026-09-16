# Cytech T501 Linux Driver

> [!WARNING]
> **v1.0.6 current test release.** The driver reads interface 1's 64-byte interrupt endpoint (`0x83`) directly, sends the vendor full-area sequence through USB control transfers, and now exposes all twelve frame buttons as remappable Linux input buttons. It is working on the tested T501, but broader hardware testing is still welcome.

Native Linux HID driver research for the SZ PING-IT / Gotop **T501** graphics tablet with USB ID `08f2:6811` and product string **`[T501] Driver Inside Tablet`**.

This driver was reverse-engineered from the tablet's bundled Windows driver and the vendor's 2018 macOS `MyTabletDaemon`. The goal is to expose the device as a real Linux tablet instead of translating it into a relative mouse.

## Current status

- Protocol decoding: working
- PC/full-area initialization: working
- Absolute coordinates / pressure model: working
- Kernel HID device creation: working
- Kernel USB/HID report transport: direct interrupt URB on endpoint `0x83`, working on the tested device
- Direct feature-report control path: working on the tested device
- Native frame buttons: 12 distinct remappable buttons
- Userspace absolute-tablet path: retained only as a development fallback

## Intended features

- Native absolute pen coordinates
- Pressure reporting (`0..2047`)
- Pen tip / proximity
- Two stylus side buttons
- 12 individually addressable tablet frame buttons
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

v1.0.6 bypasses the broken generic-HID receive path for the vendor packet stream. It still uses the Linux HID layer for device binding/hidraw, but owns a direct interrupt URB for endpoint `0x83`.

## DKMS install

The installer also installs the udev tablet-pad classification rule for the frame buttons.

```bash
sudo mkdir -p /usr/src/cytech-t501-1.0.6
sudo cp hid-cytech-t501.c Makefile dkms.conf /usr/src/cytech-t501-1.0.6/
sudo cp 99-cytech-t501-pad.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo dkms add -m cytech-t501 -v 1.0.6
sudo dkms build -m cytech-t501 -v 1.0.6
sudo dkms install -m cytech-t501 -v 1.0.6
sudo modprobe hid-cytech-t501
```

When the kernel path works, Linux exposes input devices similar to:

```text
Cytech T501 Pen
Cytech T501 Pad Buttons
```


## Tablet frame buttons

The T501 sends twelve distinct frame-button values. Older revisions of this driver hard-coded them to keyboard shortcuts. v1.0.6 exposes them as real evdev pad buttons instead:

```text
Pad1..Pad10  -> BTN_0..BTN_9
Pad11        -> BTN_TRIGGER_HAPPY1
Pad12        -> BTN_TRIGGER_HAPPY2
```

The included udev rule marks `Cytech T501 Pad Buttons` as `ID_INPUT_TABLET_PAD=1`, so libinput/Wayland compositors can treat it as a tablet pad rather than a keyboard or joystick. Stylus side buttons remain `BTN_STYLUS` and `BTN_STYLUS2`.

On labwc, the compositor currently exposes pad mappings as `Pad`, `Pad2`, ... `Pad9`; additional buttons are still available at the evdev level for applications/remappers.

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

### Pad bitfield note (v1.0.6)

The frame-button bytes are an **active-low bitfield**, not one 16-bit key value. This means simultaneous presses are valid. v1.0.6 decodes each of the 12 bits independently and emits correct press/release transitions; e.g. `0xfa33` represents two active buttons rather than an unknown key.
