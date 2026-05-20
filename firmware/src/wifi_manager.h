#pragma once
#include <Arduino.h>

void wifi_init(void);
void wifi_check_connection(void);
bool wifi_is_connected(void);
const char* wifi_get_ip(void);
int wifi_get_rssi(void);
const char* wifi_get_ssid(void);
