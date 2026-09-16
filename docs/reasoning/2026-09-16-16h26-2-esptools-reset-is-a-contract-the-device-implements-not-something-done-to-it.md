---
id: 2026-09-16-16h26-2
date: 2026-09-16
time: "16:26"
title: esptool's reset is a contract the device implements, not something done to it
builds-on: 2026-09-16-16h26
supersedes:
---

**Before:** taking the connector for USB-OTG was assumed to cost the development cycle outright.
`idf.py flash` works today because the USB Serial/JTAG peripheral turns the host's DTR/RTS lines into
a chip reset in hardware; hand the PHY to OTG and that hardware is off the wire, so esptool has no
way to reset anything and flashing means holding BOOT. Automatic flashing and a Comble COM port
looked mutually exclusive on one cable.

**What changed it:** ESP-IDF already ships the missing half, for its own ROM CDC console. In
`components/esp_usb_cdc_rom_console/usb_console.c` the device watches its CDC control lines and
reboots *itself*:

```c
if (!rts && s_prev_rts_state) {                              // RTS falling edge
    s_queue_reboot = dtr ? REBOOT_BOOTLOADER : REBOOT_NORMAL;
}
...
REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
```

Read against esptool 5.4's actual sequence, that is not a private convention. `classic_bootloader_reset`
in `esp_pylib/serial_reset.py` — the one Windows always gets, because `UnixTightReset` raises
`NotImplementedError` without `ioctl` — walks the CDC bits `dtr=0,rts=1` → `dtr=1,rts=1` →
`dtr=1,rts=0`: an RTS falling edge with DTR asserted, exactly the bootloader branch. `hard_reset`
produces the same edge with DTR low, exactly the normal-reboot branch. The two halves were written
to fit.

**Now:** "reset the chip" is a **contract expressed in CDC control lines**, and the device is the
party that implements it. Nothing about it is owned by the USB Serial/JTAG peripheral or by the ROM;
TinyUSB surfaces the same signal as `tud_cdc_line_state_cb(itf, dtr, rts)`, so an application CDC port
can honour it too. The BOOT button was never a consequence of using TinyUSB.

What the trigger does *not* solve is **identity**, and that is where the remaining difficulty actually
lives. After the reboot the ROM enumerates as a different USB device — `303A:1001` for Serial/JTAG,
`303A:0009` for OTG (esptool's `uses_usb_otg()` is literally `pid == IMAGE_CHIP_ID`) — against
whatever PID our own device presented, so the host assigns a different COM port and esptool's
retry-the-same-port loop cannot follow. The ROM's escape hatch does not apply either:
`USBDC_PERSIST_ENA` is documented in `esp_rom/esp32s3/include/esp32s3/rom/usb/usb_persist.h` as valid
only when "the host detected it with the same cdcacm/dfu descriptor as the ROM uses", which a device
with its own product strings — let alone a composite one — is not. So the problem to solve is port
discovery on the host, not signalling on the device.

The trigger also has a cost worth stating once: a bare RTS falling edge is something any Windows
program produces when it closes a port. A *data* port that carries this contract can be rebooted into
download mode by ordinary software that has no idea it is talking to an ESP32.

Still open: which path is taken — hand the PHY back to Serial/JTAG, reboot into the ROM's OTG
download mode, or carry firmware over the Comble port with the command protocol we already have. All
three need the same small host-side step that finds the port that appeared.
