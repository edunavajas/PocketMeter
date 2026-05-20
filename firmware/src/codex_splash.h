#pragma once
#include <stdint.h>
#include <lvgl.h>

void codex_splash_init(lv_obj_t *parent);
void codex_splash_tick(void);
void codex_splash_next(void);
void codex_splash_show(void);
void codex_splash_hide(void);
bool codex_splash_is_active(void);
lv_obj_t* codex_splash_get_root(void);
