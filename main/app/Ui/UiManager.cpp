#include "UiManager.h"
#include "esp_log.h"

#ifdef BOARD_WT_SC01_PLUS

#include "BoardContext.h"
#include "StruxProvider.h"
#include "NetworkManager.h"
#include "SettingsManager.h"
#include "SystemManager.h"
#include "CommandManager.h"
#include "RelayManager.h"
#include "TimeManager.h"
#include "DateTime.h"
#include "Task.h"
#include "Ble/BleHostManager.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "CandidateIcons.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>

// ──────────────────────────────────────────────────────────────
// Everything below runs on the LVGL task unless it says otherwise. LVGL is not
// thread-safe, so anything touching a widget from another task takes
// lvgl_port_lock() first — the Wi-Fi scan is the only such place, and it says so.
//
// The screens are drawn to a concept: a dark ground, a column of rounded cards
// with a gap between them, a coloured state dot at the left of each, and one
// full-width blue action anchored at the bottom. Three things follow from that
// and are worth knowing before changing anything here:
//
//   * PORTRAIT. 320x480, which is the panel's own scan order — see the
//     orientation note in the board's BoardConfig.h. The layout is a column,
//     not a wide screen stood on end, so the constants below assume the narrow
//     axis is the horizontal one.
//
//   * NOT lv_list. lv_list draws one flat surface with dividers, and there is
//     no seam in it to put a gap or a per-row radius. Every list here is an
//     ordinary flex column of card objects instead, which costs a builder
//     function and buys back full control of each row.
//
//   * SHARED STYLES, not per-object setters. A card costs about a dozen style
//     properties; setting them per object writes a local style per object and
//     the RAM adds up over thirty rows. The lv_style_t block below is
//     initialised once and attached by reference.
//
// Screens live in this one file rather than a class each. They share the
// chrome, the palette and the back-stack; splitting them would mean an internal
// header existing only to let them call each other.
// ──────────────────────────────────────────────────────────────

namespace
{
    constexpr const char *TAG = "UiManager";

    // ── Palette ────────────────────────────────────────────────
    // Near-black ground, one step of elevation for cards, one blue accent, and
    // three state colours. Deliberately short: a fourth surface tone or a second
    // accent is how a UI stops looking like one thing.
    constexpr uint32_t kBg        = 0x0A0E14;   // screen ground
    constexpr uint32_t kSurface   = 0x151B24;   // a card at rest
    constexpr uint32_t kSurfaceHi = 0x1E2632;   // a card under a finger
    constexpr uint32_t kSelected  = 0x15293F;   // the row that is already true
    constexpr uint32_t kLine      = 0x232C38;   // hairline

    constexpr uint32_t kText      = 0xE8EDF4;   // primary line of a row
    constexpr uint32_t kTextDim   = 0x8A94A3;   // secondary line, values
    constexpr uint32_t kTextFaint = 0x5A6474;   // section labels, placeholders

    constexpr uint32_t kAccent    = 0x2E90FA;   // the one blue
    constexpr uint32_t kAccentBox = 0x16324D;   // tinted square behind a menu icon

    constexpr uint32_t kOk        = 0x22C55E;   // connected / in range
    constexpr uint32_t kIdle      = 0x4B5563;   // known but not here
    constexpr uint32_t kWarn      = 0xF5A524;   // somebody else's
    constexpr uint32_t kDanger    = 0xE5484D;   // failed

    // The relay-status mark, picked off the ten-candidate strip this bar used to
    // draw in place of the radios. C7 was the rightmost of them. CandidateIcons.h
    // still carries the whole shortlist so trying another is this one line — the
    // nine unused masks are file-scope statics in a single translation unit, so
    // they cost nothing in flash until one is named here.
    constexpr const lv_image_dsc_t *kRelayIcon = &kIcon_c7;

    // ── Metrics (320x480 portrait) ─────────────────────────────
    constexpr int kPad     = 14;   // side gutter, everywhere
    constexpr int kStatusH = 28;   // the thin strip: clock, radios, gear
    constexpr int kTitleH  = 48;   // back chevron, title, optional action
    constexpr int kHeadH   = kStatusH + kTitleH;
    constexpr int kRowH    = 64;   // a two-line card
    constexpr int kRadius  = 14;
    constexpr int kGap     = 8;    // between cards
    constexpr int kCtaH    = 52;   // the bottom action
    constexpr int kDot     = 10;   // state dot

    int ScreenW() { return lv_display_get_horizontal_resolution(nullptr); }
    int ScreenH() { return lv_display_get_vertical_resolution(nullptr); }

    /// Where a bottom-anchored action starts, and where the list above it ends.
    int CtaY()    { return ScreenH() - kPad - kCtaH; }
    int ListBot() { return CtaY() - 12; }

    // ── Shared state. Single-instance by nature: there is one panel. ──
    AppProvider *g_app = nullptr;

    lv_obj_t *g_homeScreen     = nullptr;
    lv_obj_t *g_settingsScreen = nullptr;
    lv_obj_t *g_wifiScreen     = nullptr;
    lv_obj_t *g_passScreen     = nullptr;
    lv_obj_t *g_slavesScreen   = nullptr;
    lv_obj_t *g_codeScreen     = nullptr;
    lv_obj_t *g_pairScreen     = nullptr;

    // ── The remote pointer ──────────────────────────────
    // A second POINTER input device beside the panel's own touch controller, reporting
    // whatever the last `ui touch` said. LVGL polls it on its own task every refresh
    // period, so a tap that arrives over the WebSocket travels the identical path a
    // finger does — the same press, the same widget hit-test, the same event. Nothing
    // in a screen knows which device pressed it, and nothing had to be written twice.
    lv_indev_t *g_remoteIndev = nullptr;
    int32_t     g_remoteX = 0;
    int32_t     g_remoteY = 0;
    bool        g_remotePressed = false;

    void RemotePointerRead(lv_indev_t *, lv_indev_data_t *data)
    {
        data->point.x = g_remoteX;
        data->point.y = g_remoteY;
        data->state = g_remotePressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }

    /// The screens by name, for the two commands at the bottom of this file. Pointers
    /// TO the variables rather than the screens: this table is written before a single
    /// screen exists, the same deferral MakeTitleBar's back chevron relies on.
    struct NamedScreen { const char *name; lv_obj_t **screen; };
    constexpr NamedScreen kNamedScreens[] = {
        { "home",     &g_homeScreen     },
        { "settings", &g_settingsScreen },
        { "wifi",     &g_wifiScreen     },
        { "password", &g_passScreen     },
        { "slaves",   &g_slavesScreen   },
        { "code",     &g_codeScreen     },
        { "pairing",  &g_pairScreen     },
    };

    lv_obj_t *ScreenByName(const char *name)
    {
        for (const auto &s : kNamedScreens)
            if (strcmp(name, s.name) == 0) return *s.screen;
        return nullptr;
    }

    /// What is on the panel right now, named. "?" only if a screen were loaded that
    /// this file did not build, which nothing does.
    const char *ActiveScreenName()
    {
        lv_obj_t *active = lv_screen_active();
        for (const auto &s : kNamedScreens)
            if (*s.screen == active) return s.name;
        return "?";
    }

    // ── Styles, built once in InitStyles() ─────────────────────
    lv_style_t g_stPlain;      // strips lv_obj's default chrome
    lv_style_t g_stCard;       // a row at rest
    lv_style_t g_stCardPress;  // ...and under a finger
    lv_style_t g_stCta;        // the blue bottom action
    lv_style_t g_stCtaPress;
    lv_style_t g_stGhost;      // a borderless tap target (chevron, gear)
    lv_style_t g_stKeys;       // keyboard body
    lv_style_t g_stKeysItem;   // keyboard keys
    lv_style_t g_stInput;      // text area

    // ── The status strip, once per screen ──────────────────────
    // Every screen carries its own copy of the strip, so the widgets are per
    // screen and a single global pair of labels would only ever update the last
    // one built. They register here instead and the refresh timer walks the lot;
    // there are six screens, so the loop is cheaper than the bookkeeping any
    // cleverer scheme would need.
    struct StatusBar
    {
        lv_obj_t *clock;
        lv_obj_t *wifi;
        lv_obj_t *ble;
        lv_obj_t *relay;   // an lv_image, not a label — recoloured, not retexted
    };
    constexpr int kMaxStatusBars = 8;
    StatusBar g_status[kMaxStatusBars] = {};
    int g_statusCount = 0;

    // ── Home list cache ────────────────────────────────────────
    // What the home list last drew, so it is rebuilt only when something really
    // changed — an LVGL list rebuilt under a finger eats the tap.
    lv_obj_t *g_homeList = nullptr;
    BleHostManager::PairedSlave g_homePaired[BleHostManager::kMaxPaired] = {};
    int  g_homePairedCount = 0;
    BleHostManager::Slave g_homeFound[BleHostManager::kMaxResults] = {};
    int  g_homeFoundCount = 0;

    // ── Wi-Fi screen ───────────────────────────────────────────
    lv_obj_t *g_wifiCardName  = nullptr;   // SSID, or the mode we are in
    lv_obj_t *g_wifiCardState = nullptr;   // address / progress
    lv_obj_t *g_wifiCardDot   = nullptr;
    lv_obj_t *g_wifiList      = nullptr;
    lv_obj_t *g_passTitle     = nullptr;
    lv_obj_t *g_passInput     = nullptr;

    // ── Slaves screen ──────────────────────────────────────────
    lv_obj_t *g_slavesState = nullptr;
    lv_obj_t *g_slavesList  = nullptr;
    lv_obj_t *g_codeTitle   = nullptr;
    lv_obj_t *g_codeInput   = nullptr;

    // ── Pairing screen ─────────────────────────────────────────
    lv_obj_t *g_pairSpinner = nullptr;
    lv_obj_t *g_pairGlyph   = nullptr;   // tick or cross, once it is over
    lv_obj_t *g_pairName    = nullptr;
    lv_obj_t *g_pairState   = nullptr;
    lv_obj_t *g_pairBack    = nullptr;
    int g_pairSettleTicks = 0;           // how long the result has been up

    // The slave a pairing code is being typed for. A COPY, not an index into the
    // scan results: the results array is refilled by the next scan, and the user
    // may well start one while the keypad is open.
    BleHostManager::Slave g_pendingSlave = {};
    BleHostManager::Slave g_slaveView[BleHostManager::kMaxResults] = {};
    int g_slaveViewCount = 0;

    // The SSID a password is currently being typed for.
    char g_pendingSsid[33] = {};

    // ── Scan plumbing. Scan() blocks for seconds, so it cannot run on the LVGL
    // task or the UI would freeze mid-tap. It runs on its own task and the
    // results are copied back under the LVGL lock.
    constexpr int kMaxScanResults = 20;
    WiFiInterface::ScanResult g_scanResults[kMaxScanResults];
    int  g_scanCount = 0;
    bool g_scanBusy  = false;
    Task g_scanTask;

    // ──────────────────────────────────────────────────────────
    // Styles
    // ──────────────────────────────────────────────────────────
    /// A style selector is a part OR'd with a state. LVGL declares the two as
    /// separate enums, and C++ deprecates a bitwise op between different
    /// enumeration types — so the OR happens in the type the API actually takes.
    constexpr lv_style_selector_t Pressed(lv_style_selector_t part)
    {
        return part | (lv_style_selector_t)LV_STATE_PRESSED;
    }

    /// A keyboard's control keys — the mode switch, enter, backspace, the hide-keyboard
    /// key — are button-matrix buttons LVGL marks CHECKED, and the stock theme paints a
    /// checked button in its light "selected" colour. On a dark keyboard that reads as
    /// two or three keys someone forgot to style, so the state is answered here rather
    /// than left to the theme.
    constexpr lv_style_selector_t Checked(lv_style_selector_t part)
    {
        return part | (lv_style_selector_t)LV_STATE_CHECKED;
    }

    constexpr lv_style_selector_t CheckedPressed(lv_style_selector_t part)
    {
        return part | (lv_style_selector_t)(LV_STATE_CHECKED | LV_STATE_PRESSED);
    }

    void InitStyles()
    {
        // lv_obj_create hands back a white, bordered, padded, scrollable box.
        // Every container here wants none of that, so it is stripped once.
        lv_style_init(&g_stPlain);
        lv_style_set_bg_opa(&g_stPlain, LV_OPA_TRANSP);
        lv_style_set_border_width(&g_stPlain, 0);
        lv_style_set_outline_width(&g_stPlain, 0);
        lv_style_set_shadow_width(&g_stPlain, 0);
        lv_style_set_radius(&g_stPlain, 0);
        lv_style_set_pad_all(&g_stPlain, 0);

        lv_style_init(&g_stCard);
        lv_style_set_bg_opa(&g_stCard, LV_OPA_COVER);
        lv_style_set_bg_color(&g_stCard, lv_color_hex(kSurface));
        lv_style_set_radius(&g_stCard, kRadius);
        lv_style_set_border_width(&g_stCard, 1);
        lv_style_set_border_color(&g_stCard, lv_color_hex(kLine));
        lv_style_set_shadow_width(&g_stCard, 0);
        lv_style_set_pad_all(&g_stCard, 0);

        lv_style_init(&g_stCardPress);
        lv_style_set_bg_color(&g_stCardPress, lv_color_hex(kSurfaceHi));

        lv_style_init(&g_stCta);
        lv_style_set_bg_opa(&g_stCta, LV_OPA_COVER);
        lv_style_set_bg_color(&g_stCta, lv_color_hex(kAccent));
        lv_style_set_radius(&g_stCta, kRadius);
        lv_style_set_border_width(&g_stCta, 0);
        lv_style_set_shadow_width(&g_stCta, 0);
        lv_style_set_text_color(&g_stCta, lv_color_hex(0xFFFFFF));
        lv_style_set_text_font(&g_stCta, &lv_font_montserrat_16);

        lv_style_init(&g_stCtaPress);
        lv_style_set_bg_color(&g_stCtaPress, lv_color_hex(0x1D6FD0));

        lv_style_init(&g_stGhost);
        lv_style_set_bg_opa(&g_stGhost, LV_OPA_TRANSP);
        lv_style_set_border_width(&g_stGhost, 0);
        lv_style_set_shadow_width(&g_stGhost, 0);
        lv_style_set_pad_all(&g_stGhost, 0);
        lv_style_set_text_color(&g_stGhost, lv_color_hex(kText));

        // The keyboard is a button matrix and inherits none of the above, so it
        // gets its own pair: the body, and the keys as LV_PART_ITEMS.
        lv_style_init(&g_stKeys);
        lv_style_set_bg_color(&g_stKeys, lv_color_hex(0x0F141B));
        lv_style_set_border_width(&g_stKeys, 0);
        lv_style_set_radius(&g_stKeys, 0);
        lv_style_set_pad_all(&g_stKeys, 6);

        lv_style_init(&g_stKeysItem);
        lv_style_set_bg_color(&g_stKeysItem, lv_color_hex(kSurface));
        lv_style_set_bg_opa(&g_stKeysItem, LV_OPA_COVER);
        lv_style_set_text_color(&g_stKeysItem, lv_color_hex(kText));
        lv_style_set_text_font(&g_stKeysItem, &lv_font_montserrat_16);
        lv_style_set_border_width(&g_stKeysItem, 0);
        lv_style_set_radius(&g_stKeysItem, 8);

        lv_style_init(&g_stInput);
        lv_style_set_bg_color(&g_stInput, lv_color_hex(kSurface));
        lv_style_set_bg_opa(&g_stInput, LV_OPA_COVER);
        lv_style_set_border_width(&g_stInput, 1);
        lv_style_set_border_color(&g_stInput, lv_color_hex(kLine));
        lv_style_set_radius(&g_stInput, 12);
        lv_style_set_text_color(&g_stInput, lv_color_hex(kText));
        lv_style_set_text_font(&g_stInput, &lv_font_montserrat_16);
        lv_style_set_pad_all(&g_stInput, 12);
    }

    // ──────────────────────────────────────────────────────────
    // Small builders
    // ──────────────────────────────────────────────────────────
    lv_obj_t *MakeLabel(lv_obj_t *parent, const char *text,
                        uint32_t color, const lv_font_t *font)
    {
        lv_obj_t *l = lv_label_create(parent);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
        return l;
    }

    /// A filled circle — the state marker at the left of every adapter row.
    lv_obj_t *MakeDot(lv_obj_t *parent, uint32_t color, int size = kDot)
    {
        lv_obj_t *d = lv_obj_create(parent);
        lv_obj_add_style(d, &g_stPlain, LV_PART_MAIN);
        lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(d, size, size);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(d, lv_color_hex(color), LV_PART_MAIN);
        return d;
    }

    lv_obj_t *MakeScreen()
    {
        lv_obj_t *scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(scr, lv_color_hex(kBg), LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        return scr;
    }

    void OnBackClicked(lv_event_t *e)
    {
        auto *target = static_cast<lv_obj_t **>(lv_event_get_user_data(e));
        if (target && *target) lv_screen_load(*target);
    }

    void OnGearClicked(lv_event_t *)  { lv_screen_load(g_settingsScreen); }

    /// The thin strip across the top: clock at the left, radio state at the
    /// right, and on the home screen a gear beside them.
    ///
    /// The clock is drawn only once SNTP has actually landed (TimeManager says
    /// so). A device fresh off the bench has no idea what time it is, and a
    /// confident 00:00 in the corner is worse than a blank one.
    void MakeStatusBar(lv_obj_t *parent, bool withGear)
    {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_add_style(bar, &g_stPlain, LV_PART_MAIN);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bar, LV_PCT(100), kStatusH);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

        lv_obj_t *clock = MakeLabel(bar, "", kTextDim, &lv_font_montserrat_14);
        lv_obj_align(clock, LV_ALIGN_LEFT_MID, kPad, 0);

        int rightEdge = -kPad;
        if (withGear)
        {
            // Sized as a touch target rather than as an icon: 44px is about a
            // fingertip, and the glyph alone is not.
            lv_obj_t *gear = lv_button_create(bar);
            lv_obj_add_style(gear, &g_stGhost, LV_PART_MAIN);
            lv_obj_set_size(gear, 44, kStatusH);
            lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -kPad + 10, 0);
            lv_obj_add_event_cb(gear, OnGearClicked, LV_EVENT_CLICKED, nullptr);

            lv_obj_t *gl = MakeLabel(gear, LV_SYMBOL_SETTINGS, kTextDim,
                                     &lv_font_montserrat_16);
            lv_obj_center(gl);
            rightEdge = -kPad - 34;
        }

        lv_obj_t *wifi = MakeLabel(bar, LV_SYMBOL_WIFI, kTextFaint, &lv_font_montserrat_14);
        lv_obj_align(wifi, LV_ALIGN_RIGHT_MID, rightEdge, 0);

        lv_obj_t *ble = MakeLabel(bar, LV_SYMBOL_BLUETOOTH, kTextFaint, &lv_font_montserrat_14);
        lv_obj_align(ble, LV_ALIGN_RIGHT_MID, rightEdge - 24, 0);

        // The relay link, left of the two radios, because it reads as the third
        // thing in the same sentence: Wi-Fi says we have an uplink, Bluetooth says
        // we can hear the adapters, this says the uplink reaches the Strux server.
        //
        // An A8 mask rather than a font glyph (CandidateIcons.h, generated), so the
        // shape is the asset and the colour is a style — which is what lets
        // RefreshStatus swap it exactly as it swaps the two labels beside it.
        // Starts faint: the link is down until RelayManager says otherwise, and a
        // strip that lights up before the first refresh would lie for 500 ms.
        lv_obj_t *relay = lv_image_create(bar);
        lv_image_set_src(relay, kRelayIcon);
        lv_obj_set_style_image_recolor_opa(relay, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_image_recolor(relay, lv_color_hex(kTextFaint), LV_PART_MAIN);
        lv_obj_align(relay, LV_ALIGN_RIGHT_MID, rightEdge - 48, 0);

        if (g_statusCount < kMaxStatusBars)
            g_status[g_statusCount++] = StatusBar{ clock, wifi, ble, relay };
        else
            ESP_LOGW(TAG, "More status bars than the refresh list holds");
    }

    /// Back chevron, centred title, and an optional action at the right.
    ///
    /// `backTo` is a POINTER TO the screen variable, not the screen: headers are
    /// built before every screen exists, and taking the address defers the read
    /// to the moment the chevron is actually tapped.
    lv_obj_t *MakeTitleBar(lv_obj_t *parent, const char *title, lv_obj_t **backTo)
    {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_add_style(bar, &g_stPlain, LV_PART_MAIN);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(bar, LV_PCT(100), kTitleH);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, kStatusH);

        lv_obj_t *lbl = MakeLabel(bar, title, kText, &lv_font_montserrat_20);
        lv_obj_center(lbl);

        if (backTo)
        {
            lv_obj_t *back = lv_button_create(bar);
            lv_obj_add_style(back, &g_stGhost, LV_PART_MAIN);
            lv_obj_set_size(back, 48, kTitleH);
            lv_obj_align(back, LV_ALIGN_LEFT_MID, kPad - 12, 0);
            lv_obj_add_event_cb(back, OnBackClicked, LV_EVENT_CLICKED, backTo);

            lv_obj_t *bl = MakeLabel(back, LV_SYMBOL_LEFT, kText, &lv_font_montserrat_16);
            lv_obj_center(bl);
        }
        return bar;
    }

    /// A scrolling flex column of cards. Everything that lists things uses one.
    lv_obj_t *MakePanel(lv_obj_t *parent, int y, int h)
    {
        lv_obj_t *p = lv_obj_create(parent);
        lv_obj_add_style(p, &g_stPlain, LV_PART_MAIN);
        lv_obj_set_size(p, ScreenW() - 2 * kPad, h);
        lv_obj_set_pos(p, kPad, y);
        lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(p, kGap, LV_PART_MAIN);
        lv_obj_set_scroll_dir(p, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
        return p;
    }

    /// A small dim heading between groups of cards. Not a card itself — it sits
    /// in the gutter between them, which is what makes the grouping read.
    lv_obj_t *MakeSection(lv_obj_t *panel, const char *text)
    {
        lv_obj_t *l = MakeLabel(panel, text, kTextFaint, &lv_font_montserrat_14);
        lv_obj_set_style_text_letter_space(l, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_left(l, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_top(l, 6, LV_PART_MAIN);
        return l;
    }

    /// The base every tappable card starts from.
    lv_obj_t *MakeCard(lv_obj_t *panel, int h)
    {
        lv_obj_t *c = lv_obj_create(panel);
        lv_obj_add_style(c, &g_stCard, LV_PART_MAIN);
        lv_obj_add_style(c, &g_stCardPress, Pressed(LV_PART_MAIN));
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_width(c, LV_PCT(100));
        lv_obj_set_height(c, h);
        lv_obj_set_style_flex_grow(c, 0, LV_PART_MAIN);
        return c;
    }

    /// One adapter / network row: state dot, name over a secondary line, an
    /// optional value at the right, and a chevron when there is somewhere to go.
    struct RowSpec
    {
        uint32_t    dot;        // the state colour
        const char *name;
        const char *sub;
        uint32_t    subColor;
        const char *value;      // nullptr for none
        bool        chevron;
        bool        selected;   // the row that is already true (current network)
    };

    lv_obj_t *MakeRow(lv_obj_t *panel, const RowSpec &spec)
    {
        lv_obj_t *c = MakeCard(panel, kRowH);
        if (spec.selected)
        {
            lv_obj_set_style_bg_color(c, lv_color_hex(kSelected), LV_PART_MAIN);
            lv_obj_set_style_border_color(c, lv_color_hex(kAccent), LV_PART_MAIN);
        }

        lv_obj_t *d = MakeDot(c, spec.dot);
        lv_obj_align(d, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *name = MakeLabel(c, spec.name, kText, &lv_font_montserrat_16);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 150);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 40, 12);

        lv_obj_t *sub = MakeLabel(c, spec.sub, spec.subColor, &lv_font_montserrat_14);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sub, 190);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 40, 35);

        const int valueRight = spec.chevron ? -34 : -16;
        if (spec.value)
        {
            lv_obj_t *v = MakeLabel(c, spec.value, kTextDim, &lv_font_montserrat_14);
            lv_obj_align(v, LV_ALIGN_TOP_RIGHT, valueRight, 14);
        }
        if (spec.chevron)
        {
            lv_obj_t *ch = MakeLabel(c, LV_SYMBOL_RIGHT, kTextFaint, &lv_font_montserrat_14);
            lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -16, 0);
        }
        return c;
    }

    /// A settings row: icon in a tinted rounded square, title over a subtitle.
    lv_obj_t *MakeMenuRow(lv_obj_t *panel, const char *icon,
                          const char *title, const char *sub)
    {
        lv_obj_t *c = MakeCard(panel, kRowH);

        lv_obj_t *box = lv_obj_create(c);
        lv_obj_add_style(box, &g_stPlain, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(box, 36, 36);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(box, lv_color_hex(kAccentBox), LV_PART_MAIN);

        lv_obj_t *ic = MakeLabel(box, icon, kAccent, &lv_font_montserrat_16);
        lv_obj_center(ic);

        lv_obj_t *t = MakeLabel(c, title, kText, &lv_font_montserrat_16);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 58, 12);

        lv_obj_t *s = MakeLabel(c, sub, kTextDim, &lv_font_montserrat_14);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s, 190);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 58, 35);

        lv_obj_t *ch = MakeLabel(c, LV_SYMBOL_RIGHT, kTextFaint, &lv_font_montserrat_14);
        lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -16, 0);
        return c;
    }

    /// A dim card that is a sentence rather than a row — "nothing here yet".
    void MakeEmptyCard(lv_obj_t *panel, const char *text)
    {
        lv_obj_t *c = MakeCard(panel, 52);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_t *l = MakeLabel(c, text, kTextFaint, &lv_font_montserrat_14);
        lv_obj_center(l);
    }

    /// The one blue action, anchored at the bottom of a screen.
    lv_obj_t *MakeCta(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
    {
        lv_obj_t *b = lv_button_create(parent);
        lv_obj_add_style(b, &g_stCta, LV_PART_MAIN);
        lv_obj_add_style(b, &g_stCtaPress, Pressed(LV_PART_MAIN));
        lv_obj_set_size(b, ScreenW() - 2 * kPad, kCtaH);
        lv_obj_set_pos(b, kPad, CtaY());
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_center(l);
        return b;
    }

    void StyleKeyboard(lv_obj_t *kb)
    {
        lv_obj_add_style(kb, &g_stKeys, LV_PART_MAIN);
        lv_obj_add_style(kb, &g_stKeysItem, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(kb, lv_color_hex(kAccent), Pressed(LV_PART_ITEMS));

        // Every key the same, control keys included. LOCAL styles rather than another
        // lv_obj_add_style: a local style beats every added one whatever the theme did
        // first, so there is no ordering to get right here later.
        lv_obj_set_style_bg_color(kb, lv_color_hex(kSurface), Checked(LV_PART_ITEMS));
        lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, Checked(LV_PART_ITEMS));
        lv_obj_set_style_text_color(kb, lv_color_hex(kText), Checked(LV_PART_ITEMS));
        lv_obj_set_style_bg_color(kb, lv_color_hex(kAccent), CheckedPressed(LV_PART_ITEMS));
        lv_obj_set_style_text_color(kb, lv_color_hex(kText), CheckedPressed(LV_PART_ITEMS));
    }

    // ──────────────────────────────────────────────────────────
    // The status strip's contents — the only thing every screen shares.
    // ──────────────────────────────────────────────────────────
    void RefreshStatus(lv_timer_t *)
    {
        auto &net  = g_app->getStrux().getNetworkManager();
        auto &time = g_app->getStrux().getTimeManager();
        auto &ble  = g_app->getBleHost();

        char clock[8] = {};
        if (time.IsTimeValid())
            DateTime::Now().ToStringLocal(clock, sizeof(clock), "%H:%M");

        // Green on a real uplink, blue while we are the access point somebody is
        // standing in front of to configure us, faint when there is neither.
        uint32_t wifiColor = kTextFaint;
        if (net.HasUpstream())       wifiColor = kOk;
        else if (net.IsAccessPoint()) wifiColor = kAccent;

        const BleHostManager::State bs = ble.GetState();
        const uint32_t bleColor =
            (bs == BleHostManager::State::Idle)   ? kTextFaint :
            (bs == BleHostManager::State::Failed) ? kDanger    : kAccent;

        // Two states only, and deliberately so: the pipe to the Strux server is up
        // or it is not. `relay.enabled` off looks the same as unreachable, which is
        // the truth from this strip's point of view — the Relay card in settings is
        // where the difference between "off" and "trying" belongs.
        const uint32_t relayColor =
            g_app->getStrux().getRelayManager().IsConnected() ? kAccent : kTextFaint;

        for (int i = 0; i < g_statusCount; ++i)
        {
            lv_label_set_text(g_status[i].clock, clock);
            lv_obj_set_style_text_color(g_status[i].wifi, lv_color_hex(wifiColor), LV_PART_MAIN);
            lv_obj_set_style_text_color(g_status[i].ble,  lv_color_hex(bleColor),  LV_PART_MAIN);
            lv_obj_set_style_image_recolor(g_status[i].relay, lv_color_hex(relayColor),
                                           LV_PART_MAIN);
        }
    }

    // ──────────────────────────────────────────────────────────
    // Wi-Fi: scan, pick, type a password, save.
    // ──────────────────────────────────────────────────────────
    void RefreshWifiCard(lv_timer_t *)
    {
        if (!g_wifiCardName) return;

        auto &net = g_app->getStrux().getNetworkManager();

        char ip[16] = {};
        const bool haveIp = net.GetIpv4(ip, sizeof(ip));

        char ssid[33] = {};
        g_app->getStrux().getSettingsManager().ReadString("wifi.ssid", ssid, sizeof(ssid));

        if (net.IsAccessPoint())
        {
            // The recovery/provisioning AP. Say so plainly: this is the state
            // someone standing in front of the device most needs explained.
            lv_label_set_text(g_wifiCardName, "Access point mode");
            lv_label_set_text_fmt(g_wifiCardState, "%s  -  no network joined",
                                  haveIp ? ip : "no address");
            lv_obj_set_style_bg_color(g_wifiCardDot, lv_color_hex(kAccent), LV_PART_MAIN);
        }
        else if (net.HasUpstream())
        {
            lv_label_set_text(g_wifiCardName, ssid[0] ? ssid : "Connected");
            int8_t rssi = 0;
            if (net.GetRssi(rssi))
                lv_label_set_text_fmt(g_wifiCardState, "%s  -  %d dBm",
                                      haveIp ? ip : "no address", rssi);
            else
                lv_label_set_text(g_wifiCardState, haveIp ? ip : "no address");
            lv_obj_set_style_bg_color(g_wifiCardDot, lv_color_hex(kOk), LV_PART_MAIN);
        }
        else if (ssid[0])
        {
            lv_label_set_text(g_wifiCardName, ssid);
            lv_label_set_text(g_wifiCardState, "Connecting...");
            lv_obj_set_style_bg_color(g_wifiCardDot, lv_color_hex(kWarn), LV_PART_MAIN);
        }
        else
        {
            lv_label_set_text(g_wifiCardName, "Not configured");
            lv_label_set_text(g_wifiCardState, "Scan below to join a network");
            lv_obj_set_style_bg_color(g_wifiCardDot, lv_color_hex(kIdle), LV_PART_MAIN);
        }
    }

    void OnNetworkPicked(lv_event_t *e)
    {
        auto *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
        const int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
        if (idx < 0 || idx >= g_scanCount) return;

        snprintf(g_pendingSsid, sizeof(g_pendingSsid), "%s", g_scanResults[idx].ssid);

        lv_label_set_text_fmt(g_passTitle, "Password for %s", g_pendingSsid);
        lv_textarea_set_text(g_passInput, "");
        lv_screen_load(g_passScreen);
    }

    /// Caller must hold the LVGL lock (or be on the LVGL task).
    void PopulateScanList()
    {
        if (!g_wifiList) return;
        lv_obj_clean(g_wifiList);

        if (g_scanCount == 0)
        {
            MakeEmptyCard(g_wifiList, g_scanBusy ? "Scanning..." : "No networks found");
            return;
        }

        char joined[33] = {};
        g_app->getStrux().getSettingsManager().ReadString("wifi.ssid", joined, sizeof(joined));
        const bool up = g_app->getStrux().getNetworkManager().HasUpstream();

        for (int i = 0; i < g_scanCount; ++i)
        {
            const WiFiInterface::ScanResult &r = g_scanResults[i];
            const bool current = up && joined[0] && strcmp(joined, r.ssid) == 0;

            char value[16];
            snprintf(value, sizeof(value), "%d dBm", r.rssi);

            RowSpec spec{};
            spec.dot      = current ? kOk : (r.secure ? kIdle : kWarn);
            spec.name     = r.ssid;
            spec.sub      = current ? "Connected"
                                    : (r.secure ? "Secured" : "Open - no password");
            spec.subColor = current ? kOk : kTextDim;
            spec.value    = value;
            spec.chevron  = true;
            spec.selected = current;

            lv_obj_t *row = MakeRow(g_wifiList, spec);
            // Store the INDEX, not a pointer into the results array: the array is
            // refilled by the next scan, and a stored pointer would outlive the
            // entry it pointed at.
            lv_obj_set_user_data(row, (void *)(intptr_t)i);
            lv_obj_add_event_cb(row, OnNetworkPicked, LV_EVENT_CLICKED, nullptr);
        }
    }

    /// Runs on its OWN task: Scan() blocks for several seconds.
    void ScanTaskBody()
    {
        auto &wifi = g_app->getStrux().getNetworkManager().wifi();
        const int n = wifi.Scan(g_scanResults, kMaxScanResults);

        // From here on this touches widgets, and it is the one place in this file
        // not already running on the LVGL task — so it takes the lock.
        if (lvgl_port_lock(0))
        {
            g_scanCount = (n > 0) ? n : 0;
            g_scanBusy  = false;
            PopulateScanList();
            lvgl_port_unlock();
        }
        else
        {
            g_scanCount = (n > 0) ? n : 0;
            g_scanBusy  = false;
        }
        ESP_LOGI(TAG, "Scan found %d networks", g_scanCount);
    }

    void OnScanClicked(lv_event_t *)
    {
        if (g_scanBusy) return;
        g_scanBusy = true;
        g_scanCount = 0;
        PopulateScanList();

        g_scanTask.Init("wifi_scan", 4, 4096);
        g_scanTask.SetHandler(ScanTaskBody);
        if (!g_scanTask.Run())
        {
            ESP_LOGE(TAG, "Could not start the scan task");
            g_scanBusy = false;
            lv_obj_clean(g_wifiList);
            MakeEmptyCard(g_wifiList, "Scan failed to start");
        }
    }

    /// Persist the credentials, then nudge the radio at them.
    ///
    /// Saving alone would be correct — NetworkManager re-reads wifi.ssid and
    /// wifi.password at the start of every station round — but not enough to feel
    /// right: that round can be up to fifteen minutes away if the device happens
    /// to be sitting in an AP window, and a settings screen that appears to do
    /// nothing for fifteen minutes reads as broken. ConnectSta() makes the attempt
    /// now. The two do not fight: if this attempt succeeds, the cycle timer sees a
    /// connected station and returns early; if it fails, the next round picks up
    /// the very same credentials from NVS.
    void SaveAndConnect(const char *ssid, const char *password)
    {
        auto &settings = g_app->getStrux().getSettingsManager();
        settings.WriteString("wifi.ssid", ssid);
        settings.WriteString("wifi.password", password);
        settings.Save();

        ESP_LOGI(TAG, "Credentials saved for '%s'; attempting connection", ssid);
        g_app->getStrux().getNetworkManager().wifi().ConnectSta(ssid, password);
    }

    void OnKeyboardEvent(lv_event_t *e)
    {
        const lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_READY)
        {
            SaveAndConnect(g_pendingSsid, lv_textarea_get_text(g_passInput));
            lv_label_set_text(g_wifiCardState, "Saved - connecting...");
            lv_screen_load(g_wifiScreen);
        }
        else if (code == LV_EVENT_CANCEL)
        {
            lv_screen_load(g_wifiScreen);
        }
    }

    void BuildPasswordScreen()
    {
        g_passScreen = MakeScreen();
        MakeStatusBar(g_passScreen, false);
        MakeTitleBar(g_passScreen, "Password", &g_wifiScreen);

        g_passTitle = MakeLabel(g_passScreen, "Password", kTextDim, &lv_font_montserrat_14);
        lv_label_set_long_mode(g_passTitle, LV_LABEL_LONG_DOT);
        lv_obj_set_width(g_passTitle, ScreenW() - 2 * kPad);
        lv_obj_set_pos(g_passTitle, kPad, kHeadH + 12);

        g_passInput = lv_textarea_create(g_passScreen);
        lv_obj_add_style(g_passInput, &g_stInput, LV_PART_MAIN);
        lv_textarea_set_one_line(g_passInput, true);
        lv_textarea_set_password_mode(g_passInput, true);
        lv_textarea_set_placeholder_text(g_passInput, "Network password");
        lv_obj_set_width(g_passInput, ScreenW() - 2 * kPad);
        lv_obj_set_pos(g_passInput, kPad, kHeadH + 38);

        lv_obj_t *kb = lv_keyboard_create(g_passScreen);
        lv_keyboard_set_textarea(kb, g_passInput);
        lv_obj_set_size(kb, LV_PCT(100), 220);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        StyleKeyboard(kb);

        // The keyboard has its own tick and cross keys, so they are the commit and
        // the cancel — no extra buttons competing for the little room above it.
        lv_obj_add_event_cb(kb, OnKeyboardEvent, LV_EVENT_ALL, nullptr);
    }

    void BuildWifiScreen()
    {
        g_wifiScreen = MakeScreen();
        MakeStatusBar(g_wifiScreen, false);
        MakeTitleBar(g_wifiScreen, "Wi-Fi", &g_settingsScreen);

        // The state we are in, as a card rather than as a paragraph. It is the
        // answer to the question the screen is opened with.
        lv_obj_t *card = lv_obj_create(g_wifiScreen);
        lv_obj_add_style(card, &g_stCard, LV_PART_MAIN);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(card, ScreenW() - 2 * kPad, 72);
        lv_obj_set_pos(card, kPad, kHeadH + 6);

        g_wifiCardDot = MakeDot(card, kIdle);
        lv_obj_align(g_wifiCardDot, LV_ALIGN_LEFT_MID, 16, 0);

        g_wifiCardName = MakeLabel(card, "", kText, &lv_font_montserrat_16);
        lv_label_set_long_mode(g_wifiCardName, LV_LABEL_LONG_DOT);
        lv_obj_set_width(g_wifiCardName, ScreenW() - 2 * kPad - 56);
        lv_obj_align(g_wifiCardName, LV_ALIGN_TOP_LEFT, 40, 16);

        g_wifiCardState = MakeLabel(card, "", kTextDim, &lv_font_montserrat_14);
        lv_label_set_long_mode(g_wifiCardState, LV_LABEL_LONG_DOT);
        lv_obj_set_width(g_wifiCardState, ScreenW() - 2 * kPad - 56);
        lv_obj_align(g_wifiCardState, LV_ALIGN_TOP_LEFT, 40, 40);

        const int listY = kHeadH + 6 + 72 + 16;
        lv_obj_t *hdr = MakeLabel(g_wifiScreen, "AVAILABLE NETWORKS",
                                  kTextFaint, &lv_font_montserrat_14);
        lv_obj_set_style_text_letter_space(hdr, 1, LV_PART_MAIN);
        lv_obj_set_pos(hdr, kPad + 4, listY);

        g_wifiList = MakePanel(g_wifiScreen, listY + 24, ListBot() - (listY + 24));
        PopulateScanList();

        MakeCta(g_wifiScreen, LV_SYMBOL_REFRESH "   Scan for networks", OnScanClicked);
    }

    void OnWifiEntryClicked(lv_event_t *) { lv_screen_load(g_wifiScreen); }

    // ──────────────────────────────────────────────────────────
    // Pairing: the screen the six digits lead to.
    //
    // The state machine is BleHostManager's, not this file's — it is read, never
    // driven, and every branch below is one of its State values.
    // ──────────────────────────────────────────────────────────
    void OnPairBackClicked(lv_event_t *) { lv_screen_load(g_homeScreen); }

    void ShowPairingScreen(const BleHostManager::Slave &slave)
    {
        g_pendingSlave = slave;
        g_pairSettleTicks = 0;

        lv_label_set_text(g_pairName, slave.name[0] ? slave.name : "Unnamed slave");
        lv_label_set_text(g_pairState, "Connecting...");
        lv_obj_remove_flag(g_pairSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_pairGlyph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_pairBack, LV_OBJ_FLAG_HIDDEN);
        lv_screen_load(g_pairScreen);
    }

    void RefreshPairing(lv_timer_t *)
    {
        if (!g_pairState) return;
        if (lv_screen_active() != g_pairScreen) return;

        const BleHostManager::State s = g_app->getBleHost().GetState();

        switch (s)
        {
        case BleHostManager::State::Connecting:
            lv_label_set_text(g_pairState, "Connecting...");
            return;

        case BleHostManager::State::Scanning:
        case BleHostManager::State::Pairing:
            lv_label_set_text(g_pairState, "Exchanging keys...");
            return;

        case BleHostManager::State::Paired:
            lv_obj_add_flag(g_pairSpinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(g_pairGlyph, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_pairGlyph, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(g_pairGlyph, lv_color_hex(kOk), LV_PART_MAIN);
            lv_label_set_text(g_pairState, "Paired");
            lv_obj_set_style_text_color(g_pairState, lv_color_hex(kOk), LV_PART_MAIN);
            // Leave the result up long enough to be read, then go where the new
            // adapter now appears. Counted in ticks of this timer, not wall
            // clock: there is nothing else this screen is waiting for.
            if (++g_pairSettleTicks >= 4) lv_screen_load(g_homeScreen);
            return;

        case BleHostManager::State::Failed:
            lv_obj_add_flag(g_pairSpinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(g_pairGlyph, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_pairGlyph, LV_SYMBOL_CLOSE);
            lv_obj_set_style_text_color(g_pairGlyph, lv_color_hex(kDanger), LV_PART_MAIN);
            lv_label_set_text(g_pairState, "Failed - wrong code?");
            lv_obj_set_style_text_color(g_pairState, lv_color_hex(kDanger), LV_PART_MAIN);
            lv_obj_remove_flag(g_pairBack, LV_OBJ_FLAG_HIDDEN);
            return;

        case BleHostManager::State::Idle:
            // Idle here means the attempt ended without reaching either verdict —
            // the radio went back to resting. Say nothing confident about it.
            lv_obj_add_flag(g_pairSpinner, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(g_pairState, "Nothing in progress");
            lv_obj_remove_flag(g_pairBack, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }

    void BuildPairingScreen()
    {
        g_pairScreen = MakeScreen();
        MakeStatusBar(g_pairScreen, false);
        MakeTitleBar(g_pairScreen, "Pairing", &g_slavesScreen);

        const int cx = ScreenW() / 2;

        g_pairSpinner = lv_spinner_create(g_pairScreen);
        lv_obj_set_size(g_pairSpinner, 108, 108);
        lv_obj_set_pos(g_pairSpinner, cx - 54, 150);
        lv_obj_set_style_arc_color(g_pairSpinner, lv_color_hex(kSurface), LV_PART_MAIN);
        lv_obj_set_style_arc_width(g_pairSpinner, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_color(g_pairSpinner, lv_color_hex(kAccent), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(g_pairSpinner, 6, LV_PART_INDICATOR);

        // The verdict takes the spinner's place rather than sitting beside it:
        // one thing in the middle of the screen at a time.
        g_pairGlyph = MakeLabel(g_pairScreen, LV_SYMBOL_OK, kOk, &lv_font_montserrat_28);
        lv_obj_set_pos(g_pairGlyph, cx - 18, 188);
        lv_obj_add_flag(g_pairGlyph, LV_OBJ_FLAG_HIDDEN);

        g_pairName = MakeLabel(g_pairScreen, "", kText, &lv_font_montserrat_20);
        lv_obj_set_width(g_pairName, ScreenW() - 2 * kPad);
        lv_obj_set_style_text_align(g_pairName, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_label_set_long_mode(g_pairName, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(g_pairName, kPad, 300);

        g_pairState = MakeLabel(g_pairScreen, "", kTextDim, &lv_font_montserrat_16);
        lv_obj_set_width(g_pairState, ScreenW() - 2 * kPad);
        lv_obj_set_style_text_align(g_pairState, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(g_pairState, kPad, 334);

        g_pairBack = MakeCta(g_pairScreen, "Back", OnPairBackClicked);
        lv_obj_add_flag(g_pairBack, LV_OBJ_FLAG_HIDDEN);
    }

    // ──────────────────────────────────────────────────────────
    // Slaves: scan, pick one, type its code, pair.
    // ──────────────────────────────────────────────────────────
    const char *SlaveStateText(BleHostManager::State s)
    {
        switch (s)
        {
        case BleHostManager::State::Idle:       return "Idle";
        case BleHostManager::State::Scanning:   return "Scanning...";
        case BleHostManager::State::Connecting: return "Connecting...";
        case BleHostManager::State::Pairing:    return "Pairing...";
        case BleHostManager::State::Paired:     return "Paired";
        case BleHostManager::State::Failed:     return "Failed - wrong code?";
        }
        return "";
    }

    /// Ask for the six digits the slave is showing on its own display.
    void AskForCode(const BleHostManager::Slave &slave)
    {
        g_pendingSlave = slave;
        lv_label_set_text_fmt(g_codeTitle, "Enter the code shown on %s",
                              slave.name[0] ? slave.name : "the slave");
        lv_textarea_set_text(g_codeInput, "");
        lv_screen_load(g_codeScreen);
    }

    void OnSlavePicked(lv_event_t *e)
    {
        auto *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
        const int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
        if (idx < 0 || idx >= g_slaveViewCount) return;
        AskForCode(g_slaveView[idx]);
    }

    /// Rebuild the list from a snapshot. Called only when the contents actually
    /// changed — redrawing every tick would steal taps out from under a finger.
    void PopulateSlaveList()
    {
        if (!g_slavesList) return;
        lv_obj_clean(g_slavesList);

        if (g_slaveViewCount == 0)
        {
            MakeEmptyCard(g_slavesList, "Nothing on the air yet");
            return;
        }

        for (int i = 0; i < g_slaveViewCount; ++i)
        {
            const BleHostManager::Slave &s = g_slaveView[i];

            char value[16];
            snprintf(value, sizeof(value), "%d dBm", s.rssi);

            // Three states worth telling apart at a glance: ours, somebody
            // else's, and free to take.
            RowSpec spec{};
            spec.dot      = s.bonded ? kOk : (s.advBonded ? kWarn : kAccent);
            spec.name     = s.name[0] ? s.name : "Unnamed slave";
            spec.sub      = s.bonded    ? "Already yours"
                          : s.advBonded ? "Owned by another host"
                                        : "Available to pair";
            spec.subColor = s.bonded ? kOk : (s.advBonded ? kWarn : kTextDim);
            spec.value    = value;
            spec.chevron  = !s.bonded;

            lv_obj_t *row = MakeRow(g_slavesList, spec);
            if (s.bonded)
            {
                lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
                continue;
            }
            lv_obj_set_user_data(row, (void *)(intptr_t)i);
            lv_obj_add_event_cb(row, OnSlavePicked, LV_EVENT_CLICKED, nullptr);
        }
    }

    void RefreshSlaves(lv_timer_t *)
    {
        if (!g_slavesState) return;
        if (lv_screen_active() != g_slavesScreen) return;

        auto &ble = g_app->getBleHost();

        BleHostManager::Slave snapshot[BleHostManager::kMaxResults];
        const int n = ble.GetResults(snapshot, BleHostManager::kMaxResults);

        // Only touch the widget tree when something actually changed. Names and
        // flags arrive across several advertising packets, so the list grows and
        // fills in for a second or two after a scan starts.
        bool changed = (n != g_slaveViewCount);
        if (!changed)
            for (int i = 0; i < n && !changed; ++i)
                changed = memcmp(&snapshot[i], &g_slaveView[i],
                                 sizeof(BleHostManager::Slave)) != 0;

        if (changed)
        {
            memcpy(g_slaveView, snapshot, sizeof(BleHostManager::Slave) * n);
            g_slaveViewCount = n;
            PopulateSlaveList();
        }

        lv_label_set_text_fmt(g_slavesState, "%s  -  %d owned",
                              SlaveStateText(ble.GetState()), ble.BondedCount());
    }

    void OnSlaveScanClicked(lv_event_t *) { g_app->getBleHost().StartScan(6000); }

    void OnCodeKeyboardEvent(lv_event_t *e)
    {
        const lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_READY)
        {
            const char *typed = lv_textarea_get_text(g_codeInput);
            const uint32_t passkey = (uint32_t)strtoul(typed, nullptr, 10);
            g_app->getBleHost().BeginPairing(g_pendingSlave, passkey);
            ShowPairingScreen(g_pendingSlave);
        }
        else if (code == LV_EVENT_CANCEL)
        {
            lv_screen_load(g_slavesScreen);
        }
    }

    void BuildCodeScreen()
    {
        g_codeScreen = MakeScreen();
        MakeStatusBar(g_codeScreen, false);
        MakeTitleBar(g_codeScreen, "Pairing code", &g_slavesScreen);

        g_codeTitle = MakeLabel(g_codeScreen, "Pairing code", kTextDim,
                                &lv_font_montserrat_14);
        lv_label_set_long_mode(g_codeTitle, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(g_codeTitle, ScreenW() - 2 * kPad);
        lv_obj_set_style_text_align(g_codeTitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(g_codeTitle, kPad, kHeadH + 12);

        g_codeInput = lv_textarea_create(g_codeScreen);
        lv_obj_add_style(g_codeInput, &g_stInput, LV_PART_MAIN);
        lv_textarea_set_one_line(g_codeInput, true);
        lv_textarea_set_max_length(g_codeInput, 6);
        lv_textarea_set_placeholder_text(g_codeInput, "000000");
        lv_obj_set_style_text_align(g_codeInput, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(g_codeInput, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_letter_space(g_codeInput, 6, LV_PART_MAIN);
        lv_obj_set_width(g_codeInput, ScreenW() - 2 * kPad);
        lv_obj_set_pos(g_codeInput, kPad, kHeadH + 52);

        // A NUMBER keypad, not the full keyboard: the code is six digits, and
        // big keys matter more than letters nobody will type.
        lv_obj_t *kb = lv_keyboard_create(g_codeScreen);
        lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(kb, g_codeInput);
        lv_obj_set_size(kb, LV_PCT(100), 240);
        lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
        StyleKeyboard(kb);
        lv_obj_add_event_cb(kb, OnCodeKeyboardEvent, LV_EVENT_ALL, nullptr);
    }

    void BuildSlavesScreen()
    {
        g_slavesScreen = MakeScreen();
        MakeStatusBar(g_slavesScreen, false);
        MakeTitleBar(g_slavesScreen, "Select a slave", &g_settingsScreen);

        g_slavesState = MakeLabel(g_slavesScreen, "Idle", kTextFaint,
                                  &lv_font_montserrat_14);
        lv_obj_set_style_text_letter_space(g_slavesState, 1, LV_PART_MAIN);
        lv_obj_set_pos(g_slavesState, kPad + 4, kHeadH + 8);

        const int listY = kHeadH + 32;
        g_slavesList = MakePanel(g_slavesScreen, listY, ListBot() - listY);
        PopulateSlaveList();

        MakeCta(g_slavesScreen, LV_SYMBOL_REFRESH "   Scan for slaves", OnSlaveScanClicked);
    }

    void OnSlavesEntryClicked(lv_event_t *)
    {
        g_app->getBleHost().StartScan(6000);
        lv_screen_load(g_slavesScreen);
    }

    // ──────────────────────────────────────────────────────────
    // Settings — the menu, and nothing else. Every row here goes somewhere that
    // exists; a row that opens a screen drawing placeholder data would be worse
    // than its absence.
    // ──────────────────────────────────────────────────────────
    void BuildSettingsScreen()
    {
        g_settingsScreen = MakeScreen();
        MakeStatusBar(g_settingsScreen, false);
        MakeTitleBar(g_settingsScreen, "Settings", &g_homeScreen);

        lv_obj_t *panel = MakePanel(g_settingsScreen, kHeadH + 8,
                                    ScreenH() - (kHeadH + 8) - kPad);

        lv_obj_t *wifi = MakeMenuRow(panel, LV_SYMBOL_WIFI,
                                     "Wi-Fi", "Connect to your network");
        lv_obj_add_event_cb(wifi, OnWifiEntryClicked, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *slaves = MakeMenuRow(panel, LV_SYMBOL_BLUETOOTH,
                                       "Pair new slave", "Discover and pair devices");
        lv_obj_add_event_cb(slaves, OnSlavesEntryClicked, LV_EVENT_CLICKED, nullptr);
    }

    // ──────────────────────────────────────────────────────────
    // Home — the product. The adapters this host owns, and what else is around.
    // ──────────────────────────────────────────────────────────
    void OnHomeFoundPicked(lv_event_t *e)
    {
        auto *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
        const int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
        if (idx < 0 || idx >= g_homeFoundCount) return;
        // Tapping a found slave goes straight to the code pad — pairing is the
        // only thing you can do with one you do not own yet.
        AskForCode(g_homeFound[idx]);
    }

    void PopulateHomeList()
    {
        if (!g_homeList) return;
        lv_obj_clean(g_homeList);

        // ── Paired, first. These are the product: the adapters this host owns
        // and will eventually expose as COM ports. They stay listed whether or
        // not they are switched on, because a missing row would read as "lost"
        // when it only means "out of range".
        MakeSection(g_homeList, "PAIRED");
        if (g_homePairedCount == 0)
        {
            MakeEmptyCard(g_homeList, "None yet - pair one below");
        }
        else
        {
            for (int i = 0; i < g_homePairedCount; ++i)
            {
                const BleHostManager::PairedSlave &s = g_homePaired[i];

                char value[16] = {};
                if (s.inRange) snprintf(value, sizeof(value), "%d dBm", s.rssi);

                RowSpec spec{};
                spec.dot      = s.inRange ? kOk : kIdle;
                spec.name     = s.name[0] ? s.name : "Unnamed slave";
                spec.sub      = s.inRange ? "In range" : "Not seen";
                spec.subColor = s.inRange ? kOk : kTextDim;
                spec.value    = s.inRange ? value : "-";
                spec.chevron  = false;

                lv_obj_t *row = MakeRow(g_homeList, spec);
                // No handler yet. The switch that turns a slave into a COM port
                // belongs here, and goes in when there is a port to turn on.
                lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
            }
        }

        // ── Then whatever else is on the air and not ours.
        MakeSection(g_homeList, "FOUND");
        if (g_homeFoundCount == 0)
        {
            MakeEmptyCard(g_homeList, "Nothing new nearby");
            return;
        }
        for (int i = 0; i < g_homeFoundCount; ++i)
        {
            const BleHostManager::Slave &s = g_homeFound[i];

            char value[16];
            snprintf(value, sizeof(value), "%d dBm", s.rssi);

            RowSpec spec{};
            spec.dot      = s.advBonded ? kWarn : kAccent;
            spec.name     = s.name[0] ? s.name : "Unnamed slave";
            spec.sub      = s.advBonded ? "Owned by another host" : "Available to pair";
            spec.subColor = s.advBonded ? kWarn : kTextDim;
            spec.value    = value;
            spec.chevron  = true;

            lv_obj_t *row = MakeRow(g_homeList, spec);
            lv_obj_set_user_data(row, (void *)(intptr_t)i);
            lv_obj_add_event_cb(row, OnHomeFoundPicked, LV_EVENT_CLICKED, nullptr);
        }
    }

    void BuildHomeScreen()
    {
        g_homeScreen = MakeScreen();
        MakeStatusBar(g_homeScreen, true);

        // The wordmark, not the device name. The device name is configuration;
        // what someone looking at the panel wants confirmed is which product
        // they are holding. (It is on the Wi-Fi screen as the AP SSID, and over
        // the web UI, both of which are where a name is actually acted on.)
        lv_obj_t *mark = MakeLabel(g_homeScreen, "Comble", kAccent, &lv_font_montserrat_28);
        lv_obj_set_width(mark, ScreenW());
        lv_obj_set_style_text_align(mark, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(mark, 0, kStatusH + 6);

        lv_obj_t *tag = MakeLabel(g_homeScreen, "Wireless UART over BLE",
                                  kTextFaint, &lv_font_montserrat_14);
        lv_obj_set_width(tag, ScreenW());
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(tag, 0, kStatusH + 44);

        const int listY = kStatusH + 74;
        g_homeList = MakePanel(g_homeScreen, listY, ListBot() - listY);
        PopulateHomeList();

        MakeCta(g_homeScreen, LV_SYMBOL_PLUS "   Pair new slave", OnSlavesEntryClicked);
    }

    /// Keeps the home list current, and keeps a scan running while the home
    /// screen is the one being looked at.
    ///
    /// Scanning only while visible is deliberate. A permanent background scan
    /// would cost radio time that Wi-Fi shares an antenna with, for a list
    /// nobody is reading — and the home screen is exactly where "what is around
    /// me" has to be true without anyone pressing anything.
    void RefreshHome(lv_timer_t *)
    {
        if (!g_homeList) return;
        if (lv_screen_active() != g_homeScreen) return;

        auto &ble = g_app->getBleHost();

        if (ble.GetState() == BleHostManager::State::Idle)
            ble.StartScan(4000);

        BleHostManager::PairedSlave paired[BleHostManager::kMaxPaired];
        const int np = ble.GetPaired(paired, BleHostManager::kMaxPaired);

        BleHostManager::Slave seen[BleHostManager::kMaxResults];
        const int ns = ble.GetResults(seen, BleHostManager::kMaxResults);

        // Anything already paired is shown in the top section, so drop it here
        // rather than listing the same adapter twice under two headings.
        BleHostManager::Slave found[BleHostManager::kMaxResults];
        int nf = 0;
        for (int i = 0; i < ns; ++i)
        {
            if (seen[i].bonded) continue;
            found[nf++] = seen[i];
        }

        bool changed = (np != g_homePairedCount) || (nf != g_homeFoundCount);
        if (!changed)
            for (int i = 0; i < np && !changed; ++i)
                changed = memcmp(&paired[i], &g_homePaired[i],
                                 sizeof(BleHostManager::PairedSlave)) != 0;
        if (!changed)
            for (int i = 0; i < nf && !changed; ++i)
                changed = memcmp(&found[i], &g_homeFound[i],
                                 sizeof(BleHostManager::Slave)) != 0;

        if (!changed) return;

        memcpy(g_homePaired, paired, sizeof(BleHostManager::PairedSlave) * np);
        g_homePairedCount = np;
        memcpy(g_homeFound, found, sizeof(BleHostManager::Slave) * nf);
        g_homeFoundCount = nf;
        PopulateHomeList();
    }
}   // namespace

// ──────────────────────────────────────────────────────────────
// Commands. Both run on the TRANSPORT's task — the httpd task for a browser socket,
// the relay's own for the pipe — and never on the LVGL task, so every LVGL call below
// takes the port lock. What they deliberately do NOT do is hold it across the settle
// delay: the per-screen refresh timers that fill a screen with data run on the LVGL
// task, and holding the lock while waiting for them would starve exactly the work the
// wait is for.
// ──────────────────────────────────────────────────────────────

namespace
{
    /// Put a screen on the panel and give its refresh timers time to fill it in.
    /// RefreshSlaves and its siblings return early unless their own screen is the
    /// active one, so a frame captured the instant after a load shows the layout
    /// without the data — which is a screenshot of the wrong thing.
    void LoadAndSettle(lv_obj_t *screen, uint32_t settleMs)
    {
        if (lvgl_port_lock(0))
        {
            lv_screen_load(screen);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(settleMs));
    }

    /// The reply for a name that is not one of ours. Form was fine — a string arrived
    /// where a string was declared — so this is meaning, and meaning goes in the reply,
    /// where it can carry the list of names that would have worked.
    void ReplyUnknownScreen(CommandContext &ctx, const char *asked)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "unknown screen");
        resp.field("asked", asked);
        auto known = resp.array("screens");
        for (const auto &s : kNamedScreens) known.value(s.name);
    }
}

RequestError UiManager::Cmd_Goto(CommandContext &ctx)
{
    char name[16] = {};
    RETURN_IF_ERROR(ctx.readArgs(Required("screen", name)));

    lv_obj_t *target = ScreenByName(name);
    if (target == nullptr)
    {
        ReplyUnknownScreen(ctx, name);
        return RequestError::Ok;
    }

    LoadAndSettle(target, 0);

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("screen", name);
    return RequestError::Ok;
}

RequestError UiManager::Cmd_Screenshot(CommandContext &ctx)
{
    char     name[16] = {};
    uint32_t settleMs = 900;
    uint32_t scale    = 1;
    RETURN_IF_ERROR(ctx.readArgs(Optional("screen", name),
                                 Optional("settle", settleMs),
                                 Optional("scale",  scale)));

    // 1, 2 or 4. Nearest-neighbour, applied on the way OUT rather than to the render:
    // the snapshot is always full resolution, and every second or fourth pixel is what
    // reaches the wire. A live view in a browser wants the quarter-size frame far more
    // than it wants the detail — 300 KB a frame is most of a second of the radio.
    if (scale != 1 && scale != 2 && scale != 4)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "scale must be 1, 2 or 4");
        return RequestError::Ok;
    }

    // No screen named means "whatever is on the panel", which is what makes this
    // usable while somebody is driving the thing by hand.
    if (name[0] != 0)
    {
        lv_obj_t *target = ScreenByName(name);
        if (target == nullptr)
        {
            ReplyUnknownScreen(ctx, name);
            return RequestError::Ok;
        }
        LoadAndSettle(target, settleMs);
    }

    const uint32_t w      = static_cast<uint32_t>(ScreenW());
    const uint32_t h      = static_cast<uint32_t>(ScreenH());
    const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    const uint32_t bytes  = stride * h;

    // The frame buffer is ours, not LVGL's: lv_draw_buf_create() allocates from the
    // builtin lv_malloc pool, which is 64 KB against the 300 KB a full frame needs.
    // PSRAM has the room, and nothing in the hot path is affected — the DRAW buffers
    // stay in internal DMA RAM for the reason Init() gives below. A capture happens
    // once in a while, so rendering into slow memory costs nothing that matters.
    uint8_t *pixels = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pixels == nullptr)
        pixels = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_8BIT));
    if (pixels == nullptr)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "out of memory");
        resp.field("bytes", bytes);
        return RequestError::Ok;
    }

    bool taken = false;
    if (lvgl_port_lock(0))
    {
        lv_draw_buf_t buf;
        if (lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_RGB565, stride, pixels, bytes) == LV_RESULT_OK)
            taken = lv_snapshot_take_to_draw_buf(lv_screen_active(),
                                                 LV_COLOR_FORMAT_RGB565, &buf) == LV_RESULT_OK;
        lvgl_port_unlock();
    }

    if (!taken)
    {
        heap_caps_free(pixels);
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "snapshot failed");
        return RequestError::Ok;
    }

    const uint32_t outW = w / scale;
    const uint32_t outH = h / scale;
    const uint32_t outStride = outW * 2;

    // A header record, a newline, then the frame as raw bytes — `web read`'s shape and
    // for its reason: the reply is not one document, so the scope closes before the
    // divider. RGB565 because that is what the panel itself holds; expanding it into
    // something a PNG encoder likes is the caller's job and costs the device nothing.
    // The dimensions reported are the ones SENT, so a scaled frame needs no agreement
    // between the two ends about what the number means.
    {
        auto head = ctx.reply.object();
        head.field("ok", true);
        head.field("screen", ActiveScreenName());
        head.field("width", outW);
        head.field("height", outH);
        head.field("stride", outStride);   // bytes per row, of what is on the wire
        head.field("format", "rgb565");    // little-endian uint16 per pixel
        head.field("scale", scale);
        head.field("bytes", outStride * outH);
    }
    ctx.out.write("\n", 1);

    // A row at a time, which is at most 640 bytes — inside the transport's window for
    // the reason `web read` chunks at all, and the natural unit here because a scaled
    // row is gathered from a source row rather than copied from it. The snapshot's own
    // stride is read from the buffer rather than assumed to be width*2: LVGL is free
    // to pad it.
    uint16_t row[320];
    for (uint32_t y = 0; y < outH; ++y)
    {
        const uint16_t *src = reinterpret_cast<const uint16_t *>(pixels + (y * scale) * stride);
        if (scale == 1)
        {
            ctx.out.write(src, outStride);
            continue;
        }
        for (uint32_t x = 0; x < outW; ++x)
            row[x] = src[x * scale];
        ctx.out.write(row, outStride);
    }

    heap_caps_free(pixels);
    return RequestError::Ok;
}

RequestError UiManager::Cmd_Touch(CommandContext &ctx)
{
    uint32_t x = 0;
    uint32_t y = 0;
    char     action[10] = "tap";
    RETURN_IF_ERROR(ctx.readArgs(Required("x", x),
                                 Required("y", y),
                                 Optional("action", action)));

    const uint32_t w = static_cast<uint32_t>(ScreenW());
    const uint32_t h = static_cast<uint32_t>(ScreenH());
    if (x >= w || y >= h)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "outside the panel");
        resp.field("width", w);
        resp.field("height", h);
        return RequestError::Ok;
    }

    const bool tap     = strcmp(action, "tap") == 0;
    const bool press   = strcmp(action, "press") == 0;
    const bool release = strcmp(action, "release") == 0;
    const bool move    = strcmp(action, "move") == 0;
    if (!tap && !press && !release && !move)
    {
        auto resp = ctx.reply.object();
        resp.field("ok", false);
        resp.field("error", "action must be tap, press, release or move");
        return RequestError::Ok;
    }

    if (lvgl_port_lock(0))
    {
        g_remoteX = static_cast<int32_t>(x);
        g_remoteY = static_cast<int32_t>(y);
        if (!release) g_remotePressed = !move;
        else          g_remotePressed = false;
        lvgl_port_unlock();
    }

    // A tap is a press somebody let go of, and the letting go cannot be in the same
    // breath: LVGL polls its input devices on its own task, about every 30 ms, and a
    // press that is gone before the next poll never happened. Held for kTapHoldMs, it
    // is read several times — press, then release — which is exactly what a finger
    // looks like from LVGL's side.
    if (tap)
    {
        constexpr uint32_t kTapHoldMs = 120;
        vTaskDelay(pdMS_TO_TICKS(kTapHoldMs));
        if (lvgl_port_lock(0))
        {
            g_remotePressed = false;
            lvgl_port_unlock();
        }
        // Long enough for the release to be read before the reply says it happened.
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    auto resp = ctx.reply.object();
    resp.field("ok", true);
    resp.field("x", x);
    resp.field("y", y);
    resp.field("action", action);
    resp.field("screen", ActiveScreenName());
    return RequestError::Ok;
}

void UiManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init)
    {
        ESP_LOGW(TAG, "Already initialized or initializing");
        return;
    }

    if (!app_.getBoard().HasDisplay())
    {
        ESP_LOGW(TAG, "No display — UI not started");
        init.SetReady();
        return;
    }

    g_app = &app_;
    Display &lcd = app_.getBoard().GetDisplay();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.task_affinity = 1;   // core 1; leave core 0 to the radio
    if (lvgl_port_init(&port_cfg) != ESP_OK)
    {
        ESP_LOGE(TAG, "lvgl_port_init failed");
        return;
    }

    // Command-driven panel: the port needs the panel handle to push pixels AND
    // the IO handle, which is where it hooks flush-done. Two partial draw buffers
    // in internal DMA RAM let LVGL render the next band while the i80 DMA is
    // still sending the previous one.
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = lcd.io();
    disp_cfg.panel_handle = lcd.panel();
    disp_cfg.buffer_size = Display::Width() * BoardConfig::LCD_DRAW_BUFFER_LINES;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = Display::Width();
    disp_cfg.vres = Display::Height();
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.buff_spiram = false;
    disp_cfg.flags.swap_bytes = BoardConfig::LCD_SWAP_BYTES;
    disp_cfg.flags.full_refresh = false;
    // Rotation lives here and nowhere else — see Display.h.
    disp_cfg.rotation.swap_xy = BoardConfig::LCD_SWAP_XY;
    disp_cfg.rotation.mirror_x = BoardConfig::LCD_MIRROR_X;
    disp_cfg.rotation.mirror_y = BoardConfig::LCD_MIRROR_Y;

    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == nullptr)
    {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    if (app_.getBoard().HasTouch())
    {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = disp;
        touch_cfg.handle = app_.getBoard().GetTouch().handle();
        if (lvgl_port_add_touch(&touch_cfg) == nullptr)
            ESP_LOGW(TAG, "lvgl_port_add_touch failed — UI will be read-only");
    }

    // The wire's finger. Created before the screens so that the very first `ui touch`
    // has somewhere to land, and given no cursor image — a pointer nobody can see is
    // the point, since the person pressing it is looking at a screenshot.
    g_remoteIndev = lv_indev_create();
    lv_indev_set_type(g_remoteIndev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_remoteIndev, RemotePointerRead);
    lv_indev_set_display(g_remoteIndev, disp);

    // Build under the lock: the LVGL task is already running by now.
    if (lvgl_port_lock(0))
    {
        InitStyles();

        // Order matters only in that every screen's back chevron is given the
        // ADDRESS of its target, so a screen may be built before the one it
        // returns to exists — see MakeTitleBar.
        BuildHomeScreen();
        BuildSettingsScreen();
        BuildWifiScreen();
        BuildPasswordScreen();
        BuildSlavesScreen();
        BuildCodeScreen();
        BuildPairingScreen();
        lv_screen_load(g_homeScreen);

        RefreshStatus(nullptr);
        RefreshWifiCard(nullptr);
        lv_timer_create(RefreshStatus, 1000, nullptr);
        lv_timer_create(RefreshWifiCard, 1000, nullptr);
        // Faster than the network status: a scan fills in over a second or two,
        // and a list that lags behind the radio feels broken.
        lv_timer_create(RefreshSlaves, 500, nullptr);
        lv_timer_create(RefreshPairing, 400, nullptr);
        lv_timer_create(RefreshHome, 700, nullptr);
        lvgl_port_unlock();
    }

    // Only reached on a board that HAS a panel, which is exactly where these two
    // commands mean anything — so a display-less board never lists them.
    app_.getStrux().getCommandManager().Register(this, commands_);

    lcd.Backlight(true);   // first frame is up — light the panel

    init.SetReady();
    ESP_LOGI(TAG, "Initialized (%dx%d)", Display::Width(), Display::Height());
}

#else   // Board without a panel — the class exists and does nothing.

void UiManager::Init()
{
    auto init = initState_.TryBeginInit();
    if (!init) return;
    ESP_LOGD(TAG, "This board has no display; no UI to build");
    init.SetReady();
}

// Never registered here, so never reachable — but the command table in the header
// takes their addresses, so they have to link. They answer rather than abort, in case a
// future board ever registers them by mistake.
RequestError UiManager::Cmd_Goto(CommandContext &ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());
    auto resp = ctx.reply.object();
    resp.field("ok", false);
    resp.field("error", "no display on this board");
    return RequestError::Ok;
}

RequestError UiManager::Cmd_Screenshot(CommandContext &ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());
    auto resp = ctx.reply.object();
    resp.field("ok", false);
    resp.field("error", "no display on this board");
    return RequestError::Ok;
}

RequestError UiManager::Cmd_Touch(CommandContext &ctx)
{
    RETURN_IF_ERROR(ctx.readArgs());
    auto resp = ctx.reply.object();
    resp.field("ok", false);
    resp.field("error", "no display on this board");
    return RequestError::Ok;
}

#endif
