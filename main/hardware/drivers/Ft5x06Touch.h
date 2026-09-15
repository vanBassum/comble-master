#pragma once

#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_err.h"
#include <cstdint>

// ──────────────────────────────────────────────────────────────
// Reusable driver — FocalTech FT5x06 family (incl. the FT6336U on the
// WT-SC01 Plus) capacitive touch over I2C. Ported from the KC1245 Thermostat,
// where it was verified on this panel.
//
// Bus-agnostic: the board supplies the bus, the NATIVE (pre-rotation) panel
// extents, the reset/INT pins and the rotation flags that bring raw controller
// coordinates into the displayed frame. The board wraps this in its own Touch.
//
// Note the asymmetry with the display: the panel's rotation is handed to
// esp_lvgl_port, but the touch controller's is applied HERE, because
// esp_lcd_touch does the transform itself and the LVGL port only reads the
// already-transformed point. The two sets of flags are therefore configured in
// different places and need not match — on this board they do not.
//
// Init() is best-effort: a false return means no touch, not a dead display.
// ──────────────────────────────────────────────────────────────

struct Ft5x06Config
{
    i2c_master_bus_handle_t bus = nullptr;
    uint32_t addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    int x_max = 0;          // native, pre-rotation
    int y_max = 0;          // native, pre-rotation
    gpio_num_t rst = GPIO_NUM_NC;
    gpio_num_t intr = GPIO_NUM_NC;
    uint32_t scl_speed_hz = 400000;
    bool swap_xy = false;
    bool mirror_x = false;
    bool mirror_y = false;
};

class Ft5x06Touch
{
    static constexpr const char *TAG = "Touch";

public:
    bool Init(const Ft5x06Config &cfg)
    {
        if (!cfg.bus) return false;

        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t tp_io_cfg = {};
        tp_io_cfg.dev_addr = cfg.addr;
        tp_io_cfg.scl_speed_hz = cfg.scl_speed_hz;
        tp_io_cfg.control_phase_bytes = 1;
        tp_io_cfg.dc_bit_offset = 0;
        tp_io_cfg.lcd_cmd_bits = 8;
        tp_io_cfg.flags.disable_control_phase = 1;
        esp_err_t err = esp_lcd_new_panel_io_i2c(cfg.bus, &tp_io_cfg, &tp_io);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Touch panel IO init failed: %s", esp_err_to_name(err));
            return false;
        }

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = cfg.x_max;
        tp_cfg.y_max = cfg.y_max;
        tp_cfg.rst_gpio_num = cfg.rst;
        tp_cfg.int_gpio_num = cfg.intr;
        tp_cfg.flags.swap_xy = cfg.swap_xy;
        tp_cfg.flags.mirror_x = cfg.mirror_x;
        tp_cfg.flags.mirror_y = cfg.mirror_y;

        err = esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &touch_);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "FT5x06 init failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    esp_lcd_touch_handle_t handle() const { return touch_; }
    bool ok() const { return touch_ != nullptr; }

private:
    esp_lcd_touch_handle_t touch_ = nullptr;
};
