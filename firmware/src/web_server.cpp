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

static String make_bar_html(float pct) {
    const char* col = pct >= 80.0f ? "#c0392b" : (pct >= 50.0f ? "#d97757" : "#788c5d");
    String bar = "<div style='background:#2a2a28;border-radius:4px;height:8px;margin:6px 0'>";
    bar += "<div style='background:";
    bar += col;
    bar += ";height:8px;border-radius:4px;width:";
    bar += String((int)pct);
    bar += "%'></div></div>";
    return bar;
}

static String make_provider_card(const char* name, const char* color,
                                 const char* icon, const ProviderData* pd) {
    String html;
    html.reserve(800);
    bool ok = pd && pd->ok;

    html += "<div class='card' style='border-left:3px solid ";
    html += ok ? color : "#3a3a38";
    html += "'>";

    // Header row: icon + name + status dot
    html += "<div style='display:flex;align-items:center;justify-content:space-between;margin-bottom:12px'>";
    html += "<div style='display:flex;align-items:center;gap:8px'>";
    html += "<span style='font-size:20px'>";
    html += icon;
    html += "</span>";
    html += "<span style='font-size:16px;font-weight:700;color:";
    html += color;
    html += "'>";
    html += name;
    html += "</span>";
    html += "</div>";
    if (ok) {
        html += "<span style='background:";
        html += color;
        html += ";color:#000;font-size:11px;font-weight:600;padding:2px 8px;border-radius:20px'>Connected</span>";
    } else {
        html += "<span style='background:#2a2a28;color:#b0aea5;font-size:11px;padding:2px 8px;border-radius:20px'>Not configured</span>";
    }
    html += "</div>";

    if (ok) {
        if (strlen(pd->plan_type) > 0) {
            html += "<div class='row'><span class='label'>Plan</span><span class='value'>";
            html += pd->plan_type;
            html += "</span></div>";
        }
        html += "<div class='row'><span class='label'>Session</span><span class='value'>";
        html += String(pd->session_pct, 1);
        html += "%</span></div>";
        html += make_bar_html(pd->session_pct);
        html += "<div class='row'><span class='label'>Weekly</span><span class='value'>";
        html += String(pd->weekly_pct, 1);
        html += "%</span></div>";
        html += make_bar_html(pd->weekly_pct);
        if (pd->has_credits) {
            html += "<div class='row'><span class='label'>Credits</span><span class='value'>$";
            html += String(pd->credits_balance, 2);
            html += "</span></div>";
        }
        if (pd->simulated) {
            html += "<div style='margin-top:8px;font-size:11px;color:#b0aea5'>&#9888; Simulated data (token may be expired)</div>";
        }
    } else {
        const char* cmd = (strcmp(name, "Claude") == 0) ? "claude" : "codex";
        html += "<div style='padding:8px 0;color:#b0aea5;font-size:13px'>";
        if (pd && strlen(pd->error) > 0) {
            html += pd->error;
        } else {
            html += "No data received from daemon.";
        }
        html += "</div>";
        html += "<div style='margin-top:8px;background:#0a0a0a;border-radius:6px;padding:8px;font-family:monospace;font-size:12px;color:#788c5d'>";
        html += cmd;
        html += " login";
        html += "</div>";
    }

    html += "</div>";
    return html;
}

// Build HTML dashboard dynamically in RAM (no PROGMEM to avoid LoadProhibited)
static String build_dashboard() {
    String html;
    html.reserve(4000);

    bool conn = wifi_is_connected();

    html += "<!DOCTYPE html><html><head>";
    html += "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>PocketMeter</title>";
    html += "<meta http-equiv='refresh' content='10'>";
    html += "<style>";
    html += "body{background:#0a0a0a;color:#faf9f5;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:20px;max-width:480px;margin-left:auto;margin-right:auto}";
    html += "h1{font-size:20px;font-weight:700;margin:0 0 4px 0;color:#faf9f5}";
    html += ".subtitle{font-size:13px;color:#b0aea5;margin-bottom:20px}";
    html += ".card{background:#1f1f1e;border-radius:12px;padding:16px;margin-bottom:14px}";
    html += ".card h2{font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:0.08em;color:#b0aea5;margin:0 0 10px 0}";
    html += ".row{display:flex;justify-content:space-between;align-items:center;padding:5px 0;border-bottom:1px solid #2a2a28}";
    html += ".row:last-child{border-bottom:none}";
    html += ".label{color:#b0aea5;font-size:13px}";
    html += ".value{font-weight:500;font-size:13px}";
    html += ".pill{display:inline-block;background:#2a2a28;border-radius:4px;padding:1px 7px;font-size:11px;font-family:monospace}";
    html += ".footer{text-align:center;color:#444;font-size:11px;margin-top:20px}";
    html += "</style></head><body>";

    // Header
    html += "<h1>PocketMeter</h1>";
    html += "<div class='subtitle'>AI Usage Monitor";
    if (last_data_time > 0) {
        html += " &middot; Updated ";
        html += String((millis() - last_data_time) / 1000);
        html += "s ago";
    }
    html += "</div>";

    // WiFi status (compact)
    html += "<div class='card'><h2>Network</h2>";
    html += "<div class='row'><span class='label'>WiFi</span><span class='value' style='color:";
    html += conn ? "#788c5d" : "#c0392b";
    html += "'>";
    html += conn ? "Connected" : "Disconnected";
    html += "</span></div>";
    if (conn) {
        html += "<div class='row'><span class='label'>SSID</span><span class='value'>";
        html += wifi_get_ssid();
        html += "</span></div>";
        html += "<div class='row'><span class='label'>IP</span><span class='value pill'>";
        html += wifi_get_ip();
        html += "</span></div>";
        html += "<div class='row'><span class='label'>Signal</span><span class='value'>";
        html += String(wifi_get_rssi());
        html += " dBm</span></div>";
    }
    html += "</div>";

    // Provider cards
    const ProviderData* claude = find_provider("claude");
    html += make_provider_card("Claude", "#d97757", "&#129302;", claude);

    const ProviderData* codex = find_provider("codex");
    html += make_provider_card("Codex", "#10a37f", "&#9729;", codex);

    // Device info
    html += "<div class='card'><h2>Device</h2>";
    html += "<div class='row'><span class='label'>Uptime</span><span class='value'>";
    html += String(millis() / 1000);
    html += "s</span></div>";
    html += "<div class='row'><span class='label'>Providers</span><span class='value'>";
    html += String(last_data.provider_count);
    html += " active</span></div>";
    html += "</div>";

    html += "<div class='footer'>PocketMeter &middot; Auto-refresh 10s</div>";
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
