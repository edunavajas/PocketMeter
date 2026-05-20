#include "wifi_manager.h"
#include "config.h"
#include <WiFi.h>

static bool connected = false;
static char ip_str[16] = "0.0.0.0";
static int last_rssi = 0;

void wifi_init(void) {
    Serial.printf("WiFi: connecting to %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        connected = true;
        snprintf(ip_str, sizeof(ip_str), "%s", WiFi.localIP().toString().c_str());
        last_rssi = WiFi.RSSI();
        Serial.printf("WiFi: connected, IP=%s, RSSI=%d dBm\n", ip_str, last_rssi);
    } else {
        connected = false;
        Serial.println("WiFi: connection failed, will retry in loop");
    }
}

void wifi_check_connection(void) {
    static unsigned long last_check = 0;
    unsigned long now = millis();
    if (now - last_check < 5000) return;
    last_check = now;

    if (WiFi.status() != WL_CONNECTED) {
        if (connected) {
            connected = false;
            Serial.println("WiFi: disconnected, reconnecting...");
        }
        WiFi.reconnect();
        // Wait a bit for reconnect
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
            delay(100);
        }
        if (WiFi.status() == WL_CONNECTED) {
            connected = true;
            snprintf(ip_str, sizeof(ip_str), "%s", WiFi.localIP().toString().c_str());
            last_rssi = WiFi.RSSI();
            Serial.printf("WiFi: reconnected, IP=%s, RSSI=%d dBm\n", ip_str, last_rssi);
        }
    } else {
        if (!connected) {
            connected = true;
            snprintf(ip_str, sizeof(ip_str), "%s", WiFi.localIP().toString().c_str());
            last_rssi = WiFi.RSSI();
            Serial.printf("WiFi: connected, IP=%s, RSSI=%d dBm\n", ip_str, last_rssi);
        }
        last_rssi = WiFi.RSSI();
    }
}

bool wifi_is_connected(void) {
    return WiFi.status() == WL_CONNECTED;
}

const char* wifi_get_ip(void) {
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(ip_str, sizeof(ip_str), "%s", WiFi.localIP().toString().c_str());
    }
    return ip_str;
}

int wifi_get_rssi(void) {
    if (WiFi.status() == WL_CONNECTED) {
        last_rssi = WiFi.RSSI();
    }
    return last_rssi;
}

const char* wifi_get_ssid(void) {
    return WIFI_SSID;
}
