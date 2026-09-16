---
id: 2026-09-16-16h26
date: 2026-09-16
time: "16:26"
title: The S3's USB PHY is a runtime mux, and only the ROM is bound by the eFuse
builds-on:
supersedes:
---

**Before:** the WT-SC01 Plus has one USB-C, and that connector belongs to USB Serial/JTAG. ESP-IDF's
own guides say so in as many words — `api-guides/usb-otg-console.rst` and `api-guides/dfu.rst` both
state that on the ESP32-S3 "the USB_SERIAL_JTAG module is connected to the internal PHY, while the
USB OTG peripheral can be used only if an external USB PHY is connected", and offer exactly one way
out: permanently burn the `USB_PHY_SEL` eFuse. So exposing a CDC COM port over this cable looked
like it required an irreversible fuse on every unit, and the question was whether that price was
payable.

**What changed it:** `components/esp_hal_usb/esp32s3/include/hal/usb_wrap_ll.h`. The routing is two
bits in the RTC domain, and they are ordinary writable registers:

```c
usb_wrap_ll_phy_enable_external(hw, enable) {
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;    // take the choice away from the eFuse
    RTCCNTL.usb_conf.sw_usb_phy_sel = !enable; // 1 = internal PHY -> OTG, 0 -> USJ
}
```

`esp_hw_support/usb_phy/usb_phy.c` calls it for `USB_PHY_CTRL_OTG`, so `tinyusb_driver_install()`
already performs this on a stock chip — which is why every S3 TinyUSB example runs on boards whose
only USB is the native one. The documentation is not wrong; it is describing the **ROM**, which runs
before any application code and therefore can only ever obey the eFuse. The distinction is between
what the chip can do and what the chip can do *before our firmware exists*, and the two guides only
had occasion to talk about the second.

**Now:** the internal PHY is a runtime mux between exactly one of USB-OTG and USB Serial/JTAG. The
two cannot coexist on one connector — one PHY, one owner — but the owner is a register write away,
with no reset, no eFuse, and no external hardware. Two consequences follow from where those bits
live:

- The choice **survives a software reset.** `esp_restart()` on the S3 resets WiFi/BT, timers, SPI,
  UART, DMA and crypto (`esp_system/port/soc/esp32s3/system_internal.c`) and touches nothing USB,
  and RTC_CNTL is cleared only by power-on reset. So the application decides which USB the ROM
  comes up on after a reboot it asked for, and a power cycle is the recovery if it decides wrongly.
- Burning `USB_PHY_SEL` on this board would be actively harmful, not merely unnecessary. The eFuse
  reads "1: internal PHY is assigned to USB OTG while **external** PHY is assigned to USB Device"
  (`soc/esp32s3/register/soc/efuse_struct.h`), and there is no external PHY here — burning it would
  move USB Serial/JTAG onto pins that do not exist and destroy the board's flashing path,
  irreversibly.

Rests on the eFuse being unburned, which is the factory default and the state of our units.
