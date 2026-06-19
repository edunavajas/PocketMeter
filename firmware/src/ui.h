#pragma once
#include "data.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,         // Claude usage
    SCREEN_CODEX,         // Codex usage (only shown when Codex is configured)
    SCREEN_CODEX_SPLASH,  // Codex full-screen cloud animation
    SCREEN_PROVIDER,      // Generic provider screen (Gemini, Copilot, etc.)
    SCREEN_NETWORK,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_network_status(bool connected, const char* ssid, const char* ip, int rssi);
void ui_update_battery(int percent, bool charging);
void ui_set_codex_available(bool available);
void ui_reconcile_provider_visibility(bool prefer_primary_provider);
// Set the data shown on SCREEN_PROVIDER. Pass nullptr to hide the screen.
void ui_set_generic_provider(const ProviderData* pd);
