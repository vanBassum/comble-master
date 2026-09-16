#pragma once

// ──────────────────────────────────────────────────────────────
// BoardContext configuration — Wireless-Tag WT-SC01 Plus.
//
//   ESP32-S3-WROOM-1-N16R2 — 16 MB flash, 2 MB embedded QUAD PSRAM. Both
//   numbers are measured on our unit, not read off a datasheet: esptool
//   reports "Embedded PSRAM 2MB (AP_3v3)" and the bootloader detects 16384k
//   of flash. The Thermostat's copy of this file said 8 MB octal PSRAM (an
//   N16R8); that is a DIFFERENT unit. See sdkconfig.defaults next door.
//   3.5" 320x480 IPS, ST7796(UI) controller over an 8-bit i80/8080 bus
//   FT6336U capacitive touch over I2C (FocalTech FT5x06 family) — pins listed
//   below but not yet driven; this board brings up the panel only.
//   No rotary knob, no user LED.
//
// The pin map is the canonical WT-SC01 Plus assignment (the one the widely
// shared LovyanGFX config uses). It came over from the KC1245 Thermostat,
// where this exact map was verified on hardware — boots, displays, touch
// works. It is not guesswork, so treat a blank screen as a config or
// orientation problem before suspecting a pin.
// ──────────────────────────────────────────────────────────────

namespace BoardConfig
{
    // ── USB: how many Comble COM ports this board can expose ───────
    // One. That is a property of the silicon and this board's wiring, not a
    // number anybody gets to configure — see UsbPortManager for the model that
    // reads it.
    //
    // The ceiling is TWO and the arithmetic is in the endpoints, not in the
    // stack: a CDC-ACM function costs two IN endpoints (a notification
    // interrupt IN and a bulk IN), and the S3's USB-OTG has four non-zero IN
    // endpoints with dedicated TX FIFOs (OTG_NUM_EPS 6, OTG_NUM_IN_EPS 5, four
    // OTG_TX_DINEP_DFIFO_DEPTH_n in soc/esp32s3/include/soc/usb_dwc_cfg.h).
    // Four divided by two is two, which is also why esp_tinyusb caps
    // TINYUSB_CDC_COUNT at "range 1 2" — component and chip agree.
    //
    // We ship ONE of those two, deliberately. The radio holds one link at a
    // time (CONFIG_BT_NIMBLE_MAX_CONNECTIONS in the root sdkconfig.defaults),
    // and the second CDC would otherwise have to be spent on keeping the
    // development cycle alive once TinyUSB takes the connector away from
    // USB-Serial/JTAG. A host that wants several ports is a different chip, and
    // it says so here rather than anywhere else in the tree.
    static constexpr int USB_COM_PORTS = 1;

    // LED — no user LED is fitted. The only thing resembling one is the LCD
    // backlight on GPIO45, which belongs to the display, not to the Led role.
    // BoardContext binds MockLed so the role is still satisfied.
    static constexpr int LED_PIN = -1;
    static constexpr bool LED_ACTIVE_HIGH = true;

    // ── ST7796 LCD panel (8-bit i80/8080 parallel) ─────────────────
    // The panel scans natively in PORTRAIT 320x480. The constants below are
    // the LOGICAL orientation after MADCTL, which is what drawing code sees.
    static constexpr int LCD_NATIVE_H_RES = 320;
    static constexpr int LCD_NATIVE_V_RES = 480;

    // PORTRAIT orientation. CONSUMED BY esp_lvgl_port, not by the panel driver:
    // the port re-applies swap_xy/mirror on the panel itself and would clobber
    // anything set behind its back, so rotation is owned in exactly one place.
    // (The Thermostat learned that the expensive way — a manual swap_xy in the
    // board's Display.h produced a sideways picture that no amount of fiddling
    // with the driver would fix.)
    //
    // It was landscape (swap_xy alone, 480x320) until the UI was drawn to match
    // the concept art, which is portrait: a column of adapter cards over a
    // full-width primary action. That layout is not a wide screen turned on its
    // side, it is a different one, so the panel turns instead of the design.
    //
    // MIRROR_X is not decoration, and turning all three off does NOT give
    // portrait — it gives portrait held up to a mirror, which is exactly what
    // the first attempt put on the glass. The reason is that swap_xy on its own
    // is a TRANSPOSE, and a transpose is a reflection across the diagonal, not a
    // rotation. So the working landscape picture was raw-panel-reflected, and
    // dropping the transpose leaves the reflection rather than undoing it. A
    // real quarter turn is transpose PLUS one mirror, so going from that
    // landscape to portrait means swapping which of the two is on:
    //
    //     landscape:  swap_xy,           no mirror   = T
    //     portrait:   no swap,  mirror_x            = T applied to the above
    //
    // WHICH mirror is the 180° question, and it was got wrong once: MIRROR_X and
    // MIRROR_Y both give an unmirrored portrait, differing by a half turn, and
    // the first attempt picked X and came up upside down. Never set both — that
    // lands back at a reflection.
    //
    // What settled it was the TOUCH dial below disagreeing by exactly 180°. That
    // is the diagnostic worth keeping: a tap that is wrong on ONE axis is a
    // reflection, and means the touch transform is wrong; a tap that is wrong on
    // BOTH is a half turn, and — since touch is derived to match the display —
    // means the touch transform is right and the DISPLAY took the wrong arm.
    // Reading the error that way names the culprit without a second guess.
    static constexpr bool LCD_SWAP_XY  = false;
    static constexpr bool LCD_MIRROR_X = false;
    static constexpr bool LCD_MIRROR_Y = true;

    // Logical resolution. No swap, so this is the native scan order.
    static constexpr int LCD_H_RES = 320;
    static constexpr int LCD_V_RES = 480;

    // Bits per pixel on the wire (RGB565). The 8-bit bus sends 2 bytes/pixel.
    static constexpr int LCD_BITS_PER_PIXEL = 16;

    // RGB565 reaches the panel high byte first over an 8-bit bus, while the
    // ESP32 stores it little-endian — so pixel data is byte-swapped on the way
    // out. LVGL does the swap for us (lvgl_port disp_cfg.flags.swap_bytes), so
    // this is read there. If red and blue look right but every colour is wrong,
    // this is the flag to flip; if red and blue are specifically swapped, it is
    // the BGR element order in Display.h instead.
    static constexpr bool LCD_SWAP_BYTES = true;

    // i80 pixel-clock (WR strobe). ST7796 over an 8-bit bus is comfortable at
    // 20 MHz. The Thermostat later found 16 MHz tore the picture on a
    // different (RGB) panel — unrelated to this bus, noted only so the number
    // is not mistaken for a tuned one. Raise during bring-up if the panel keeps up.
    static constexpr int LCD_PIXEL_CLOCK_HZ = 20000000;

    // Control signals
    static constexpr int LCD_PIN_DC   = 0;    // data/command (RS)
    static constexpr int LCD_PIN_WR   = 47;   // write strobe (i80 PCLK)
    static constexpr int LCD_PIN_CS   = -1;   // tied active on this board
    static constexpr int LCD_PIN_RST  = 4;    // panel hardware reset

    // Backlight (active high). Drive high to turn the panel on.
    static constexpr int LCD_PIN_BACKLIGHT = 45;

    // 8 data lines D0..D7.
    static constexpr int LCD_DATA_PINS[8] = {
        9, 46, 3, 8, 18, 17, 16, 15,
    };

    // Scanlines per LVGL draw buffer, and the i80 bus max transfer with it.
    // TWO of these are allocated, in internal DMA RAM: 30 * 320 * 2 = 19200 B
    // each, 38400 B the pair. It was 20 lines while the panel ran landscape;
    // portrait made a line 320 px instead of 480, so 30 lines is the SAME number
    // of bytes — the budget below is unchanged, only the shape of it is.
    //
    // Before that it was 40, which is the size the Thermostat used and the size
    // LVGL would prefer (about 1/8 of the screen). It does not fit HERE, and the
    // reason is ordering, not arithmetic: main.cpp brings the framework up
    // before the application, so the Wi-Fi AP has already taken its cut of
    // internal DMA RAM by the time UiManager runs, and the second 38400-byte
    // buffer had nowhere to go — lvgl_port_add_disp failed outright and the
    // panel stayed dark. Halving the buffers keeps double-buffering, which is
    // what lets LVGL render the next band while the i80 DMA sends the last one.
    // The cost is more, smaller flushes; PSRAM is NOT the answer here, see the
    // note in this board's sdkconfig.defaults.
    static constexpr int LCD_DRAW_BUFFER_LINES = 30;

    // ── FT6336U capacitive touch (I2C, FocalTech FT5x06 family) ─────
    static constexpr int TOUCH_PIN_SDA = 6;
    static constexpr int TOUCH_PIN_SCL = 5;
    static constexpr int TOUCH_PIN_RST = -1;  // not wired on this board
    static constexpr int TOUCH_PIN_INT = 7;

    // Touch rotation. Applied by esp_lcd_touch itself, NOT by the LVGL port —
    // so these are a separate dial from LCD_SWAP_XY above and are deliberately
    // NOT the same values.
    //
    // Both mirrors on, no swap. The raw FT6336U frame relates to the panel's own
    // frame as tx = 319 - px, ty = py — pinned by observation, not assumed: with
    // these very values against a mirror_x display, every tap landed a half turn
    // out, and only one raw relation does that.
    //
    // Against the mirror_y display above, which puts panel pixel (px, py) on
    // screen at (px, 479 - py), substituting that relation means the touch layer
    // must report (319 - tx, 479 - ty) — both mirrors, which is what is here.
    //
    // Note these do NOT follow from the landscape pair (display: swap, no mirror
    // — touch: swap, mirror_x) by the obvious algebra; that route gives a
    // different raw relation and a wrong answer, most likely over the order
    // esp_lcd_touch composes swap and mirror in and with which extents. The
    // measured relation above wins. If a tap ever lands wrong again, tap a known
    // corner and read off WHICH axes are inverted — one axis blames this block,
    // both axes blame the display arm above.
    static constexpr bool TOUCH_SWAP_XY  = false;
    static constexpr bool TOUCH_MIRROR_X = true;
    static constexpr bool TOUCH_MIRROR_Y = true;
}
