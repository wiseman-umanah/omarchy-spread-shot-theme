// Game simulation and rendering. Platform-independent: main.c feeds it input and ticks.
#pragma once

#include <stdbool.h>

#define TICK_HZ 30

typedef struct {
    bool left, right, up, down;
} input_t;

extern bool game_mode; // true while a human is playing; false = AI demo
extern input_t keys;   // human input, used only in game mode
extern bool game_verbose;   // log stage events to stderr
extern bool game_persist;   // load/save the high score file

void game_init(void);
void game_step(void);
void game_render(bool hud);

void game_start_run(void); // fresh run: stage 1, full lives, score 0
void game_end_run(void);   // back to the AI demo
bool game_exit_requested(void); // true once after a game over finishes

void game_preview(int stage, bool boss); // jump to a stage (0-based), optionally at its boss
bool game_player_settled(void);           // player alive, standing, not blinking (for snapshots)
void game_save_hiscore(void);
