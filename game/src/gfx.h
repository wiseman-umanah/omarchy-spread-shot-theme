// Framebuffer, drawing primitives, sprites, the 5x7 font and the RNG.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The game renders at this native resolution and is scaled up nearest-neighbor.
#define W 342
#define H 192

typedef uint32_t px; // 0x00RRGGBB

#define rgb(r, g, b) ((px)(((r) << 16) | ((g) << 8) | (b)))

#define C_BLACK rgb(0, 0, 0)
#define C_RED rgb(224, 24, 24)
#define C_DARK_RED rgb(90, 0, 0)
#define C_ORANGE rgb(255, 128, 0)
#define C_YELLOW rgb(255, 216, 64)
#define C_WHITE rgb(255, 244, 220)
#define C_SMOKE rgb(48, 22, 14)
#define C_TEXT rgb(200, 200, 200)
#define C_DIM rgb(110, 110, 110)
#define C_OUTLINE rgb(14, 14, 18)

extern px fb[H][W];

void put(int x, int y, px c);
void fill_rect(int x, int y, int w, int h, px c);
void fill_circle(int cx, int cy, int r, px c);
void fade_framebuffer(float level); // 0 = black, 1 = unchanged

// A sprite is an array of strings; each character indexes a palette entry ('.' is transparent).
typedef struct {
    px c[128];
    bool on[128];
} palette;

void pal_set(palette *p, char ch, px c);
void draw_sprite(const char **rows, int nrows, int x, int y, bool flip, const palette *p);

int text_width(const char *s, int scale);
void draw_text(const char *s, int x, int y, int scale, px color);
void draw_text_shadow(const char *s, int x, int y, int scale, px color);
void draw_text_centered(const char *s, int y, int scale, px color);
bool glyph_pixel(char c, int gx, int gy); // true if the font glyph has pixel (gx, gy) set

void rng_seed(uint32_t seed);
uint32_t rnd(void);
int rrange(int lo, int hi); // [lo, hi)
