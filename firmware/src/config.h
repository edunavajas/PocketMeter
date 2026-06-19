#pragma once

// WiFi credentials are read from wifi_credentials.h (ignored by git).
// Copy wifi_credentials.h.example to wifi_credentials.h and fill in your values.
#if __has_include("wifi_credentials.h")
  #include "wifi_credentials.h"
#else
  #define WIFI_SSID ""
  #define WIFI_PASSWORD ""
#endif

#define WIFI_HOSTNAME "pocketmeter"
#define WIFI_TIMEOUT_MS 10000

// Web server
#define WEB_SERVER_PORT 80

// Data endpoint
#define DATA_ENDPOINT "/api/usage"
#define STATUS_ENDPOINT "/api/status"
#define CONFIG_ENDPOINT "/api/config"
#define PETDEX_SEARCH_ENDPOINT "/api/petdex/search"

// Petdex proxy
#define PETDEX_SEARCH_URL "https://petdex.crafter.run/api/pets/search"
#define PETDEX_CONNECT_TIMEOUT_MS 15000
#define PETDEX_TIMEOUT_MS 20000
