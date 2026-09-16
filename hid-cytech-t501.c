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

static bool legacy_pad_shortcuts = true;
module_param(legacy_pad_shortcuts, bool, 0644);
MODULE_PARM_DESC(legacy_pad_shortcuts, "Also emit vendor-style keyboard shortcuts for frame buttons (default yes)");

struct t501_state {
	struct hid_device *hdev;
	struct input_dev *pen;
	struct input_dev *pad;
	u8 ifnum;
	bool touching;
	u16 last_pad_state;
	u64 packets;
	struct urb *data_urb;
	u8 *data_buf;
	int data_buf_len;
	bool transport_running;
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

struct t501_pad_button {
	u16 active_low_mask;
	u16 code;
	const char *name;
};

/*
 * The two pad bytes are an active-low bitfield, not a single button code.
 * Idle is 0xff33. Each physical frame button clears one bit. Multiple
 * buttons can therefore be pressed at once (for example 0xfa33 = Pad8+Pad12).
 *
 * Older revisions matched the entire 16-bit value, which made presses appear
 * to stick or disappear as soon as another bit changed. Decode each bit
 * independently and emit proper press/release transitions instead.
 */
static const struct t501_pad_button t501_pad_buttons[] = {
	{ 0x0002, BTN_0, "Pad1" },
	{ 0x0010, BTN_1, "Pad2" },
	{ 0x8000, BTN_2, "Pad3" },
	{ 0x0001, BTN_3, "Pad4" },
	{ 0x4000, BTN_4, "Pad5" },
	{ 0x0020, BTN_5, "Pad6" },
	{ 0x2000, BTN_6, "Pad7" },
	{ 0x0100, BTN_7, "Pad8" },
	{ 0x1000, BTN_8, "Pad9" },
	{ 0x0200, BTN_9, "Pad10" },
	{ 0x0800, BTN_TRIGGER_HAPPY1, "Pad11" },
	{ 0x0400, BTN_TRIGGER_HAPPY2, "Pad12" },
};

static void t501_report_pad(struct t501_state *st, u16 raw)
{
	u16 old_state, new_state, changed;
	int i;

	if (!st->pad)
		return;

	old_state = st->last_pad_state;
	new_state = raw;
	changed = old_state ^ new_state;
	if (!changed)
		return;

	for (i = 0; i < ARRAY_SIZE(t501_pad_buttons); i++) {
		const struct t501_pad_button *button = &t501_pad_buttons[i];
		bool was_down, is_down;

		if (!(changed & button->active_low_mask))
			continue;

		was_down = !(old_state & button->active_low_mask);
		is_down = !(new_state & button->active_low_mask);
		if (was_down == is_down)
			continue;

		input_report_key(st->pad, button->code, is_down);
		if (unlikely(debug_packets))
			hid_info(st->hdev, "%s %s (raw=0x%04x)\n",
				 button->name, is_down ? "pressed" : "released", raw);
	}

	st->last_pad_state = new_state;
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

static void t501_process_packet(struct t501_state *st, const u8 *data, int size)
{
	u16 pad_key;

	if (!st || size < T501_PC_MIN_REPORT_SIZE || data[0] != T501_PC_REPORT_ID)
		return;

	st->packets++;
	if (unlikely(debug_packets && (st->packets <= 8 || !(st->packets % 500))))
		hid_info(st->hdev, "pkt=%llu len=%d %*phN\n",
			 st->packets, size, min(size, 16), data);

	pad_key = ((u16)data[11] << 8) | data[12];
	t501_report_pad(st, pad_key);
	t501_report_pc_pen(st, data);
}

static void t501_data_irq(struct urb *urb)
{
	struct t501_state *st = urb->context;
	int ret;

	if (!st)
		return;

	switch (urb->status) {
	case 0:
		t501_process_packet(st, st->data_buf, urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;
	case -EPIPE:
		/* Endpoint halt is unusual on this tablet; recover on next submit. */
		break;
	default:
		if (debug_packets)
			hid_warn(st->hdev, "interrupt status %d\n", urb->status);
		break;
	}

	if (!READ_ONCE(st->transport_running))
		return;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret && ret != -ENODEV && ret != -EPERM)
		hid_err(st->hdev, "failed to resubmit data URB: %d\n", ret);
}

static int t501_start_data_transport(struct t501_state *st)
{
	struct usb_interface *intf = to_usb_interface(st->hdev->dev.parent);
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_host_interface *alts = intf->cur_altsetting;
	struct usb_endpoint_descriptor *ep = NULL;
	int i, pipe, interval, len, ret;

	for (i = 0; i < alts->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *candidate = &alts->endpoint[i].desc;
		if (usb_endpoint_is_int_in(candidate) &&
		    usb_endpoint_maxp(candidate) >= T501_PC_MIN_REPORT_SIZE) {
			ep = candidate;
			if (usb_endpoint_maxp(candidate) == 64)
				break;
		}
	}
	if (!ep)
		return -ENODEV;

	len = usb_endpoint_maxp(ep);
	st->data_buf = kmalloc(len, GFP_KERNEL);
	if (!st->data_buf)
		return -ENOMEM;
	st->data_buf_len = len;

	st->data_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!st->data_urb) {
		kfree(st->data_buf);
		st->data_buf = NULL;
		return -ENOMEM;
	}

	pipe = usb_rcvintpipe(udev, ep->bEndpointAddress);
	interval = ep->bInterval;
	usb_fill_int_urb(st->data_urb, udev, pipe, st->data_buf, len,
			 t501_data_irq, st, interval);
	WRITE_ONCE(st->transport_running, true);
	ret = usb_submit_urb(st->data_urb, GFP_KERNEL);
	if (ret) {
		WRITE_ONCE(st->transport_running, false);
		usb_free_urb(st->data_urb);
		st->data_urb = NULL;
		kfree(st->data_buf);
		st->data_buf = NULL;
		return ret;
	}

	hid_info(st->hdev, "direct interrupt transport started on endpoint 0x%02x (%d bytes)\n",
		 ep->bEndpointAddress, len);
	return 0;
}

static void t501_stop_data_transport(struct t501_state *st)
{
	if (!st)
		return;
	WRITE_ONCE(st->transport_running, false);
	if (st->data_urb) {
		usb_kill_urb(st->data_urb);
		usb_free_urb(st->data_urb);
		st->data_urb = NULL;
	}
	kfree(st->data_buf);
	st->data_buf = NULL;
}

static int t501_raw_event(struct hid_device *hdev, struct hid_report *report,
			  u8 *data, int size)
{
	/*
	 * The T501 emits report 0x06 even though its HID descriptor does not
	 * describe that report.  Some usbhid paths discard it before raw_event().
	 * We therefore read interface 1's interrupt endpoint directly above.
	 * Keep raw_event passive so ordinary descriptor reports are unaffected.
	 */
	return 0;
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

	/* Expose hardware frame keys as remappable tablet-pad buttons. */
	for (ret = 0; ret < ARRAY_SIZE(t501_pad_buttons); ret++)
		input_set_capability(pad, EV_KEY, t501_pad_buttons[ret].code);

	/* Optional vendor-style defaults so frame keys do something immediately.
	 * Native BTN_* events are still emitted in parallel and remain remappable. */
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
	st->last_pad_state = T501_PAD_IDLE;
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
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	struct usb_device *udev = interface_to_usbdev(intf);
	u8 *buf;
	int i, ret = 0;

	buf = kmalloc(8, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(reports); i++) {
		memcpy(buf, reports[i], 8);
		/* Mirror the vendor/userspace path exactly: bmRequestType=0x21,
		 * SET_REPORT, wValue=0x0308, wIndex=2. */
		ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				      HID_REQ_SET_REPORT,
				      USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
				      0x0308, T501_IFACE_CONTROL,
				      buf, 8, 250);
		if (ret == -ETIMEDOUT) {
			hid_warn(hdev, "full-area report %d timed out; continuing\n", i + 1);
			ret = 0;
		} else if (ret < 0) {
			hid_err(hdev, "full-area init report %d failed: %d\n", i + 1, ret);
			break;
		} else {
			/* usb_control_msg returns the number of bytes transferred. */
			ret = 0;
		}
		msleep(20);
	}
	kfree(buf);
	if (!ret)
		hid_info(hdev, "T501 PC/full-area mode enabled via direct USB control\n");
	return ret;
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

	if (ifnum == T501_IFACE_DATA) {
		ret = t501_start_data_transport(st);
		if (ret) {
			hid_err(hdev, "failed to start direct data transport: %d\n", ret);
			hid_hw_stop(hdev);
			return ret;
		}
	}

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
	struct t501_state *st = hid_get_drvdata(hdev);

	if (st && st->ifnum == T501_IFACE_DATA)
		t501_stop_data_transport(st);
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
