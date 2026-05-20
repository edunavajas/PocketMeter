#include "codex_splash.h"
#include "codex_animations.h"
#include "theme.h"
#include <Arduino.h>
#include <string.h>
#include <esp_heap_caps.h>

#define GRID     20
#define CELL     24
#define CANVAS_W (GRID * CELL)
#define CANVAS_H (GRID * CELL)

static lv_obj_t  *container  = NULL;
static lv_obj_t  *canvas     = NULL;
static uint16_t  *canvas_buf = NULL;
static uint16_t   cur_anim   = 0;
static uint16_t   cur_frame  = 0;
static uint32_t   frame_ms   = 0;
static bool       active     = false;

static void render_frame(const uint8_t *cells, const uint16_t *palette) {
    for (int gy = 0; gy < GRID; gy++) {
        uint16_t row[CANVAS_W];
        for (int gx = 0; gx < GRID; gx++) {
            uint8_t code = cells[gy * GRID + gx];
            uint16_t color = (palette && code < CODEX_PALETTE_SIZE) ? palette[code] : 0x0000;
            uint16_t *p = &row[gx * CELL];
            for (int i = 0; i < CELL; i++) p[i] = color;
        }
        for (int dy = 0; dy < CELL; dy++)
            memcpy(&canvas_buf[(gy * CELL + dy) * CANVAS_W], row, CANVAS_W * 2);
    }
    if (canvas) lv_obj_invalidate(canvas);
}

void codex_splash_init(lv_obj_t *parent) {
    canvas_buf = (uint16_t*)heap_caps_malloc(CANVAS_W * CANVAS_H * 2, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        Serial.println("codex_splash: alloc failed");
        return;
    }

    container = lv_obj_create(parent);
    lv_obj_set_size(container, 480, 480);
    lv_obj_set_pos(container, 0, 0);
    lv_obj_set_style_bg_color(container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    canvas = lv_canvas_create(container);
    lv_canvas_set_buffer(canvas, canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas);

    if (CODEX_ANIM_COUNT > 0) {
        render_frame(codex_anims[0].frames[0], codex_anims[0].palette);
        frame_ms = millis();
    }

    lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

void codex_splash_tick(void) {
    if (!active || CODEX_ANIM_COUNT == 0) return;
    const codex_anim_def_t *a = &codex_anims[cur_anim];
    if (a->frame_count == 0) return;
    if (millis() - frame_ms >= a->holds[cur_frame]) {
        cur_frame = (cur_frame + 1) % a->frame_count;
        frame_ms  = millis();
        render_frame(a->frames[cur_frame], a->palette);
    }
}

void codex_splash_next(void) {
    if (CODEX_ANIM_COUNT == 0) return;
    cur_anim  = (cur_anim + 1) % CODEX_ANIM_COUNT;
    cur_frame = 0;
    frame_ms  = millis();
    render_frame(codex_anims[cur_anim].frames[0], codex_anims[cur_anim].palette);
    Serial.printf("codex_splash: -> %s\n", codex_anims[cur_anim].name);
}

void codex_splash_show(void) {
    if (CODEX_ANIM_COUNT > 0) {
        cur_frame = 0;
        frame_ms  = millis();
        render_frame(codex_anims[cur_anim].frames[0], codex_anims[cur_anim].palette);
    }
    if (container) lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
    active = true;
}

void codex_splash_hide(void) {
    if (container) lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
    active = false;
}

bool codex_splash_is_active(void) { return active; }

lv_obj_t* codex_splash_get_root(void) { return container; }
