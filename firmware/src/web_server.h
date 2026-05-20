#pragma once
#include <Arduino.h>
#include "data.h"

enum web_provider_t {
    WEB_PROVIDER_CLAUDE = 0,
    WEB_PROVIDER_CODEX = 1,
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
