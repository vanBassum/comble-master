#pragma once

#include "InitState.h"
#include "BoardConfig.h"
#include "interfaces/BoardProvider.h"
#include "drivers/MockLed.h"
#include "Display.h"
#include "Touch.h"

// ──────────────────────────────────────────────────────────────
// The board layer's context for the Wireless-Tag WT-SC01 Plus: owns every
// hardware driver instance and answers BoardProvider.
//
// Two things here are worth reading rather than copying:
//
//   • GetLed() binds a MockLed. No user LED is fitted — the only LED-ish thing
//     on the board is the LCD backlight, which belongs to the display. Binding
//     a mock is the template's existing discipline for an unfitted role, not a
//     workaround.
//
//   • GetDisplay() is a CONCRETE accessor and stays OFF BoardProvider. That is
//     the escape hatch BoardProvider.h describes, and using it here is what
//     keeps the role list from growing a MockDisplay that the DevKit and the
//     SuperMini would both have to bind. The cost is that display code only
//     compiles for this board, which is exactly the compile-time check wanted.
// ──────────────────────────────────────────────────────────────

class BoardContext : public BoardProvider
{
    static constexpr const char *TAG = "Board";

public:
    BoardContext() = default;

    BoardContext(const BoardContext &) = delete;
    BoardContext &operator=(const BoardContext &) = delete;
    BoardContext(BoardContext &&) = delete;
    BoardContext &operator=(BoardContext &&) = delete;

    void Init();

    // ── Roles (BoardProvider) ──
    Led &GetLed() override { return led_; }

    // ── Concrete drivers (this board only) ──
    Display &GetDisplay() { return display_; }
    Touch &GetTouch() { return touch_; }

    /// Whether the panel came up. UiManager refuses to start LVGL without it.
    bool HasDisplay() const { return displayOk_; }

    /// Whether the touch controller answered. Separate from HasDisplay on
    /// purpose: a panel with dead touch is still worth lighting, it just cannot
    /// be driven by a fingertip.
    bool HasTouch() const { return touchOk_; }

private:
    InitState initState_;

    MockLed led_;
    Display display_;
    Touch touch_;
    bool displayOk_ = false;
    bool touchOk_ = false;
};
