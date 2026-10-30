#include "art.h"

#include <math.h>
#include <string.h>

palette pal_player, pal_capsule;
palette pal_grunt[NUM_STAGES], pal_sniper[NUM_STAGES];

const char *PLAYER_TORSO[14] = {
    "....KKKKK.......", "...KSSSSSK......", "..KRRRRRRK......", ".RRKSSSSSK......",
    "RR.KSSKSWK......", "R...KsSSK.......", "....KSSK........", "...KSSSSK.......",
    "..KSSSSSSKKKKKKK", "..KSsSSSSGGGGGGG", "..KSSSSSKgggKKK.", "...KsSSSK.......",
    "...KBBBBK.......", "...KbBBbK.......",
};

const char *LEGS[4][6] = {
    {"..KBBKKBBK......", ".KBBK..KBBK.....", ".KBK....KBBK....", "KBK......KBK....", "KbK.......KbK...",
     "KK.........KK..."},
    {"...KBBBBK.......", "...KBBKBK.......", "...KBKKBK.......", "...KBK.KBK......", "...KbK.KbK......",
     "...KK...KK......"},
    {"...KBBKBBK......", "..KBBK.KBK......", "..KBK..KBK......", ".KBK...KBK......", ".KbK....KbK.....",
     ".KK.....KK......"},
    {"...KBBBBK.......", "....KBBK........", "....KBBK........", "....KBKBK.......", "....KbKbK.......",
     "....KKKK........"},
};

const char *PLAYER_PRONE[7] = {
    "............KKKK......", "...........KRRRRK.....", "..........RKSSSSK.....",
    "KKKKKKKKKKKSSSWSKKKKKK", "KBBBBBBBBKSSSSSSGGGGGG", "KbBBBBBBBKSSSSSKgggKK.",
    "KKKKKKKKKKKKKKKK......",
};

const char *GRUNT_TORSO[14] = {
    "....KKKKK.......", "...KHHHHHK......", "..KHHHHHHHK.....", "..KKSSSSSKK.....",
    "....KSSKSK......", "....KsSSK.......", "....KSSK........", "...KCCCCK.......",
    "..KCCCCCCKKKKKKK", "..KCcCCCCGGGGGGG", "..KCCCCCKgggKKK.", "...KcCCCK.......",
    "...KBBBBK.......", "...KbBBbK.......",
};

const char *SNIPER_KNEEL[4] = {
    "..KBBBBBK.......",
    "..KBBKKBBBK.....",
    "..KbK..KbbK.....",
    "..KK....KKK.....",
};

const char *CAPSULE[7] = {
    "....KKKKKK....", "..KKGGGGGGKK..", "KKgGRRRRRRGgKK", "KGGRRYYYYRRGGK",
    "KKgGRRRRRRGgKK", "..KKGGGGGGKK..", "....KKKKKK....",
};

// Reskins a soldier: helmet, shirt (+shade), pants (+shade), skin (+shade).
static void soldier_palette(palette *p, px helm, px shirt, px shirt_s, px pants, px pants_s, px skin, px skin_s) {
    memset(p, 0, sizeof *p);
    pal_set(p, 'K', C_OUTLINE);
    pal_set(p, 'H', helm);
    pal_set(p, 'C', shirt);
    pal_set(p, 'c', shirt_s);
    pal_set(p, 'B', pants);
    pal_set(p, 'b', pants_s);
    pal_set(p, 'S', skin);
    pal_set(p, 's', skin_s);
    pal_set(p, 'G', rgb(110, 110, 118));
    pal_set(p, 'g', rgb(70, 70, 78));
    pal_set(p, 'W', rgb(255, 255, 255));
}

// ---------------------------------------------------------------------------
// Logo: "SPREAD SHOT" in the 5x7 font at 3x, with a fire gradient
// ---------------------------------------------------------------------------

#define LOGO_TEXT "SPREAD SHOT"
#define LOGO_SCALE 3
#define LOGO_W ((int)(sizeof(LOGO_TEXT) - 1) * 6 * LOGO_SCALE - LOGO_SCALE)
#define LOGO_H (7 * LOGO_SCALE)

static uint8_t logo_mask[LOGO_H][LOGO_W];

int logo_width(void) { return LOGO_W; }
int logo_height(void) { return LOGO_H; }

static bool logo_at(int x, int y) { return x >= 0 && y >= 0 && x < LOGO_W && y < LOGO_H && logo_mask[y][x]; }

void draw_logo(int ox, int oy) {
    static const int offs[][2] = {{1, 1}, {2, 2}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int y = 0; y < LOGO_H; y++)
        for (int x = 0; x < LOGO_W; x++) {
            if (!logo_mask[y][x]) continue;
            for (int k = 0; k < 6; k++) {
                int nx = x + offs[k][0], ny = y + offs[k][1];
                if (!logo_at(nx, ny)) put(ox + nx, oy + ny, offs[k][1] < 2 ? C_DARK_RED : rgb(40, 0, 0));
            }
        }
    static const px grad[LOGO_H] = {
        C_YELLOW, C_YELLOW, C_YELLOW, C_ORANGE, C_ORANGE, C_ORANGE, C_ORANGE, C_RED, C_RED, C_RED, C_RED,
        C_RED, C_RED, C_RED, 0xc81010, 0xb40a0a, 0xaa0808, 0xa00000, 0x960000, 0x8c0000, 0x820000,
    };
    for (int y = 0; y < LOGO_H; y++)
        for (int x = 0; x < LOGO_W; x++)
            if (logo_mask[y][x]) put(ox + x, oy + y, grad[y]);
}

void draw_ball(int x, int y, int frame) {
    const palette *p = &pal_player;
    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 12; i++) {
            float dx = i - 5.5f, dy = j - 5.5f, d = sqrtf(dx * dx + dy * dy);
            if (d > 5.6f) continue;
            if (d > 4.6f) {
                put(x + i, y + j, p->c['K']);
                continue;
            }
            float a = atan2f(dy, dx) + frame * (float)M_PI / 2.0f;
            int q = ((int)floorf(a / ((float)M_PI / 2.0f)) % 4 + 4) % 4;
            put(x + i, y + j, (q & 1) ? p->c['B'] : p->c['S']);
        }
    }
    float a = frame * (float)M_PI / 2.0f;
    put(x + 6 + (int)(cosf(a) * 3), y + 6 + (int)(sinf(a) * 3), C_RED);
}

void art_init(void) {
    palette *p = &pal_player;
    pal_set(p, 'K', C_OUTLINE);
    pal_set(p, 'S', rgb(196, 120, 78));
    pal_set(p, 's', rgb(150, 84, 52));
    pal_set(p, 'R', C_RED);
    pal_set(p, 'B', rgb(46, 70, 160));
    pal_set(p, 'b', rgb(30, 44, 110));
    pal_set(p, 'G', rgb(130, 130, 140));
    pal_set(p, 'g', rgb(80, 80, 90));
    pal_set(p, 'W', rgb(255, 255, 255));

    p = &pal_capsule;
    pal_set(p, 'K', C_OUTLINE);
    pal_set(p, 'G', rgb(150, 150, 160));
    pal_set(p, 'g', rgb(90, 90, 100));
    pal_set(p, 'R', C_RED);
    pal_set(p, 'Y', C_YELLOW);

    px skin = rgb(170, 110, 80), skin_s = rgb(120, 74, 50);
    // Jungle: olive helmets, grey shirts.
    soldier_palette(&pal_grunt[0], rgb(70, 82, 52), rgb(96, 98, 104), rgb(66, 68, 74), rgb(60, 62, 44),
                    rgb(40, 42, 30), skin, skin_s);
    soldier_palette(&pal_sniper[0], rgb(130, 30, 30), rgb(92, 72, 46), rgb(64, 50, 32), rgb(60, 62, 44),
                    rgb(40, 42, 30), skin, skin_s);
    // Snowfield: white winter camo.
    soldier_palette(&pal_grunt[1], rgb(200, 204, 214), rgb(150, 156, 170), rgb(110, 116, 130), rgb(120, 126, 140),
                    rgb(84, 90, 104), skin, skin_s);
    soldier_palette(&pal_sniper[1], rgb(200, 40, 40), rgb(150, 156, 170), rgb(110, 116, 130), rgb(70, 76, 92),
                    rgb(50, 54, 66), skin, skin_s);
    // Enemy base: black armor, red visors.
    soldier_palette(&pal_grunt[2], rgb(34, 34, 40), rgb(54, 56, 64), rgb(36, 38, 44), rgb(44, 44, 50),
                    rgb(30, 30, 34), rgb(200, 30, 30), rgb(120, 10, 10));
    soldier_palette(&pal_sniper[2], rgb(160, 130, 20), rgb(54, 56, 64), rgb(36, 38, 44), rgb(44, 44, 50),
                    rgb(30, 30, 34), rgb(200, 30, 30), rgb(120, 10, 10));
    // Alien hive: infected troopers with green skin.
    soldier_palette(&pal_grunt[3], rgb(140, 40, 110), rgb(90, 30, 80), rgb(60, 18, 54), rgb(70, 24, 60),
                    rgb(46, 14, 40), rgb(120, 170, 100), rgb(80, 120, 64));
    soldier_palette(&pal_sniper[3], rgb(200, 60, 160), rgb(90, 30, 80), rgb(60, 18, 54), rgb(70, 24, 60),
                    rgb(46, 14, 40), rgb(120, 170, 100), rgb(80, 120, 64));

    const char *text = LOGO_TEXT;
    for (int ci = 0; text[ci]; ci++)
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++)
                if (glyph_pixel(text[ci], gx, gy))
                    for (int sy = 0; sy < LOGO_SCALE; sy++)
                        for (int sx = 0; sx < LOGO_SCALE; sx++)
                            logo_mask[gy * LOGO_SCALE + sy][ci * 6 * LOGO_SCALE + gx * LOGO_SCALE + sx] = 1;
}
