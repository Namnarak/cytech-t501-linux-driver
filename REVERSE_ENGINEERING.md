# T501 protocol notes

Target: SZ PING-IT / Gotop T501, USB `08f2:6811`, product string `[T501] Driver Inside Tablet`.

## USB layout

- interface 0: mass-storage CD containing vendor drivers
- interface 1: HID, interrupt IN endpoint `0x83`, 64-byte packets
- interface 2: HID, interrupt IN endpoint `0x85`, 8-byte control/report path

## Proven PC/full-area mode

The Windows-derived feature-report sequence sent to interface 2 is:

```
08 04 1d 01 ff ff 06 2e
08 03 00 ff f0 00 ff f0
08 06 01 00 00 00 00 00
08 03 00 ff f0 00 ff f0
```

After that, interface 1 emits report ID `0x06` with the full tablet area.

Observed PC packet fields used by the driver:

- byte 0: report id `0x06`
- bytes 1..2: X, big-endian
- bytes 3..4: Y, big-endian
- bytes 5..6: raw pressure, big-endian; byte 5 also tracks firmware action/state
- byte 9: stylus side-button state
- bytes 11..12: frame/pad key state

Raw X/Y are `0..4095`. Pressure is inverted on this firmware: hover is around 1740,
full pressure around 890. The Linux driver normalizes this to `0..2047`.

## macOS vendor-driver evidence

`tabletdriver.dmg` contains `MyTabletDaemon`, built 2018-09-03 by Ping-IT Computer
System Inc. The binary uses `IOHIDDeviceSetReport`,
`IOHIDDeviceRegisterInputReportCallback`, `IOHIDPostEvent`, and retains symbols such
as `_handleAbsoluteReport`, `_adjustPressure`, `_InitTabletBounds`,
`_getPressureLevels`, and `_switchToTablet`.

Recovered static 8-byte commands:

```
RotateToLandscape = 08 06 00 00 00 00 00 00
RotateToPortrait  = 08 06 00 01 00 00 00 00
switchToTablet    = 08 05 05 e0 00 00 00 00
```

Its report-ID 5 absolute path uses a different packet layout than PC/full-area mode:
flags at byte 1, little-endian X at 2..3, Y at 4..5, pressure at 6..7. Do not mix
that layout with the verified `0x06` PC layout.

The macOS daemon also contains 4096-axis bounds and explicit pressure-curve and
screen-mapping logic, corroborating native absolute-tablet behavior rather than a
relative mouse emulation.
