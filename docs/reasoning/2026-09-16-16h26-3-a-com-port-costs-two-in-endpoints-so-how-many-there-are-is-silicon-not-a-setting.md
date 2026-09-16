---
id: 2026-09-16-16h26-3
date: 2026-09-16
time: "16:26"
title: A COM port costs two IN endpoints, so how many there are is silicon and not a setting
builds-on: 2026-08-07-10h58
supersedes:
---

**Before:** how many COM ports the host exposes was a user preference. `UsbPortManager` carried
`usb.ports` as an NVS-backed `UInt32Setting` with `kMaxPorts = 4`, a `usb set` command on the wire,
and a −/+ stepper at the top of the panel's USB screen. The comment in the file said the default was
1 rather than 4 because the radio can only hold one link today — which quietly conceded that the
number was not really the user's to pick, while still offering them the dial.

**What changed it:** counting endpoints. A CDC-ACM function costs three: an interrupt IN for
notifications, a bulk IN and a bulk OUT — so **two IN endpoints per COM port**. The S3's USB-OTG has
four non-zero IN endpoints with dedicated TX FIFOs: `OTG_NUM_EPS 6`, `OTG_NUM_IN_EPS 5` and
`OTG_TX_DINEP_DFIFO_DEPTH_1..4` in `soc/esp32s3/include/soc/usb_dwc_cfg.h`, with EP0 taking one of
the five. Four divided by two is two, and a third function does not fit at any price. `esp_tinyusb`
arrives at the same answer independently — its Kconfig declares `TINYUSB_CDC_COUNT` as `range 1 2` —
so the component and the silicon agree, and the ceiling is the chip's rather than the stack's.

Two more facts closed the question. Descriptors are fixed at enumeration, so even *within* the
ceiling a port count cannot change while the device is plugged in — a setting for it would have to
disconnect and re-present the device to take effect. And three of the setting's four values were
undeliverable on this board no matter what.

**Now:** the port count is a property of the chip and the board's wiring, settled before the device
boots, and offering it as a setting was a category error — a dial over a decision the hardware had
already made. It belongs in the board folder beside the pin map, which is the same boundary
2026-08-07-10h58 drew for the chip itself: a board folder owns what changes when you swap boards, and
this changes when you swap boards.

The shape that matters more than the number: **one port is a capability, not an assumption.** The
model stays N-wide — assignment, the one-slave-one-port move rule, the commands — and the S3 is
simply a host where N is one. A future host on a chip with a bigger endpoint budget raises its own
constant and changes nothing in the BLE, slave or application model.

We ship one of the two the S3 could carry, deliberately. The radio holds one link at a time
(`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1`), so a second port would promise a link that cannot exist; and
the second slot is the only currency available for keeping the development cycle alive once TinyUSB
takes the connector away from USB Serial/JTAG (2026-09-16-16h26, 2026-09-16-16h26-2). Spending it
before that question is settled would be spending it twice.

**Follows:** `BoardConfig::USB_COM_PORTS` on every board — 1 on the WT-SC01 Plus, 0 on the two with
no USB device peripheral we can drive, with the endpoint arithmetic recorded next to the constant.
`UsbPortManager::kMaxPorts` reads it; `usb.ports`, `SetPortCount`/`PortCount` and the `usb set`
command are deleted, and a board with no ports registers no settings and no commands at all. The
panel loses the stepper and becomes what it always was under it: the host pairs and remembers many
slaves, and one of them is on the port.
