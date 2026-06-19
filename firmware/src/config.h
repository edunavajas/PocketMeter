#pragma once

// WiFi credentials - HARDCODED (editar antes de compilar)
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
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
