// SPDX-License-Identifier: GPL-2.0
/*
 * hid-cytech-t501.c - native Linux HID driver for SZ PING-IT / Gotop T501
 * USB ID 08f2:6811 ("[T501] Driver Inside Tablet")
 *
 * Protocol evidence comes from the vendor Windows driver and the 2018 macOS
 * MyTabletDaemon (com.pingit.TabletDriver).  The macOS daemon uses IOKit
 * IOHIDDeviceSetReport / IOHIDDeviceRegisterInputReportCallback and exposes
 * explicit absolute-tablet, pressure-curve and screen-mapping paths.
 *
 * Hardware layout:
 *   if0: USB mass storage (the built-in "Pen Driver" CD)
 *   if1: 64-byte interrupt IN, full-area pen/pad reports
 *   if2: HID control channel used for 8-byte feature reports
 *
 * The Windows/full-area mode is kept as the default because it is verified on
 * the user's exact T501.  macOS evidence is used to validate the native tablet
 * model and packet semantics; known macOS commands are documented below.
 */

#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb.h>

#define T501_VENDOR_ID          0x08f2
#define T501_PRODUCT_ID         0x6811
#define T501_IFACE_DATA         1
#define T501_IFACE_CONTROL      2
#define T501_PC_REPORT_ID       0x06
#define T501_PC_MIN_REPORT_SIZE 13
#define T501_AXIS_MAX           4095
#define T501_PRESSURE_MAX       2047
#define T501_PAD_IDLE           0xff33

/*
 * MyTabletDaemon constants recovered from __DATA:
 *   RotateToLandscape = 08 06 00 00 00 00 00 00
 *   RotateToPortrait  = 08 06 00 01 00 00 00 00
 *   switchToTablet    = 08 05 05 e0 00 00 00 00
 * They are not sent by default; the proven Windows full-area sequence below
 * is safer on this exact firmware.
 */
static const u8 t501_mac_rotate_landscape[8] __maybe_unused =
	{ 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 t501_mac_rotate_portrait[8] __maybe_unused =
	{ 0x08, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
static const u8 t501_mac_switch_tablet[8] __maybe_unused =
	{ 0x08, 0x05, 0x05, 0xe0, 0x00, 0x00, 0x00, 0x00 };

static int pressure_hover = 1740;
module_param(pressure_hover, int, 0644);
MODULE_PARM_DESC(pressure_hover, "Raw pressure at hover/no-contact (default 1740)");

static int pressure_full = 890;
module_param(pressure_full, int, 0644);
MODULE_PARM_DESC(pressure_full, "Raw pressure at full contact (default 890)");

static int pressure_press = 300;
module_param(pressure_press, int, 0644);
MODULE_PARM_DESC(pressure_press, "Normalized pressure threshold for BTN_TOUCH (default 300)");

static int pressure_release = 200;
module_param(pressure_release, int, 0644);
MODULE_PARM_DESC(pressure_release, "Normalized release threshold for BTN_TOUCH (default 200)");

static bool debug_packets;
module_param(debug_packets, bool, 0644);
MODULE_PARM_DESC(debug_packets, "Log sampled raw T501 reports");

struct t501_state {
	struct hid_device *hdev;
	struct input_dev *pen;
	struct input_dev *pad;
	u8 ifnum;
	bool touching;
	u16 last_pad_key;
	u64 packets;
};

static int t501_iface_number(struct hid_device *hdev)
{
	struct usb_interface *intf;

	if (hdev->bus != BUS_USB || !hdev->dev.parent)
		return -ENODEV;
	intf = to_usb_interface(hdev->dev.parent);
	return intf->cur_altsetting->desc.bInterfaceNumber;
}

static int t501_normalize_pressure(u16 raw)
{
	int hover = READ_ONCE(pressure_hover);
	int full = READ_ONCE(pressure_full);
	int value;

	if (hover <= full)
		return 0;
	if (raw >= hover)
		return 0;
	if (raw <= full)
		return T501_PRESSURE_MAX;

	value = (hover - raw) * T501_PRESSURE_MAX / (hover - full);
	return clamp(value, 0, T501_PRESSURE_MAX);
}

static void t501_emit_pad_key(struct input_dev *pad, u16 raw, int value)
{
	switch (raw) {
	case 65329: input_report_key(pad, KEY_E, value); break;
	case 65315: input_report_key(pad, KEY_B, value); break;
	case 32563:
		input_report_key(pad, KEY_LEFTCTRL, value);
		input_report_key(pad, KEY_KPMINUS, value);
		break;
	case 65330:
		input_report_key(pad, KEY_LEFTCTRL, value);
		input_report_key(pad, KEY_KPPLUS, value);
		break;
	case 48947: input_report_key(pad, KEY_LEFTBRACE, value); break;
	case 65299: input_report_key(pad, KEY_RIGHTBRACE, value); break;
	case 57139: input_report_key(pad, KEY_SCROLLUP, value); break;
	case 65075: input_report_key(pad, KEY_TAB, value); break;
	case 61235: input_report_key(pad, KEY_SCROLLDOWN, value); break;
	case 64819: input_report_key(pad, KEY_SPACE, value); break;
	case 63283: input_report_key(pad, KEY_LEFTCTRL, value); break;
	case 64307: input_report_key(pad, KEY_LEFTALT, value); break;
	default: break;
	}
}

static bool t501_pad_key_known(u16 raw)
{
	switch (raw) {
	case 65329: case 65315: case 32563: case 65330:
	case 48947: case 65299: case 57139: case 65075:
	case 61235: case 64819: case 63283: case 64307:
		return true;
	default:
		return false;
	}
}

static void t501_report_pad(struct t501_state *st, u16 raw)
{
	if (!st->pad || raw == st->last_pad_key)
		return;
	if (t501_pad_key_known(st->last_pad_key))
		t501_emit_pad_key(st->pad, st->last_pad_key, 0);
	if (t501_pad_key_known(raw))
		t501_emit_pad_key(st->pad, raw, 1);
	st->last_pad_key = raw;
	input_sync(st->pad);
}

/*
 * Verified PC/full-area report (report ID 0x06):
 *   [0]    report id
 *   [1:2]  X, big-endian
 *   [3:4]  Y, big-endian
 *   [5:6]  raw pressure, big-endian; high byte also carries firmware state
 *   [9]    stylus side-button state
 *   [11:12] frame/pad key state
 *
 * The macOS report-ID 5 path is a different firmware mode.  Its callback sees
 * flags at byte 1, little-endian X at 2:3, Y at 4:5 and pressure at 6:7.
 * We deliberately do not mix those layouts.
 */
static void t501_report_pc_pen(struct t501_state *st, const u8 *data)
{
	struct input_dev *pen = st->pen;
	u16 x = ((u16)data[1] << 8) | data[2];
	u16 y = ((u16)data[3] << 8) | data[4];
	u16 raw_pressure = ((u16)data[5] << 8) | data[6];
	u8 action = data[5];
	u8 pen_button = data[9];
	int pressure = t501_normalize_pressure(raw_pressure);
	bool proximity = action >= 3 && action <= 6;
	bool touching;

	if (!pen)
		return;

	if (!proximity || x > T501_AXIS_MAX || y > T501_AXIS_MAX) {
		st->touching = false;
		input_report_key(pen, BTN_STYLUS, 0);
		input_report_key(pen, BTN_STYLUS2, 0);
		input_report_key(pen, BTN_TOUCH, 0);
		input_report_key(pen, BTN_TOOL_PEN, 0);
		input_report_abs(pen, ABS_PRESSURE, 0);
		input_sync(pen);
		return;
	}

	if (st->touching)
		touching = pressure > READ_ONCE(pressure_release);
	else
		touching = pressure >= READ_ONCE(pressure_press);
	st->touching = touching;

	input_report_key(pen, BTN_TOOL_PEN, 1);
	/* Native absolute coordinates: do not turn hover into relative motion. */
	input_report_abs(pen, ABS_X, x);
	input_report_abs(pen, ABS_Y, y);
	input_report_abs(pen, ABS_PRESSURE, touching ? pressure : 0);
	input_report_key(pen, BTN_TOUCH, touching);
	input_report_key(pen, BTN_STYLUS, pen_button == 4 || pen_button == 10);
	input_report_key(pen, BTN_STYLUS2, pen_button == 6);
	input_sync(pen);
}

static int t501_raw_event(struct hid_device *hdev, struct hid_report *report,
			  u8 *data, int size)
{
	struct t501_state *st = hid_get_drvdata(hdev);
	u16 pad_key;

	if (!st || st->ifnum != T501_IFACE_DATA)
		return 0;
	if (size < T501_PC_MIN_REPORT_SIZE || data[0] != T501_PC_REPORT_ID)
		return 0;

	st->packets++;
	if (unlikely(debug_packets && (st->packets <= 8 || !(st->packets % 500))))
		hid_info(hdev,
			 "pkt=%llu len=%d %*phN\n",
			 st->packets, size, min(size, 16), data);

	pad_key = ((u16)data[11] << 8) | data[12];
	t501_report_pad(st, pad_key);
	t501_report_pc_pen(st, data);

	/* Positive means handled: suppress generic parsing without reporting an error. */
	return 1;
}

static int t501_create_pen(struct t501_state *st)
{
	struct input_dev *pen;
	int ret;

	pen = devm_input_allocate_device(&st->hdev->dev);
	if (!pen)
		return -ENOMEM;

	pen->name = "Cytech T501 Pen";
	pen->phys = st->hdev->phys;
	pen->id.bustype = BUS_USB;
	pen->id.vendor = T501_VENDOR_ID;
	pen->id.product = T501_PRODUCT_ID;
	pen->id.version = st->hdev->version;
	pen->dev.parent = &st->hdev->dev;

	__set_bit(INPUT_PROP_POINTER, pen->propbit);
	input_set_capability(pen, EV_KEY, BTN_TOOL_PEN);
	input_set_capability(pen, EV_KEY, BTN_TOUCH);
	input_set_capability(pen, EV_KEY, BTN_STYLUS);
	input_set_capability(pen, EV_KEY, BTN_STYLUS2);
	input_set_abs_params(pen, ABS_X, 0, T501_AXIS_MAX, 0, 0);
	input_set_abs_params(pen, ABS_Y, 0, T501_AXIS_MAX, 0, 0);
	input_set_abs_params(pen, ABS_PRESSURE, 0, T501_PRESSURE_MAX, 0, 0);
	/* Approx. 10 x 6.25 inch active area from the vendor-family hardware. */
	input_abs_set_res(pen, ABS_X, 16);
	input_abs_set_res(pen, ABS_Y, 26);

	ret = input_register_device(pen);
	if (ret)
		return ret;
	st->pen = pen;
	return 0;
}

static int t501_create_pad(struct t501_state *st)
{
	struct input_dev *pad;
	int ret;

	pad = devm_input_allocate_device(&st->hdev->dev);
	if (!pad)
		return -ENOMEM;
	pad->name = "Cytech T501 Pad Buttons";
	pad->phys = st->hdev->phys;
	pad->id.bustype = BUS_USB;
	pad->id.vendor = T501_VENDOR_ID;
	pad->id.product = T501_PRODUCT_ID;
	pad->id.version = st->hdev->version;
	pad->dev.parent = &st->hdev->dev;

	input_set_capability(pad, EV_KEY, KEY_E);
	input_set_capability(pad, EV_KEY, KEY_B);
	input_set_capability(pad, EV_KEY, KEY_LEFTCTRL);
	input_set_capability(pad, EV_KEY, KEY_KPMINUS);
	input_set_capability(pad, EV_KEY, KEY_KPPLUS);
	input_set_capability(pad, EV_KEY, KEY_LEFTBRACE);
	input_set_capability(pad, EV_KEY, KEY_RIGHTBRACE);
	input_set_capability(pad, EV_KEY, KEY_SCROLLUP);
	input_set_capability(pad, EV_KEY, KEY_SCROLLDOWN);
	input_set_capability(pad, EV_KEY, KEY_TAB);
	input_set_capability(pad, EV_KEY, KEY_SPACE);
	input_set_capability(pad, EV_KEY, KEY_LEFTALT);

	ret = input_register_device(pad);
	if (ret)
		return ret;
	st->pad = pad;
	st->last_pad_key = T501_PAD_IDLE;
	return 0;
}

static int t501_enable_full_mode(struct hid_device *hdev)
{
	static const u8 reports[][8] = {
		{ 0x08, 0x04, 0x1d, 0x01, 0xff, 0xff, 0x06, 0x2e },
		{ 0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0 },
		{ 0x08, 0x06, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 },
		{ 0x08, 0x03, 0x00, 0xff, 0xf0, 0x00, 0xff, 0xf0 },
	};
	u8 buf[8];
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(reports); i++) {
		memcpy(buf, reports[i], sizeof(buf));
		ret = hid_hw_raw_request(hdev, 0x08, buf, sizeof(buf),
					 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
		if (ret < 0) {
			hid_err(hdev, "full-area init report %d failed: %d\n", i, ret);
			return ret;
		}
		msleep(20);
	}
	hid_info(hdev, "T501 PC/full-area mode enabled\n");
	return 0;
}

static int t501_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct t501_state *st;
	int ifnum, ret;

	ifnum = t501_iface_number(hdev);
	if (ifnum != T501_IFACE_DATA && ifnum != T501_IFACE_CONTROL)
		return -ENODEV;
	/* 08f2:6811 is reused by related devices; bind only the observed T501. */
	if (!strstr(hdev->name, "[T501]"))
		return -ENODEV;

	st = devm_kzalloc(&hdev->dev, sizeof(*st), GFP_KERNEL);
	if (!st)
		return -ENOMEM;
	st->hdev = hdev;
	st->ifnum = ifnum;
	hid_set_drvdata(hdev, st);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	if (ifnum == T501_IFACE_DATA) {
		/* Firmware emits 0x06 in PC mode although descriptor omits it. */
		if (!hid_register_report(hdev, HID_INPUT_REPORT, T501_PC_REPORT_ID, 0))
			return -ENOMEM;
		ret = t501_create_pen(st);
		if (ret)
			return ret;
		ret = t501_create_pad(st);
		if (ret)
			return ret;
	}

	/* hidraw remains available for protocol diagnostics. */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	if (ifnum == T501_IFACE_CONTROL) {
		ret = t501_enable_full_mode(hdev);
		if (ret) {
			hid_hw_stop(hdev);
			return ret;
		}
	}

	hid_info(hdev, "native T501 driver bound on interface %d\n", ifnum);
	return 0;
}

static void t501_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

static const struct hid_device_id t501_devices[] = {
	{ HID_USB_DEVICE(T501_VENDOR_ID, T501_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, t501_devices);

static struct hid_driver t501_driver = {
	.name = "cytech-t501",
	.id_table = t501_devices,
	.probe = t501_probe,
	.remove = t501_remove,
	.raw_event = t501_raw_event,
};
module_hid_driver(t501_driver);

MODULE_AUTHOR("Cytech Team Development");
MODULE_DESCRIPTION("Native HID driver for SZ PING-IT/Gotop T501 08f2:6811, reverse-engineered from vendor Windows/macOS drivers");
MODULE_LICENSE("GPL");
