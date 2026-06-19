#pragma once
#include <stdint.h>
#include <lvgl.h>

// Initialize splash module. Creates the canvas widget inside `parent` and
// allocates the 480x480 pixel buffer (PSRAM).
void splash_init(lv_obj_t *parent);

// Advance animation frame if hold time elapsed. Call from main loop.
void splash_tick(void);

// Cycle to the next animation in the catalog.
void splash_next(void);

// Show/hide the splash container.
void splash_show(void);
void splash_hide(void);

// Pick the next animation matching the current usage-rate group.
// Called automatically by splash_show(); also exposed so other modules can
// trigger a re-pick when the rate group changes mid-display.
void splash_pick_for_current_rate(void);

// True when splash is currently rendering (used to gate re-picks).
bool splash_is_active(void);

// Root container (so ui.cpp can attach a click event).
lv_obj_t* splash_get_root(void);

// Pin a specific animation by name, bypassing rate-based selection.
// Pass "" or NULL to unpin and return to automatic selection.
void splash_pin_by_name(const char* name);

// Returns the currently pinned animation name, or "" if in auto mode.
const char* splash_get_pinned_name(void);

// Returns the total number of available animations.
int splash_anim_count(void);
// Returns the name of animation at index idx, or NULL if out of range.
const char* splash_anim_name(int idx);
// Returns the category of animation at index idx, or NULL if out of range.
const char* splash_anim_category(int idx);

// Load a custom animation at runtime (e.g. converted from a PetDex spritesheet).
// provider:   "claude", "codex", etc. — used to route the animation to the right splash screen.
// frames_str: flat string of digit chars '0'-'9', exactly frame_count*400 chars.
// palette:    10 RGB565 values.
// holds:      frame_count hold times in ms (NULL → 150 ms each).
// Returns true on success; replaces any previously loaded custom animation.
bool splash_set_custom(const char* name, const char* category, const char* provider,
                       const uint16_t palette[10],
                       const char* frames_str, uint16_t frame_count,
                       const uint16_t* holds);

// True if a custom animation has been loaded via splash_set_custom().
bool splash_has_custom(void);
// True if a custom animation for the given provider has been loaded.
bool splash_has_custom_for(const char* provider);

// Returns the provider that owns the generic custom splash slot, or "".
const char* splash_custom_provider(void);

// Accessors for the custom animation data (for use by other splash renderers).
const uint8_t*  splash_custom_frame_pixels(int frame_idx);
const uint16_t* splash_custom_palette(void);
uint16_t        splash_custom_frame_count(void);
uint16_t        splash_custom_hold_ms(int frame_idx);
