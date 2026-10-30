#include "game.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "stage.h"

#define WORLD_SPEED 1.5f
#define STAGE_LEN 3600.0f // pixels of scrolling before the boss (~80 s)
#define START_LIVES 3
#define EXTRA_LIFE_EVERY 20000
#define LIFE_BONUS 5000
#define ENDING_TICKS 330
#define PH 20 // player and grunt height

#define MAX_PLATFORMS 8
#define MAX_BULLETS 160
#define MAX_ENEMIES 32
#define MAX_EXPLOSIONS 40

typedef struct {
    float x;
    int y, w;
    bool alive;
} platform;

enum { BUL_NORMAL, BUL_SHELL, BUL_ORB };

typedef struct {
    float x, y, vx, vy, g;
    bool alive, hostile;
    int kind;
} bullet;

enum { E_GRUNT, E_SNIPER, E_CAPSULE, E_PICKUP };

typedef struct {
    int type;
    float x, y, vx, vy, base_y;
    bool alive, on_ground;
    int timer, anim;
} enemy;

typedef struct {
    int x, y, age, seed;
    float size;
    bool alive;
} explosion;

typedef struct {
    float x, y, vy;
    bool on_ground, balled, ducking, alive;
    int face, dead_timer, invuln, fire_cd, flash, weapon, weapon_timer, anim;
} player_t;

// A boss is a set of damageable parts; it falls when every "core" part is destroyed.
typedef struct {
    float rx;
    int ry, w, h, hp, max_hp, timer, flash;
    bool alive, core;
} part;

enum { BP_ARRIVING, BP_FIGHT, BP_DYING };

typedef struct {
    bool active;
    int kind, phase, timer;
    long t;
    float x, target_x;
    part parts[4];
    int nparts;
} boss_t;

enum { ST_PLAY, ST_CLEAR, ST_ENDING, ST_FADE_OUT };

bool game_mode;
input_t keys;
bool game_verbose;
bool game_persist = true;

static platform platforms[MAX_PLATFORMS];
static bullet bullets[MAX_BULLETS];
static enemy enemies[MAX_ENEMIES];
static explosion explosions[MAX_EXPLOSIONS];
static player_t pl;
static boss_t boss;

static long tick_count;
static float scroll, stage_dist, world_speed = WORLD_SPEED;
static long score, hi_score, run_start_hi, next_extra = EXTRA_LIFE_EVERY, ending_bonus;
static int lives = START_LIVES, loop_count;
static int stage_state = ST_PLAY, state_timer, fade_in, title_timer, warning_timer, game_over_timer;
static int plat_timer, grunt_timer, sniper_timer, capsule_timer;
static int ai_target_x = 80, ai_retarget, ai_jump_hold;
static bool exit_requested;

#define LOG(...)                                                                                                       \
    do {                                                                                                               \
        if (game_verbose) fprintf(stderr, "[%6ld] ", tick_count), fprintf(stderr, __VA_ARGS__);                          \
    } while (0)

static float difficulty(void) { return 1.0f + 0.3f * loop_count; }

// ---------------------------------------------------------------------------
// High score file: $XDG_STATE_HOME/spread-shot/hiscore
// ---------------------------------------------------------------------------

static bool hiscore_path(char *buf, size_t n, bool create_dir) {
    const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[512];
    if (state && *state) snprintf(dir, sizeof dir, "%s/spread-shot", state);
    else if (home) snprintf(dir, sizeof dir, "%s/.local/state/spread-shot", home);
    else return false;
    if (create_dir) {
        char parent[512];
        snprintf(parent, sizeof parent, "%s", dir);
        *strrchr(parent, '/') = '\0';
        mkdir(parent, 0755);
        if (mkdir(dir, 0755) < 0 && errno != EEXIST) return false;
    }
    snprintf(buf, n, "%s/hiscore", dir);
    return true;
}

static void load_hiscore(void) {
    char path[600];
    if (!game_persist || !hiscore_path(path, sizeof path, false)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fscanf(f, "%ld", &hi_score) != 1 || hi_score < 0) hi_score = 0;
    fclose(f);
}

void game_save_hiscore(void) {
    char path[600], tmp[620];
    if (!game_persist || hi_score <= 0 || !hiscore_path(path, sizeof path, true)) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ld\n", hi_score);
    if (fclose(f) == 0) rename(tmp, path);
}

// ---------------------------------------------------------------------------
// Spawning helpers
// ---------------------------------------------------------------------------

static bool overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void player_hitbox(float *x, float *y, float *w, float *h) {
    if (pl.ducking) *x = pl.x, *y = pl.y + 14, *w = 18, *h = 6;
    else if (pl.balled) *x = pl.x + 1, *y = pl.y + 7, *w = 10, *h = 10;
    else *x = pl.x + 3, *y = pl.y + 2, *w = 10, *h = 18;
}

static void spawn_explosion(float x, float y, float size) {
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
        if (!explosions[i].alive) {
            explosions[i] = (explosion){(int)x, (int)y, 0, (int)rnd(), size, true};
            return;
        }
}

static void spawn_bullet(float x, float y, float vx, float vy, bool hostile, int kind, float g) {
    for (int i = 0; i < MAX_BULLETS; i++)
        if (!bullets[i].alive) {
            bullets[i] = (bullet){x, y, vx, vy, g, true, hostile, kind};
            return;
        }
}

static void aimed_bullet(float x, float y, float speed, float spread, int kind) {
    float a = atan2f((pl.y + 10) - y, (pl.x + 8) - x) + spread;
    spawn_bullet(x, y, cosf(a) * speed, sinf(a) * speed, true, kind, 0);
}

static enemy *spawn_enemy(int type, float x, float y) {
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (!enemies[i].alive) {
            enemies[i] = (enemy){.type = type, .x = x, .y = y, .base_y = y, .alive = true};
            return &enemies[i];
        }
    return NULL;
}

static void player_respawn(void) { pl = (player_t){.x = 60, .y = 30, .alive = true, .face = 1, .invuln = 90}; }

static void player_die(void) {
    if (!pl.alive || pl.invuln > 0) return;
    pl.alive = false;
    pl.dead_timer = 60;
    pl.vy = -4.0f;
    pl.ducking = pl.balled = false;
    spawn_explosion(pl.x + 8, pl.y + 10, 0.7f);
    if (game_mode) {
        if (lives > 0) {
            lives--;
        } else {
            game_over_timer = 150;
            LOG("game over: score %ld\n", score);
        }
    }
}

static void clear_hostiles(void) {
    for (int i = 0; i < MAX_BULLETS; i++)
        if (bullets[i].hostile) bullets[i].alive = false;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].type != E_PICKUP) enemies[i].alive = false;
}

static void load_stage(int n) {
    stage_build(n);
    LOG("stage %d: %s\n", n + 1, cur->name);
    memset(platforms, 0, sizeof platforms);
    memset(bullets, 0, sizeof bullets);
    memset(enemies, 0, sizeof enemies);
    memset(explosions, 0, sizeof explosions);
    boss.active = false;
    scroll = stage_dist = 0;
    world_speed = WORLD_SPEED;
    plat_timer = 40;
    grunt_timer = 80;
    sniper_timer = 240;
    capsule_timer = 300;
    stage_state = ST_PLAY;
    title_timer = 110;
    warning_timer = 0;
    if (pl.alive) {
        pl.x = 60, pl.y = 30, pl.vy = 0, pl.invuln = 60;
        pl.on_ground = pl.balled = pl.ducking = false;
    }
}

// Land a falling body (feet at y + h) on the ground or a platform. Returns true if landed.
static bool land(float x, float *y, float *vy, float w, float h, float prev_feet) {
    if (*vy < 0) return false;
    float feet = *y + h;
    for (int i = 0; i < MAX_PLATFORMS; i++) {
        platform *p = &platforms[i];
        if (!p->alive) continue;
        if (prev_feet <= p->y && feet >= p->y && x + w > p->x && x < p->x + p->w) {
            *y = p->y - h;
            *vy = 0;
            return true;
        }
    }
    if (feet >= GROUND_Y) {
        *y = GROUND_Y - h;
        *vy = 0;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bosses
// ---------------------------------------------------------------------------

static void add_part(float rx, int ry, int w, int h, int hp, bool core) {
    part *p = &boss.parts[boss.nparts++];
    *p = (part){rx, ry, w, h, hp, hp, rrange(30, 70), 0, true, core};
}

static void start_boss(void) {
    boss = (boss_t){.active = true, .kind = cur->boss, .phase = BP_ARRIVING, .x = W + 4};
    int bonus = loop_count * 10;
    switch (boss.kind) {
    case B_WALL: // defense wall: two turrets guarding a core
        boss.target_x = W - 74;
        add_part(14, 70, 14, 10, 12 + bonus, false);
        add_part(14, 124, 14, 10, 12 + bonus, false);
        add_part(38, 92, 20, 24, 40 + bonus, true);
        break;
    case B_REACTOR: // three turrets and a core that is only open part of the time
        boss.target_x = W - 74;
        add_part(12, 58, 14, 10, 14 + bonus, false);
        add_part(12, 98, 14, 10, 14 + bonus, false);
        add_part(12, 138, 14, 10, 14 + bonus, false);
        add_part(40, 90, 20, 26, 55 + bonus, true);
        break;
    case B_TANK:
        boss.target_x = W - 110;
        add_part(0, GROUND_Y - 24, 52, 24, 70 + bonus, true);
        break;
    case B_HEART:
        boss.target_x = W - 84;
        add_part(14, 66, 40, 40, 100 + bonus, true);
        break;
    }
    warning_timer = 90;
}

static float part_x(const part *p) { return boss.x + p->rx; }

static bool reactor_shielded(void) { return boss.kind == B_REACTOR && (boss.t % 150) < 90; }

// Bounding box used for the death fireworks.
static void boss_bounds(float *x, float *y, float *w, float *h) {
    switch (boss.kind) {
    case B_TANK: *x = boss.x, *y = GROUND_Y - 24, *w = 52, *h = 24; break;
    case B_HEART: *x = boss.x + 10, *y = 60, *w = 48, *h = GROUND_Y - 60; break;
    default: *x = boss.x, *y = 40, *w = 74, *h = GROUND_Y - 40; break;
    }
}

static void boss_defeated(void) {
    boss.active = false;
    score += 10000;
    if (stage < NUM_STAGES - 1) {
        stage_state = ST_CLEAR;
        state_timer = 150;
        return;
    }
    // Final boss: mission complete. Remaining lives become points.
    ending_bonus = game_mode ? (long)lives * LIFE_BONUS : 0;
    score += ending_bonus;
    if (game_mode && score > hi_score) hi_score = score;
    game_save_hiscore();
    stage_state = ST_ENDING;
    state_timer = ENDING_TICKS;
    LOG("mission complete: score %ld\n", score);
}

static void update_boss(void) {
    if (!boss.active) return;
    boss.t++;
    for (int i = 0; i < boss.nparts; i++)
        if (boss.parts[i].flash > 0) boss.parts[i].flash--;

    if (boss.phase == BP_ARRIVING) {
        boss.x -= WORLD_SPEED;
        if (boss.x <= boss.target_x) {
            boss.x = boss.target_x;
            boss.phase = BP_FIGHT;
            world_speed = 0;
            LOG("stage %d: boss fight\n", stage + 1);
        }
        return;
    }

    if (boss.phase == BP_DYING) {
        float bx, by, bw, bh;
        boss_bounds(&bx, &by, &bw, &bh);
        if (boss.timer % 4 == 0)
            spawn_explosion(bx + rrange(0, (int)bw), by + rrange(0, (int)bh), 0.6f + (rnd() % 5) * 0.1f);
        if (++boss.timer >= 90) boss_defeated();
        return;
    }

    float diff = difficulty();
    if (boss.kind == B_TANK) boss.x = boss.target_x + sinf(boss.t * 0.02f) * 30;
    if (!pl.alive) return; // hold fire while the player respawns

    for (int i = 0; i < boss.nparts; i++) {
        part *p = &boss.parts[i];
        if (!p->alive || --p->timer > 0) continue;
        float cx = part_x(p) + p->w / 2.0f, cy = p->ry + p->h / 2.0f;
        switch (boss.kind) {
        case B_WALL:
        case B_REACTOR:
            if (!p->core) {
                aimed_bullet(part_x(p) - 2, cy, 2.4f * diff, 0, BUL_NORMAL);
                p->timer = (int)(rrange(50, 80) / diff);
            } else if (!reactor_shielded()) {
                for (int k = -1; k <= 1; k++)
                    spawn_bullet(part_x(p) - 2, cy, -2.2f * diff, k * 0.7f, true, BUL_NORMAL, 0);
                p->timer = (int)(90 / diff);
            } else {
                p->timer = 10;
            }
            break;
        case B_TANK: {
            // Lob an artillery shell that lands where the player stands.
            float mx = part_x(p) + 2, my = p->ry + 1, T = 40, g = 0.15f;
            float dx = (pl.x + 8) - mx, dy = (GROUND_Y - 4) - my;
            spawn_bullet(mx, my, dx / T, (dy - 0.5f * g * T * T) / T, true, BUL_SHELL, g);
            if (rnd() % 2) // low machine-gun burst: duck or jump
                for (int k = 0; k < 3; k++)
                    spawn_bullet(part_x(p), GROUND_Y - 8, -3.0f - k * 0.4f, 0, true, BUL_NORMAL, 0);
            p->timer = (int)(70 / diff);
            break;
        }
        case B_HEART:
            for (int k = -1; k <= 1; k++) aimed_bullet(cx - 12, cy, 1.7f * diff, k * 0.25f, BUL_ORB);
            p->timer = (int)(80 / diff);
            break;
        }
    }

    if (boss.kind == B_HEART && boss.t % (int)(55 / diff) == 0) {
        enemy *e = spawn_enemy(E_GRUNT, boss.x + 10, GROUND_Y - PH);
        if (e) e->vx = 0.6f; // larvae scuttle a bit faster than grunts
    }
    if (boss.kind == B_TANK) {
        float x, y, w, h;
        player_hitbox(&x, &y, &w, &h);
        part *p = &boss.parts[0];
        if (overlap(x, y, w, h, part_x(p), p->ry + 4, p->w, p->h - 4)) player_die();
    }
}

// Returns true if the bullet hit (or was absorbed by) the boss.
static bool boss_hit(const bullet *b) {
    if (!boss.active || boss.phase != BP_FIGHT) return false;
    for (int i = 0; i < boss.nparts; i++) {
        part *p = &boss.parts[i];
        if (!p->alive || !overlap(b->x - 1, b->y - 1, 3, 3, part_x(p), p->ry, p->w, p->h)) continue;
        if (p->core && reactor_shielded()) return true;
        p->flash = 2;
        score += 10;
        if (--p->hp <= 0) {
            p->alive = false;
            score += 1000;
            spawn_explosion(part_x(p) + p->w / 2.0f, p->ry + p->h / 2.0f, 1.0f);
            bool cores_left = false;
            for (int k = 0; k < boss.nparts; k++)
                if (boss.parts[k].alive && boss.parts[k].core) cores_left = true;
            if (!cores_left) {
                boss.phase = BP_DYING;
                boss.timer = 0;
                clear_hostiles();
                LOG("stage %d: boss defeated\n", stage + 1);
            }
        }
        return true;
    }
    // Wall bosses soak up shots that miss every part (the parts sit in front of x + 64).
    return (boss.kind == B_WALL || boss.kind == B_REACTOR) && b->x > boss.x + 64 && b->y > 34;
}

// ---------------------------------------------------------------------------
// Demo AI
// ---------------------------------------------------------------------------

static input_t ai_input(void) {
    input_t in = {0};
    if (!pl.alive || stage_state != ST_PLAY) return in;

    int max_x = boss.active ? (int)boss.x - 40 : 200;
    if (max_x < 40) max_x = 40;
    if (--ai_retarget <= 0 || ai_target_x > max_x) {
        ai_target_x = rrange(30, max_x);
        ai_retarget = rrange(60, 150);
    }

    // Grunts that ran past are harmless; only turn around for a sniper behind us,
    // and only when nothing is charging from the front.
    float pcx = pl.x + 8, pcy = pl.y + 10;
    bool ahead = false, sniper_behind = false;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemy *e = &enemies[i];
        if (!e->alive) continue;
        if (e->type == E_GRUNT && e->x > pcx && e->x - pcx < 140) ahead = true;
        if (e->type == E_SNIPER && e->x + 8 < pcx - 8 && e->x > 0) sniper_behind = true;
    }
    float tx = (sniper_behind && !ahead && !boss.active) ? pl.x - 16 : (float)ai_target_x;
    if ((ahead || boss.active) && pl.face < 0) tx = pl.x + 16;
    if (tx < pl.x - 3) in.left = true;
    else if (tx > pl.x + 3) in.right = true;

    // Dodge hostile bullets heading our way.
    for (int i = 0; i < MAX_BULLETS; i++) {
        bullet *b = &bullets[i];
        if (!b->alive || !b->hostile) continue;
        float dx = b->x - pcx;
        bool incoming = (dx > 0 && b->vx < 0) || (dx < 0 && b->vx > 0);
        if (!incoming || fabsf(dx) > 40) continue;
        if (b->kind == BUL_SHELL) {
            if (fabsf(dx) < 24) in.right = !(in.left = b->x >= pcx);
        } else if (fabsf(b->y - pcy) < 14) {
            if (b->y < pl.y + 12 && pl.on_ground) in.down = true;
            else ai_jump_hold = 8;
        }
    }

    // Jump over grunts that get close, and onto platforms now and then.
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemy *e = &enemies[i];
        if (e->alive && e->type == E_GRUNT && fabsf(e->x - pl.x) < 30 && fabsf(e->y - pl.y) < 12) ai_jump_hold = 10;
    }
    for (int i = 0; i < MAX_PLATFORMS; i++) {
        platform *p = &platforms[i];
        if (p->alive && p->x - pl.x > 6 && p->x - pl.x < 36 && pl.on_ground && rnd() % 25 == 0) ai_jump_hold = 12;
    }
    // Go for the weapon pickup.
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].alive && enemies[i].type == E_PICKUP) {
            in.left = enemies[i].x < pl.x - 3;
            in.right = enemies[i].x > pl.x + 3;
        }

    if (ai_jump_hold > 0) {
        ai_jump_hold--;
        in.up = true;
        in.down = false;
    }
    return in;
}

// ---------------------------------------------------------------------------
// Simulation step
// ---------------------------------------------------------------------------

static void spawn_world(void) {
    if (boss.active || stage_state != ST_PLAY || title_timer > 50) return;
    float diff = difficulty();

    // Stop adding ledges shortly before the boss so its arena stays clear.
    if (--plat_timer <= 0 && stage_dist < STAGE_LEN - 400) {
        float rightmost = 0;
        for (int i = 0; i < MAX_PLATFORMS; i++)
            if (platforms[i].alive && platforms[i].x + platforms[i].w > rightmost)
                rightmost = platforms[i].x + platforms[i].w;
        if (rightmost > W - 24) {
            plat_timer = 10;
        } else {
            static const int heights[] = {136, 124, 112};
            for (int i = 0; i < MAX_PLATFORMS; i++)
                if (!platforms[i].alive) {
                    platform *p = &platforms[i];
                    *p = (platform){W + 2, heights[rrange(0, 3)], rrange(40, 96), true};
                    if (p->w >= 50 && rnd() % 100 < 35) spawn_enemy(E_SNIPER, p->x + p->w / 2.0f - 8, p->y - 18);
                    break;
                }
            plat_timer = rrange(60, 150);
        }
    }

    if (--grunt_timer <= 0) {
        int n = rnd() % 3 == 0 ? 2 : 1;
        for (int k = 0; k < n; k++) {
            float y = GROUND_Y - PH;
            for (int i = 0; i < MAX_PLATFORMS; i++)
                if (platforms[i].alive && platforms[i].x > W - 40 && rnd() % 100 < 30) y = platforms[i].y - PH;
            spawn_enemy(E_GRUNT, W + 4 + k * 18, y);
        }
        grunt_timer = (int)(rrange(40, 110) / diff);
    }

    if (--sniper_timer <= 0) {
        spawn_enemy(E_SNIPER, W + 4, GROUND_Y - 18);
        sniper_timer = (int)(rrange(220, 420) / diff);
    }

    if (--capsule_timer <= 0) {
        enemy *c = spawn_enemy(E_CAPSULE, -14, rrange(46, 80));
        if (c) c->vx = 1.6f;
        capsule_timer = rrange(500, 900);
    }
}

static void update_player(input_t in) {
    if (pl.invuln > 0) pl.invuln--;
    if (pl.flash > 0) pl.flash--;
    if (pl.weapon_timer > 0 && --pl.weapon_timer == 0) pl.weapon = 0;

    if (!pl.alive) {
        pl.vy += 0.32f;
        pl.y += pl.vy;
        pl.x -= pl.face * 1.0f;
        if (pl.y > GROUND_Y - PH) pl.y = GROUND_Y - PH, pl.vy = 0;
        if (--pl.dead_timer <= 0 && game_over_timer == 0) player_respawn();
        return;
    }

    pl.ducking = in.down && pl.on_ground;
    if (!pl.ducking) {
        if (in.left) pl.x -= 2.0f, pl.face = -1;
        if (in.right) pl.x += 2.0f, pl.face = 1;
    }
    float max_x = W - 20;
    if (boss.active && boss.kind != B_TANK && boss.x - 18 < max_x) max_x = boss.x - 18;
    if (pl.x > max_x) pl.x = max_x;
    if (pl.x < 2) pl.x = 2;

    if (in.up && pl.on_ground && !pl.ducking) {
        pl.vy = -5.4f;
        pl.on_ground = false;
        pl.balled = true;
    }

    float prev_feet = pl.y + PH;
    pl.vy += 0.32f;
    if (pl.vy > 6) pl.vy = 6;
    pl.y += pl.vy;
    pl.on_ground = land(pl.x + 3, &pl.y, &pl.vy, 10, PH, prev_feet);
    if (pl.on_ground) pl.balled = false;
    pl.anim++;

    if (--pl.fire_cd > 0 || stage_state != ST_PLAY) return;
    float mx, my;
    if (pl.ducking) mx = pl.face > 0 ? pl.x + 22 : pl.x - 4, my = pl.y + 17;
    else if (pl.balled) mx = pl.x + 6 + pl.face * 6, my = pl.y + 12;
    else mx = pl.face > 0 ? pl.x + 16 : pl.x - 2, my = pl.y + 9;
    // Up is jump, so there's no manual aiming: during a boss fight the gun locks
    // onto the nearest boss part in front of the player.
    float aim = pl.face > 0 ? 0 : (float)M_PI, best = 1e9f;
    if (boss.active && boss.phase == BP_FIGHT)
        for (int i = 0; i < boss.nparts; i++) {
            part *p = &boss.parts[i];
            if (!p->alive || (p->core && reactor_shielded())) continue;
            float tx = part_x(p) + p->w / 2.0f, ty = p->ry + p->h / 2.0f, d = fabsf(tx - mx) + fabsf(ty - my);
            if ((tx - mx) * pl.face > 0 && d < best) best = d, aim = atan2f(ty - my, tx - mx);
        }
    if (pl.weapon == 1) {
        for (int i = -2; i <= 2; i++)
            spawn_bullet(mx, my, cosf(aim + i * 0.18f) * 6.0f, sinf(aim + i * 0.18f) * 6.0f, false, BUL_NORMAL, 0);
        pl.fire_cd = 10;
    } else {
        spawn_bullet(mx, my, cosf(aim) * 6.5f, sinf(aim) * 6.5f, false, BUL_NORMAL, 0);
        pl.fire_cd = 7;
    }
    pl.flash = 2;
}

static void update_enemies(void) {
    float px_, py_, pw, ph;
    player_hitbox(&px_, &py_, &pw, &ph);
    float diff = difficulty();

    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemy *e = &enemies[i];
        if (!e->alive) continue;
        e->anim++;
        float prev_feet;
        switch (e->type) {
        case E_GRUNT:
            e->x -= world_speed + 1.0f + e->vx;
            prev_feet = e->y + PH;
            e->vy += 0.32f;
            e->y += e->vy;
            e->on_ground = land(e->x + 3, &e->y, &e->vy, 10, PH, prev_feet);
            if (e->on_ground && rnd() % 150 == 0) e->vy = -4.5f;
            if (pl.alive && overlap(e->x + 3, e->y + 2, 10, 18, px_, py_, pw, ph)) player_die();
            break;
        case E_SNIPER:
            e->x -= world_speed;
            prev_feet = e->y + 18;
            e->vy += 0.32f;
            e->y += e->vy;
            land(e->x + 3, &e->y, &e->vy, 10, 18, prev_feet);
            if (--e->timer <= 0) {
                if (pl.alive && e->x < W - 10 && e->x > 10)
                    aimed_bullet(e->x + (pl.x < e->x ? -1 : 16), e->y + 9, 2.4f * diff, 0, BUL_NORMAL);
                e->timer = (int)(rrange(60, 110) / diff);
            }
            break;
        case E_CAPSULE:
            e->x += e->vx;
            e->y = e->base_y + sinf(e->anim * 0.08f) * 14;
            if (e->x > W + 16) e->alive = false;
            break;
        case E_PICKUP:
            prev_feet = e->y + 8;
            e->vy += 0.2f;
            e->y += e->vy;
            e->on_ground = land(e->x, &e->y, &e->vy, 10, 8, prev_feet);
            if (e->on_ground) e->x -= world_speed;
            if (pl.alive && overlap(e->x, e->y, 10, 8, px_, py_, pw, ph)) {
                pl.weapon = 1;
                pl.weapon_timer = 30 * TICK_HZ;
                score += 1000;
                e->alive = false;
            }
            break;
        }
        if (e->x < -24) e->alive = false;
    }
}

static void update_bullets(void) {
    float px_, py_, pw, ph;
    player_hitbox(&px_, &py_, &pw, &ph);

    for (int i = 0; i < MAX_BULLETS; i++) {
        bullet *b = &bullets[i];
        if (!b->alive) continue;
        b->vy += b->g;
        b->x += b->vx;
        b->y += b->vy;
        if (b->y >= GROUND_Y && b->kind == BUL_SHELL) spawn_explosion(b->x, GROUND_Y - 3, 0.5f);
        if (b->x < -4 || b->x > W + 4 || b->y < -40 || b->y >= GROUND_Y) {
            b->alive = false;
            continue;
        }
        if (b->hostile) {
            if (pl.alive && overlap(b->x - 1, b->y - 1, 3, 3, px_, py_, pw, ph)) {
                b->alive = false;
                if (b->kind == BUL_SHELL) spawn_explosion(b->x, b->y, 0.5f);
                player_die();
            }
            continue;
        }
        if (boss_hit(b)) {
            b->alive = false;
            continue;
        }
        for (int j = 0; j < MAX_ENEMIES; j++) {
            enemy *e = &enemies[j];
            if (!e->alive || e->type == E_PICKUP) continue;
            float ex = e->x + 3, ey = e->y + 2, ew = 10, eh = 18;
            if (e->type == E_SNIPER) eh = 16;
            if (e->type == E_CAPSULE) ex = e->x, ey = e->y, ew = 14, eh = 7;
            if (overlap(b->x - 1, b->y - 1, 3, 3, ex, ey, ew, eh)) {
                b->alive = false;
                e->alive = false;
                spawn_explosion(ex + ew / 2, ey + eh / 2, e->type == E_CAPSULE ? 0.8f : 0.6f);
                score += e->type == E_GRUNT ? 100 : e->type == E_SNIPER ? 300 : 500;
                if (e->type == E_CAPSULE) {
                    enemy *p = spawn_enemy(E_PICKUP, e->x + 2, e->y);
                    if (p) p->vy = -3.0f;
                }
                break;
            }
        }
    }
}

void game_step(void) {
    tick_count++;
    scroll += world_speed;
    stage_dist += world_speed;

    for (int i = 0; i < MAX_PLATFORMS; i++) {
        platform *p = &platforms[i];
        if (!p->alive) continue;
        p->x -= world_speed;
        if (p->x + p->w < 0) p->alive = false;
    }

    if (stage_state == ST_PLAY && !boss.active && stage_dist >= STAGE_LEN) start_boss();
    if ((stage_state == ST_CLEAR || stage_state == ST_ENDING) && --state_timer <= 0)
        stage_state = ST_FADE_OUT, state_timer = 30;
    if (stage_state == ST_FADE_OUT && --state_timer <= 0) {
        if (stage == NUM_STAGES - 1) loop_count++;
        load_stage((stage + 1) % NUM_STAGES);
        fade_in = 30;
    }
    if (fade_in > 0) fade_in--;
    if (title_timer > 0) title_timer--;
    if (warning_timer > 0) warning_timer--;

    spawn_world();
    update_player(game_mode ? keys : ai_input());
    update_boss();
    update_enemies();
    update_bullets();

    for (int i = 0; i < MAX_EXPLOSIONS; i++)
        if (explosions[i].alive && ++explosions[i].age >= 22) explosions[i].alive = false;

    if (game_mode) {
        if (score >= next_extra) lives++, next_extra += EXTRA_LIFE_EVERY;
        if (score > hi_score) hi_score = score;
        if (game_over_timer > 0 && --game_over_timer == 0) {
            game_save_hiscore();
            exit_requested = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void draw_platform(const platform *p) {
    int x = (int)p->x;
    for (int i = 0; i < p->w; i++) {
        put(x + i, p->y, cur->ledge_top);
        put(x + i, p->y + 1, ((x + i + (int)scroll) / 3) % 4 ? cur->ledge_top : cur->ledge_top2);
        for (int j = 2; j < 8; j++) {
            if ((i == 0 || i == p->w - 1) && j > 4) continue;
            put(x + i, p->y + j, ((i * 7 + j * 13) % 17 == 0) ? cur->ledge_dot : cur->ledge);
        }
    }
}

static void draw_explosion(const explosion *ex) {
    uint32_t seed = (uint32_t)ex->seed;
    int age = ex->age, r;
    px core, mid, rim;
    if (age < 8) r = 2 + age, core = C_WHITE, mid = C_YELLOW, rim = C_ORANGE;
    else r = 10 + (age - 8) / 3, core = C_ORANGE, mid = C_RED, rim = C_SMOKE;
    r = (int)(r * ex->size + 0.5f);
    if (r < 1) r = 1;
    float density = age < 12 ? 1.0f : fmaxf(0.0f, 1.0f - (age - 12) / 10.0f);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            seed = seed * 1664525u + 1013904223u;
            float n = (float)(seed >> 8) / (float)(1 << 24);
            float d = sqrtf((float)(dx * dx + dy * dy));
            if (d > r || n > density) continue;
            float f = d / r + (n - 0.5f) * 0.3f;
            put(ex->x + dx, ex->y + dy, f < 0.35f ? core : f < 0.7f ? mid : rim);
        }
    if (age < 16)
        for (int i = 0; i < 10; i++) {
            float a = 2 * (float)M_PI * i / 10 + 0.3f, dist = age * 1.6f * ex->size;
            put(ex->x + (int)(cosf(a) * dist), ex->y + (int)(sinf(a) * dist), age < 8 ? C_YELLOW : C_ORANGE);
        }
}

static void draw_player(void) {
    if ((pl.invuln > 0 && (pl.invuln / 3) % 2) || game_over_timer > 0) return;
    int x = (int)pl.x, y = (int)pl.y;
    bool flip = pl.face < 0;
    if (!pl.alive) {
        draw_sprite(PLAYER_PRONE, 7, x - 3, y + 13, !flip, &pal_player);
        return;
    }
    if (pl.balled) {
        draw_ball(x, y + 6, (pl.anim / 3) % 4);
    } else if (pl.ducking) {
        draw_sprite(PLAYER_PRONE, 7, flip ? x - 4 : x, y + 13, flip, &pal_player);
    } else {
        int frame = pl.on_ground && world_speed > 0 ? (pl.anim / 4) % 4 : (pl.anim / 6) % 4;
        draw_sprite(PLAYER_TORSO, 14, x, y, flip, &pal_player);
        draw_sprite(LEGS[frame], 6, x, y + 14, flip, &pal_player);
    }
    if (pl.flash > 0 && !pl.balled) {
        int mx = pl.ducking ? (pl.face > 0 ? x + 22 : x - 5) : (pl.face > 0 ? x + 16 : x - 2);
        int my = pl.ducking ? y + 17 : y + 9;
        put(mx, my, C_WHITE), put(mx + pl.face, my, C_YELLOW), put(mx, my - 1, C_YELLOW), put(mx, my + 1, C_YELLOW);
    }
}

static void draw_enemy(const enemy *e) {
    int x = (int)e->x, y = (int)e->y;
    switch (e->type) {
    case E_GRUNT:
        draw_sprite(GRUNT_TORSO, 14, x, y, true, &pal_grunt[stage]);
        draw_sprite(LEGS[(e->anim / 3) % 4], 6, x, y + 14, true, &pal_grunt[stage]);
        break;
    case E_SNIPER: {
        bool face_left = pl.x < e->x;
        draw_sprite(GRUNT_TORSO, 14, x, y, face_left, &pal_sniper[stage]);
        draw_sprite(SNIPER_KNEEL, 4, x, y + 14, face_left, &pal_sniper[stage]);
        break;
    }
    case E_CAPSULE:
        draw_sprite(CAPSULE, 7, x, y, false, &pal_capsule);
        for (int i = 0; i < 4; i++) { // flapping wings
            int wy = (e->anim / 4) % 2 ? -i / 2 : i / 2;
            put(x - 1 - i, y + 3 + wy, rgb(150, 150, 160));
            put(x + 14 + i, y + 3 + wy, rgb(150, 150, 160));
        }
        break;
    case E_PICKUP: // weapon badge with an "S"
        fill_rect(x, y, 10, 8, C_DARK_RED);
        fill_rect(x + 1, y + 1, 8, 6, C_RED);
        if ((e->anim / 6) % 2) fill_rect(x, y, 10, 1, C_YELLOW);
        draw_text("S", x + 3, y + 1, 1, C_YELLOW);
        break;
    }
}

static void draw_turret(const part *p) {
    int x = (int)part_x(p), y = p->ry;
    if (!p->alive) {
        fill_rect(x, y, p->w, p->h, rgb(28, 18, 12));
        if (rnd() % 6 == 0) put(x + rrange(0, p->w), y + rrange(0, p->h), C_ORANGE);
        return;
    }
    fill_rect(x - 1, y - 1, p->w + 2, p->h + 2, C_OUTLINE);
    fill_rect(x, y, p->w, p->h, p->flash ? C_WHITE : rgb(90, 90, 100));
    fill_rect(x + 1, y + 1, p->w - 2, 2, rgb(130, 130, 140));
    float cx = x + 3, cy = y + p->h / 2.0f;
    float a = atan2f((pl.y + 10) - cy, (pl.x + 8) - cx);
    for (int i = 0; i < 9; i++)
        for (int t = 0; t < 2; t++) put((int)(cx + cosf(a) * i), (int)(cy + sinf(a) * i) + t, rgb(50, 50, 58));
    put(x + p->w - 4, y + p->h / 2, (boss.t / 8) % 2 ? C_RED : C_DARK_RED);
}

static void draw_core(const part *p, px hot, px warm, px cool) {
    int x = (int)part_x(p), y = p->ry, cx = x + p->w / 2, cy = y + p->h / 2;
    fill_rect(x - 2, y - 2, p->w + 4, p->h + 4, C_OUTLINE);
    if (!p->alive) {
        if (rnd() % 3 == 0) put(cx + rrange(-6, 7), cy + rrange(-8, 9), C_ORANGE);
        return;
    }
    int r = 8 + (int)(sinf(boss.t * 0.15f) * 1.5f);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            px c = d2 < r * r / 6 ? hot : d2 < r * r / 2 ? warm : cool;
            put(cx + dx, cy + dy, p->flash ? C_WHITE : c);
        }
    if (reactor_shielded()) // shutters
        for (int j = -r; j <= r; j += 3) fill_rect(cx - r, cy + j, 2 * r + 1, 2, rgb(100, 104, 112));
}

static void draw_wall(px body, px panel, px rivet, px stripe) {
    int x = (int)boss.x;
    fill_rect(x, 40, 74, GROUND_Y - 40, body);
    for (int i = 0; i < 74; i += 10) fill_rect(x + i, 34, 6, 6, body);
    for (int y = 46; y < GROUND_Y; y += 12) fill_rect(x, y, 74, 1, panel);
    fill_rect(x + 30, 40, 1, GROUND_Y - 40, panel);
    fill_rect(x + 62, 40, 1, GROUND_Y - 40, panel);
    for (int y = 50; y < GROUND_Y; y += 12)
        for (int i = 4; i < 74; i += 12) put(x + i, y, rivet);
    for (int y = 40; y < GROUND_Y; y++)
        if ((y / 4) % 2) fill_rect(x + 66, y, 3, 1, stripe);
}

static void draw_tank(const part *p) {
    int x = (int)part_x(p), y = p->ry;
    px hull = p->flash ? C_WHITE : rgb(90, 96, 110), hi = p->flash ? C_WHITE : rgb(150, 158, 176);
    fill_rect(x, y + 16, 52, 8, rgb(20, 20, 24));
    for (int i = 0; i < 5; i++) {
        fill_circle(x + 6 + i * 10, y + 20, 3, rgb(60, 60, 68));
        put(x + 6 + i * 10 + (int)((boss.t / 3 + i) % 2), y + 20, rgb(30, 30, 34));
    }
    fill_rect(x + 2, y + 8, 48, 9, hull);
    fill_rect(x + 8, y + 5, 36, 3, hull);
    fill_rect(x + 16, y, 18, 6, hi);
    for (int i = 0; i < 16; i++) fill_rect(x + 16 - i, y + 2 - i / 3, 1, 2, rgb(60, 64, 76)); // cannon
    for (int i = 0; i < 8; i++) put(x + 6 + i * 6, y + 9 + i % 2, rgb(210, 214, 226)); // snow camo
    fill_rect(x + 40, y + 10, 4, 2, (boss.t / 8) % 2 ? C_RED : C_DARK_RED);
}

static void draw_heart(const part *p) {
    int cx = (int)part_x(p) + 20, cy = p->ry + 20;
    for (int side = -1; side <= 1; side += 2) // tendrils rooting it to the floor
        for (int y = cy + 10; y < GROUND_Y; y++) {
            int x = cx + side * 12 + (int)(sinf(y * 0.2f + boss.t * 0.1f) * 3);
            fill_rect(x, y, 3, 1, rgb(70, 10, 40));
        }
    float r = 17 + 2 * sinf(boss.t * 0.2f);
    for (int dy = -21; dy <= 21; dy++)
        for (int dx = -21; dx <= 21; dx++) {
            float d = sqrtf((float)(dx * dx + dy * dy)), a = atan2f((float)dy, (float)dx);
            if (d > r * (1 + 0.08f * sinf(a * 5 + boss.t * 0.1f))) continue;
            px c;
            if (p->flash) c = C_WHITE;
            else if (d < r * 0.35f) c = abs(dx) < 2 ? C_BLACK : rgb(255, 220, 120); // slit pupil
            else if (((dx * 3 + dy * 5 + (int)(boss.t / 4)) % 11 + 11) % 11 == 0) c = rgb(240, 90, 150);
            else c = d < r * 0.6f ? rgb(200, 40, 90) : rgb(130, 16, 60);
            put(cx + dx, cy + dy, c);
        }
}

static void draw_boss(void) {
    if (!boss.active) return;
    switch (boss.kind) {
    case B_WALL:
        draw_wall(rgb(32, 34, 40), rgb(22, 24, 28), rgb(60, 62, 70), rgb(120, 10, 10));
        for (int i = 0; i < boss.nparts; i++) {
            if (boss.parts[i].core) draw_core(&boss.parts[i], C_WHITE, C_ORANGE, C_RED);
            else draw_turret(&boss.parts[i]);
        }
        break;
    case B_REACTOR:
        draw_wall(rgb(26, 30, 38), rgb(16, 20, 26), rgb(70, 90, 110), rgb(110, 92, 18));
        for (int i = 0; i < boss.nparts; i++) {
            if (boss.parts[i].core) draw_core(&boss.parts[i], C_WHITE, rgb(80, 220, 255), rgb(20, 90, 160));
            else draw_turret(&boss.parts[i]);
        }
        break;
    case B_TANK:
        if (boss.parts[0].alive || boss.phase == BP_DYING) draw_tank(&boss.parts[0]);
        break;
    case B_HEART:
        draw_heart(&boss.parts[0]);
        break;
    }
}

static void draw_bullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        bullet *b = &bullets[i];
        if (!b->alive) continue;
        int x = (int)b->x, y = (int)b->y;
        if (!b->hostile) {
            put(x, y, C_WHITE);
            put(x - 1, y, C_RED), put(x + 1, y, C_RED), put(x, y - 1, C_RED), put(x, y + 1, C_RED);
        } else if (b->kind == BUL_SHELL) {
            fill_rect(x - 1, y - 1, 3, 3, C_ORANGE);
            put(x, y, C_YELLOW);
        } else if (b->kind == BUL_ORB) {
            fill_rect(x - 1, y - 1, 3, 3, (tick_count / 3) % 2 ? rgb(200, 60, 220) : rgb(140, 30, 160));
            put(x, y, C_WHITE);
        } else {
            fill_rect(x, y, 2, 2, C_WHITE);
        }
    }
}

static void draw_bar(int x, int y, int w, float frac, px fg) {
    fill_rect(x - 1, y - 1, w + 2, 5, C_BLACK);
    fill_rect(x, y, w, 3, rgb(50, 50, 50));
    fill_rect(x, y, (int)(w * fmaxf(0, fminf(1, frac))), 3, fg);
}

static void draw_boss_bar(int y) {
    if (!boss.active || boss.phase == BP_ARRIVING) return;
    int hp = 0, max = 0;
    for (int i = 0; i < boss.nparts; i++) hp += boss.parts[i].alive ? boss.parts[i].hp : 0, max += boss.parts[i].max_hp;
    draw_text_shadow("BOSS", W / 2 - 64, y - 2, 1, C_RED);
    draw_bar(W / 2 - 36, y, 100, (float)hp / max, C_YELLOW);
}

static void draw_ending(void) {
    int t = ENDING_TICKS - state_timer;
    char buf[48];
    if (t > 20) draw_text_centered("MISSION COMPLETE", 40, 2, C_YELLOW);
    if (t > 60) draw_text_centered(STAGES[NUM_STAGES - 1].ending, 64, 1, C_TEXT);
    if (t > 100) {
        snprintf(buf, sizeof buf, "SCORE %06ld", score - ending_bonus);
        draw_text_centered(buf, 84, 1, C_TEXT);
    }
    if (t > 130 && ending_bonus > 0) {
        snprintf(buf, sizeof buf, "LIVES BONUS %ld", ending_bonus);
        draw_text_centered(buf, 96, 1, C_TEXT);
    }
    if (t > 160) {
        snprintf(buf, sizeof buf, "FINAL SCORE %06ld", score);
        draw_text_centered(buf, 110, 1, C_YELLOW);
    }
    if (t > 190 && game_mode && score > run_start_hi && (t / 8) % 2) draw_text_centered("NEW HIGH SCORE!", 124, 1, C_RED);
    if (t > 230) {
        snprintf(buf, sizeof buf, "LOOP %d BEGINS", loop_count + 2);
        draw_text_centered(buf, 146, 1, C_DIM);
    }
}

static void draw_hud(void) {
    char buf[32];
    if (game_mode) {
        snprintf(buf, sizeof buf, "1P %06ld", score);
        draw_text_shadow(buf, 8, 10, 1, C_TEXT);
        for (int i = 0; i < lives && i < 8; i++) { // medals
            fill_rect(8 + i * 6, 20, 3, 2, C_RED);
            fill_rect(8 + i * 6, 22, 3, 3, C_YELLOW);
        }
        snprintf(buf, sizeof buf, "HI %06ld", hi_score);
        draw_text_shadow(buf, W - 8 - text_width(buf, 1), 10, 1, C_TEXT);
        if (pl.weapon == 1) {
            snprintf(buf, sizeof buf, "S %02d", pl.weapon_timer / TICK_HZ);
            draw_text_shadow(buf, 8, 28, 1, C_YELLOW);
        }
        if (boss.active) {
            draw_boss_bar(14);
        } else if (stage_state == ST_PLAY) {
            snprintf(buf, sizeof buf, "STAGE %d", stage + 1);
            draw_text_centered(buf, 6, 1, C_TEXT);
            draw_bar(W / 2 - 50, 16, 100, stage_dist / STAGE_LEN, C_RED);
        }
        draw_text_shadow("ESC: EXIT", W - 8 - text_width("ESC: EXIT", 1), H - 10, 1, C_DIM);
    } else if (stage_state != ST_ENDING) {
        draw_logo((W - logo_width()) / 2, 18);
        if ((tick_count / 30) % 2 == 0) draw_text_centered("PRESS SUPER+CTRL+G", 18 + logo_height() + 10, 1, C_TEXT);
        draw_boss_bar(66);
    }

    if (title_timer > 0 && title_timer < 100) {
        snprintf(buf, sizeof buf, loop_count ? "STAGE %d  LOOP %d" : "STAGE %d", stage + 1, loop_count + 1);
        draw_text_centered(buf, 76, 2, C_YELLOW);
        draw_text_centered(cur->name, 96, 1, C_TEXT);
    }
    if (warning_timer > 0 && (warning_timer / 8) % 2) draw_text_centered("WARNING!", 80, 2, C_RED);
    if (stage_state == ST_CLEAR) draw_text_centered("STAGE CLEAR", 80, 2, C_YELLOW);
    if (stage_state == ST_ENDING) draw_ending();
    if (game_over_timer > 0) draw_text_centered("GAME OVER", 80, 2, C_RED);
}

void game_render(bool hud) {
    memset(fb, 0, sizeof fb);
    stage_draw_background(tick_count, scroll);
    draw_boss();
    for (int i = 0; i < MAX_PLATFORMS; i++)
        if (platforms[i].alive) draw_platform(&platforms[i]);
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (enemies[i].alive) draw_enemy(&enemies[i]);
    draw_player();
    draw_bullets();
    for (int i = 0; i < MAX_EXPLOSIONS; i++)
        if (explosions[i].alive) draw_explosion(&explosions[i]);
    stage_draw_weather(tick_count);

    float fade = 1;
    if (stage_state == ST_FADE_OUT) fade = state_timer / 30.0f;
    else if (stage_state == ST_ENDING) fade = fmaxf(0.25f, 1 - (ENDING_TICKS - state_timer) / 30.0f);
    else if (fade_in > 0) fade = 1 - fade_in / 30.0f;
    fade_framebuffer(fade);
    if (hud) draw_hud();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void game_init(void) {
    art_init();
    stage_init();
    load_hiscore();
    player_respawn();
    pl.invuln = 0;
    load_stage(0);
    title_timer = 0;
}

void game_start_run(void) {
    game_mode = true;
    memset(&keys, 0, sizeof keys);
    score = ending_bonus = 0;
    lives = START_LIVES;
    next_extra = EXTRA_LIFE_EVERY;
    loop_count = 0;
    game_over_timer = 0;
    exit_requested = false;
    run_start_hi = hi_score;
    player_respawn();
    load_stage(0);
    fade_in = 30;
}

void game_end_run(void) {
    game_mode = false;
    memset(&keys, 0, sizeof keys);
    game_over_timer = 0;
    game_save_hiscore();
    if (!pl.alive) player_respawn();
}

bool game_exit_requested(void) {
    bool r = exit_requested;
    exit_requested = false;
    return r;
}

void game_preview(int n, bool at_boss) {
    load_stage(((n % NUM_STAGES) + NUM_STAGES) % NUM_STAGES);
    title_timer = 0;
    if (at_boss) stage_dist = STAGE_LEN;
}

bool game_player_settled(void) { return pl.alive && pl.on_ground && !pl.ducking && pl.invuln == 0; }
