#pragma once

#include "AppProvider.h"
#include "InitState.h"
#include "CommandEntry.h"

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

    // ── Commands ──
    // The panel is the one part of this product a browser cannot see, so it gets a
    // surface on the wire like everything else: `ui goto` drives the screen stack, and
    // `ui screenshot` hands back the frame the panel is actually drawing — a header
    // record, then the raw framebuffer, the shape `web read` already uses. That is how
    // a screen is verified from a script instead of from a camera. `ui touch` is the
    // other half: it moves a second LVGL pointer, so a press from the browser reaches
    // a widget down the same path a finger does. Together they are the Panel page.
    //
    // Declared here on every board, because the table below takes their addresses;
    // registered only where there IS a panel, so on a display-less board they are not
    // in `help list` and cannot be called. Neither signature names an LVGL type, which
    // is what lets this header stay board-independent (see the note at the top).
    RequestError Cmd_Goto(CommandContext &ctx);
    RequestError Cmd_Screenshot(CommandContext &ctx);
    RequestError Cmd_Touch(CommandContext &ctx);

    inline static CommandEntry commands_[] = {
        { "ui", "goto",       &InvokeCommand<&UiManager::Cmd_Goto> },
        { "ui", "screenshot", &InvokeCommand<&UiManager::Cmd_Screenshot> },
        { "ui", "touch",      &InvokeCommand<&UiManager::Cmd_Touch> },
    };
};
