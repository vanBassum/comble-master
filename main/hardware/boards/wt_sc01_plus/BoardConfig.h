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
    // MIRROR_Y instead of MIRROR_X is the same picture turned 180°, so if this
    // comes up upside down rather than mirrored, move the true from X to Y — do
    // not add it to both, which lands back at a reflection.
    //
    // The TOUCH dial below is separate and has to move WITH this one — see the
    // derivation there.
    static constexpr bool LCD_SWAP_XY  = false;
    static constexpr bool LCD_MIRROR_X = true;
    static constexpr bool LCD_MIRROR_Y = false;

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
    // DERIVED from the landscape pair, not measured, so this is the first thing
    // to suspect if a tap lands wrong. The landscape values were verified on the
    // Thermostat's hardware: display (swap, no mirror) against touch (swap,
    // mirror_x). esp_lcd_touch swaps before it mirrors, so with W = 480 the
    // landscape width, those two agreeing means
    //
    //     display:  (px, py) -> (py, px)
    //     touch:    (tx, ty) -> (ty, tx) -> (W-1-ty, tx)
    //
    // and equating them gives tx = px, ty = 479 - py. In other words the raw
    // touch frame is the panel's own portrait frame with Y running backwards.
    //
    // The portrait display above is mirror_x, so it puts panel pixel (px, py) at
    // screen (319 - px, py). Substituting the two relations, the touch layer has
    // to report (319 - tx, 479 - ty) — which is no swap and BOTH mirrors on.
    // Both, because the two trues are doing different jobs: mirror_x matches the
    // display's own mirror, and mirror_y undoes the backwards raw Y that was
    // there all along and that the landscape config was spending its mirror_x on.
    static constexpr bool TOUCH_SWAP_XY  = false;
    static constexpr bool TOUCH_MIRROR_X = true;
    static constexpr bool TOUCH_MIRROR_Y = true;
}
