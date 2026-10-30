// Sprites, palettes and the title logo.
#pragma once

#include "gfx.h"

#define NUM_STAGES 4

extern palette pal_player, pal_capsule;
extern palette pal_grunt[NUM_STAGES], pal_sniper[NUM_STAGES]; // reskinned per stage

extern const char *PLAYER_TORSO[14];
extern const char *LEGS[4][6]; // run cycle
extern const char *PLAYER_PRONE[7];
extern const char *GRUNT_TORSO[14];
extern const char *SNIPER_KNEEL[4];
extern const char *CAPSULE[7];

void art_init(void);
void draw_ball(int x, int y, int frame); // the somersault jump
void draw_logo(int x, int y);
int logo_width(void);
int logo_height(void);
