#pragma once
#include <Arduino.h>
#include "data.h"

enum web_provider_t {
    WEB_PROVIDER_CLAUDE = 0,
    WEB_PROVIDER_CODEX = 1,
};

struct WebPetConfig {
    bool assigned;
    char id[48];
    char slug[64];
    char display_name[64];
    char spritesheet_path[208];
    char pet_json_path[208];
    char dominant_color[16];
    char preview_image_path[208];
};

void web_server_init(void);
void web_server_handle(void);
bool web_server_has_data(void);
const char* web_server_get_data(void);
void web_server_clear_data(void);
void web_server_set_last_data(const UsageData* data);

// Display visibility toggles (set from /api/config, read by ui.cpp)
bool web_server_claude_visible(void);
bool web_server_codex_visible(void);
web_provider_t web_server_selected_provider(void);
const WebPetConfig* web_server_pet_config(web_provider_t provider);

// Extended multi-provider API
// Returns true if provider `name` should be shown (defaults to true for any
// provider that hasn't been explicitly hidden via the web config).
bool        web_server_provider_visible(const char* name);
// Returns the string name of the selected provider ("claude", "codex",
// "gemini", "copilot", …). Never returns NULL.
const char* web_server_selected_provider_name(void);

// Returns the pinned splash animation name, or "" if in auto (rate-based) mode.
const char* web_server_splash_pin(void);
// Clear the pinned splash animation (e.g. when the pinned anim no longer exists).
void web_server_clear_splash_pin(void);
