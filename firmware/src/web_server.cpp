#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "data.h"
#include <WebServer.h>
#include <ArduinoJson.h>

static WebServer server(WEB_SERVER_PORT);

// Incoming data buffer
static char rx_buf[512];
static volatile bool data_ready = false;

// Last received usage data for status endpoint
static UsageData last_data = {};
static unsigned long last_data_time = 0;

static const ProviderData* find_provider(const char* name) {
    for (int i = 0; i < last_data.provider_count && i < 2; ++i) {
        if (strcmp(last_data.providers[i].name, name) == 0) {
            return &last_data.providers[i];
        }
    }
    return nullptr;
}

// Build HTML dashboard dynamically in RAM (no PROGMEM to avoid LoadProhibited)
static String build_dashboard() {
    String html;
    html.reserve(3000);
    
    bool conn = wifi_is_connected();
    const char* status_color = conn ? "#788c5d" : "#c0392b";
    const char* status_text = conn ? "Connected" : "Disconnected";
    
    html += "<!DOCTYPE html><html><head>";
    html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>PocketMeter Web Panel</title>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<style>";
    html += "body{background:#0a0a0a;color:#faf9f5;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:24px;max-width:480px;margin-left:auto;margin-right:auto}";
    html += "h1{font-size:22px;font-weight:600;margin:0 0 20px 0;color:#d97757}";
    html += ".card{background:#1f1f1e;border-radius:12px;padding:16px;margin-bottom:16px}";
    html += ".card h2{font-size:14px;font-weight:600;text-transform:uppercase;letter-spacing:0.05em;color:#b0aea5;margin:0 0 12px 0}";
    html += ".row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #2a2a28}";
    html += ".row:last-child{border-bottom:none}";
    html += ".label{color:#b0aea5}";
    html += ".value{font-weight:500}";
    html += ".pill{display:inline-block;background:#2a2a28;border-radius:4px;padding:2px 8px;font-size:12px;font-family:monospace}";
    html += ".footer{margin-top:24px;text-align:center;color:#666;font-size:12px}";
    html += ".provider-icon{font-size:18px;margin-right:8px}";
    html += ".connected{color:#788c5d}";
    html += ".disconnected{color:#c0392b}";
    html += ".config-link{color:#d97757;text-decoration:none;font-size:12px}";
    html += "</style></head><body>";
    
    html += "<h1>PocketMeter</h1>";
    
    // WiFi Status Card
    html += "<div class='card'><h2>WiFi Status</h2>";
    html += "<div class='row'><span class='label'>Status</span><span class='value' style='color:";
    html += status_color;
    html += "'>";
    html += status_text;
    html += "</span></div>";
    html += "<div class='row'><span class='label'>Network</span><span class='value'>";
    html += wifi_get_ssid();
    html += "</span></div>";
    html += "<div class='row'><span class='label'>IP Address</span><span class='value pill'>";
    html += wifi_get_ip();
    html += "</span></div>";
    html += "<div class='row'><span class='label'>Signal</span><span class='value'>";
    html += wifi_get_rssi();
    html += " dBm</span></div></div>";
    
    // Claude Card
    html += "<div class='card'><h2>Claude</h2>";
    const ProviderData* claude = find_provider("claude");
    if (last_data_time > 0 && claude) {
        const ProviderData* pd = claude;
        html += "<div class='row'><span class='label'>Session</span><span class='value'>";
        html += String(pd->session_pct, 1);
        html += "%</span></div>";
        html += "<div class='row'><span class='label'>Weekly</span><span class='value'>";
        html += String(pd->weekly_pct, 1);
        html += "%</span></div>";
        html += "<div class='row'><span class='label'>Status</span><span class='value'>";
        html += pd->status;
        html += "</span></div>";
        html += "<div class='row'><span class='label'>Source</span><span class='value'>";
        html += pd->simulated ? "simulated fallback" : "real OAuth session";
        html += "</span></div>";
        if (strlen(pd->plan_type) > 0) {
            html += "<div class='row'><span class='label'>Plan</span><span class='value'>";
            html += pd->plan_type;
            html += "</span></div>";
        }
        if (strlen(pd->error) > 0) {
            html += "<div class='row'><span class='label'>Note</span><span class='value' style='font-size:12px'>";
            html += pd->error;
            html += "</span></div>";
        }
    } else {
        html += "<div class='row'><span class='label'>Status</span><span class='value disconnected'>No data received</span></div>";
        html += "<div class='row'><span class='label'>Info</span><span class='value' style='font-size:12px'>Run: python3 daemon/clawdmeter-daemon.py</span></div>";
    }
    html += "</div>";

    // Codex Card
    html += "<div class='card'><h2>Codex</h2>";
    const ProviderData* codex = find_provider("codex");
    if (last_data_time > 0 && codex && codex->ok) {
        html += "<div class='row'><span class='label'>Plan</span><span class='value'>";
        html += codex->plan_type;
        html += "</span></div>";
        html += "<div class='row'><span class='label'>Primary</span><span class='value'>";
        html += String(codex->session_pct, 1);
        html += "%</span></div>";
        html += "<div class='row'><span class='label'>Secondary</span><span class='value'>";
        html += String(codex->weekly_pct, 1);
        html += "%</span></div>";
        if (codex->has_credits) {
            html += "<div class='row'><span class='label'>Credits</span><span class='value'>";
            html += String(codex->credits_balance, 1);
            html += "</span></div>";
        }
    } else if (codex && strlen(codex->error) > 0) {
        html += "<div class='row'><span class='label'>Status</span><span class='value disconnected'>Not configured</span></div>";
        html += "<div class='row'><span class='label'>Setup</span><span class='value' style='font-size:12px'>";
        html += codex->error;
        html += "</span></div>";
    } else {
        html += "<div class='row'><span class='label'>Status</span><span class='value disconnected'>No data</span></div>";
    }
    html += "</div>";
    
    // Device Card
    html += "<div class='card'><h2>Device</h2>";
    html += "<div class='row'><span class='label'>Uptime</span><span class='value'>";
    html += String(millis() / 1000);
    html += "s</span></div>";
    html += "<div class='row'><span class='label'>Last Update</span><span class='value'>";
    if (last_data_time > 0) {
        html += String((millis() - last_data_time) / 1000);
        html += "s ago";
    } else {
        html += "Never";
    }
    html += "</span></div></div>";
    
    html += "<div class='footer'>PocketMeter Web Panel &middot; Auto-refresh 5s</div>";
    html += "</body></html>";
    
    return html;
}

static void handle_root() {
    String html = build_dashboard();
    server.send(200, "text/html", html);
}

static void handle_usage() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    
    // Read raw body from the client
    String body = "";
    if (server.hasArg("plain")) {
        body = server.arg("plain");
    } else if (server.args() > 0) {
        body = server.arg(0);
    }
    
    if (body.length() == 0 || body.length() >= sizeof(rx_buf)) {
        Serial.printf("HTTP POST error: body length=%d\n", body.length());
        server.send(400, "text/plain", "Bad Request: empty body");
        return;
    }
    
    strncpy(rx_buf, body.c_str(), sizeof(rx_buf) - 1);
    rx_buf[sizeof(rx_buf) - 1] = '\0';
    data_ready = true;
    
    Serial.printf("HTTP POST received: %s\n", rx_buf);
    server.send(200, "text/plain", "OK");
}

static void handle_status() {
    JsonDocument doc;
    doc["wifi_connected"] = wifi_is_connected();
    doc["ssid"] = wifi_get_ssid();
    doc["ip"] = wifi_get_ip();
    doc["rssi"] = wifi_get_rssi();
    doc["uptime_ms"] = millis();
    doc["has_data"] = last_data_time > 0 && last_data.provider_count > 0;
    doc["provider_count"] = last_data.provider_count;
    JsonArray providers = doc["providers"].to<JsonArray>();
    for (int i = 0; i < last_data.provider_count && i < 2; ++i) {
        JsonObject p = providers.add<JsonObject>();
        const ProviderData* pd = &last_data.providers[i];
        p["provider"] = pd->name;
        p["session_pct"] = pd->session_pct;
        p["weekly_pct"] = pd->weekly_pct;
        p["status"] = pd->status;
        p["plan_type"] = pd->plan_type;
        p["credits_balance"] = pd->credits_balance;
        p["has_credits"] = pd->has_credits;
        p["simulated"] = pd->simulated;
        p["ok"] = pd->ok;
        p["error"] = pd->error;
    }
    
    if (last_data_time > 0 && last_data.provider_count > 0) {
        const ProviderData* pd = &last_data.providers[0];
        doc["session_pct"] = pd->session_pct;
        doc["weekly_pct"] = pd->weekly_pct;
        doc["status"] = pd->status;
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

static void handle_health() {
    server.send(200, "text/plain", "OK");
}

void web_server_init() {
    server.on("/", HTTP_GET, handle_root);
    server.on(DATA_ENDPOINT, HTTP_POST, handle_usage);
    server.on(STATUS_ENDPOINT, HTTP_GET, handle_status);
    server.on("/api/health", HTTP_GET, handle_health);
    
    server.begin();
    Serial.printf("Web server started on port %d\n", WEB_SERVER_PORT);
}

void web_server_handle() {
    server.handleClient();
}

bool web_server_has_data() {
    return data_ready;
}

const char* web_server_get_data() {
    return rx_buf;
}

void web_server_clear_data() {
    data_ready = false;
    rx_buf[0] = '\0';
}

void web_server_set_last_data(const UsageData* data) {
    if (data) {
        last_data = *data;
        last_data_time = millis();
    }
}
