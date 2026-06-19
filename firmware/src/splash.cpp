#include "splash.h"
#include "splash_animations.h"
#include "theme.h"
#include "usage_rate.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "data.h"

// 20x20 grid scaled 24x to fill 480x480
#define GRID         20
#define CELL         24
#define CANVAS_W     (GRID * CELL)
#define CANVAS_H     (GRID * CELL)

// Background fallback when palette is missing
#define COL_EMPTY    0x0000  // true black (matches THEME_BG)

LV_FONT_DECLARE(font_styrene_28);

static lv_obj_t *splash_container = NULL;
static lv_obj_t *canvas = NULL;
static lv_obj_t *label_status = NULL;     // shown only when no animations loaded
static uint16_t *canvas_buf = NULL;        // 480x480 RGB565 (PSRAM)

static uint16_t cur_anim = 0;
static uint16_t cur_frame = 0;
static uint32_t frame_started_ms = 0;
static uint32_t last_pick_ms = 0;
static bool active = false;
static char s_pinned_name[64] = "";

// ── Custom (runtime-loaded) animation ────────────────────────────────────────
#define CUSTOM_MAX_FRAMES 8

// Claude slot — integrated into get_anim() / splash_tick() via s_custom_def
static bool     s_custom_valid = false;
static char     s_custom_name[64]     = "";
static char     s_custom_cat[32]      = "petdex";
static char     s_custom_provider_name[PROVIDER_NAME_LEN] = "";
static uint16_t s_custom_pal[10]   = {};
static uint8_t  s_custom_frames[CUSTOM_MAX_FRAMES][400] = {};
static uint16_t s_custom_holds[CUSTOM_MAX_FRAMES] = {};
static uint16_t s_custom_fc = 0;
static splash_anim_def_t s_custom_def = {};

// Codex slot — served by splash_custom_* accessors (used by codex_splash.cpp)
static bool     s_codex_valid = false;
static char     s_codex_name[64]  = "";
static char     s_codex_cat[32]   = "petdex";
static uint16_t s_codex_pal[10]   = {};
static uint8_t  s_codex_frames[CUSTOM_MAX_FRAMES][400] = {};
static uint16_t s_codex_holds[CUSTOM_MAX_FRAMES] = {};
static uint16_t s_codex_fc = 0;

static const splash_anim_def_t* get_anim(int idx) {
    if (idx >= 0 && idx < SPLASH_ANIM_COUNT) return &splash_anims[idx];
    if (s_custom_valid && idx == SPLASH_ANIM_COUNT) return &s_custom_def;
    return nullptr;
}

bool splash_set_custom(const char* name, const char* cat, const char* provider,
                       const uint16_t pal[10],
                       const char* frames_str, uint16_t fc,
                       const uint16_t* holds) {
    if (!name || !frames_str || fc == 0 || fc > CUSTOM_MAX_FRAMES) return false;
    const char* prov = provider ? provider : "claude";
    int slen = (int)strlen(frames_str);

    if (strcmp(prov, "codex") == 0) {
        strlcpy(s_codex_name, name, sizeof(s_codex_name));
        strlcpy(s_codex_cat, cat ? cat : "petdex", sizeof(s_codex_cat));
        memcpy(s_codex_pal, pal, 10 * sizeof(uint16_t));
        s_codex_fc = fc;
        for (int f = 0; f < fc; f++) {
            for (int c = 0; c < 400; c++) {
                int i = f * 400 + c;
                s_codex_frames[f][c] = (i < slen) ? (uint8_t)(frames_str[i] - '0') : 0;
            }
            s_codex_holds[f] = holds ? holds[f] : 150;
        }
        s_codex_valid = true;
        Serial.printf("splash: codex custom '%s' loaded (%d frames)\n", name, fc);
    } else {
        strlcpy(s_custom_name, name, sizeof(s_custom_name));
        strlcpy(s_custom_cat, cat ? cat : "petdex", sizeof(s_custom_cat));
        strlcpy(s_custom_provider_name, prov, sizeof(s_custom_provider_name));
        memcpy(s_custom_pal, pal, 10 * sizeof(uint16_t));
        s_custom_fc = fc;
        for (int f = 0; f < fc; f++) {
            for (int c = 0; c < 400; c++) {
                int i = f * 400 + c;
                s_custom_frames[f][c] = (i < slen) ? (uint8_t)(frames_str[i] - '0') : 0;
            }
            s_custom_holds[f] = holds ? holds[f] : 150;
        }
        s_custom_def.name        = s_custom_name;
        s_custom_def.category    = s_custom_cat;
        s_custom_def.frame_count = s_custom_fc;
        s_custom_def.palette     = s_custom_pal;
        s_custom_def.frames      = (const uint8_t (*)[400])s_custom_frames;
        s_custom_def.holds       = s_custom_holds;
        s_custom_valid = true;
        Serial.printf("splash: %s custom '%s' loaded (%d frames)\n", prov, name, fc);
    }
    return true;
}

bool splash_has_custom(void) { return s_custom_valid || s_codex_valid; }
bool splash_has_custom_for(const char* provider) {
    if (!provider) return false;
    if (strcmp(provider, "codex") == 0) return s_codex_valid;
    return s_custom_valid && strcmp(s_custom_provider_name, provider) == 0;
}
const char* splash_custom_provider(void) { return s_custom_provider_name; }
const uint8_t* splash_custom_frame_pixels(int frame_idx) {
    if (!s_codex_valid || frame_idx < 0 || frame_idx >= (int)s_codex_fc) return nullptr;
    return s_codex_frames[frame_idx];
}
const uint16_t* splash_custom_palette(void) { return s_codex_valid ? s_codex_pal : nullptr; }
uint16_t splash_custom_frame_count(void) { return s_codex_fc; }
uint16_t splash_custom_hold_ms(int frame_idx) {
    if (!s_codex_valid || frame_idx < 0 || frame_idx >= (int)s_codex_fc) return 150;
    return s_codex_holds[frame_idx];
}

// While splash is showing, auto-cycle to the next animation in the current
// rate-driven group every this many ms.
#define SPLASH_ROTATE_INTERVAL_MS 20000

// Usage-rate animation groups: 4 groups × up to 4 animations each.
// Filled at init by matching literal names from splash_anims[].
#define GROUP_COUNT 4
#define GROUP_MAX   4
static int8_t  group_lists[GROUP_COUNT][GROUP_MAX];
static uint8_t group_size[GROUP_COUNT] = {0};
static uint8_t group_rotation[GROUP_COUNT] = {0};

static const char* GROUP_NAMES[GROUP_COUNT][GROUP_MAX] = {
    // Group 0 — idle / sleepy
    { "expression sleep", "idle breathe", "idle blink", "expression wink" },
    // Group 1 — normal pace
    { "idle look around", "work think", "work coding", NULL },
    // Group 2 — active
    { "dance sway", "expression surprise", "dance bounce", NULL },
    // Group 3 — heavy
    { "dance bounce dj", "dance sway dj", "dance djmix", NULL },
};

static void resolve_group_lists(void) {
    for (int g = 0; g < GROUP_COUNT; g++) {
        group_size[g] = 0;
        for (int s = 0; s < GROUP_MAX; s++) {
            group_lists[g][s] = -1;
            const char* want = GROUP_NAMES[g][s];
            if (!want) continue;
            for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
                if (strcmp(splash_anims[i].name, want) == 0) {
                    group_lists[g][group_size[g]++] = (int8_t)i;
                    break;
                }
            }
        }
    }
}

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    for (int gy = 0; gy < GRID; gy++) {
        uint16_t row[CANVAS_W];
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < SPLASH_PALETTE_SIZE) ? palette[code] : COL_EMPTY;
            uint16_t *p = &row[gx * CELL];
            for (int i = 0; i < CELL; i++) p[i] = color;
        }
        for (int dy = 0; dy < CELL; dy++) {
            memcpy(&canvas_buf[(gy * CELL + dy) * CANVAS_W], row, CANVAS_W * 2);
        }
    }
    if (canvas) lv_obj_invalidate(canvas);
}

static void show_placeholder() {
    // Solid dark background + centered status label.
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) canvas_buf[i] = COL_EMPTY;
    if (canvas) lv_obj_invalidate(canvas);
    if (label_status) lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN);
}

void splash_init(lv_obj_t *parent) {
    canvas_buf = (uint16_t*)heap_caps_malloc(CANVAS_W * CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        Serial.println("splash: failed to alloc canvas buffer");
        return;
    }

    splash_container = lv_obj_create(parent);
    lv_obj_set_size(splash_container, 480, 480);
    lv_obj_set_pos(splash_container, 0, 0);
    lv_obj_set_style_bg_color(splash_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(splash_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(splash_container, 0, 0);
    lv_obj_set_style_pad_all(splash_container, 0, 0);
    lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(splash_container);
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    // Placeholder label (visible only when no animations are loaded)
    label_status = lv_label_create(splash_container);
    lv_label_set_text(label_status,
        "no animations loaded\n\n"
        "run tools/scrape_claudepix.js\n"
        "then tools/convert_to_c.js");
    lv_obj_set_style_text_font(label_status, &font_styrene_28, 0);
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xb0aea5), 0);
    lv_obj_set_style_text_align(label_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_status);

    resolve_group_lists();

    if (SPLASH_ANIM_COUNT == 0) {
        show_placeholder();
    } else {
        lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        const splash_anim_def_t *a = &splash_anims[0];
        render_frame(a->frames[0], a->palette);
        frame_started_ms = millis();
    }

    lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
}

void splash_tick(void) {
    int total = SPLASH_ANIM_COUNT + (s_custom_valid ? 1 : 0);
    if (!active || total == 0) return;

    if (millis() - last_pick_ms >= SPLASH_ROTATE_INTERVAL_MS) {
        splash_pick_for_current_rate();
    }

    const splash_anim_def_t *a = get_anim(cur_anim);
    if (!a || a->frame_count == 0) return;

    uint16_t hold = a->holds[cur_frame];
    if (millis() - frame_started_ms >= hold) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_started_ms = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void splash_next(void) {
    int total = SPLASH_ANIM_COUNT + (s_custom_valid ? 1 : 0);
    if (total == 0) return;
    cur_anim = (cur_anim + 1) % total;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = get_anim(cur_anim);
    if (a) render_frame(a->frames[0], a->palette);
    Serial.printf("splash: -> %s\n", a ? a->name : "?");
}

void splash_pick_for_current_rate(void) {
    if (SPLASH_ANIM_COUNT == 0 && !s_custom_valid) return;

    if (s_pinned_name[0] != '\0') {
        // Check custom anim first
        if (s_custom_valid && strcmp(s_custom_name, s_pinned_name) == 0) {
            cur_anim = (uint16_t)SPLASH_ANIM_COUNT;
            cur_frame = 0;
            frame_started_ms = millis();
            last_pick_ms = frame_started_ms;
            if (active) render_frame(s_custom_def.frames[0], s_custom_def.palette);
            return;
        }
        for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
            if (strcmp(splash_anims[i].name, s_pinned_name) == 0) {
                cur_anim = (uint16_t)i;
                cur_frame = 0;
                frame_started_ms = millis();
                last_pick_ms = frame_started_ms;
                if (active) render_frame(splash_anims[i].frames[0], splash_anims[i].palette);
                return;
            }
        }
        Serial.printf("splash: pinned '%s' not found, clearing pin\n", s_pinned_name);
        s_pinned_name[0] = '\0';
        extern void web_server_clear_splash_pin(void);
        web_server_clear_splash_pin();
        return;
    }

    if (SPLASH_ANIM_COUNT == 0) return;
    int g = usage_rate_group();
    if (g < 0 || g >= GROUP_COUNT) g = 0;
    if (group_size[g] == 0) return;

    uint8_t slot = group_rotation[g] % group_size[g];
    group_rotation[g]++;
    int8_t idx = group_lists[g][slot];
    if (idx < 0) return;

    cur_anim = (uint16_t)idx;
    cur_frame = 0;
    frame_started_ms = millis();
    last_pick_ms = frame_started_ms;
    const splash_anim_def_t *a = &splash_anims[cur_anim];
    render_frame(a->frames[0], a->palette);
}

void splash_pin_by_name(const char* name) {
    if (!name || name[0] == '\0') {
        s_pinned_name[0] = '\0';
        Serial.println("splash: unpinned (auto mode)");
        return;
    }
    strlcpy(s_pinned_name, name, sizeof(s_pinned_name));
    // Check custom anim
    if (s_custom_valid && strcmp(s_custom_name, name) == 0) {
        cur_anim = (uint16_t)SPLASH_ANIM_COUNT;
        cur_frame = 0;
        frame_started_ms = millis();
        last_pick_ms = frame_started_ms;
        if (active) render_frame(s_custom_def.frames[0], s_custom_def.palette);
        Serial.printf("splash: pinned custom -> %s\n", name);
        return;
    }
    for (int i = 0; i < SPLASH_ANIM_COUNT; i++) {
        if (strcmp(splash_anims[i].name, name) == 0) {
            cur_anim = (uint16_t)i;
            cur_frame = 0;
            frame_started_ms = millis();
            last_pick_ms = frame_started_ms;
            if (active) render_frame(splash_anims[i].frames[0], splash_anims[i].palette);
            Serial.printf("splash: pinned -> %s\n", name);
            return;
        }
    }
    Serial.printf("splash: unknown anim '%s'\n", name);
}

const char* splash_get_pinned_name(void) { return s_pinned_name; }

int splash_anim_count(void) { return SPLASH_ANIM_COUNT + (s_custom_valid ? 1 : 0); }

const char* splash_anim_name(int idx) {
    const splash_anim_def_t* a = get_anim(idx);
    return a ? a->name : nullptr;
}

const char* splash_anim_category(int idx) {
    const splash_anim_def_t* a = get_anim(idx);
    return a ? a->category : nullptr;
}

bool splash_is_active(void) { return active; }

void splash_show(void) {
    splash_pick_for_current_rate();
    if (splash_container) lv_obj_clear_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void splash_hide(void) {
    if (splash_container) lv_obj_add_flag(splash_container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

lv_obj_t* splash_get_root(void) {
    return splash_container;
}
