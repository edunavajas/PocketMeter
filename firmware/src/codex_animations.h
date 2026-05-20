// Codex cloud pixel-art animations — 20×20 palette-indexed, 24× upscale.
// Palette: teal (#10a37f), highlight (#4ec9a4), shadow (#0d6b54), black bg.
// Format matches splash_animations.h (codex_anim_def_t is compatible).
#pragma once
#include <stdint.h>

#define CODEX_PALETTE_SIZE 10

typedef struct {
    const char *name;
    const char *category;
    uint16_t frame_count;
    const uint16_t *palette;
    const uint8_t (*frames)[400];
    const uint16_t *holds;
} codex_anim_def_t;

// RGB565: 0=black bg, 1=#10a37f main teal, 2=#4ec9a4 highlight, 3=#0d6b54 dark eye
static const uint16_t codex_anim_palette[10] = {
    0x0000, 0x150F, 0x4E54, 0x0B4A,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

// ---- Row shape macros (20 values each) ----
// Cloud body: rows 3-12. Three distinct bumps at row 3 (cols 2-5, 8-11, 14-17).
// Row 4 connects them into a full-width cloud body.
// Eyes at rows 6-7, highlights at rows 5-7.
#define CL_E   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
// Row 3: three isolated bumps (L=cols2-5, C=cols8-11, R=cols14-17)
#define CL_R3  0,0,1,1,1,1,0,0,1,1,1,1,0,0,1,1,1,1,0,0
// Row 4: full cloud body, bumps join
#define CL_R4  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0
// Row 5: highlights (cheek areas at 3-4 and 14-15)
#define CL_R5  0,1,1,2,2,1,1,1,1,1,1,1,1,1,2,2,1,1,1,0
// Rows 6-7: eyes (dark at cols 6-7 and 12-13, inside highlights)
#define CL_R6  0,1,1,2,2,1,3,3,1,1,1,1,3,3,2,2,1,1,1,0
#define CL_R7  0,1,1,2,2,1,3,3,1,1,1,1,3,3,2,2,1,1,1,0
// Rows 8-11: solid body
#define CL_R8  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0
#define CL_R9  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0
#define CL_RA  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0
#define CL_RB  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0
// Row 12: taper bottom
#define CL_RC  0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0
// Extra: center-bump extension for "breathe" puff (cols 8-11 only)
#define CL_RT  0,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0

// ---- Frame macros: 20 rows × 20 cols = 400 values ----

// Base: cloud at neutral position (rows 3-12)
#define FRAME_BASE \
    CL_E,  CL_E,  CL_E,  CL_R3, CL_R4, CL_R5, CL_R6, CL_R7, \
    CL_R8, CL_R9, CL_RA, CL_RB, CL_RC, CL_E,  CL_E,  CL_E,  \
    CL_E,  CL_E,  CL_E,  CL_E

// Up 1px: each display row gets the next row's content (cloud floats up)
#define FRAME_UP1 \
    CL_E,  CL_E,  CL_R3, CL_R4, CL_R5, CL_R6, CL_R7, CL_R8, \
    CL_R9, CL_RA, CL_RB, CL_RC, CL_E,  CL_E,  CL_E,  CL_E,  \
    CL_E,  CL_E,  CL_E,  CL_E

// Down 1px: each display row gets the previous row's content (cloud sinks)
#define FRAME_DN1 \
    CL_E,  CL_E,  CL_E,  CL_E,  CL_R3, CL_R4, CL_R5, CL_R6, \
    CL_R7, CL_R8, CL_R9, CL_RA, CL_RB, CL_RC, CL_E,  CL_E,  \
    CL_E,  CL_E,  CL_E,  CL_E

// Tall (breathe puff): center bump extends 1 row higher at row 2
#define FRAME_TALL \
    CL_E,  CL_E,  CL_RT, CL_R3, CL_R4, CL_R5, CL_R6, CL_R7, \
    CL_R8, CL_R9, CL_RA, CL_RB, CL_RC, CL_E,  CL_E,  CL_E,  \
    CL_E,  CL_E,  CL_E,  CL_E

// ---- "idle bob" animation: 6 frames, cloud bobs up and down ----
static const uint8_t codex_idle_frames[6][400] = {
    { FRAME_BASE },
    { FRAME_UP1  },
    { FRAME_UP1  },
    { FRAME_BASE },
    { FRAME_DN1  },
    { FRAME_DN1  },
};
static const uint16_t codex_idle_holds[6] = {700, 300, 250, 700, 300, 250};

// ---- "breathe" animation: 4 frames, center bump puffs taller ----
static const uint8_t codex_breathe_frames[4][400] = {
    { FRAME_BASE },
    { FRAME_TALL },
    { FRAME_TALL },
    { FRAME_BASE },
};
static const uint16_t codex_breathe_holds[4] = {500, 300, 300, 500};

// ---- Catalog ----
static const codex_anim_def_t codex_anims[] = {
    {"idle bob", "idle",    6, codex_anim_palette, codex_idle_frames,    codex_idle_holds},
    {"breathe",  "breathe", 4, codex_anim_palette, codex_breathe_frames, codex_breathe_holds},
};
#define CODEX_ANIM_COUNT (int)(sizeof(codex_anims) / sizeof(codex_anims[0]))
