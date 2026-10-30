// spread-shot: a playable run-and-gun wallpaper for Wayland compositors with wlr-layer-shell.
//
// The game is drawn at 342x192 on the CPU and scaled nearest-neighbor into a shared-memory
// buffer, so it never touches the GPU. Frames are driven by the compositor's frame callbacks,
// so it idles at 0% CPU whenever the wallpaper is covered.
//
// Demo mode (default): sits on the bottom layer while an AI plays.
// Game mode (SIGUSR1 toggles): moves to the top layer and takes the keyboard.
//   Left/Right move, Up jumps, Down ducks. Esc (or 45 s idle) hands the keyboard back.

#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "game.h"
#include "gfx.h"
#include "art.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#ifndef VERSION
#define VERSION "dev"
#endif

#define TICK_MS (1000.0 / TICK_HZ)
#define GAME_IDLE_TICKS (45 * TICK_HZ)

typedef struct {
    struct wl_buffer *wl;
    uint32_t *data;
    bool busy;
} buffer;

static struct {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_callback *frame_cb;
    buffer buffers[2];
    void *pool_data;
    size_t pool_size;
    int width, height;
    int *xmap;
    bool configured, running;
    double last_ms, acc_ms;
    long idle_ticks;
} st = {.running = true};

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// ---------------------------------------------------------------------------
// Shared-memory buffers
// ---------------------------------------------------------------------------

static void buffer_release(void *data, struct wl_buffer *wl_buffer) {
    (void)wl_buffer;
    ((buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void destroy_buffers(void) {
    for (int i = 0; i < 2; i++)
        if (st.buffers[i].wl) wl_buffer_destroy(st.buffers[i].wl), st.buffers[i].wl = NULL;
    if (st.pool_data) munmap(st.pool_data, st.pool_size), st.pool_data = NULL;
}

static bool create_buffers(void) {
    destroy_buffers();
    int stride = st.width * 4;
    size_t one = (size_t)stride * st.height;
    st.pool_size = one * 2;
    int fd = memfd_create("spread-shot", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)st.pool_size) < 0) {
        perror("spread-shot: shm");
        if (fd >= 0) close(fd);
        return false;
    }
    st.pool_data = mmap(NULL, st.pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (st.pool_data == MAP_FAILED) {
        st.pool_data = NULL;
        close(fd);
        perror("spread-shot: mmap");
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(st.shm, fd, (int32_t)st.pool_size);
    for (int i = 0; i < 2; i++) {
        st.buffers[i].wl =
            wl_shm_pool_create_buffer(pool, (int32_t)(one * i), st.width, st.height, stride, WL_SHM_FORMAT_XRGB8888);
        st.buffers[i].data = (uint32_t *)((char *)st.pool_data + one * i);
        st.buffers[i].busy = false;
        wl_buffer_add_listener(st.buffers[i].wl, &buffer_listener, &st.buffers[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);

    free(st.xmap);
    st.xmap = malloc(sizeof(int) * st.width);
    if (!st.xmap) return false;
    for (int x = 0; x < st.width; x++) st.xmap[x] = x * W / st.width;
    return true;
}

// Nearest-neighbor upscale of the game framebuffer into the output buffer.
static void present(buffer *b) {
    int prev_sy = -1;
    uint32_t *prev_row = NULL;
    for (int y = 0; y < st.height; y++) {
        int sy = y * H / st.height;
        uint32_t *row = b->data + (size_t)y * st.width;
        if (sy == prev_sy) {
            memcpy(row, prev_row, (size_t)st.width * 4);
        } else {
            for (int x = 0; x < st.width; x++) row[x] = fb[sy][st.xmap[x]];
            prev_sy = sy;
        }
        prev_row = row;
    }
}

// ---------------------------------------------------------------------------
// Frame loop and game mode
// ---------------------------------------------------------------------------

static void request_frame(void);

static void set_game_mode(bool on) {
    if (game_mode == on) return;
    if (on) game_start_run();
    else game_end_run();
    st.idle_ticks = 0;
    zwlr_layer_surface_v1_set_layer(st.layer_surface,
                                    on ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        st.layer_surface, on ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                             : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    request_frame();
    wl_surface_commit(st.surface);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data, (void)time;
    wl_callback_destroy(cb);
    st.frame_cb = NULL;

    double now = now_ms();
    double dt = now - st.last_ms;
    st.last_ms = now;
    // A long gap means we were hidden; resume without fast-forwarding.
    if (dt > 250) dt = TICK_MS;
    st.acc_ms += dt;

    int steps = 0;
    while (st.acc_ms >= TICK_MS && steps < 4) {
        game_step();
        st.acc_ms -= TICK_MS;
        steps++;
        if (game_mode && (++st.idle_ticks > GAME_IDLE_TICKS || game_exit_requested())) set_game_mode(false);
    }
    if (st.acc_ms > TICK_MS) st.acc_ms = 0;

    buffer *b = NULL;
    if (steps > 0)
        for (int i = 0; i < 2; i++)
            if (!st.buffers[i].busy) {
                b = &st.buffers[i];
                break;
            }

    request_frame();
    if (b) {
        game_render(true);
        present(b);
        wl_surface_attach(st.surface, b->wl, 0, 0);
        wl_surface_damage_buffer(st.surface, 0, 0, st.width, st.height);
        b->busy = true;
    }
    wl_surface_commit(st.surface);
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void request_frame(void) {
    if (st.frame_cb) return;
    st.frame_cb = wl_surface_frame(st.surface);
    wl_callback_add_listener(st.frame_cb, &frame_listener, NULL);
}

// ---------------------------------------------------------------------------
// Layer surface
// ---------------------------------------------------------------------------

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t w,
                            uint32_t h) {
    (void)data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if ((int)w != st.width || (int)h != st.height || !st.pool_data) {
        st.width = (int)w;
        st.height = (int)h;
        if (!create_buffers()) {
            st.running = false;
            return;
        }
    }
    if (!st.configured) {
        st.configured = true;
        st.last_ms = now_ms();
        game_render(true);
        present(&st.buffers[0]);
        wl_surface_attach(st.surface, st.buffers[0].wl, 0, 0);
        st.buffers[0].busy = true;
        request_frame();
    }
    wl_surface_commit(st.surface);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data, (void)surface;
    st.running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

// ---------------------------------------------------------------------------
// Keyboard (raw evdev codes; only the arrows and Esc matter, so no keymap needed)
// ---------------------------------------------------------------------------

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size) {
    (void)d, (void)k, (void)fmt, (void)size;
    close(fd);
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys_) {
    (void)d, (void)k, (void)serial, (void)s, (void)keys_;
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void)d, (void)k, (void)serial, (void)s;
    memset(&keys, 0, sizeof keys);
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)d, (void)k, (void)serial, (void)time;
    if (!game_mode) return;
    bool down = state != WL_KEYBOARD_KEY_STATE_RELEASED;
    st.idle_ticks = 0;
    switch (key) {
    case KEY_LEFT: keys.left = down; break;
    case KEY_RIGHT: keys.right = down; break;
    case KEY_UP: keys.up = down; break;
    case KEY_DOWN: keys.down = down; break;
    case KEY_ESC:
        if (down) set_game_mode(false);
        break;
    }
}

static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b, uint32_t c,
                         uint32_t g) {
    (void)d, (void)k, (void)serial, (void)a, (void)b, (void)c, (void)g;
}

static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d, (void)k, (void)rate, (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
    (void)d;
    bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    if (has_kb && !st.keyboard) {
        st.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(st.keyboard, &keyboard_listener, NULL);
    } else if (!has_kb && st.keyboard) {
        wl_keyboard_release(st.keyboard);
        st.keyboard = NULL;
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *name) { (void)d, (void)s, (void)name; }

static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = seat_name};

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name))
        st.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        st.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_seat_interface.name) && !st.seat) {
        st.seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(st.seat, &seat_listener, NULL);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        st.layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
}

static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) { (void)d, (void)reg, (void)name; }

static const struct wl_registry_listener registry_listener = {.global = reg_global, .global_remove = reg_remove};

// ---------------------------------------------------------------------------
// Omarchy background switcher: picking one of the theme's stage stills
// (Super+Ctrl+Space) moves the demo to that stage.
// ---------------------------------------------------------------------------

static const char *STAGE_BACKGROUNDS[NUM_STAGES] = {"1-jungle", "2-snowfield", "3-enemy-base", "4-alien-hive"};

static bool background_dir(char *buf, size_t n) {
    const char *home = getenv("HOME");
    if (!home) return false;
    snprintf(buf, n, "%s/.local/state/omarchy/current", home);
    return true;
}

// Returns the stage whose still Omarchy currently shows, or -1 for any other background.
static int background_stage(void) {
    char dir[512], link[600], target[1024];
    if (!background_dir(dir, sizeof dir)) return -1;
    snprintf(link, sizeof link, "%s/background", dir);
    ssize_t len = readlink(link, target, sizeof target - 1);
    if (len <= 0) return -1;
    target[len] = '\0';
    const char *base = strrchr(target, '/') ? strrchr(target, '/') + 1 : target;
    for (int i = 0; i < NUM_STAGES; i++) {
        size_t sl = strlen(STAGE_BACKGROUNDS[i]);
        if (!strncmp(base, STAGE_BACKGROUNDS[i], sl) && base[sl] == '.') return i;
    }
    return -1;
}

// Watches the directory holding the `background` symlink; -1 if unavailable.
static int watch_background(void) {
    char dir[512];
    if (!background_dir(dir, sizeof dir)) return -1;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, dir, IN_CREATE | IN_MOVED_TO) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void background_changed(int fd) {
    char events[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool relevant = false;
    ssize_t len;
    while ((len = read(fd, events, sizeof events)) > 0)
        for (char *p = events; p < events + len;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            if (ev->len && !strcmp(ev->name, "background")) relevant = true;
            p += sizeof *ev + ev->len;
        }
    if (relevant) game_select_stage(background_stage());
}

static int run_wayland(void) {
    st.display = wl_display_connect(NULL);
    if (!st.display) {
        fprintf(stderr, "spread-shot: cannot connect to a Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(st.display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(st.display);
    if (!st.compositor || !st.shm || !st.layer_shell) {
        fprintf(stderr, "spread-shot: the compositor lacks wl_shm or wlr-layer-shell\n");
        return 1;
    }
    if (zwlr_layer_shell_v1_get_version(st.layer_shell) < 2) {
        fprintf(stderr, "spread-shot: wlr-layer-shell v2 or newer is required\n");
        return 1;
    }

    st.surface = wl_compositor_create_surface(st.compositor);
    st.layer_surface = zwlr_layer_shell_v1_get_layer_surface(st.layer_shell, st.surface, NULL,
                                                             ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, "spread-shot");
    zwlr_layer_surface_v1_set_anchor(st.layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(st.layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(st.layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(st.layer_surface,
                                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(st.layer_surface, &layer_listener, NULL);
    wl_surface_commit(st.surface);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

    struct pollfd fds[3] = {
        {.fd = wl_display_get_fd(st.display), .events = POLLIN},
        {.fd = sfd, .events = POLLIN},
        {.fd = watch_background(), .events = POLLIN}, // poll() skips a negative fd
    };

    while (st.running) {
        while (wl_display_prepare_read(st.display) != 0) wl_display_dispatch_pending(st.display);
        if (wl_display_flush(st.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(st.display);
            break;
        }
        if (poll(fds, 3, -1) < 0) {
            wl_display_cancel_read(st.display);
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(st.display) < 0) break;
        } else {
            wl_display_cancel_read(st.display);
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) break;
        if (wl_display_dispatch_pending(st.display) < 0) break;

        if (fds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGUSR1) set_game_mode(!game_mode);
                else st.running = false;
            }
            wl_display_flush(st.display);
        }
        if (fds[2].revents & POLLIN) background_changed(fds[2].fd);
    }

    if (fds[2].fd >= 0) close(fds[2].fd);
    game_save_hiscore();
    destroy_buffers();
    free(st.xmap);
    wl_display_disconnect(st.display);
    return 0;
}

// ---------------------------------------------------------------------------
// Headless tools: simulation (tests) and snapshots (theme artwork)
// ---------------------------------------------------------------------------

static int simulate(long ticks, bool idle_player) {
    if (idle_player) game_start_run(); // nobody at the keys: tests lives and game over
    for (long i = 0; i < ticks; i++) {
        game_step();
        if (game_mode && game_exit_requested()) break;
    }
    return 0;
}

// Writes the framebuffer as a binary PPM, scaled up `scale` times.
static int write_ppm(const char *path, int scale) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", W * scale, H * scale);
    for (int y = 0; y < H * scale; y++)
        for (int x = 0; x < W * scale; x++) {
            px c = fb[y / scale][x / scale];
            unsigned char rgb3[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
            fwrite(rgb3, 1, 3, f);
        }
    return fclose(f) == 0 ? 0 : 1;
}

// Writes `frames` consecutive frames as raw RGB24 to stdout, e.g. for
//   spread-shot --record 300 --scale 4 | ffmpeg -f rawvideo -pix_fmt rgb24 -s 1368x768 -r 30 -i - out.mp4
static int record(long skip_ticks, long frames, int scale, bool hud) {
    for (long i = 0; i < skip_ticks; i++) game_step();
    size_t row_len = (size_t)W * scale * 3;
    unsigned char *row = malloc(row_len);
    if (!row) return 1;
    for (long f = 0; f < frames; f++) {
        game_step();
        game_render(hud);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W * scale; x++) {
                px c = fb[y][x / scale];
                row[x * 3] = (unsigned char)(c >> 16), row[x * 3 + 1] = (unsigned char)(c >> 8), row[x * 3 + 2] = (unsigned char)c;
            }
            for (int k = 0; k < scale; k++)
                if (fwrite(row, 1, row_len, stdout) != row_len) {
                    free(row);
                    return 1;
                }
        }
    }
    free(row);
    return 0;
}

static void usage(FILE *out) {
    fprintf(out,
            "Usage: spread-shot [options]\n"
            "\n"
            "A playable run-and-gun wallpaper for Wayland (wlr-layer-shell).\n"
            "Send SIGUSR1 to toggle game mode: arrows move/jump/duck, Esc exits.\n"
            "\n"
            "Options:\n"
            "  --stage N          start on stage N (1-4)\n"
            "  --boss             start at the stage's boss fight\n"
            "  --simulate TICKS   run the AI demo headlessly for TICKS (30 per second) and exit\n"
            "  --idle-player      with --simulate: play a run with nobody at the keys\n"
            "  --snapshot FILE    render one frame to a PPM image and exit\n"
            "  --ticks N          with --snapshot: simulate N ticks first (default 240)\n"
            "  --scale N          with --snapshot: integer upscale (default 1)\n"
            "  --no-hud           with --snapshot: omit the HUD and logo\n"
            "  --record FRAMES    after --ticks, write FRAMES raw RGB24 frames to stdout (pipe to ffmpeg)\n"
            "  --logo-only        with --snapshot: render just the title logo\n"
            "  --verbose          log stage events to stderr\n"
            "  --version          print the version and exit\n"
            "  --help             show this help\n");
}

int main(int argc, char **argv) {
    int start_stage = 0, scale = 1;
    long sim_ticks = -1, snap_ticks = 240, record_frames = -1;
    bool at_boss = false, idle_player = false, hud = true, logo_only = false;
    const char *snapshot = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has_value = i + 1 < argc;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--version")) {
            printf("spread-shot %s\n", VERSION);
            return 0;
        } else if (!strcmp(a, "--stage") && has_value) {
            start_stage = atoi(argv[++i]) - 1;
        } else if (!strcmp(a, "--boss")) {
            at_boss = true;
        } else if (!strcmp(a, "--simulate") && has_value) {
            sim_ticks = atol(argv[++i]);
        } else if (!strcmp(a, "--idle-player")) {
            idle_player = true;
        } else if (!strcmp(a, "--snapshot") && has_value) {
            snapshot = argv[++i];
        } else if (!strcmp(a, "--ticks") && has_value) {
            snap_ticks = atol(argv[++i]);
        } else if (!strcmp(a, "--scale") && has_value) {
            scale = atoi(argv[++i]);
        } else if (!strcmp(a, "--record") && has_value) {
            record_frames = atol(argv[++i]);
        } else if (!strcmp(a, "--no-hud")) {
            hud = false;
        } else if (!strcmp(a, "--logo-only")) {
            logo_only = true;
        } else if (!strcmp(a, "--verbose")) {
            game_verbose = true;
        } else {
            fprintf(stderr, "spread-shot: unknown or incomplete option '%s'\n\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (start_stage < 0 || start_stage >= NUM_STAGES || scale < 1 || scale > 16) {
        fprintf(stderr, "spread-shot: --stage must be 1-4 and --scale 1-16\n");
        return 2;
    }

    bool headless = sim_ticks >= 0 || snapshot || record_frames >= 0;
    game_persist = !headless; // tests and artwork never touch the saved high score
    rng_seed(headless ? 1 : (uint32_t)time(NULL));
    game_init();
    if (start_stage || at_boss) game_preview(start_stage, at_boss);
    else if (!headless && background_stage() > 0) game_preview(background_stage(), false); // resume the picked stage

    if (sim_ticks >= 0) return simulate(sim_ticks, idle_player);
    if (record_frames >= 0) return record(snap_ticks, record_frames, scale, hud);
    if (snapshot) {
        for (long i = 0; i < snap_ticks; i++) game_step();
        for (int i = 0; i < 600 && !game_player_settled(); i++) game_step(); // skip respawns and dodges
        if (logo_only) {
            memset(fb, 0, sizeof fb);
            draw_logo((W - logo_width()) / 2, (H - logo_height()) / 2);
        } else {
            game_render(hud);
        }
        return write_ppm(snapshot, scale);
    }
    return run_wayland();
}
