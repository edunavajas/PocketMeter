#include "ui.h"
#include "splash.h"
#include "codex_splash.h"
#include "web_server.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "codex_icon.h"
#include "display_cfg.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_mono_32);

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Layout constants for 480x480 (scaled for 2.16" high-DPI + rounded corners) ----
#define SCR_W         480
#define SCR_H         480
#define MARGIN        20    // wider margin for rounded display corners
#define TITLE_Y       30
#define CONTENT_Y     100
#define CONTENT_W     (SCR_W - 2 * MARGIN)   // 440

// ---- Usage screen widgets ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* lbl_anim;

// ---- Codex screen widgets ----
static lv_obj_t* codex_container;
static lv_obj_t* lbl_codex_session_pct;
static lv_obj_t* lbl_codex_session_label;
static lv_obj_t* bar_codex_session;
static lv_obj_t* lbl_codex_session_reset;
static lv_obj_t* lbl_codex_weekly_pct;
static lv_obj_t* lbl_codex_weekly_label;
static lv_obj_t* bar_codex_weekly;
static lv_obj_t* lbl_codex_weekly_reset;
static lv_obj_t* lbl_codex_credits;
static lv_obj_t* codex_icon_img;
static lv_image_dsc_t codex_icon_dsc;
static bool codex_available = false;

// ---- Network screen widgets ----
static lv_obj_t* net_container;
static lv_obj_t* lbl_net_status;
static lv_obj_t* lbl_net_ssid;
static lv_obj_t* lbl_net_ip;
static lv_obj_t* lbl_net_rssi;

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

// Per-frame hold time. Modeled on Claude Code's spinner (Cavalry triangle
// oscillator, range 0..5, period 5s) — turn-around frames (0 and 5) appear
// once per cycle, middle frames twice, so 0/5 read as held longer.
static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    // Bubble click events up to the screen / usage_container so a tap anywhere
    // on the panel fires the global click handler.
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

// RGB565A8: planar — w*h RGB565 pixels followed by w*h alpha bytes.
// Stride is RGB565-only (w*2); LVGL infers alpha plane location from header.
static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}

// ---- Battery icon initialization ----
static void init_battery_icons(void) {
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen (480x480) ========

#define PANEL_H     150
#define PANEL_GAP   16

// One Session/Weekly panel: big % label, pill on the right, bar, reset label.
// Pill y=1: symmetric inside the panel — panel-outer-top → pill-top equals
// pill-bottom → bar-top (pill height 42 + panel pad_top 12 + bar y=56).
static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, MARGIN, y, CONTENT_W, PANEL_H);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, 56, CONTENT_W - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, 94);
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, SCR_W, SCR_H);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    make_usage_panel(usage_container, CONTENT_Y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);
    make_usage_panel(usage_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);

    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);
}

// ======== Codex Screen (480x480) ========

static void init_codex_screen(lv_obj_t* scr) {
    codex_container = lv_obj_create(scr);
    lv_obj_set_size(codex_container, SCR_W, SCR_H);
    lv_obj_set_pos(codex_container, 0, 0);
    lv_obj_set_style_bg_opa(codex_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(codex_container, 0, 0);
    lv_obj_set_style_pad_all(codex_container, 0, 0);
    lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(codex_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Title "Codex" in teal
    lv_obj_t* lbl_title = lv_label_create(codex_container);
    lv_label_set_text(lbl_title, "Codex");
    lv_obj_set_style_text_font(lbl_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_title, THEME_CODEX, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Session/Weekly panels (same layout as usage screen)
    make_usage_panel(codex_container, CONTENT_Y, "Current",
                     &lbl_codex_session_pct, &lbl_codex_session_label,
                     &bar_codex_session, &lbl_codex_session_reset);
    make_usage_panel(codex_container, CONTENT_Y + PANEL_H + PANEL_GAP, "Weekly",
                     &lbl_codex_weekly_pct, &lbl_codex_weekly_label,
                     &bar_codex_weekly, &lbl_codex_weekly_reset);

    // Override bar indicator color to Codex teal
    lv_obj_set_style_bg_color(bar_codex_session, THEME_CODEX, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar_codex_weekly,  THEME_CODEX, LV_PART_INDICATOR);

    // Credits label at bottom
    lbl_codex_credits = lv_label_create(codex_container);
    lv_label_set_text(lbl_codex_credits, "");
    lv_obj_set_style_text_font(lbl_codex_credits, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_codex_credits, THEME_CODEX, 0);
    lv_obj_align(lbl_codex_credits, LV_ALIGN_BOTTOM_MID, 0, -15);

    // Cloud icon (top-left, same position as logo — logo is hidden on SCREEN_CODEX)
    init_icon_dsc_rgb565a8(&codex_icon_dsc, CODEX_ICON_W, CODEX_ICON_H, codex_icon_data);
    codex_icon_img = lv_image_create(scr);
    lv_image_set_src(codex_icon_img, &codex_icon_dsc);
    lv_obj_set_pos(codex_icon_img, MARGIN, TITLE_Y - 10);
    lv_obj_add_flag(codex_icon_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Network Screen (480x480) ========

static void init_network_screen(lv_obj_t* scr) {
    net_container = lv_obj_create(scr);
    lv_obj_set_size(net_container, SCR_W, SCR_H);
    lv_obj_set_pos(net_container, 0, 0);
    lv_obj_set_style_bg_opa(net_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(net_container, 0, 0);
    lv_obj_set_style_pad_all(net_container, 0, 0);
    lv_obj_clear_flag(net_container, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t* lbl_net_title = lv_label_create(net_container);
    lv_label_set_text(lbl_net_title, "Network");
    lv_obj_set_style_text_font(lbl_net_title, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(lbl_net_title, COL_TEXT, 0);
    lv_obj_align(lbl_net_title, LV_ALIGN_TOP_MID, 16, TITLE_Y);

    // Info panel
    lv_obj_t* p_info = make_panel(net_container, MARGIN, CONTENT_Y, CONTENT_W, 200);

    // WiFi label
    lv_obj_t* wifi_icon = lv_label_create(p_info);
    lv_label_set_text(wifi_icon, "WiFi");
    lv_obj_set_style_text_font(wifi_icon, &font_styrene_28, 0);
    lv_obj_set_style_text_color(wifi_icon, COL_DIM, 0);
    lv_obj_set_pos(wifi_icon, 0, 4);

    lbl_net_status = lv_label_create(p_info);
    lv_label_set_text(lbl_net_status, "Disconnected");
    lv_obj_set_style_text_font(lbl_net_status, &font_styrene_48, 0);
    lv_obj_set_style_text_color(lbl_net_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_net_status, 56, 2);

    lbl_net_ssid = lv_label_create(p_info);
    lv_label_set_text(lbl_net_ssid, "SSID: ---");
    lv_obj_set_style_text_font(lbl_net_ssid, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_net_ssid, COL_DIM, 0);
    lv_obj_set_pos(lbl_net_ssid, 0, 64);

    lbl_net_ip = lv_label_create(p_info);
    lv_label_set_text(lbl_net_ip, "IP: ---");
    lv_obj_set_style_text_font(lbl_net_ip, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_net_ip, COL_DIM, 0);
    lv_obj_set_pos(lbl_net_ip, 0, 100);

    lbl_net_rssi = lv_label_create(p_info);
    lv_label_set_text(lbl_net_rssi, "RSSI: --- dBm");
    lv_obj_set_style_text_font(lbl_net_rssi, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl_net_rssi, COL_DIM, 0);
    lv_obj_set_pos(lbl_net_rssi, 0, 136);

    // Attribution
    lv_obj_t* lbl_credit = lv_label_create(net_container);
    lv_label_set_text(lbl_credit, "Built by @hermannbjorgvin");
    lv_obj_set_style_text_font(lbl_credit, &font_styrene_24, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(net_container);
    lv_label_set_text(lbl_credit2, "Clawd animation by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, &font_styrene_20, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Start hidden
    lv_obj_add_flag(net_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // Logo (shared, always visible, on top of all containers)
    // Logo is RGB565A8 (planar: w*h RGB565 then w*h alpha) so it composites
    // cleanly against whatever bg is behind it.
    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    // Initialize battery icon descriptors
    init_battery_icons();

    init_usage_screen(scr);
    init_codex_screen(scr);
    init_network_screen(scr);
    splash_init(scr);

    codex_splash_init(scr);

    // Tap on either splash dismisses it
    if (splash_get_root())
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    if (codex_splash_get_root())
        lv_obj_add_event_cb(codex_splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);

    // Logo on top of all containers (inset for rounded corners)
    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, MARGIN, TITLE_Y - 10);

    // Battery indicator on top of all containers (upper-right, inset)
    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, SCR_W - 48 - MARGIN, TITLE_Y);
}

void ui_update(const UsageData* data) {
    if (!data->valid || data->provider_count == 0) return;

    // Claude usage screen (provider 0)
    const ProviderData* pd = &data->providers[0];
    int s_pct = (int)(pd->session_pct + 0.5f);

    lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(pd->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(pd->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_session_reset, buf);

    int w_pct = (int)(pd->weekly_pct + 0.5f);
    lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_weekly, pct_color(pd->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(pd->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(lbl_weekly_reset, buf);

    // Codex screen (provider 1, if present and ok)
    if (data->provider_count > 1) {
        const ProviderData* cx = &data->providers[1];
        if (cx->ok) {
            int cs_pct = (int)(cx->session_pct + 0.5f);
            lv_label_set_text_fmt(lbl_codex_session_pct, "%d%%", cs_pct);
            lv_bar_set_value(bar_codex_session, cs_pct, LV_ANIM_ON);
            lv_obj_set_style_bg_color(bar_codex_session, pct_color(cx->session_pct), LV_PART_INDICATOR);
            format_reset_time(cx->session_reset_mins, buf, sizeof(buf));
            lv_label_set_text(lbl_codex_session_reset, buf);

            int cw_pct = (int)(cx->weekly_pct + 0.5f);
            lv_label_set_text_fmt(lbl_codex_weekly_pct, "%d%%", cw_pct);
            lv_bar_set_value(bar_codex_weekly, cw_pct, LV_ANIM_ON);
            lv_obj_set_style_bg_color(bar_codex_weekly, pct_color(cx->weekly_pct), LV_PART_INDICATOR);
            format_reset_time(cx->weekly_reset_mins, buf, sizeof(buf));
            lv_label_set_text(lbl_codex_weekly_reset, buf);

            if (cx->has_credits) {
                static char credits_buf[32];
                snprintf(credits_buf, sizeof(credits_buf), "$%.2f credits",
                         (double)cx->credits_balance);
                lv_label_set_text(lbl_codex_credits, credits_buf);
            } else {
                lv_label_set_text(lbl_codex_credits, cx->plan_type[0] ? cx->plan_type : "");
            }
        }
    }
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
                 spinner_frames[anim_spinner_idx],
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
// Hide the battery indicator on the splash screen — the icon is visually
// noisy over the pixel-art creature animations.
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    bool splash_mode = (current_screen == SCREEN_SPLASH || current_screen == SCREEN_CODEX_SPLASH);
    if (splash_mode) lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else             lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static bool ui_claude_enabled(void) {
    return web_server_claude_visible();
}

static bool ui_codex_enabled(void) {
    return web_server_codex_visible() && codex_available;
}

static screen_t preferred_provider_usage_screen(void) {
    if (web_server_selected_provider() == WEB_PROVIDER_CODEX) {
        if (ui_codex_enabled())  return SCREEN_CODEX;
        if (ui_claude_enabled()) return SCREEN_USAGE;
    } else {
        if (ui_claude_enabled()) return SCREEN_USAGE;
        if (ui_codex_enabled())  return SCREEN_CODEX;
    }
    return SCREEN_NETWORK;
}

static screen_t fallback_for_hidden_screen(screen_t screen) {
    switch (screen) {
    case SCREEN_SPLASH:
    case SCREEN_USAGE:
    case SCREEN_CODEX_SPLASH:
    case SCREEN_CODEX:
        return preferred_provider_usage_screen();
    case SCREEN_NETWORK:
    default:
        return SCREEN_NETWORK;
    }
}

static bool screen_is_visible(screen_t screen) {
    switch (screen) {
    case SCREEN_SPLASH:
    case SCREEN_USAGE:
        return ui_claude_enabled();
    case SCREEN_CODEX_SPLASH:
    case SCREEN_CODEX:
        return ui_codex_enabled();
    case SCREEN_NETWORK:
        return true;
    default:
        return false;
    }
}

// LVGL handles click debouncing internally. Screen-level handler fires when
// no child consumed the event (children only consume if they have their own
// event callback, e.g. the Reset Bluetooth zone). On BT screen we skip the
// splash toggle so only the reset zone is interactive there.
static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (ui_get_current_screen() == SCREEN_NETWORK) return;
    ui_toggle_splash();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(net_container, LV_OBJ_FLAG_HIDDEN);
    if (codex_icon_img) lv_obj_add_flag(codex_icon_img, LV_OBJ_FLAG_HIDDEN);
    splash_hide();
    codex_splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:
        splash_show();
        break;
    case SCREEN_USAGE:
        lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_CODEX:
        lv_obj_clear_flag(codex_container, LV_OBJ_FLAG_HIDDEN);
        if (codex_icon_img) lv_obj_clear_flag(codex_icon_img, LV_OBJ_FLAG_HIDDEN);
        break;
    case SCREEN_CODEX_SPLASH:
        codex_splash_show();
        break;
    case SCREEN_NETWORK:
        lv_obj_clear_flag(net_container, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }

    // Logo: hidden on splash screens and Codex screen (cloud icon takes its place)
    if (logo_img) {
        bool hide_logo = (screen == SCREEN_SPLASH || screen == SCREEN_CODEX ||
                          screen == SCREEN_CODEX_SPLASH);
        if (hide_logo) lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    bool is_splash = (screen == SCREEN_SPLASH || screen == SCREEN_CODEX_SPLASH);
    if (!is_splash) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_cycle_screen(void) {
    bool show_claude = ui_claude_enabled();
    bool show_codex  = ui_codex_enabled();

    screen_t next;
    if (current_screen == SCREEN_USAGE) {
        next = show_codex ? SCREEN_CODEX : SCREEN_NETWORK;
    } else if (current_screen == SCREEN_CODEX) {
        next = SCREEN_NETWORK;
    } else {
        next = preferred_provider_usage_screen();
    }
    ui_show_screen(next);
}

void ui_set_codex_available(bool available) {
    bool changed = (codex_available != available);
    codex_available = available;
    if (changed) {
        ui_reconcile_provider_visibility(true);
    }
}

void ui_reconcile_provider_visibility(bool prefer_primary_provider) {
    screen_t target = current_screen;

    if (prefer_primary_provider) {
        target = preferred_provider_usage_screen();
    } else if (!screen_is_visible(current_screen)) {
        target = fallback_for_hidden_screen(current_screen);
    } else if (current_screen == SCREEN_NETWORK && preferred_provider_usage_screen() != SCREEN_NETWORK) {
        target = preferred_provider_usage_screen();
    }

    if (target != current_screen) {
        ui_show_screen(target);
    }
}

void ui_toggle_splash(void) {
    bool claude_on = ui_claude_enabled();
    bool codex_on  = ui_codex_enabled();

    if (current_screen == SCREEN_SPLASH) {
        // Claude splash → Usage Claude
        if (claude_on) ui_show_screen(SCREEN_USAGE);
        else if (codex_on) ui_show_screen(SCREEN_CODEX_SPLASH);
    } else if (current_screen == SCREEN_USAGE) {
        // Usage Claude → Codex splash (or back to Claude splash)
        if (codex_on) ui_show_screen(SCREEN_CODEX_SPLASH);
        else if (claude_on) ui_show_screen(SCREEN_SPLASH);
    } else if (current_screen == SCREEN_CODEX_SPLASH) {
        // Codex splash → Usage Codex
        if (codex_on) ui_show_screen(SCREEN_CODEX);
        else if (claude_on) ui_show_screen(SCREEN_SPLASH);
    } else if (current_screen == SCREEN_CODEX) {
        // Usage Codex → Claude splash (or back to Codex splash)
        if (claude_on) ui_show_screen(SCREEN_SPLASH);
        else if (codex_on) ui_show_screen(SCREEN_CODEX_SPLASH);
    } else {
        // For other screens (NETWORK, etc.), go to the preferred provider's splash
        screen_t preferred = preferred_provider_usage_screen();
        if (preferred == SCREEN_CODEX && codex_on) ui_show_screen(SCREEN_CODEX_SPLASH);
        else if (preferred == SCREEN_USAGE && claude_on) ui_show_screen(SCREEN_SPLASH);
    }
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_network_status(bool connected, const char* ssid, const char* ip, int rssi) {
    if (connected) {
        lv_label_set_text(lbl_net_status, "Connected");
        lv_obj_set_style_text_color(lbl_net_status, COL_GREEN, 0);
    } else {
        lv_label_set_text(lbl_net_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_net_status, COL_RED, 0);
    }

    if (ssid) {
        static char sbuf[48];
        snprintf(sbuf, sizeof(sbuf), "SSID: %s", ssid);
        lv_label_set_text(lbl_net_ssid, sbuf);
    }
    if (ip) {
        static char ibuf[48];
        snprintf(ibuf, sizeof(ibuf), "IP: %s", ip);
        lv_label_set_text(lbl_net_ip, ibuf);
    }
    static char rbuf[32];
    snprintf(rbuf, sizeof(rbuf), "RSSI: %d dBm", rssi);
    lv_label_set_text(lbl_net_rssi, rbuf);
}

void ui_update_battery(int percent, bool charging) {
    int idx;
    if (charging) {
        idx = 4;  // charging icon
    } else if (percent < 0) {
        idx = 0;  // no battery / unknown
    } else if (percent <= 10) {
        idx = 0;  // empty
    } else if (percent <= 35) {
        idx = 1;  // low
    } else if (percent <= 75) {
        idx = 2;  // medium
    } else {
        idx = 3;  // full
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}
