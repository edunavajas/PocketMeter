#include "web_server.h"
#include "config.h"
#include "wifi_manager.h"
#include "data.h"
#include <WebServer.h>

// Forward declarations for splash functions (avoids pulling in lvgl.h)
extern int splash_anim_count(void);
extern const char* splash_anim_name(int idx);
extern const char* splash_anim_category(int idx);
extern bool splash_set_custom(const char* name, const char* cat, const char* provider,
                               const uint16_t pal[10],
                               const char* frames_str, uint16_t fc,
                               const uint16_t* holds);
extern void splash_pin_by_name(const char* name);
#include <ArduinoJson.h>
#include <Preferences.h>

static WebServer server(WEB_SERVER_PORT);
static Preferences prefs;

// Incoming data buffer — sized for up to MAX_PROVIDERS providers
static char rx_buf[4096];
// Separate buffer for custom-anim payloads (frames string alone can be 3200 chars)
static char custom_anim_buf[4096];
static volatile bool data_ready = false;

// Last received usage data for status endpoint
static UsageData last_data = {};
static unsigned long last_data_time = 0;

// Display visibility toggles — changed via POST /api/config
static bool g_claude_visible = true;
static bool g_codex_visible  = true;
static web_provider_t g_selected_provider = WEB_PROVIDER_CLAUDE;
static WebPetConfig g_pet_configs[2] = {};

// Extended multi-provider state
static char g_selected_provider_name[PROVIDER_NAME_LEN] = "claude";
static char g_splash_pin[64] = "";

struct ExtraProviderConfig {
    char name[PROVIDER_NAME_LEN];
    bool visible;
};
static ExtraProviderConfig g_extra_configs[MAX_PROVIDERS] = {};
static int g_extra_config_count = 0;

struct ExtraPetEntry {
    char name[PROVIDER_NAME_LEN];
    WebPetConfig pet;
};
static ExtraPetEntry g_extra_pets[MAX_PROVIDERS] = {};
static int g_extra_pet_count = 0;

static void extra_config_set_visible(const char* name, bool visible);

static const char* kPrefsNamespace = "webcfg";
static const char* kPrefsStateKey = "state";

static const size_t PET_ID_LEN = sizeof(g_pet_configs[0].id);
static const size_t PET_SLUG_LEN = sizeof(g_pet_configs[0].slug);
static const size_t PET_NAME_LEN = sizeof(g_pet_configs[0].display_name);
static const size_t PET_PATH_LEN = sizeof(g_pet_configs[0].spritesheet_path);
static const size_t PET_COLOR_LEN = sizeof(g_pet_configs[0].dominant_color);

static int provider_index(web_provider_t provider) {
    return provider == WEB_PROVIDER_CODEX ? 1 : 0;
}

static WebPetConfig* pet_config_for(web_provider_t provider) {
    return &g_pet_configs[provider_index(provider)];
}

static const WebPetConfig* pet_config_for_const(web_provider_t provider) {
    return &g_pet_configs[provider_index(provider)];
}

static const char* provider_name(web_provider_t provider) {
    return provider == WEB_PROVIDER_CODEX ? "codex" : "claude";
}

static web_provider_t provider_from_name(const char* value) {
    return (value && strcmp(value, "codex") == 0) ? WEB_PROVIDER_CODEX
                                                    : WEB_PROVIDER_CLAUDE;
}

static web_provider_t normalize_selected_provider(bool claude_visible,
                                                  bool codex_visible,
                                                  web_provider_t selected) {
    if (selected == WEB_PROVIDER_CODEX && !codex_visible && claude_visible) {
        return WEB_PROVIDER_CLAUDE;
    }
    if (selected == WEB_PROVIDER_CLAUDE && !claude_visible && codex_visible) {
        return WEB_PROVIDER_CODEX;
    }
    return selected;
}

static WebPetConfig* find_pet_by_name(const char* name) {
    if (!name) return nullptr;
    if (strcmp(name, "claude") == 0) return &g_pet_configs[0];
    if (strcmp(name, "codex") == 0)  return &g_pet_configs[1];
    for (int i = 0; i < g_extra_pet_count; ++i)
        if (strcmp(g_extra_pets[i].name, name) == 0)
            return &g_extra_pets[i].pet;
    return nullptr;
}

static WebPetConfig* get_or_create_pet_slot(const char* name) {
    WebPetConfig* existing = find_pet_by_name(name);
    if (existing) return existing;
    if (g_extra_pet_count >= MAX_PROVIDERS) return nullptr;
    strlcpy(g_extra_pets[g_extra_pet_count].name, name, PROVIDER_NAME_LEN);
    memset(&g_extra_pets[g_extra_pet_count].pet, 0, sizeof(WebPetConfig));
    return &g_extra_pets[g_extra_pet_count++].pet;
}

static void clear_pet_config(WebPetConfig* pet) {
    if (!pet) return;
    memset(pet, 0, sizeof(*pet));
}

static void sanitize_string_copy(char* dest, size_t dest_len, const char* src) {
    if (!dest || dest_len == 0) return;
    if (!src) {
        dest[0] = '\0';
        return;
    }
    strlcpy(dest, src, dest_len);
}

static bool has_text(const char* value) {
    return value && value[0] != '\0';
}

static void write_pet_json(JsonObject json, const WebPetConfig& pet) {
    json["assigned"] = pet.assigned;
    json["id"] = pet.id;
    json["slug"] = pet.slug;
    json["displayName"] = pet.display_name;
    json["spritesheetPath"] = pet.spritesheet_path;
    json["petJsonPath"] = pet.pet_json_path;
    json["dominantColor"] = pet.dominant_color;
    json["previewImagePath"] = pet.preview_image_path;
}

static void write_config_json(JsonObject json) {
    json["claude"] = g_claude_visible;
    json["codex"]  = g_codex_visible;
    json["selected_provider"]     = provider_name(g_selected_provider);
    json["selected_provider_ext"] = g_selected_provider_name;
    json["splash_pin"] = g_splash_pin;

    JsonArray providers_arr = json["providers"].to<JsonArray>();
    for (int i = 0; i < last_data.provider_count && i < MAX_PROVIDERS; ++i) {
        providers_arr.add(last_data.providers[i].name);
    }
    if (last_data.provider_count == 0) {
        providers_arr.add("claude");
        providers_arr.add("codex");
    }

    JsonObject pets = json["pets"].to<JsonObject>();
    write_pet_json(pets["claude"].to<JsonObject>(), *pet_config_for_const(WEB_PROVIDER_CLAUDE));
    write_pet_json(pets["codex"].to<JsonObject>(),  *pet_config_for_const(WEB_PROVIDER_CODEX));
    for (int i = 0; i < g_extra_pet_count; ++i)
        write_pet_json(pets[g_extra_pets[i].name].to<JsonObject>(), g_extra_pets[i].pet);

    JsonObject extra_vis = json["provider_visibility"].to<JsonObject>();
    for (int i = 0; i < g_extra_config_count; ++i) {
        extra_vis[g_extra_configs[i].name] = g_extra_configs[i].visible;
    }
}

static String serialize_current_config() {
    JsonDocument doc;
    write_config_json(doc.to<JsonObject>());
    String body;
    serializeJson(doc, body);
    return body;
}

static void save_config_state() {
    if (!prefs.begin(kPrefsNamespace, false)) {
        Serial.println("Config save failed: prefs begin");
        return;
    }
    prefs.putString(kPrefsStateKey, serialize_current_config());
    prefs.end();
}

static bool parse_pet_config(JsonVariantConst source, WebPetConfig* out_pet, String* error) {
    if (!out_pet) return false;

    if (source.isNull()) {
        clear_pet_config(out_pet);
        return true;
    }

    if (!source.is<JsonObjectConst>()) {
        if (error) *error = "Pet config must be an object or null";
        return false;
    }

    JsonObjectConst pet = source.as<JsonObjectConst>();
    const char* id = pet["id"] | "";
    const char* slug = pet["slug"] | "";
    const char* display_name = pet["displayName"] | "";
    const char* spritesheet_path = pet["spritesheetPath"] | "";
    const char* pet_json_path = pet["petJsonPath"] | "";
    const char* dominant_color = pet["dominantColor"] | "";
    const char* preview_image_path = pet["previewImagePath"] | spritesheet_path;

    bool assigned_flag = pet["assigned"] | true;
    if (!assigned_flag || (!has_text(id) && !has_text(slug))) {
        clear_pet_config(out_pet);
        return true;
    }
    if (!has_text(display_name)) {
        if (error) *error = "Pet config requires displayName";
        return false;
    }
    if (!has_text(spritesheet_path) && !has_text(pet_json_path)) {
        if (error) *error = "Pet config requires spritesheetPath or petJsonPath";
        return false;
    }

    WebPetConfig next = {};
    next.assigned = true;
    sanitize_string_copy(next.id, PET_ID_LEN, id);
    sanitize_string_copy(next.slug, PET_SLUG_LEN, slug);
    sanitize_string_copy(next.display_name, PET_NAME_LEN, display_name);
    sanitize_string_copy(next.spritesheet_path, PET_PATH_LEN, spritesheet_path);
    sanitize_string_copy(next.pet_json_path, PET_PATH_LEN, pet_json_path);
    sanitize_string_copy(next.dominant_color, PET_COLOR_LEN, dominant_color);
    sanitize_string_copy(next.preview_image_path, PET_PATH_LEN, preview_image_path);
    *out_pet = next;
    return true;
}

static bool apply_config_json(JsonVariantConst input, bool allow_partial_pets, String* error) {
    if (!input.is<JsonObjectConst>()) {
        if (error) *error = "Config must be an object";
        return false;
    }

    JsonObjectConst doc = input.as<JsonObjectConst>();
    if (!doc["claude"].is<bool>() || !doc["codex"].is<bool>() || !doc["selected_provider"].is<const char*>()) {
        if (error) *error = "Expected claude, codex, and selected_provider";
        return false;
    }

    bool next_claude_visible = doc["claude"].as<bool>();
    bool next_codex_visible = doc["codex"].as<bool>();
    web_provider_t next_selected_provider = normalize_selected_provider(
        next_claude_visible,
        next_codex_visible,
        provider_from_name(doc["selected_provider"].as<const char*>())
    );

    JsonVariantConst pets = doc["pets"];
    if (!pets.isNull()) {
        if (!pets.is<JsonObjectConst>()) {
            if (error) *error = "pets must be an object";
            return false;
        }
        for (JsonPairConst kv : pets.as<JsonObjectConst>()) {
            const char* pname = kv.key().c_str();
            if (!pname || !pname[0]) continue;
            WebPetConfig* slot = get_or_create_pet_slot(pname);
            if (slot) {
                String pet_error;
                parse_pet_config(kv.value(), slot, &pet_error);
            }
        }
    }

    g_claude_visible = next_claude_visible;
    g_codex_visible  = next_codex_visible;
    g_selected_provider = next_selected_provider;

    const char* sel_ext = doc["selected_provider_ext"] | "";
    if (sel_ext && sel_ext[0] != '\0') {
        strlcpy(g_selected_provider_name, sel_ext, PROVIDER_NAME_LEN);
        if (strcmp(sel_ext, "codex") == 0) g_selected_provider = WEB_PROVIDER_CODEX;
        else if (strcmp(sel_ext, "claude") == 0) g_selected_provider = WEB_PROVIDER_CLAUDE;
    } else {
        strlcpy(g_selected_provider_name, provider_name(next_selected_provider), PROVIDER_NAME_LEN);
    }

    JsonVariantConst extra_vis = doc["provider_visibility"];
    if (!extra_vis.isNull() && extra_vis.is<JsonObjectConst>()) {
        for (JsonPairConst kv : extra_vis.as<JsonObjectConst>()) {
            if (kv.value().is<bool>()) {
                extra_config_set_visible(kv.key().c_str(), kv.value().as<bool>());
            }
        }
    }

    // Only update splash pin when field is explicitly present;
    // absent means "don't touch it" (managed by /api/splash endpoint).
    if (doc["splash_pin"].is<const char*>()) {
        strlcpy(g_splash_pin, doc["splash_pin"].as<const char*>(), sizeof(g_splash_pin));
    }

    return true;
}

static void load_config_state() {
    clear_pet_config(&g_pet_configs[0]);
    clear_pet_config(&g_pet_configs[1]);
    g_claude_visible = true;
    g_codex_visible = true;
    g_selected_provider = WEB_PROVIDER_CLAUDE;
    g_splash_pin[0] = '\0';

    if (!prefs.begin(kPrefsNamespace, true)) {
        Serial.println("Config load skipped: prefs begin");
        return;
    }

    String saved = prefs.getString(kPrefsStateKey, "");
    prefs.end();
    if (saved.isEmpty()) return;

    JsonDocument doc;
    if (deserializeJson(doc, saved) != DeserializationError::Ok) {
        Serial.println("Config load skipped: invalid JSON");
        return;
    }

    String error;
    if (!apply_config_json(doc.as<JsonVariantConst>(), true, &error)) {
        Serial.printf("Config load skipped: %s\n", error.c_str());
    }
}

bool web_server_claude_visible(void) { return g_claude_visible; }
bool web_server_codex_visible(void)  { return g_codex_visible; }
web_provider_t web_server_selected_provider(void) { return g_selected_provider; }
const WebPetConfig* web_server_pet_config(web_provider_t provider) { return pet_config_for_const(provider); }

const char* web_server_selected_provider_name(void) {
    return g_selected_provider_name;
}

bool web_server_provider_visible(const char* name) {
    if (!name) return true;
    if (strcmp(name, "claude") == 0) return g_claude_visible;
    if (strcmp(name, "codex")  == 0) return g_codex_visible;
    for (int i = 0; i < g_extra_config_count; ++i) {
        if (strcmp(g_extra_configs[i].name, name) == 0)
            return g_extra_configs[i].visible;
    }
    return true;
}

static void extra_config_set_visible(const char* name, bool visible) {
    for (int i = 0; i < g_extra_config_count; ++i) {
        if (strcmp(g_extra_configs[i].name, name) == 0) {
            g_extra_configs[i].visible = visible;
            return;
        }
    }
    if (g_extra_config_count < MAX_PROVIDERS) {
        strlcpy(g_extra_configs[g_extra_config_count].name, name, PROVIDER_NAME_LEN);
        g_extra_configs[g_extra_config_count].visible = visible;
        g_extra_config_count++;
    }
}

static void extra_config_ensure(const char* name) {
    if (!name || strcmp(name, "claude") == 0 || strcmp(name, "codex") == 0) return;
    for (int i = 0; i < g_extra_config_count; ++i) {
        if (strcmp(g_extra_configs[i].name, name) == 0) return;
    }
    if (g_extra_config_count < MAX_PROVIDERS) {
        strlcpy(g_extra_configs[g_extra_config_count].name, name, PROVIDER_NAME_LEN);
        g_extra_configs[g_extra_config_count].visible = true;
        g_extra_config_count++;
    }
}

static void send_config_response() {
    JsonDocument response;
    write_config_json(response.to<JsonObject>());

    String body;
    serializeJson(response, body);
    server.send(200, "application/json", body);
}

static const ProviderData* find_provider(const char* name) {
    for (int i = 0; i < last_data.provider_count && i < MAX_PROVIDERS; ++i) {
        if (strcmp(last_data.providers[i].name, name) == 0)
            return &last_data.providers[i];
    }
    return nullptr;
}

static void handle_usage() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    
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
    doc["hostname"] = wifi_get_hostname();
    doc["ip"] = wifi_get_ip();
    doc["rssi"] = wifi_get_rssi();
    doc["uptime_ms"] = millis();
    doc["has_data"] = last_data_time > 0 && last_data.provider_count > 0;
    doc["provider_count"] = last_data.provider_count;
    write_config_json(doc["config"].to<JsonObject>());
    JsonArray providers = doc["providers"].to<JsonArray>();
    for (int i = 0; i < last_data.provider_count && i < MAX_PROVIDERS; ++i) {
        JsonObject p = providers.add<JsonObject>();
        const ProviderData* pd = &last_data.providers[i];
        p["provider"] = pd->name;
        p["session_pct"] = pd->session_pct;
        p["weekly_pct"] = pd->weekly_pct;
        p["status"] = pd->status;
        p["plan_type"] = pd->plan_type;
        p["credits_balance"] = pd->credits_balance;
        p["has_credits"] = pd->has_credits;
        p["metrics_available"] = pd->metrics_available;
        p["simulated"] = pd->simulated;
        p["configured"] = pd->configured;
        p["ok"] = pd->ok;
        p["note"] = pd->note;
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

static void handle_config() {
    if (server.method() != HTTP_POST) {
        server.send(405, "text/plain", "Method Not Allowed");
        return;
    }
    String body = server.hasArg("plain") ? server.arg("plain") : server.arg(0);
    if (body.length() == 0) { server.send(400, "text/plain", "Bad Request"); return; }

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }

    String error;
    if (!apply_config_json(doc.as<JsonVariantConst>(), true, &error)) {
        server.send(400, "text/plain", error);
        return;
    }

    save_config_state();
    send_config_response();
}

static void handle_splash_get() {
    JsonDocument doc;
    doc["pinned"] = g_splash_pin;
    JsonArray anims = doc["animations"].to<JsonArray>();
    int count = splash_anim_count();
    for (int i = 0; i < count; i++) {
        JsonObject a = anims.add<JsonObject>();
        a["name"]     = splash_anim_name(i);
        a["category"] = splash_anim_category(i);
    }
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
}

static void handle_splash_post() {
    String body = server.hasArg("plain") ? server.arg("plain") : server.arg(0);
    if (body.length() == 0) { server.send(400, "text/plain", "Bad Request"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "text/plain", "Bad JSON"); return;
    }
    const char* name = doc["name"] | "";
    strlcpy(g_splash_pin, name, sizeof(g_splash_pin));
    splash_pin_by_name(name);   // apply immediately
    save_config_state();
    JsonDocument resp;
    resp["ok"] = true;
    resp["pinned"] = g_splash_pin;
    String respBody;
    serializeJson(resp, respBody);
    server.send(200, "application/json", respBody);
}

const char* web_server_splash_pin(void) { return g_splash_pin; }
void web_server_clear_splash_pin(void) { g_splash_pin[0] = '\0'; }

static void handle_anim_custom() {
    String body = server.hasArg("plain") ? server.arg("plain") : server.arg(0);
    if (body.length() == 0 || body.length() >= sizeof(custom_anim_buf)) {
        server.send(400, "text/plain", "Bad Request");
        return;
    }
    memcpy(custom_anim_buf, body.c_str(), body.length() + 1);

    JsonDocument doc;
    if (deserializeJson(doc, custom_anim_buf) != DeserializationError::Ok) {
        server.send(400, "text/plain", "Bad JSON");
        return;
    }

    const char* name       = doc["name"]        | "";
    const char* cat        = doc["category"]    | "petdex";
    const char* provider   = doc["provider"]    | "claude";
    const char* frames_str = doc["frames"]      | "";
    uint16_t    fc         = doc["frame_count"] | (uint16_t)0;

    if (!name[0] || !frames_str[0] || fc == 0 || fc > 8) {
        server.send(400, "text/plain", "Missing or invalid fields");
        return;
    }

    uint16_t pal[10] = {};
    JsonArrayConst pal_arr = doc["palette"].as<JsonArrayConst>();
    for (int i = 0; i < 10 && i < (int)pal_arr.size(); i++)
        pal[i] = (uint16_t)pal_arr[i].as<unsigned int>();

    uint16_t holds[8] = {};
    JsonArrayConst holds_arr = doc["holds"].as<JsonArrayConst>();
    bool has_holds = !holds_arr.isNull() && (int)holds_arr.size() > 0;
    if (has_holds)
        for (int i = 0; i < (int)fc && i < (int)holds_arr.size(); i++)
            holds[i] = (uint16_t)holds_arr[i].as<unsigned int>();

    if (!splash_set_custom(name, cat, provider, pal, frames_str, fc, has_holds ? holds : nullptr)) {
        server.send(500, "text/plain", "Conversion failed");
        return;
    }

    // Only the Claude splash uses the pin system; Codex uses s_codex_valid directly.
    if (strcmp(provider, "claude") == 0) {
        strlcpy(g_splash_pin, name, sizeof(g_splash_pin));
        splash_pin_by_name(name);   // apply immediately
    }
    save_config_state();

    JsonDocument resp;
    resp["ok"]   = true;
    resp["name"] = name;
    String respBody;
    serializeJson(resp, respBody);
    server.send(200, "application/json", respBody);
}

static void handle_root() {
    // The web UI is now served by the PC. ESP32 only exposes REST API.
    server.send(200, "text/plain", "PocketMeter API. Use the web UI on your PC.");
}

void web_server_init() {
    load_config_state();
    server.on("/", HTTP_GET, handle_root);
    server.on(DATA_ENDPOINT, HTTP_POST, handle_usage);
    server.on(STATUS_ENDPOINT, HTTP_GET, handle_status);
    server.on("/api/health", HTTP_GET, handle_health);
    server.on(CONFIG_ENDPOINT, HTTP_POST, handle_config);
    server.on("/api/splash", HTTP_GET, handle_splash_get);
    server.on("/api/splash", HTTP_POST, handle_splash_post);
    server.on("/api/anim/custom", HTTP_POST, handle_anim_custom);
    
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
    if (!data) return;
    last_data      = *data;
    last_data_time = millis();
    for (int i = 0; i < data->provider_count && i < MAX_PROVIDERS; ++i)
        extra_config_ensure(data->providers[i].name);
}
