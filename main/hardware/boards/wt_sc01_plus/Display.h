#pragma once

#include "BoardConfig.h"
#include "drivers/St7796Panel.h"
#include "driver/gpio.h"
#include "esp_log.h"

// ──────────────────────────────────────────────────────────────
// The WT-SC01 Plus panel, as this board presents it.
//
// A CONCRETE driver, reached through BoardContext::GetDisplay() and
// deliberately NOT a role on BoardProvider — see the note there: the day one
// board grows a display is not the day every other board owes a MockDisplay.
// Application code that calls this only compiles for a board that has one,
// which is the intended check.
//
// The surface is two handles and a backlight pin, and that is the whole job.
// Drawing belongs to LVGL: esp_lvgl_port takes panel() to push pixels and io()
// to hook its flush-done callback, and from then on nothing else writes to the
// panel. There is deliberately no FillRect here any more — a second drawing
// path sharing one bus with LVGL is a race, not a convenience.
//
// ROTATION IS NOT SET HERE. esp_lvgl_port re-applies swap_xy/mirror on the
// panel from its own disp_cfg.rotation, so a call to esp_lcd_panel_swap_xy()
// in this file would be silently undone. BoardConfig's LCD_SWAP_XY/MIRROR_*
// are read by UiManager where the port is configured.
// ──────────────────────────────────────────────────────────────

class Display
{
    static constexpr const char *TAG = "Display";

public:
    bool Init()
    {
        if (!InitBacklight()) return false;
        return InitPanel();
    }

    esp_lcd_panel_handle_t panel() const { return drv_.panel(); }
    esp_lcd_panel_io_handle_t io() const { return drv_.io(); }
    bool ok() const { return drv_.ok(); }

    /// Backlight on/off. Held dark from Init until the first frame has been
    /// rendered, so the panel never shows its power-on noise.
    void Backlight(bool on)
    {
        gpio_set_level((gpio_num_t)BoardConfig::LCD_PIN_BACKLIGHT, on ? 1 : 0);
    }

    /// Logical, post-rotation extents — what LVGL and every widget sees.
    static constexpr int Width()  { return BoardConfig::LCD_H_RES; }
    static constexpr int Height() { return BoardConfig::LCD_V_RES; }

private:
    bool InitBacklight()
    {
        gpio_config_t bk = {};
        bk.mode = GPIO_MODE_OUTPUT;
        bk.pin_bit_mask = 1ULL << BoardConfig::LCD_PIN_BACKLIGHT;
        if (gpio_config(&bk) != ESP_OK)
        {
            ESP_LOGE(TAG, "Backlight GPIO config failed");
            return false;
        }
        Backlight(false);
        return true;
    }

    bool InitPanel()
    {
        St7796Config cfg{};
        cfg.dc = (gpio_num_t)BoardConfig::LCD_PIN_DC;
        cfg.wr = (gpio_num_t)BoardConfig::LCD_PIN_WR;
        cfg.cs = (gpio_num_t)BoardConfig::LCD_PIN_CS;
        cfg.data_pins = BoardConfig::LCD_DATA_PINS;
        cfg.pclk_hz = BoardConfig::LCD_PIXEL_CLOCK_HZ;
        // Max bytes per transfer: one LVGL flush buffer.
        cfg.max_transfer_bytes =
            (size_t)BoardConfig::LCD_H_RES * BoardConfig::LCD_DRAW_BUFFER_LINES * 2;
        cfg.dma_burst_size = 64;
        cfg.reset_gpio = (gpio_num_t)BoardConfig::LCD_PIN_RST;
        cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;  // most ST7796 modules are BGR
        cfg.bits_per_pixel = BoardConfig::LCD_BITS_PER_PIXEL;
        cfg.invert_color = true;                        // ST7796 typically needs inversion on
        // Rotation intentionally left at its defaults — esp_lvgl_port owns it.
        return drv_.Init(cfg);
    }

    St7796Panel drv_;
};
