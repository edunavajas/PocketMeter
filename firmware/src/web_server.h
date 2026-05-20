#pragma once
#include <Arduino.h>
#include "data.h"

void web_server_init(void);
void web_server_handle(void);
bool web_server_has_data(void);
const char* web_server_get_data(void);
void web_server_clear_data(void);
void web_server_set_last_data(const UsageData* data);
