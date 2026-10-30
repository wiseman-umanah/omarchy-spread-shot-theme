#include "stage.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const stage_def STAGES[NUM_STAGES] = {
    {"JUNGLE", NULL, true, rgb(6, 13, 9), rgb(6, 13, 9), rgb(10, 22, 14), rgb(10, 22, 14), rgb(18, 40, 18),
     rgb(16, 34, 20), rgb(20, 14, 10), rgb(34, 24, 16), rgb(30, 70, 30), rgb(18, 40, 18), rgb(26, 19, 13),
     rgb(40, 29, 20), WX_NONE, B_WALL},
    {"SNOWFIELD", NULL, true, rgb(14, 18, 28), rgb(70, 78, 96), rgb(24, 34, 46), rgb(90, 100, 120),
     rgb(100, 108, 126), rgb(80, 88, 104), rgb(34, 38, 48), rgb(54, 60, 74), rgb(130, 150, 180), rgb(100, 116, 140),
     rgb(40, 46, 60), rgb(60, 68, 86), WX_SNOW, B_TANK},
    {"ENEMY BASE", NULL, false, rgb(16, 16, 22), rgb(70, 62, 24), rgb(22, 24, 30), rgb(60, 64, 76),
     rgb(110, 92, 18), rgb(20, 20, 20), rgb(26, 28, 34), rgb(60, 62, 70), rgb(90, 90, 100), rgb(60, 60, 70),
     rgb(40, 42, 48), rgb(70, 72, 80), WX_NONE, B_REACTOR},
    {"ALIEN HIVE", "THE HIVE HAS FALLEN", false, rgb(24, 5, 20), rgb(60, 12, 44), rgb(36, 10, 30),
     rgb(90, 40, 70), rgb(120, 24, 70), rgb(80, 16, 50), rgb(36, 6, 22), rgb(70, 14, 44), rgb(150, 40, 100),
     rgb(110, 26, 76), rgb(50, 10, 35), rgb(80, 20, 56), WX_SPORES, B_HEART},
};

int stage;
const stage_def *cur = &STAGES[0];

// Layer pixels: 0 = empty, 1 = base color, 2 = highlight color.
static uint8_t far_px[H][TILE], mid_px[H][TILE];
static px ground_tile[H - GROUND_Y][TILE];
static int towers[8][2], n_towers; // warning-light positions (tile x, y) on the base stage

typedef struct {
    int x, y, phase, period;
} star;
static star stars[70];

static int wrapx(int x) { return ((x % TILE) + TILE) % TILE; }

static void far_set(int x, int y, uint8_t v) {
    if (y >= 0 && y < H) far_px[y][wrapx(x)] = v;
}

static void mid_set(int x, int y, uint8_t v) {
    if (y >= 0 && y < H) mid_px[y][wrapx(x)] = v;
}

static int wrap_dist(int a, int b) {
    int d = abs(a - b) % TILE;
    return d < TILE - d ? d : TILE - d;
}

// Sum of sines with integer frequencies, so the curve tiles seamlessly.
static double periodic(int x, double base, const double terms[][3], int n) {
    double v = base;
    for (int i = 0; i < n; i++) v += terms[i][0] * sin(2 * M_PI * terms[i][1] * x / TILE + terms[i][2]);
    return v;
}

static void mid_band(double base, const double terms[][3], int n) {
    for (int x = 0; x < TILE; x++)
        for (int y = (int)periodic(x, base, terms, n); y < H; y++) mid_px[y][x] = 1;
}

static void palm(int cx, int top, int lean) {
    for (int y = top; y < GROUND_Y; y++) {
        double t = (double)(GROUND_Y - y) / (GROUND_Y - top);
        int x = cx + (int)(lean * t * t);
        mid_set(x, y, 1);
        mid_set(x + 1, y, 1);
    }
    int hx = cx + lean;
    static const int angs[] = {15, 40, 70, 110, 140, 165};
    for (int k = 0; k < 6; k++) {
        double a = angs[k] * M_PI / 180.0;
        for (int i = 0; i < 18; i++) {
            int fx = hx + (int)(cos(a) * i);
            int fy = top + (int)(-sin(a) * i * 0.7 + i * i * 0.06);
            for (int dy = 0; dy < (i < 10 ? 3 : 2); dy++) mid_set(fx, fy + dy, 1);
        }
    }
}

static void gen_jungle(void) {
    static const double far_t[][3] = {{6, 1, 0.3}, {3, 3, 1.7}, {2, 7, 2.2}};
    static const double mid_t[][3] = {{4, 2, 0.9}, {2, 5, 0.1}, {1.5, 11, 2.0}};
    for (int x = 0; x < TILE; x++)
        for (int y = (int)periodic(x, 122, far_t, 3); y < H; y++) far_px[y][x] = 1;
    mid_band(146, mid_t, 3);
    palm(20, 92, 6);
    palm(95, 104, -5);
    palm(150, 88, 4);
    palm(205, 100, -7);
}

static void gen_snow(void) {
    static const int peaks[][2] = {{20, 70}, {75, 94}, {130, 62}, {188, 84}, {226, 100}};
    for (int x = 0; x < TILE; x++) {
        int top = 140;
        for (int k = 0; k < 5; k++) {
            int t = peaks[k][1] + (int)(wrap_dist(x, peaks[k][0]) * 0.85);
            if (t < top) top = t;
        }
        for (int y = top; y < H; y++) far_px[y][x] = (y < top + 4 && top < 100) ? 2 : 1; // snow caps
    }
    static const double mid_t[][3] = {{3, 2, 0.4}, {2, 5, 1.3}};
    mid_band(150, mid_t, 2);
    static const int pines[][2] = {{10, 44}, {48, 34}, {90, 50}, {122, 38}, {170, 46}, {206, 36}};
    for (int k = 0; k < 6; k++) {
        int cx = pines[k][0], top = GROUND_Y - pines[k][1];
        for (int y = GROUND_Y - 8; y < GROUND_Y; y++) mid_set(cx, y, 1), mid_set(cx + 1, y, 1);
        for (int y = top; y < GROUND_Y - 6; y++) {
            int rel = y - top, hw = 1 + (rel % 12) * 6 / 10 + rel / 6;
            for (int dx = -hw; dx <= hw + 1; dx++) mid_set(cx + dx, y, (rel % 12) < 2 ? 2 : 1);
        }
    }
}

static void gen_base(void) {
    for (int x = 0; x < TILE;) {
        int bw = rrange(10, 27), top = 150 - rrange(30, 75);
        for (int i = x; i < x + bw && i < TILE; i++)
            for (int y = top; y < H; y++) far_px[y][i] = 1;
        for (int wy = top + 3; wy < 146; wy += 5) // lit windows
            for (int wx = x + 2; wx < x + bw - 2 && wx < TILE; wx += 4)
                if (rnd() % 3 == 0) far_set(wx, wy, 2), far_set(wx, wy + 1, 2);
        x += bw + rrange(0, 4);
    }
    for (int x = 0; x < TILE; x++)
        for (int y = 140; y < 146; y++) mid_px[y][x] = y == 140 ? 2 : 1;
    static const int tw[][2] = {{30, 70}, {110, 84}, {180, 64}};
    n_towers = 3;
    for (int k = 0; k < 3; k++) {
        int x0 = tw[k][0], top = tw[k][1];
        for (int y = top; y < GROUND_Y; y++)
            for (int dx = 0; dx < 10; dx++) {
                bool edge = dx == 0 || dx == 9, lattice = (dx + y) % 8 == 0 || (dx - y + 800) % 8 == 0;
                if (y < top + 3) mid_set(x0 + dx, y, y == top ? 2 : 1);
                else if (edge || lattice) mid_set(x0 + dx, y, 1);
            }
        towers[k][0] = x0 + 4;
        towers[k][1] = top - 2;
    }
}

static void gen_alien(void) {
    for (int x = 0; x < TILE; x++) {
        int top = 118 - (int)(18 * fabs(sin(M_PI * 3 * x / TILE)) + 6 * fabs(sin(M_PI * 7 * x / TILE + 1)));
        for (int y = top; y < H; y++) far_px[y][x] = (y > top + 3 && (x * 3 + y * 5) % 23 == 0) ? 2 : 1; // veins
    }
    static const double mid_t[][3] = {{3, 3, 0.2}, {2, 7, 1.1}};
    mid_band(152, mid_t, 2);
    static const int ribs[] = {20, 80, 140, 200};
    for (int k = 0; k < 4; k++)
        for (int s = 0; s <= 60; s++) {
            double t = M_PI * s / 60;
            int x = ribs[k] + (int)(cos(t) * 22), y = GROUND_Y - (int)(sin(t) * 50);
            for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 3; dx++) mid_set(x + dx, y + dy, dy == 2 ? 2 : 1);
            if (s % 10 == 5) // spikes along the rib
                for (int i = 1; i < 5; i++) mid_set(x + 1, y - i, 1);
        }
}

static void build_ground(void) {
    for (int r = 0; r < H - GROUND_Y; r++)
        for (int tx = 0; tx < TILE; tx++) {
            int y = GROUND_Y + r;
            px c = cur->gnd;
            switch (stage) {
            case 0: // dirt with a grass edge
                if (r < 2) c = ((tx / 3 + y) % 5) ? cur->gnd_top : cur->gnd_top2;
                else if ((tx * 7 + y * 13) % 29 == 0 || (tx % 32 == 5 && y % 6 == 1)) c = cur->gnd_dot;
                break;
            case 1: // packed snow
                if (r < 2) c = ((tx / 5 + r) % 7) ? cur->gnd_top : cur->gnd_top2;
                else if ((tx * 5 + y * 11) % 23 == 0) c = cur->gnd_dot;
                break;
            case 2: // steel plates under a hazard stripe
                if (r < 3) c = (((tx + r) / 4) % 2) ? cur->gnd_top : cur->gnd_top2;
                else if (tx % 24 == 0) c = rgb(14, 14, 18);
                else if (tx % 24 == 3 && r % 8 == 5) c = cur->gnd_dot;
                break;
            default: // veined flesh
                if (r < 2) c = ((tx / 4 + r) % 6) ? cur->gnd_top : cur->gnd_top2;
                else if ((tx * 3 + y * 7) % 19 == 0 || (tx + (int)(4 * sin(y * 0.6))) % 37 == 0) c = cur->gnd_dot;
                break;
            }
            ground_tile[r][tx] = c;
        }
}

void stage_init(void) {
    static const int periods[] = {60, 120, 240};
    for (int i = 0; i < 70; i++) stars[i] = (star){rrange(0, W), rrange(8, 110), rrange(0, 480), periods[rrange(0, 3)]};
}

void stage_build(int n) {
    static void (*const gens[NUM_STAGES])(void) = {gen_jungle, gen_snow, gen_base, gen_alien};
    stage = n;
    cur = &STAGES[n];
    memset(far_px, 0, sizeof far_px);
    memset(mid_px, 0, sizeof mid_px);
    n_towers = 0;
    gens[n]();
    build_ground();
}

void stage_draw_background(long tick, float scroll) {
    if (cur->stars)
        for (int i = 0; i < 70; i++) {
            double t = (double)((tick + stars[i].phase) % stars[i].period) / stars[i].period;
            int lvl = 40 + (int)(110 * (0.5 + 0.5 * sin(2 * M_PI * t)));
            put(stars[i].x, stars[i].y, rgb(lvl, lvl, lvl + 12));
        }

    int far_off = (int)(scroll * 0.25f) % TILE;
    int mid_off = (int)(scroll * 0.5f) % TILE;
    int gnd_off = (int)scroll % TILE;
    px far_c[3] = {0, cur->far, cur->far_hi}, mid_c[3] = {0, cur->mid, cur->mid_hi};

    for (int y = 40; y < GROUND_Y; y++)
        for (int x = 0; x < W; x++) {
            uint8_t m = mid_px[y][(x + mid_off) % TILE];
            if (m) {
                fb[y][x] = mid_c[m];
                continue;
            }
            uint8_t f = far_px[y][(x + far_off) % TILE];
            if (f) fb[y][x] = far_c[f];
        }

    for (int r = 0; r < H - GROUND_Y; r++)
        for (int x = 0; x < W; x++) fb[GROUND_Y + r][x] = ground_tile[r][(x + gnd_off) % TILE];

    // Blinking warning lights on top of the base's radio towers.
    if ((tick / 15) % 2)
        for (int k = 0; k < n_towers; k++)
            for (int sx = wrapx(towers[k][0] - mid_off); sx < W; sx += TILE) fill_rect(sx, towers[k][1], 2, 2, C_RED);
}

void stage_draw_weather(long tick) {
    if (cur->weather == WX_SNOW) {
        for (int i = 0; i < 80; i++) {
            int fx = (i * 97) % W, fy = (i * 53) % H, sp = 1 + i % 3;
            int x = ((fx - (int)(tick * (0.4f + sp * 0.3f)) + (int)(sinf(tick * 0.05f + i) * 2)) % W + W) % W;
            int y = (fy + (int)(tick * sp * 0.5f)) % H;
            put(x, y, sp == 3 ? rgb(200, 206, 220) : rgb(110, 116, 132));
        }
    } else if (cur->weather == WX_SPORES) {
        for (int i = 0; i < 40; i++) {
            int fx = (i * 89) % W, fy = (i * 61) % H;
            int y = ((fy - (int)(tick * 0.3f)) % H + H) % H;
            int x = fx + (int)(sinf(tick * 0.04f + i) * 3);
            int lvl = 90 + (int)(60 * sinf(tick * 0.1f + i));
            put(x, y, rgb(lvl, lvl / 4, lvl));
        }
    }
}
