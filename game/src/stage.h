// Stages: each one regenerates the parallax layers, ground, ledges and weather.
#pragma once

#include "art.h"

#define GROUND_Y 164
#define TILE 240 // parallax tile width; layers wrap around it

enum { WX_NONE, WX_SNOW, WX_SPORES };
enum { B_WALL, B_TANK, B_REACTOR, B_HEART };

typedef struct {
    const char *name;
    const char *ending; // shown on the mission-complete screen after the final stage
    bool stars;
    px far, far_hi, mid, mid_hi;
    px gnd_top, gnd_top2, gnd, gnd_dot;
    px ledge_top, ledge_top2, ledge, ledge_dot;
    int weather, boss;
} stage_def;

extern const stage_def STAGES[NUM_STAGES];
extern int stage;            // index into STAGES
extern const stage_def *cur; // &STAGES[stage]

void stage_init(void);   // one-time setup (stars)
void stage_build(int n); // switch to stage n and regenerate its scenery
void stage_draw_background(long tick, float scroll);
void stage_draw_weather(long tick);
