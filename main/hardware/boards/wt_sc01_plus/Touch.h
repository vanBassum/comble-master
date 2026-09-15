#pragma once

#include "BoardConfig.h"
#include "drivers/Ft5x06Touch.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

// ──────────────────────────────────────────────────────────────
// FT6336U capacitive touch (FT5x06-compatible, I2C 0x38) for the WT-SC01 Plus.
// Owns the I2C bus it sits on — nothing else on this board uses I2C yet, and
// the day something does, the bus moves up into BoardContext and is passed in.
//
// Extents are the panel's NATIVE portrait 320x480; swap_xy + mirror_x bring a
// touch into the landscape frame the display presents. These came over from the
// Thermostat, where touch was verified working on this panel.
// ──────────────────────────────────────────────────────────────

class Touch
{
    static constexpr const char *TAG = "Touch";

public:
    bool Init()
    {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = (gpio_num_t)BoardConfig::TOUCH_PIN_SDA;
        bus_cfg.scl_io_num = (gpio_num_t)BoardConfig::TOUCH_PIN_SCL;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        i2c_master_bus_handle_t bus = nullptr;
        if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK)
        {
            ESP_LOGW(TAG, "I2C bus init failed");
            return false;
        }

        Ft5x06Config cfg{};
        cfg.bus = bus;
        cfg.x_max = BoardConfig::LCD_NATIVE_H_RES;   // 320
        cfg.y_max = BoardConfig::LCD_NATIVE_V_RES;   // 480
        cfg.rst = (gpio_num_t)BoardConfig::TOUCH_PIN_RST;
        cfg.intr = (gpio_num_t)BoardConfig::TOUCH_PIN_INT;
        cfg.scl_speed_hz = 400000;
        cfg.swap_xy = BoardConfig::TOUCH_SWAP_XY;
        cfg.mirror_x = BoardConfig::TOUCH_MIRROR_X;
        cfg.mirror_y = BoardConfig::TOUCH_MIRROR_Y;
        return drv_.Init(cfg);
    }

    esp_lcd_touch_handle_t handle() const { return drv_.handle(); }
    bool ok() const { return drv_.ok(); }

private:
    Ft5x06Touch drv_;
};
