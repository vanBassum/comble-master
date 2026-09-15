#pragma once

#include "AppProvider.h"
#include "InitState.h"

// ──────────────────────────────────────────────────────────────
// The touchscreen UI: LVGL, the panel, the touch controller, and the screens
// this product shows on them.
//
// An application manager like LedManager, reaching hardware the way the
// layering allows — app_.getBoard().GetDisplay() / GetTouch(), concrete
// accessors only the WT-SC01 Plus board defines. Everything LVGL lives in the
// .cpp behind BOARD_WT_SC01_PLUS, and this header deliberately names no LVGL
// type: the C3 SuperMini does not fetch the LVGL component at all (see the
// target rules in idf_component.yml), so an lv_obj_t here would not compile
// there. On a board without a panel Init() is a no-op.
//
// Screens live in the .cpp as one cohesive unit rather than a class each. They
// share a header bar, a palette and a back-stack; splitting them would mean an
// internal header existing only to let them call each other.
// ──────────────────────────────────────────────────────────────

class UiManager
{
    static constexpr const char *TAG = "UiManager";

public:
    explicit UiManager(AppProvider &app) : app_(app) {}

    UiManager(const UiManager &) = delete;
    UiManager &operator=(const UiManager &) = delete;
    UiManager(UiManager &&) = delete;
    UiManager &operator=(UiManager &&) = delete;

    void Init();

private:
    AppProvider &app_;
    InitState initState_;
};
