/*
 * Stumped — a Game Boy–style Sokoban in a forest at night, for the Miyoo Mini / Mini Plus (Onion OS)
 *
 *   - 1,093 puzzles by David W. Skinner (Microban I–IV, Sasquatch I–XII), loaded from levels/
 *   - push acorns onto their spots as an owl, frog, fox or raccoon (random each level by default);
 *     when a level is solved, squirrels pop out of the hedges and carry the acorns away
 *   - walls are hedges that join up (built from 8×8 quarter-tiles); lone walls may be tree stumps;
 *     dark grass outside the clearing that thins out with distance; fireflies
 *   - 16×16 art shown at 1×–4×: the largest that fits, or zoom in with L2/R2; hold Y for a map
 *   - unlimited undo/redo, hold-to-restart (itself undoable), saved progress & bests
 *   - single-hue Game Boy palettes; generative music where your moves play the melody (audio.c)
 *
 * Builds for the desktop too (keyboard: arrows, Space=A, LCtrl=B, LShift=X, LAlt=Y,
 * E/T=L1/R1, Tab/Backspace=L2/R2, RCtrl=Select, Enter=Start), and for the browser with Emscripten
 * (`make web`): the __EMSCRIPTEN__ sections swap SDL for a <canvas>, Web Audio, key/touch input
 * and localStorage saves. Everything else is shared, so both versions always match.
 */
#include <SDL/SDL.h>      /* on the web only its types and key codes are used */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../deps/font8x8_basic.h"
#include "sprites.h"
#include "logo.h"
#include "fewest.h"

#define APP_NAME     "Stumped"
#ifndef APP_VERSION
#define APP_VERSION  "v?"            /* the Makefile passes it in from the VERSION file */
#endif
#define SCREEN_W     640
#define SCREEN_H     480
#define HUD_H        32
#define PLAY_Y       HUD_H
#define PLAY_H       (SCREEN_H - HUD_H)
#define OUT_RATE     48000
#define AUDIO_SAMPLES 2048
#define LEVEL_DIR    "levels"
#define SAVE_FILE    "save.txt"
#define MAX_SETS     48
#define IMPORT_DIR   "imported"   /* web: level sets you've added (kept in the browser) */
#define MAXW         64
#define MAXH         64
#define MOVE_MS      95      /* one step's slide */
#define REPEAT_FIRST 190     /* hold a direction: first repeat */
#define REPEAT_MS    120     /* ...then every this many ms */
#define UNDO_REPEAT  110
#define RESTART_HOLD_MS 700
#define SWITCH_HOLD_MS  800     /* hold L1/R1 this long to change level */
#define QUICK_SKIP_MS   2500    /* ...after which a plain tap skips again, for this long after each skip */
#define DANCE_MS     1700

/* Miyoo Mini buttons arrive as these keys */
#define BTN_UP     SDLK_UP
#define BTN_DOWN   SDLK_DOWN
#define BTN_LEFT   SDLK_LEFT
#define BTN_RIGHT  SDLK_RIGHT
#define BTN_A      SDLK_SPACE
#define BTN_B      SDLK_LCTRL
#define BTN_X      SDLK_LSHIFT
#define BTN_Y      SDLK_LALT
#define BTN_L1     SDLK_e
#define BTN_R1     SDLK_t
#define BTN_L2     SDLK_TAB
#define BTN_R2     SDLK_BACKSPACE
#define BTN_SELECT SDLK_RCTRL
#define BTN_START  SDLK_RETURN
#define BTN_MENU   SDLK_ESCAPE

static int g_on_device = 0;
/* In the browser the prompts follow the last thing you used: buttons (0: on-screen buttons or a
   controller), keys (1) or touch gestures (2). On the device it's always buttons. */
#ifdef __EMSCRIPTEN__
static int g_kbd = 0;
#else
#define g_kbd 0
#endif
#define HINT(pad, keys, touch) (g_kbd == 2 ? (touch) : g_kbd ? (keys) : (pad))

/* Touch (web): screens note what can be tapped while they draw; a tap is matched against the last frame's
   targets. PICK selects an item and chooses it, BTN presses a button, OPT selects an option row and
   then steps its value. */
enum { TAP_PICK, TAP_BTN, TAP_OPT, TAP_PICK2, TAP_MENU, TAP_BACK, TAP_NONE };
/* PICK2: the first tap selects, a second on the same one chooses it. MENU: select this one and open
   its menu (SELECT). BACK: the header's BACK button. NONE: swallows a tap (a pop-up's own panel). */
typedef struct { short x, y, w, h, kind, val; int *sel; } Tap;
static Tap g_taps[96];
static int g_ntaps;
static void tap_add(int x, int y, int w, int h, int kind, int *sel, int val)
{
    if (g_ntaps >= 96) return;
    Tap *t = &g_taps[g_ntaps++];
    t->x = (short)x; t->y = (short)y; t->w = (short)w; t->h = (short)h; t->kind = (short)kind; t->sel = sel; t->val = (short)val;
}
static volatile sig_atomic_t g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }
static int dir_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
#ifdef __EMSCRIPTEN__
static Uint32 now_ms(void) { return (Uint32)emscripten_get_now(); }
#define AUDIO_LOCK()   ((void)0)    /* the browser mixes audio on the main thread */
#define AUDIO_UNLOCK() ((void)0)
#else
static Uint32 now_ms(void) { return SDL_GetTicks(); }
#define AUDIO_LOCK()   SDL_LockAudio()
#define AUDIO_UNLOCK() SDL_UnlockAudio()
#endif
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

#include "audio.c"

/* ======================================================================
 * Palettes: Game Boy style, one hue in four shades for everything.
 * Night (the default) is a moonlit clearing in the dark.
 * ==================================================================== */

typedef struct { const char *name; Uint32 rgb[4]; } Palette;   /* shade 0 (lightest) .. 3 (darkest) */
static const Palette PALETTES[] = {
    { "DMG Green",   { 0xE0F8D0, 0x88C070, 0x346856, 0x081820 } },
    { "Pocket Grey", { 0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F } },
    { "Orange",      { 0xF6D6A8, 0xE09A58, 0x9A5830, 0x3A2218 } },
    { "Blue",        { 0xE8F4F8, 0x9CCCE4, 0x3E7CAC, 0x10203C } },
    { "Brown",       { 0xF8F0D8, 0xD8C090, 0x8A6A40, 0x302018 } },
    { "Purple",      { 0xF8E8F0, 0xD8A8C8, 0x885088, 0x281030 } },
    { "Mint",        { 0xEAF8EE, 0xA8DCC0, 0x4C9C88, 0x123830 } },
    { "Rose",        { 0xFCEAEA, 0xF0A8B0, 0xB05868, 0x381820 } },
    { "Night",       { 0x9AA8D8, 0x5C6CA8, 0x2E3868, 0x0C1028 } },
};
#define NUM_PALETTES ((int)(sizeof PALETTES / sizeof PALETTES[0]))

/* ======================================================================
 * Settings & progress (save.txt in the app folder)
 * ==================================================================== */

typedef struct {
    char name[48];        /* "Microban II" */
    char file[96];
    int imported;         /* web: a set you added from a file (it can be removed again) */
    int n;                /* number of levels */
    char **maps;          /* raw level text, rows separated by '\n' */
    int *best_moves;      /* 0 = unsolved, SOLVED_BY_CODE = solved on another device (no counts) */
    int *best_pushes;
    char **partial;       /* moves made in a level that's been started but not solved (NULL = none) */
    char **best_run;      /* the moves of the shortest solve (fewest moves, then fewest pushes); NULL = none */
    const unsigned short *fw_moves, *fw_pushes;   /* proven fewest possible (see fewest.h); 0 = not known */
} Set;

#define SOLVED_BY_CODE (-1)
static Set g_sets[MAX_SETS];
static int progress_ok(int set);      /* a set that can have a progress code (in maker.c) */
static int g_nsets;

/* "My levels": the levels you make (or enter as codes) live in a set of their own, always the last one */
#define MY_MAX 99
#define MY_NAME "My levels"
enum { MY_OK = 1, MY_CODE = 2 };       /* MY_OK: solved at least once, so it has a code; MY_CODE: entered as a code */
static int g_my = -1;
static uint8_t g_my_flags[MY_MAX];
static int g_my_par[MY_MAX];           /* the fewest moves the solver found (0 = not known) */

/* Favorites: levels from any set, marked in the pause menu. The "Favorites" set (just before My levels)
   is a window onto them: its levels are the originals, with their own records, so a solve there counts
   in the original set too. Saved by set name and level number. */
#define FAV_MAX 200
#define FAV_NAME "Favorites"
static int g_fav = -1;
static struct { char set[48]; int lv; } g_favs[FAV_MAX];
static int g_nfav;

static struct {
    int palette;
    int animal;           /* -1 = random each level */
    int music_vol;        /* 0..10 */
    int sfx_vol;          /* 0..10 */
    int cur_set, cur_level;
    char cur_moves[8192]; /* in-progress moves for the current level (resume) */
    int last_animal;
    int zoom;             /* extra zoom steps above "fit" (0 = fit the whole level) */
} cfg = { 8, -1, 6, 7, 0, 0, "", 0, 0 };

/* a save file that was written to the end (saves from before v0.58 have no "end" line: those count
   when there's no complete backup to prefer) */
static int save_complete(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char tail[64] = "";
    if (fseek(f, -4, SEEK_END) == 0) { size_t n = fread(tail, 1, 4, f); tail[n] = 0; }
    fclose(f);
    return !strcmp(tail, "end\n");
}

/* Saving is careful: the new save is written to save.txt.tmp and ends with an "end" line; only once
   it's safely on the card does the old save.txt become save.txt.bak and the new one take its place.
   Loading uses save.txt if it's complete, and falls back to save.txt.bak if it isn't. */
static void save_all(void)
{
    FILE *f = fopen(SAVE_FILE ".tmp", "w");
    if (!f) return;
    fprintf(f, "palette %d\nanimal %d\nmusic %d\nsfx %d\nzoom %d\n", cfg.palette, cfg.animal, cfg.music_vol, cfg.sfx_vol, cfg.zoom);
    if (g_my >= 0)                     /* My levels first: the lines below refer to them by number */
        for (int i = 0; i < g_sets[g_my].n; i++) {
            fprintf(f, "my %d|%d|", g_my_flags[i], g_my_par[i]);
            for (const char *c = g_sets[g_my].maps[i]; *c; c++) fputc(*c == '\n' ? (c[1] ? '|' : '\n') : *c == ' ' ? '-' : *c, f);
            if (!*g_sets[g_my].maps[i] || g_sets[g_my].maps[i][strlen(g_sets[g_my].maps[i]) - 1] != '\n') fputc('\n', f);
        }
    for (int i = 0; i < g_nfav; i++) fprintf(f, "fav %s|%d\n", g_favs[i].set, g_favs[i].lv + 1);
    if (g_nsets) fprintf(f, "current %s|%d|%d|%s\n", g_sets[cfg.cur_set].name, cfg.cur_level + 1, cfg.last_animal, cfg.cur_moves);
    for (int s = 0; s < g_nsets; s++)
        for (int i = 0; i < g_sets[s].n && s != g_fav; i++)
            if (g_sets[s].best_moves[i])
                fprintf(f, "solved %s|%d|%d|%d\n", g_sets[s].name, i + 1, g_sets[s].best_moves[i], g_sets[s].best_pushes[i]);
    for (int s = 0; s < g_nsets; s++)
        for (int i = 0; i < g_sets[s].n && s != g_fav; i++)
            if (g_sets[s].best_run[i] && g_sets[s].best_run[i][0])
                fprintf(f, "run %s|%d|%s\n", g_sets[s].name, i + 1, g_sets[s].best_run[i]);
    for (int s = 0; s < g_nsets; s++)
        for (int i = 0; i < g_sets[s].n && s != g_fav; i++)
            if (g_sets[s].partial[i] && g_sets[s].partial[i][0])
                fprintf(f, "partial %s|%d|%s\n", g_sets[s].name, i + 1, g_sets[s].partial[i]);
    fprintf(f, "end\n");
    int bad = fflush(f) != 0 || ferror(f);
#ifndef __EMSCRIPTEN__
    if (!bad) fsync(fileno(f));                /* really on the SD card before the old save is touched */
#endif
    if (fclose(f) != 0) bad = 1;
    if (bad) { remove(SAVE_FILE ".tmp"); return; }   /* out of space or the like: keep the old save */
    if (save_complete(SAVE_FILE)) rename(SAVE_FILE, SAVE_FILE ".bak");
    rename(SAVE_FILE ".tmp", SAVE_FILE);
#ifdef __EMSCRIPTEN__
    EM_ASM({ if (Module.stumpedStore) Module.stumpedStore(); });   /* copy save.txt to localStorage */
#endif
}

static void set_partial(int s, int lv, const char *moves, int n)
{
    free(g_sets[s].partial[lv]);
    g_sets[s].partial[lv] = NULL;
    if (moves && n > 0) {
        char *c = malloc(n + 1);
        if (c) { memcpy(c, moves, n); c[n] = 0; g_sets[s].partial[lv] = c; }
    }
}
static int has_partial(int s, int lv) { return g_sets[s].partial[lv] && g_sets[s].partial[lv][0]; }

static void set_best_run(int s, int lv, const char *moves, int n)
{
    free(g_sets[s].best_run[lv]);
    g_sets[s].best_run[lv] = NULL;
    if (moves && n > 0) {
        char *c = malloc(n + 1);
        if (c) { memcpy(c, moves, n); c[n] = 0; g_sets[s].best_run[lv] = c; }
    }
}
static int has_best_run(int s, int lv) { return g_sets[s].best_run[lv] && g_sets[s].best_run[lv][0]; }

static int set_by_name(const char *name)
{
    for (int s = 0; s < g_nsets; s++) if (s != g_fav && !strcmp(g_sets[s].name, name)) return s;
    return -1;
}

static int fav_find(int set, int lv)
{
    if (set < 0 || set == g_fav) return -1;
    for (int i = 0; i < g_nfav; i++) if (g_favs[i].lv == lv && !strcmp(g_favs[i].set, g_sets[set].name)) return i;
    return -1;
}

/* point the Favorites set at the original levels (and their records), dropping any that no longer exist */
static void fav_sync(void)
{
    if (g_fav < 0) return;
    Set *f = &g_sets[g_fav];
    int n = 0;
    for (int i = 0; i < g_nfav; i++) {
        int s = set_by_name(g_favs[i].set), lv = g_favs[i].lv;
        if (s < 0 || lv < 0 || lv >= g_sets[s].n) continue;
        g_favs[n] = g_favs[i];
        f->maps[n] = g_sets[s].maps[lv];
        f->best_moves[n] = g_sets[s].best_moves[lv];
        f->best_pushes[n] = g_sets[s].best_pushes[lv];
        f->partial[n] = g_sets[s].partial[lv];
        f->best_run[n] = g_sets[s].best_run[lv];
        n++;
    }
    g_nfav = f->n = n;
}

/* favorite number pos -> its set (and level in *lv), or -1 */
static int fav_ref(int pos, int *lv)
{
    if (pos < 0 || pos >= g_nfav) return -1;
    *lv = g_favs[pos].lv;
    return set_by_name(g_favs[pos].set);
}

/* the proven fewest moves and pushes for a level (0 = not known); Favorites look up the original */
static void fewest_of(int set, int lv, int *m, int *p)
{
    *m = *p = 0;
    if (set == g_fav) set = fav_ref(lv, &lv);
    if (set < 0 || lv < 0 || lv >= g_sets[set].n) return;
    if (g_sets[set].fw_moves) *m = g_sets[set].fw_moves[lv];
    if (g_sets[set].fw_pushes) *p = g_sets[set].fw_pushes[lv];
}

static void save_all(void);
static void fav_toggle(int set, int lv)
{
    int i = fav_find(set, lv);
    if (i >= 0) { memmove(g_favs + i, g_favs + i + 1, (g_nfav - i - 1) * sizeof g_favs[0]); g_nfav--; }
    else if (g_nfav < FAV_MAX && set >= 0 && set != g_fav) {
        snprintf(g_favs[g_nfav].set, sizeof g_favs[g_nfav].set, "%s", g_sets[set].name);
        g_favs[g_nfav].lv = lv;
        g_nfav++;
    }
    fav_sync();
    save_all();
}

static int g_save_restored;          /* the save was damaged and the backup was used */
static void load_save(void)
{
    const char *path = SAVE_FILE;
    if (!save_complete(SAVE_FILE) && save_complete(SAVE_FILE ".bak")) { path = SAVE_FILE ".bak"; g_save_restored = 1; }
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char line[40000];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        int v;
        if (sscanf(line, "palette %d", &v) == 1) cfg.palette = clampi(v, 0, NUM_PALETTES - 1);
        else if (sscanf(line, "animal %d", &v) == 1) cfg.animal = clampi(v, -1, NUM_ANIMALS - 1);
        else if (sscanf(line, "music %d", &v) == 1) cfg.music_vol = clampi(v, 0, 10);
        else if (sscanf(line, "sfx %d", &v) == 1) cfg.sfx_vol = clampi(v, 0, 10);
        else if (sscanf(line, "zoom %d", &v) == 1) cfg.zoom = clampi(v, 0, 3);
        else if (!strncmp(line, "fav ", 4) && g_nfav < FAV_MAX) {
            char *bar = strrchr(line + 4, '|');
            if (!bar) continue;
            *bar = 0;
            snprintf(g_favs[g_nfav].set, sizeof g_favs[g_nfav].set, "%s", line + 4);
            g_favs[g_nfav].lv = atoi(bar + 1) - 1;
            g_nfav++;
        }
        else if (!strncmp(line, "my ", 3) && g_my >= 0 && g_sets[g_my].n < MY_MAX) {
            int fl = 0, par = 0;
            char *a = strchr(line + 3, '|'), *b = a ? strchr(a + 1, '|') : NULL;
            if (!b || sscanf(line + 3, "%d|%d|", &fl, &par) != 2) continue;
            static char map[4096];
            int n = 0;
            for (char *c = b + 1; *c && n < (int)sizeof map - 2; c++) map[n++] = *c == '|' ? '\n' : *c == '-' ? ' ' : *c;
            map[n++] = '\n'; map[n] = 0;
            int i = g_sets[g_my].n++;
            g_sets[g_my].maps[i] = strdup(map);
            g_my_flags[i] = (uint8_t)fl;
            g_my_par[i] = par;
        }
        else if (!strncmp(line, "partial ", 8) || !strncmp(line, "run ", 4)) {
            int run = line[0] == 'r';
            char *bar = strchr(line + (run ? 4 : 8), '|'), *m;
            if (!bar) continue;
            *bar = 0;
            int s = set_by_name(line + (run ? 4 : 8)), lv = atoi(bar + 1);
            if (s < 0 || lv < 1 || lv > g_sets[s].n || !(m = strchr(bar + 1, '|'))) continue;
            if (run) set_best_run(s, lv - 1, m + 1, (int)strlen(m + 1));
            else set_partial(s, lv - 1, m + 1, (int)strlen(m + 1));
        }
        else if (!strncmp(line, "current ", 8) || !strncmp(line, "solved ", 7)) {
            int solved = line[0] == 's';
            char *p = line + (solved ? 7 : 8);
            char *bar = strchr(p, '|');
            if (!bar) continue;
            *bar = 0;
            int s = set_by_name(p);
            if (s < 0) continue;
            int lv = 0, a = 0, b = 0;
            if (solved) {
                if (sscanf(bar + 1, "%d|%d|%d", &lv, &a, &b) == 3 && lv >= 1 && lv <= g_sets[s].n) {
                    g_sets[s].best_moves[lv - 1] = a;
                    g_sets[s].best_pushes[lv - 1] = b;
                }
            } else if (sscanf(bar + 1, "%d|%d|", &lv, &a) == 2 && lv >= 1 && lv <= g_sets[s].n) {
                cfg.cur_set = s;
                cfg.cur_level = lv - 1;
                cfg.last_animal = clampi(a, 0, NUM_ANIMALS - 1);
                char *m = strchr(bar + 1, '|');
                m = m ? strchr(m + 1, '|') : NULL;
                snprintf(cfg.cur_moves, sizeof cfg.cur_moves, "%s", m ? m + 1 : "");
            }
        }
    }
    fclose(f);
    fav_sync();
    /* the level "Continue" goes back to is in progress too */
    if (g_nsets && cfg.cur_moves[0] && !has_partial(cfg.cur_set, cfg.cur_level))
        set_partial(cfg.cur_set, cfg.cur_level, cfg.cur_moves, (int)strlen(cfg.cur_moves));
}

static int set_solved(int s)
{
    int c = 0;
    for (int i = 0; i < g_sets[s].n; i++) c += g_sets[s].best_moves[i] != 0;
    return c;
}

/* ======================================================================
 * Level files: "; Name" header, then "; N" + map rows per level
 * ==================================================================== */

static int is_map_line(const char *s)
{
    int hash = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (!strchr(" #@$.*+-_", *s)) return 0;
        if (*s == '#') hash = 1;
    }
    return hash;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int set_name_taken(const char *name, int upto)
{
    if (!strcmp(name, MY_NAME) || !strcmp(name, FAV_NAME)) return 1;
    for (int s = 0; s < upto; s++) if (!strcmp(g_sets[s].name, name)) return 1;
    return 0;
}

static void load_set_dir(const char *dir, int imported)
{
    DIR *d = opendir(dir);
    if (!d) { if (!imported) fprintf(stderr, "no %s folder\n", dir); return; }
    char *names[MAX_SETS];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(d)) && nn < MAX_SETS - 1 - g_nsets) {
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcasecmp(e->d_name + l - 4, ".txt")) names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], cmp_str);
    for (int i = 0; i < nn; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "r");
        if (!f) { free(names[i]); continue; }
        Set *s = &g_sets[g_nsets];
        memset(s, 0, sizeof *s);
        s->imported = imported;
        snprintf(s->file, sizeof s->file, "%s", names[i]);
        /* default name: file name without "NN-" prefix and ".txt" */
        const char *nm = strchr(names[i], '-') ? strchr(names[i], '-') + 1 : names[i];
        snprintf(s->name, sizeof s->name, "%.*s", (int)(strlen(nm) - 4), nm);
        int cap = 64;
        s->maps = malloc(cap * sizeof *s->maps);
        char line[512], buf[8192];
        int blen = 0, first = 1;
        for (;;) {
            char *ok = fgets(line, sizeof line, f);
            if (ok) {
                line[strcspn(line, "\r\n")] = 0;
                size_t ll = strlen(line);
                while (ll && line[ll - 1] == ' ') line[--ll] = 0;
            }
            if (ok && first && line[0] == ';') {           /* "; Microban II" */
                const char *p = line + 1;
                while (*p == ' ') p++;
                if (*p && !isdigit((unsigned char)*p)) snprintf(s->name, sizeof s->name, "%s", p);
                first = 0;
                continue;
            }
            if (ok && is_map_line(line)) {
                if (blen + (int)strlen(line) + 2 < (int)sizeof buf) blen += sprintf(buf + blen, "%s\n", line);
                continue;
            }
            if (blen) {
                if (s->n == cap) { cap *= 2; s->maps = realloc(s->maps, cap * sizeof *s->maps); }
                buf[blen] = 0;
                s->maps[s->n++] = strdup(buf);
                blen = 0;
            }
            if (!ok) break;
        }
        fclose(f);
        free(names[i]);
        if (!s->n) { free(s->maps); continue; }
        /* two sets can't share a name (progress is saved by name) */
        for (int k = 2; set_name_taken(s->name, g_nsets) && k < 100; k++) {
            char nm[48];
            snprintf(nm, sizeof nm, "%.40s (%d)", s->name, k);
            if (!set_name_taken(nm, g_nsets)) snprintf(s->name, sizeof s->name, "%s", nm);
        }
        s->best_moves = calloc(s->n, sizeof(int));
        s->best_pushes = calloc(s->n, sizeof(int));
        s->partial = calloc(s->n, sizeof(char *));
        s->best_run = calloc(s->n, sizeof(char *));
        g_nsets++;
    }
}

static void load_sets(void)
{
    load_set_dir(LEVEL_DIR, 0);
    for (int i = 0; i < g_nsets; i++)                 /* the proven fewest moves / pushes for the built-in sets */
        for (const Fewest *f = FEWEST; f->set; f++)
            if (!strcmp(f->set, g_sets[i].name) && f->n == g_sets[i].n) { g_sets[i].fw_moves = f->moves; g_sets[i].fw_pushes = f->pushes; }
#ifdef __EMSCRIPTEN__
    load_set_dir(IMPORT_DIR, 1);
#endif
    /* Favorites: a window onto levels in the other sets (filled in by fav_sync) */
    Set *fs = &g_sets[g_nsets];
    memset(fs, 0, sizeof *fs);
    snprintf(fs->name, sizeof fs->name, FAV_NAME);
    fs->maps = calloc(FAV_MAX, sizeof *fs->maps);
    fs->best_moves = calloc(FAV_MAX, sizeof(int));
    fs->best_pushes = calloc(FAV_MAX, sizeof(int));
    fs->partial = calloc(FAV_MAX, sizeof(char *));
    fs->best_run = calloc(FAV_MAX, sizeof(char *));
    g_fav = g_nsets++;
    /* My levels: empty until you make one */
    Set *s = &g_sets[g_nsets];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, MY_NAME);
    s->maps = calloc(MY_MAX, sizeof *s->maps);
    s->best_moves = calloc(MY_MAX, sizeof(int));
    s->best_pushes = calloc(MY_MAX, sizeof(int));
    s->partial = calloc(MY_MAX, sizeof(char *));
    s->best_run = calloc(MY_MAX, sizeof(char *));
    memset(g_my_flags, 0, sizeof g_my_flags);
    memset(g_my_par, 0, sizeof g_my_par);
    g_my = g_nsets++;
}

/* everything, so the sets can be read again (after adding or removing one) */
static void free_sets(void)
{
    for (int s = 0; s < g_nsets; s++) {
        Set *st = &g_sets[s];
        for (int i = 0; i < st->n && s != g_fav; i++) { free(st->maps[i]); free(st->partial[i]); free(st->best_run[i]); }   /* Favorites only borrows */
        free(st->maps); free(st->best_moves); free(st->best_pushes); free(st->partial); free(st->best_run);
        memset(st, 0, sizeof *st);
    }
    g_nsets = 0;
    g_my = -1;
    g_fav = -1;
    g_nfav = 0;
}

/* ======================================================================
 * The puzzle
 * ==================================================================== */

enum { CL_OUT = 0, CL_FLOOR, CL_WALL };

typedef struct {
    int w, h;
    uint8_t cell[MAXH][MAXW];
    uint8_t gap[MAXH][MAXW];            /* forest between two hedges that only meet corner to corner (see below) */
    uint8_t goal[MAXH][MAXW];
    uint8_t box[MAXH][MAXW];
    int px, py;
    int sx, sy;                         /* start position */
    uint8_t sbox[MAXH][MAXW];           /* start acorns */
    uint8_t dead[MAXH][MAXW];           /* floor where an acorn can never reach a spot again */
    int nboxes;
    char *hist; int hlen, hcap, hpos;   /* lrud = step, LRUD = push; hpos = current point (redo beyond) */
    int pushes;
} Puzzle;

static Puzzle P;

static const int DX[4] = { -1, 1, 0, 0 }, DY[4] = { 0, 0, -1, 1 };
static const char DCH[4] = { 'l', 'r', 'u', 'd' };
static int dir_of(char c)
{
    switch (tolower((unsigned char)c)) { case 'l': return 0; case 'r': return 1; case 'u': return 2; case 'd': return 3; }
    return -1;
}

/* Where the outer hedge only meets corner to corner, the forest square tucked between the two hedges
   gets their end caps: each hedge reaches into it and ends in a rounded tip, and the tips overlap,
   so no grass leaks out at the corner. The square counts as hedge for joining the neighbours up
   (cell = CL_WALL) and is marked in gap[][] so draw_world draws caps there instead of a full hedge.
   (It's never reachable, so this only changes the look; the level itself is unchanged.) */
static void fill_diagonal_gaps(Puzzle *p, int w, int h)
{
    uint8_t (*cell)[MAXW] = p->cell, (*fill)[MAXW] = p->gap;
    memset(p->gap, 0, sizeof p->gap);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (cell[y][x] != CL_OUT) continue;
            for (int dy = -1; dy <= 1; dy += 2)
                for (int dx = -1; dx <= 1; dx += 2) {
                    int ax = x + dx, ay = y + dy;
                    if (ax < 0 || ay < 0 || ax >= w || ay >= h) continue;
                    if (cell[y][ax] == CL_WALL && cell[ay][x] == CL_WALL && cell[ay][ax] == CL_FLOOR) fill[y][x] = 1;
                }
        }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (fill[y][x]) cell[y][x] = CL_WALL;
}

static unsigned g_wall_gen;                 /* bumped whenever a level's walls are (re)built (stumps) */
static int parse_level(const char *map)
{
    g_wall_gen++;
    memset(&P.cell, 0, sizeof P.cell);
    memset(&P.goal, 0, sizeof P.goal);
    memset(&P.box, 0, sizeof P.box);
    int y = 0, x = 0, w = 0, minx = MAXW;
    P.px = P.py = -1;
    char rows[MAXH][MAXW + 1];
    memset(rows, 0, sizeof rows);
    const char *p = map;
    while (*p && y < MAXH) {
        const char *e = strchr(p, '\n');
        int l = e ? (int)(e - p) : (int)strlen(p);
        if (l > MAXW) l = MAXW;
        memcpy(rows[y], p, l);
        rows[y][l] = 0;
        for (x = 0; x < l; x++) if (p[x] != ' ') { if (x < minx) minx = x; break; }
        y++;
        if (!e) break;
        p = e + 1;
    }
    int h = y;
    if (minx == MAXW) minx = 0;
    for (y = 0; y < h; y++) {
        int l = (int)strlen(rows[y]);
        for (x = minx; x < l; x++) {
            char c = rows[y][x];
            int cx = x - minx;
            if (cx + 1 > w) w = cx + 1;
            if (c == '#') P.cell[y][cx] = CL_WALL;
            if (c == '.' || c == '*' || c == '+') P.goal[y][cx] = 1;
            if (c == '$' || c == '*') P.box[y][cx] = 1;
            if (c == '@' || c == '+') { P.px = cx; P.py = y; }
        }
    }
    P.w = w; P.h = h;
    if (P.px < 0) return 0;
    /* flood fill from the player: reachable non-wall squares are floor, the rest is outside */
    static int stack[MAXW * MAXH];
    int sp = 0;
    stack[sp++] = P.py * MAXW + P.px;
    P.cell[P.py][P.px] = CL_FLOOR;
    while (sp) {
        int v = stack[--sp], cx = v % MAXW, cy = v / MAXW;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            if (P.cell[ny][nx] != CL_OUT) continue;
            P.cell[ny][nx] = CL_FLOOR;
            stack[sp++] = ny * MAXW + nx;
        }
    }
    /* acorns or spots the player can't reach: if they sit in a pocket closed off by hedges, the whole
       pocket is grass (a finished corner of the level); if not, just their own squares are */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            if (!(P.box[y][x] || P.goal[y][x]) || P.cell[y][x] != CL_OUT) continue;
            int n = 0, open = 0;
            sp = 0;
            stack[sp++] = y * MAXW + x;
            P.cell[y][x] = CL_FLOOR;
            static int seen[MAXW * MAXH];
            seen[n++] = y * MAXW + x;
            while (sp) {
                int v = stack[--sp], cx = v % MAXW, cy = v / MAXW;
                if (cx == 0 || cy == 0 || cx == w - 1 || cy == h - 1) open = 1;   /* reaches the edge: not closed */
                for (int d = 0; d < 4; d++) {
                    int nx = cx + DX[d], ny = cy + DY[d];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h || P.cell[ny][nx] != CL_OUT) continue;
                    P.cell[ny][nx] = CL_FLOOR;
                    stack[sp++] = ny * MAXW + nx;
                    seen[n++] = ny * MAXW + nx;
                }
            }
            if (open)                                       /* undo, keeping only the acorns and spots */
                for (int i = 0; i < n; i++) {
                    int cx = seen[i] % MAXW, cy = seen[i] / MAXW;
                    if (!P.box[cy][cx] && !P.goal[cy][cx]) P.cell[cy][cx] = CL_OUT;
                }
        }
    fill_diagonal_gaps(&P, w, h);
    P.nboxes = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            P.nboxes += P.box[y][x];
        }
    /* dead squares: pull an imaginary acorn backwards from every spot; floor it can't reach is dead */
    {
        static uint8_t live[MAXH][MAXW];
        static int q[MAXW * MAXH];
        memset(live, 0, sizeof live);
        int qh = 0, qt = 0;
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                if (P.goal[y][x] && P.cell[y][x] == CL_FLOOR) { live[y][x] = 1; q[qt++] = y * MAXW + x; }
        while (qh < qt) {
            int v = q[qh++], cx = v % MAXW, cy = v / MAXW;
            for (int d = 0; d < 4; d++) {
                int nx = cx + DX[d], ny = cy + DY[d], px2 = nx + DX[d], py2 = ny + DY[d];
                if (px2 < 0 || py2 < 0 || px2 >= w || py2 >= h) continue;
                if (P.cell[ny][nx] != CL_FLOOR || P.cell[py2][px2] != CL_FLOOR || live[ny][nx]) continue;
                live[ny][nx] = 1;
                q[qt++] = ny * MAXW + nx;
            }
        }
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++) P.dead[y][x] = P.cell[y][x] == CL_FLOOR && !live[y][x];
    }
    P.sx = P.px; P.sy = P.py;
    memcpy(P.sbox, P.box, sizeof P.box);
    P.hlen = P.hpos = 0;
    P.pushes = 0;
    return 1;
}

static int solved(void)
{
    for (int y = 0; y < P.h; y++)
        for (int x = 0; x < P.w; x++)
            if (P.box[y][x] && !P.goal[y][x]) return 0;
    return 1;
}

static void hist_push(char c)
{
    if (P.hlen + 1 >= P.hcap) {
        P.hcap = P.hcap ? P.hcap * 2 : 1024;
        P.hist = realloc(P.hist, P.hcap);
    }
    P.hist[P.hlen++] = c;
}

/* try to move; returns 0 = blocked, 1 = step, 2 = push */
static int do_move(int d, int record)
{
    int nx = P.px + DX[d], ny = P.py + DY[d];
    if (nx < 0 || ny < 0 || nx >= P.w || ny >= P.h || P.cell[ny][nx] != CL_FLOOR) return 0;
    int push = 0;
    if (P.box[ny][nx]) {
        int bx = nx + DX[d], by = ny + DY[d];
        if (bx < 0 || by < 0 || bx >= P.w || by >= P.h || P.cell[by][bx] != CL_FLOOR || P.box[by][bx]) return 0;
        P.box[ny][nx] = 0;
        P.box[by][bx] = 1;
        push = 1;
        P.pushes++;
    }
    P.px = nx; P.py = ny;
    if (record) {
        char c = push ? (char)toupper(DCH[d]) : DCH[d];
        if (P.hpos < P.hlen && P.hist[P.hpos] == c) P.hpos++;        /* same as the redo path: keep it */
        else { P.hlen = P.hpos; hist_push(c); P.hpos = P.hlen; }
    }
    return push ? 2 : 1;
}

static int undo_move(void)
{
    if (P.hpos <= 0) return 0;
    char c = P.hist[--P.hpos];
    int d = dir_of(c);
    if (isupper((unsigned char)c)) {
        int bx = P.px + DX[d], by = P.py + DY[d];
        P.box[by][bx] = 0;
        P.box[P.py][P.px] = 1;
        P.pushes--;
    }
    P.px -= DX[d]; P.py -= DY[d];
    return isupper((unsigned char)c) ? 2 : 1;
}

static int redo_move(void)
{
    if (P.hpos >= P.hlen) return 0;
    int d = dir_of(P.hist[P.hpos]);
    int r = do_move(d, 0);
    if (r) P.hpos++;
    return r;
}

/* ======================================================================
 * Drawing
 * ==================================================================== */

/* Everything is drawn into our own 32-bit framebuffer, so the same code runs on the Miyoo (SDL 1.2)
   and in a browser (copied to a <canvas>). Pixels are PIX(r,g,b): ARGB on the device, and RGBA bytes
   in memory on the web so the buffer can go straight into an ImageData. Alpha 0 = transparent. */
#ifdef __EMSCRIPTEN__
#define PIX(r, g, b) (0xFF000000u | ((Uint32)(b) << 16) | ((Uint32)(g) << 8) | (Uint32)(r))
#else
#define PIX(r, g, b) (0xFF000000u | ((Uint32)(r) << 16) | ((Uint32)(g) << 8) | (Uint32)(b))
static SDL_Surface *scr, *video, *rotbuf;
#endif
static Uint32 g_fb[SCREEN_W * SCREEN_H];
static int g_rotate = 0;
static Uint32 g_col[4];           /* the palette's four shades, mapped */
static int clip_x0, clip_y0, clip_x1 = SCREEN_W, clip_y1 = SCREEN_H;

static void set_clip(int x, int y, int w, int h)
{
    clip_x0 = clampi(x, 0, SCREEN_W); clip_y0 = clampi(y, 0, SCREEN_H);
    clip_x1 = clampi(x + w, 0, SCREEN_W); clip_y1 = clampi(y + h, 0, SCREEN_H);
}
static void no_clip(void) { clip_x0 = clip_y0 = 0; clip_x1 = SCREEN_W; clip_y1 = SCREEN_H; }

#ifdef __EMSCRIPTEN__
static void present(void)
{
    EM_ASM({ if (Module.stumpedPresent) Module.stumpedPresent(HEAPU8, $0); }, g_fb);
}
#else
static void present(void)
{
    if (!g_rotate) {
        SDL_BlitSurface(scr, NULL, video, NULL);
    } else {
        SDL_Surface *dst = (video->format->BitsPerPixel == 32) ? video : rotbuf;
        if (SDL_MUSTLOCK(dst)) SDL_LockSurface(dst);
        for (int y = 0; y < SCREEN_H; y++) {
            const Uint32 *src = g_fb + y * SCREEN_W;
            Uint32 *out = (Uint32 *)((Uint8 *)dst->pixels + (SCREEN_H - 1 - y) * dst->pitch) + (SCREEN_W - 1);
            for (int x = 0; x < SCREEN_W; x++) *out-- = *src++;
        }
        if (SDL_MUSTLOCK(dst)) SDL_UnlockSurface(dst);
        if (dst != video) SDL_BlitSurface(rotbuf, NULL, video, NULL);
    }
    SDL_Flip(video);
}
#endif

static void rect(int x, int y, int w, int h, Uint32 c)
{
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    for (int yy = y0; yy < y1; yy++) {
        Uint32 *row = g_fb + yy * SCREEN_W;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

static void frame_rect(int x, int y, int w, int h, int t, Uint32 c)
{
    rect(x, y, w, t, c); rect(x, y + h - t, w, t, c);
    rect(x, y, t, h, c); rect(x + w - t, y, t, h, c);
}

/* a little heart: favorites */
static void heart(int x, int y, int sc, Uint32 c)
{
    static const char *H[5] = { ".X.X.", "XXXXX", "XXXXX", ".XXX.", "..X.." };
    for (int r = 0; r < 5; r++) for (int k = 0; k < 5; k++) if (H[r][k] == 'X') rect(x + k * sc, y + r * sc, sc, sc, c);
}

static void star(int x, int y, int sc, Uint32 c)
{
    static const char *T[7] = { "...X...", "...X...", "XXXXXXX", ".XXXXX.", "..XXX..", ".XX.XX.", "X.....X" };
    for (int r = 0; r < 7; r++) for (int k = 0; k < 7; k++) if (T[r][k] == 'X') rect(x + k * sc, y + r * sc, sc, sc, c);
}

static void mini_star(int x, int y, int sc, Uint32 c)
{
    static const char *T[5] = { "..X..", "XXXXX", ".XXX.", ".X.X.", "X...X" };
    for (int r = 0; r < 5; r++) for (int k = 0; k < 5; k++) if (T[r][k] == 'X') rect(x + k * sc, y + r * sc, sc, sc, c);
}

#define BG0 g_col[0]
#define BG1 g_col[1]
#define BG2 g_col[2]
#define BG3 g_col[3]

static void glyph(int x, int y, unsigned char ch, int s, Uint32 c)
{
    if (ch >= 128) ch = '?';
    for (int r = 0; r < 8; r++) {
        unsigned bits = (unsigned char)font8x8_basic[ch][r];
        for (int b = 0; b < 8; b++)
            if (bits & (1u << b)) rect(x + b * s, y + r * s, s, s, c);
    }
}

static int text(int x, int y, const char *s, int sc, Uint32 c)
{
    for (; *s; s++, x += 8 * sc) glyph(x, y, (unsigned char)*s, sc, c);
    return x;
}
static int text_w(const char *s, int sc) { return (int)strlen(s) * 8 * sc; }
static void text_c(int cx, int y, const char *s, int sc, Uint32 c) { text(cx - text_w(s, sc) / 2, y, s, sc, c); }
static void text_r(int rx, int y, const char *s, int sc, Uint32 c) { text(rx - text_w(s, sc), y, s, sc, c); }
/* touch: a button drawn like UNDO / REDO, and what it does */
static void tbtn(int x, int y, int w, int h, const char *label, int kind, int *sel, int val)
{
    rect(x, y, w, h, g_col[2]);
    text_c(x + w / 2, y + (h - 16) / 2, label, 2, g_col[0]);
    tap_add(x - 4, y - 4, w + 8, h + 8, kind, sel, val);
}
/* a pop-up: a tap outside it is B (close), a tap on its panel does nothing (items are added after) */
static void modal_taps(int x, int y, int w, int h, int back)
{
    tap_add(0, 0, 640, 480, TAP_BTN, NULL, back);
    tap_add(x, y, w, h, TAP_NONE, NULL, 0);
}

/* -------- sprite cache: palette-mapped surfaces at the current tile size -------- */

/* All art is 16×16. On screen it's shown at 1×–4×: 64, 48, 32 or 16 px tiles. */
static const int TILE_SCALES[] = { 4, 3, 2, 1 };
#define NUM_TILE_SIZES 4

/* a sprite: square, pixels with alpha 0 are see-through */
typedef struct { int w, h, opaque; Uint32 px[]; } Img;

static Img *make_sprite_n(const uint8_t *px, int n, int scale, int flip)
{
    int sz = n * scale;
    Img *s = malloc(sizeof(Img) + sizeof(Uint32) * sz * sz);
    if (!s) return NULL;
    s->w = s->h = sz; s->opaque = 1;
    for (int y = 0; y < sz; y++) {
        Uint32 *row = s->px + y * sz;
        for (int x = 0; x < sz; x++) {
            int sx = x / scale, sy = y / scale;
            if (flip) sx = n - 1 - sx;
            uint8_t v = px[sy * n + sx];
            if (v == SPR_T) { row[x] = 0; s->opaque = 0; }
            else row[x] = g_col[v & 3];
        }
    }
    return s;
}
static Img *make_sprite(const uint8_t *px, int scale, int flip) { return make_sprite_n(px, SPR, scale, flip); }

typedef struct {
    int scale, animal, palette;
    Img *tile[NUM_TILES];
    Img *tilef[NUM_TILES];          /* mirrored (squirrels running left) */
    Img *hq[NUM_QT][4];             /* hedge quarter-tiles */
    Img *anim[NUM_FRAMES][2];
} SpriteSet;

static void spriteset_free(SpriteSet *c)
{
    for (int i = 0; i < NUM_TILES; i++) { free(c->tile[i]); free(c->tilef[i]); }
    for (int i = 0; i < NUM_QT; i++)
        for (int q = 0; q < 4; q++) free(c->hq[i][q]);
    for (int f = 0; f < NUM_FRAMES; f++)
        for (int k = 0; k < 2; k++) free(c->anim[f][k]);
    memset(c, 0, sizeof *c);
}

static void spriteset_get(SpriteSet *c, int scale, int animal)
{
    if (c->tile[0] && c->scale == scale && c->animal == animal && c->palette == cfg.palette) return;
    spriteset_free(c);
    c->scale = scale; c->animal = animal; c->palette = cfg.palette;
    for (int t = 0; t < NUM_TILES; t++) {
        c->tile[t] = make_sprite(&TILE[t][0][0], scale, 0);
        if (t >= TL_SQUIRREL) c->tilef[t] = make_sprite(&TILE[t][0][0], scale, 1);
    }
    for (int i = 0; i < NUM_QT; i++)
        for (int q = 0; q < 4; q++) c->hq[i][q] = make_sprite_n(&HEDGEQ[i][q][0][0], 8, scale, 0);
    for (int f = 0; f < NUM_FRAMES; f++)
        for (int k = 0; k < 2; k++)           /* [0] faces right; the fox and raccoon are drawn facing left */
            c->anim[f][k] = make_sprite(&ANIM[animal][f][0][0], scale, k ^ (animal == 2 || animal == 3));
}

static void blit(const Img *s, int x, int y)
{
    if (!s) return;
    int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
    int x1 = x + s->w > clip_x1 ? clip_x1 : x + s->w, y1 = y + s->h > clip_y1 ? clip_y1 : y + s->h;
    if (x0 >= x1) return;
    for (int yy = y0; yy < y1; yy++) {
        const Uint32 *src = s->px + (yy - y) * s->w + (x0 - x);
        Uint32 *dst = g_fb + yy * SCREEN_W + x0;
        int n = x1 - x0;
        if (s->opaque) memcpy(dst, src, n * sizeof(Uint32));
        else for (int i = 0; i < n; i++) if (src[i]) dst[i] = src[i];
    }
}

static void apply_palette(void)
{
    for (int i = 0; i < 4; i++) {
        Uint32 v = PALETTES[cfg.palette].rgb[i];
        g_col[i] = PIX((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    }
#ifdef __EMSCRIPTEN__
    /* the page around the game takes on the palette too */
    const Uint32 *c = PALETTES[cfg.palette].rgb;
    EM_ASM({ if (Module.stumpedPalette) Module.stumpedPalette($0, $1, $2, $3); }, c[0], c[1], c[2], c[3]);
#endif
}

/* ======================================================================
 * App state
 * ==================================================================== */

enum { SC_TITLE, SC_SETS, SC_LEVELS, SC_GAME, SC_OPTIONS, SC_CREDITS, SC_LISTEN, SC_EDIT, SC_CODE };

static struct {
    int screen;
    int dirty;
    /* menus */
    int title_sel, sets_sel, sets_top, lv_sel, lv_top, opt_sel, pause_sel;
    int options_return;
    /* game */
    int set, level, animal;
    int facing;                 /* 0 = right, 1 = left */
    int ts, tscale;             /* tile size in screen px, and the art's scale (1-4) */
    int ox, oy;                 /* where the level's top-left lands on screen (may be negative when scrolling) */
    int side;                   /* HUD on the right (tall levels) instead of across the top */
    int vx, vy, vw, vh;         /* the play area on screen */
    int scroll;                 /* level bigger than the screen */
    float camx, camy;
    int paused;
    int clear;                  /* 0 none, 1 dancing, 2 panel showing */
    Uint32 clear_t;
    int new_best;
    int prev_best_moves, prev_best_pushes;
    /* animation */
    Uint32 move_t;              /* when the current slide started */
    int fromx, fromy;           /* player slide origin */
    int cmx, cmy, cfx, cfy;     /* acorn slide: to (cmx,cmy) from (cfx,cfy); cmx < 0 = none */
    int pose;                   /* FR_STEP or FR_PUSH while sliding */
    unsigned seed;              /* per-level: which lone walls are stumps, berries, fireflies */
    int fit_scale;              /* the scale that shows the whole level (or 1) */
    int map_held;               /* Y held: overview map */
    int confirm, confirm_sel;   /* clear-progress dialog (CF_*) and its cursor */
    int wob_x, wob_y; Uint32 wob_t;   /* a stuck acorn wobbles */
    float ff_pull;              /* 0..1: fireflies gathering around an idle animal */
    Uint32 level_t;             /* when this level was opened */
    Uint32 last_input;
    Uint32 blink_at, fidget_at;
    int fidget;
    /* held buttons */
    int held_dir; Uint32 dir_next;
    int queue[4], nqueue;       /* presses that arrived mid-slide: done as soon as the slide ends */
    /* tap a square to walk there (touch or a mouse click): the steps still to take, and the walk as
       one piece of history (moves wg_from..wg_to) so one UNDO takes it all back and one REDO redoes it */
    unsigned char walk[MAXW * MAXH]; int nwalk, iwalk, walking;
    int wg_from, wg_to, wg_start;
    int rewind_to, replay_to;   /* undoing / redoing a whole walk, a step at a time */
    int undo_held; Uint32 undo_next;
    int redo_held; Uint32 redo_next;
    Uint32 restart_since;
    Uint32 switch_since; int switch_dir;   /* holding L1/R1 to change level */
    Uint32 quick_until;                    /* after a held skip, taps skip too until then */
    /* now and then, on a level with a stump, a barn owl swoops in and sits on it for a while */
    int owl_state, owl_x, owl_y, owl_side, owl_look;     /* OWL_* ; the stump; which side it comes from */
    Uint32 owl_t, owl_stay, owl_look_until, owl_blink;
    int resume_set, resume_level;          /* the level the Continue / Start over dialog is about */
    char toast[96]; Uint32 toast_until;
    Uint32 save_due;
    int playback; Uint32 play_next;        /* the best run playing itself */
    int testing;                           /* test-playing a level from the level maker */
    int watching;                          /* watching the solver's run (from the maker): no records */
    int fav_play, fav_pos;                 /* playing through Favorites: next / previous level go through it */
} S;

static SpriteSet g_spr;          /* in-game sprite set */
/* menus show many animals at once: keep a small pool of ready-made sets */
#define POOL 24
static SpriteSet g_pool[POOL];
static Uint32 g_pool_used[POOL], g_pool_clock;
/* pushing: lean one art pixel toward the push (the frog's push frame leans by itself) */
static int lean_dx(int animal, int frame, int facing_left, int unit)
{
    if (frame != FR_PUSH || animal == 1) return 0;
    return facing_left ? -unit : unit;
}

static SpriteSet *ui_set(int scale, int animal)
{
    int lru = 0;
    for (int i = 0; i < POOL; i++) {
        SpriteSet *c = &g_pool[i];
        if (c->tile[0] && c->scale == scale && c->animal == animal && c->palette == cfg.palette) {
            g_pool_used[i] = ++g_pool_clock;
            return c;
        }
        if (g_pool_used[i] < g_pool_used[lru]) lru = i;
    }
    spriteset_get(&g_pool[lru], scale, animal);
    g_pool_used[lru] = ++g_pool_clock;
    return &g_pool[lru];
}

static void toast(const char *m)
{
    snprintf(S.toast, sizeof S.toast, "%s", m);
    S.toast_until = now_ms() + 1800;
    S.dirty = 1;
}

/* save the current level's moves so "Continue" resumes mid-puzzle */
static void remember_progress(void)
{
    cfg.cur_set = S.set;
    cfg.cur_level = S.level;
    cfg.last_animal = S.animal;
    int n = P.hpos < (int)sizeof cfg.cur_moves - 1 ? P.hpos : (int)sizeof cfg.cur_moves - 1;
    if (S.clear) n = 0;
    memcpy(cfg.cur_moves, P.hist ? P.hist : "", n);
    cfg.cur_moves[n] = 0;
    set_partial(S.set, S.level, cfg.cur_moves, n);
}

#define SIDE_W 160
/* Largest tile size that shows the whole level. The HUD normally runs across the top; a tall level
   that would only fit with the HUD down the right-hand side gets that layout instead. Only when
   nothing fits even at 16 px does the level scroll. */
static void report_progress(void);
static void camera_target(float *tx, float *ty);
static void squirrels_start(void);
static void panel(int x, int y, int w, int h);

static void choose_tile_size(void)
{
    int pick = -1, side = 0;
    for (int i = 0; i < NUM_TILE_SIZES && pick < 0; i++) {
        int t = SPR * TILE_SCALES[i];
        if (P.w * t <= SCREEN_W && P.h * t <= PLAY_H) { pick = i; side = 0; }
        else if (P.w * t <= SCREEN_W - SIDE_W && P.h * t <= SCREEN_H) { pick = i; side = 1; }
    }
    if (pick < 0) {                       /* scrolling at 1x: sidebar if the width then fits */
        pick = NUM_TILE_SIZES - 1;
        side = P.w * SPR <= SCREEN_W - SIDE_W;
    }
    S.fit_scale = TILE_SCALES[pick];
    S.tscale = clampi(S.fit_scale + cfg.zoom, 1, 4);
    S.ts = SPR * S.tscale;
    if (S.tscale != S.fit_scale)          /* zoomed in: sidebar only if the level still fits beside it */
        side = P.w * S.ts <= SCREEN_W - SIDE_W && P.h * S.ts <= SCREEN_H && !(P.w * S.ts <= SCREEN_W && P.h * S.ts <= PLAY_H);
    S.side = side;
    if (side) { S.vx = 0; S.vy = 0; S.vw = SCREEN_W - SIDE_W; S.vh = SCREEN_H; }
    else { S.vx = 0; S.vy = PLAY_Y; S.vw = SCREEN_W; S.vh = PLAY_H; }
    S.scroll = P.w * S.ts > S.vw || P.h * S.ts > S.vh;
}

static void set_zoom(int z)
{
    z = clampi(z, 0, 3);
    if (S.fit_scale + z > 4) z = 4 - S.fit_scale;
    if (z == cfg.zoom) { toast(z ? "Zoomed in all the way" : "Whole level in view"); return; }
    cfg.zoom = z;
    choose_tile_size();
    spriteset_get(&g_spr, S.tscale, S.animal);
    camera_target(&S.camx, &S.camy);
    char b[48];
    if (z) snprintf(b, sizeof b, "Zoom %dx", S.tscale);
    else snprintf(b, sizeof b, "Zoom: whole level");
    toast(b);
    sfx_play(SFX_MENU);
    S.save_due = now_ms() + 1000;
}

static void camera_target(float *tx, float *ty)
{
    int lw = P.w * S.ts, lh = P.h * S.ts, vw = S.vw, vh = S.vh;
    /* a level that fits is centred, snapped to the 16 px grid */
    float cx = (lw <= vw) ? (float)(((lw - vw) / 2) / 16 * 16) : P.px * S.ts + S.ts / 2.0f - vw / 2.0f;
    float cy = (lh <= vh) ? (float)(((lh - vh) / 2) / 16 * 16) : P.py * S.ts + S.ts / 2.0f - vh / 2.0f;
    if (lw > vw) cx = cx < 0 ? 0 : cx > lw - vw ? lw - vw : cx;
    if (lh > vh) cy = cy < 0 ? 0 : cy > lh - vh ? lh - vh : cy;
    *tx = cx; *ty = cy;
}

static void barn_owl_plan(void);
static void start_level(int set, int level, const char *resume_moves)
{
    if (set < 0 || set >= g_nsets) return;
    level = clampi(level, 0, g_sets[set].n - 1);
    S.set = set; S.level = level;
    if (!parse_level(g_sets[set].maps[level])) { toast("This level couldn't be read"); return; }
    if (cfg.animal >= 0) S.animal = cfg.animal;
    else if (!resume_moves || !*resume_moves) {
        int a;
        do a = rand() % NUM_ANIMALS; while (a == S.animal && NUM_ANIMALS > 1);
        S.animal = a;
    }
    if (resume_moves) {
        for (const char *m = resume_moves; *m; m++) {
            int d = dir_of(*m);
            if (d < 0 || !do_move(d, 1)) break;
        }
    }
    S.seed = (unsigned)(set * 1000 + level);
    choose_tile_size();
    spriteset_get(&g_spr, S.tscale, S.animal);
    camera_target(&S.camx, &S.camy);
    S.facing = 0;
    S.clear = 0;
    S.paused = 0;
    S.cmx = -1;
    S.move_t = 0;
    S.fromx = P.px; S.fromy = P.py;
    S.screen = SC_GAME;
    S.last_input = now_ms();
    S.level_t = now_ms();
    S.wob_t = 0;
    S.ff_pull = 0;
    S.blink_at = now_ms() + 2500;
    S.fidget_at = now_ms() + 6000;
    S.held_dir = -1;
    S.nqueue = 0;
    S.nwalk = S.iwalk = S.walking = 0; S.wg_from = S.wg_to = S.rewind_to = S.replay_to = -1;
    S.restart_since = 0;
    S.switch_since = 0;
    S.playback = 0;
    S.testing = 0;
    S.watching = 0;
    barn_owl_plan();
    remember_progress();
    save_all();
    report_progress();
    S.dirty = 1;
}

static void level_won(void)
{
    if (S.watching) {                      /* the solver's run: nothing to record */
        S.new_best = 0;
        S.clear = 1; S.clear_t = now_ms();
        squirrels_start();
        sfx_play(SFX_CLEAR);
        return;
    }
    Set *s = &g_sets[S.set];
    int m = P.hpos, pu = P.pushes;
    S.prev_best_moves = s->best_moves[S.level];
    S.prev_best_pushes = s->best_pushes[S.level];
    S.new_best = 0;
    /* fewest moves and fewest pushes are each your best in any solve; the best run (kept to replay)
       is the one with the fewest moves */
    int bm = s->best_moves[S.level] > 0 ? s->best_moves[S.level] : 0, bp = s->best_pushes[S.level];
    int fewer_moves = !bm || m < bm, fewer_pushes = !bp || pu < bp;
    if (fewer_moves) s->best_moves[S.level] = m;
    if (fewer_pushes) s->best_pushes[S.level] = pu;
    if (bm) S.new_best = (fewer_moves ? 1 : 0) | (fewer_pushes ? 2 : 0);
    if (fewer_moves || (m == bm && fewer_pushes) || !has_best_run(S.set, S.level)) set_best_run(S.set, S.level, P.hist, m);
    if (S.set == g_my) g_my_flags[S.level] |= MY_OK;      /* solved: it has a code now */
    S.clear = 1;
    S.clear_t = now_ms();
    squirrels_start();
    cfg.cur_moves[0] = 0;
    set_partial(S.set, S.level, NULL, 0);
    save_all();
    sfx_play(SFX_CLEAR);
}

/* the next unsolved level after (set, level), wrapping within the set */
static int next_level_in_set(int set, int level)
{
    Set *s = &g_sets[set];
    for (int k = 1; k <= s->n; k++) {
        int i = (level + k) % s->n;
        if (!s->best_moves[i]) return i;
    }
    return (level + 1) % s->n;
}

/* ======================================================================
 * Game screen
 * ==================================================================== */

static void report_progress(void)
{
    int on = 0;
    for (int y = 0; y < P.h; y++)
        for (int x = 0; x < P.w; x++) on += P.box[y][x] && P.goal[y][x];
    music_progress(on, P.nboxes);
}

static float ease(float t) { return t < 0 ? 0 : t > 1 ? 1 : t * (2 - t); }

/* small 16 px icons for the HUD: the animal (moves) and an acorn (pushes) */
static void hud_icon_animal(int x, int y) { blit(ui_set(1, S.animal)->anim[FR_IDLE][0], x, y); }
static void hud_icon_acorn(int x, int y) { blit(ui_set(1, 0)->tile[TL_ACORN], x, y); }

static void restart_bar(int x, int y, int w)
{
    if (!S.restart_since) return;
    float t = (now_ms() - S.restart_since) / (float)RESTART_HOLD_MS;
    if (t > 1) t = 1;
    rect(x, y, w, 4, BG2);
    rect(x, y, ((int)(t * w)) / 2 * 2, 4, BG0);
}

static void draw_side_hud(void)
{
    int x0 = SCREEN_W - SIDE_W, cx = x0 + SIDE_W / 2;
    rect(x0, 0, SIDE_W, SCREEN_H, BG3);
    rect(x0, 0, 4, SCREEN_H, BG2);
    char name[64], b[32];
    snprintf(name, sizeof name, "%s", g_sets[S.set].name);
    char *sp = strchr(name, ' ');
    int y = 16;
    if (sp && text_w(name, 2) > SIDE_W - 16) { *sp = 0; text_c(cx + 2, y, name, 2, BG0); y += 24; text_c(cx + 2, y, sp + 1, 2, BG0); }
    else text_c(cx + 2, y, name, 2, BG0);
    y += 32;
    snprintf(b, sizeof b, "%d", S.level + 1);
    text_c(cx + 2, y, b, 4, BG0);
    y += 64;
    hud_icon_animal(x0 + 32, y);
    snprintf(b, sizeof b, "%d", P.hpos);
    text(x0 + 64, y, b, 2, BG0);
    y += 32;
    hud_icon_acorn(x0 + 32, y);
    snprintf(b, sizeof b, "%d", P.pushes);
    text(x0 + 64, y, b, 2, BG0);
    y += 48;
    int bm = g_sets[S.set].best_moves[S.level];
    if (bm > 0) {
        text_c(cx + 2, y, "BEST", 2, BG1);
        snprintf(b, sizeof b, "%d/%d", bm, g_sets[S.set].best_pushes[S.level]);
        text_c(cx + 2, y + 16, b, 2, BG1);
    }
    if (g_kbd == 2) {          /* touch: menu, undo and redo down the sidebar */
        static const char *const LB[3] = { "MENU", "UNDO", "REDO" };
        static const int BT[3] = { BTN_START, BTN_B, BTN_A };
        for (int i = 0; i < 3; i++) {
            int Y = SCREEN_H - 128 + i * 40;
            rect(x0 + 24, Y, SIDE_W - 40, 32, BG2);
            text_c(cx + 2, Y + 8, LB[i], 2, BG0);
            if (!(S.clear && BT[i] == BTN_A)) tap_add(x0 + 16, Y - 4, SIDE_W - 24, 40, TAP_BTN, NULL, BT[i]);
        }
        tap_add(x0, 0, SIDE_W, 96, TAP_BTN, NULL, BTN_START);
    }
    restart_bar(x0 + 16, SCREEN_H - 32, SIDE_W - 32);
}

static void draw_hud(void)
{
    if (S.side) { draw_side_hud(); return; }
    rect(0, 0, SCREEN_W, HUD_H, BG3);
    rect(0, HUD_H - 4, SCREEN_W, 4, BG2);
    char l[96], b[32];
    snprintf(l, sizeof l, "%s %d", g_sets[S.set].name, S.level + 1);
    int lx = 16;
    if (g_kbd == 2) {                     /* touch: the menu button, like Listen's */
        rect(8, 4, 36, 20, BG2);
        for (int i = 0; i < 3; i++) rect(16, 7 + i * 6, 20, 2, BG0);
        lx = 56;
    }
    /* right side, on the 16 px grid:  [animal] 12  [acorn] 3 */
    snprintf(b, sizeof b, "%d", P.pushes);
    int x = SCREEN_W - 16 - ((text_w(b, 2) + 15) / 16) * 16;
    text(x, 8, b, 2, BG0);
    hud_icon_acorn(x - 24, 6);
    x -= 48;
    snprintf(b, sizeof b, "%d", P.hpos);
    x -= ((text_w(b, 2) + 15) / 16) * 16;
    text(x, 8, b, 2, BG0);
    hud_icon_animal(x - 24, 6);
    {                          /* the name, cut short if the touch buttons need the room */
        int fav = fav_find(S.set, S.level) >= 0;
        int room = (g_kbd == 2 ? x - 24 - 16 - 2 * 80 - 16 : x - 24 - 16) - lx - (fav ? 26 : 0);
        if (text_w(l, 2) > room) {
            char num[16]; snprintf(num, sizeof num, " %d", S.level + 1);
            const char *nm = g_sets[S.set].name, *sp = strchr(nm, ' ');
            if (sp && sp - nm > 4) snprintf(l, sizeof l, "%.4s.%s%s", nm, sp, num);   /* "Sasq. VIII 50" */
            if (text_w(l, 2) > room) {
                int keep = (room - text_w(num, 2)) / 16 - 1;
                if (keep < 1) keep = 1;
                snprintf(l, sizeof l, "%.*s.%s", keep, nm, num);
            }
        }
        text(lx, 8, l, 2, BG0);
        if (fav) heart(lx + text_w(l, 2) + 10, 9, 2, BG1);
    }
    if (g_kbd == 2) {          /* touch: tap the name for the menu, and undo / redo buttons */
        int bx = x - 24 - 16 - 2 * 80;
        tap_add(0, 0, bx - 8, HUD_H + 8, TAP_BTN, NULL, BTN_START);
        for (int i = 0; i < 2; i++) {
            int X = bx + i * 80;
            rect(X, 4, 72, 20, BG2);
            text_c(X + 36, 6, i ? "REDO" : "UNDO", 2, BG0);
            if (!(S.clear && i)) tap_add(X - 4, 0, 80, HUD_H + 12, TAP_BTN, NULL, i ? BTN_A : BTN_B);
        }
    }
    restart_bar(0, HUD_H - 4, SCREEN_W);
}

/* ---------------- the forest ---------------- */

static unsigned cell_hash(int x, int y, unsigned seed)
{
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^ seed * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return h;
}

static int is_wall(const Puzzle *p, int x, int y)
{
    return x >= 0 && y >= 0 && x < p->w && y < p->h && p->cell[y][x] == CL_WALL;
}
static int is_floor(const Puzzle *p, int x, int y)
{
    return x >= 0 && y >= 0 && x < p->w && y < p->h && p->cell[y][x] == CL_FLOOR;
}
/* a wall with no wall beside it may be drawn as a tree stump instead of a lone bush */
/* At most MAX_STUMPS per level: of the lone walls that would be stumps, the ones with the lowest hash.
   Worked out once per level (and per Listen clearing), then remembered. */
#define MAX_STUMPS 5
static struct { const Puzzle *p; unsigned seed, gen; int used; uint8_t on[MAXH][MAXW]; } g_stumps[3];
static int stump_candidate(const Puzzle *p, int x, int y, unsigned seed)
{
    if (is_wall(p, x - 1, y) || is_wall(p, x + 1, y) || is_wall(p, x, y - 1) || is_wall(p, x, y + 1)) return 0;
    return cell_hash(x, y, seed) % 100 < 55;
}
static int is_stump(const Puzzle *p, int x, int y, unsigned seed)
{
    int k;
    for (k = 0; k < 3; k++)
        if (g_stumps[k].used && g_stumps[k].p == p && g_stumps[k].seed == seed && g_stumps[k].gen == g_wall_gen) break;
    if (k == 3) {                           /* work it out: the MAX_STUMPS lowest hashes */
        static int rr;
        k = rr++ % 3;
        g_stumps[k].p = p; g_stumps[k].seed = seed; g_stumps[k].gen = g_wall_gen; g_stumps[k].used = 1;
        memset(g_stumps[k].on, 0, sizeof g_stumps[k].on);
        for (int n = 0; n < MAX_STUMPS; n++) {
            int bx = -1, by = -1; unsigned best = 0;
            for (int yy = 0; yy < p->h; yy++)
                for (int xx = 0; xx < p->w; xx++) {
                    if (p->cell[yy][xx] != CL_WALL || g_stumps[k].on[yy][xx] || !stump_candidate(p, xx, yy, seed)) continue;
                    unsigned h = cell_hash(xx, yy, seed);
                    if (bx < 0 || h < best) { best = h; bx = xx; by = yy; }
                }
            if (bx < 0) break;
            g_stumps[k].on[by][bx] = 1;
        }
    }
    return x >= 0 && y >= 0 && x < MAXW && y < MAXH && g_stumps[k].on[y][x];
}

/* Outside the clearing: darkness with dark grass tufts that thin out with distance from the level.
   d = rough distance in cells to the nearest floor or hedge. */
static int outside_dist(const Puzzle *p, int x, int y)
{
    int best = 99;
    for (int dy = -5; dy <= 5; dy++)
        for (int dx = -5; dx <= 5; dx++) {
            int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= p->w || yy >= p->h || p->cell[yy][xx] == CL_OUT) continue;
            int ax = abs(dx), ay = abs(dy);
            int d = (ax > ay ? ax : ay) + (ax < ay ? ax : ay) / 2;
            if (d < best) best = d;
        }
    return best;
}

static void draw_outside(const Puzzle *p, int x, int y, int X, int Y, int unit)
{
    rect(X, Y, SPR * unit, SPR * unit, BG3);
    int d = outside_dist(p, x, y);
    /* tufts per tile: 3 right by the hedges, thinning to the odd one, then none */
    static const int CHANCE[] = { 100, 100, 80, 60, 40, 25, 12, 5 };   /* % chance for each spot */
    int chance = d < 8 ? CHANCE[d] : 0;
    static const int SPOT[4][2] = { { 3, 4 }, { 10, 10 }, { 12, 2 }, { 5, 12 } };
    unsigned h = cell_hash(x, y, 9);
    int maxn = d <= 1 ? 3 : d <= 4 ? 2 : 1;
    for (int i = 0, n = 0; i < 4 && n < maxn; i++) {
        unsigned r = (h >> (i * 7)) % 100;
        if ((int)r >= chance) continue;
        int tx = SPOT[i][0], ty = SPOT[i][1];
        rect(X + tx * unit, Y + ty * unit, unit, unit, BG2);
        rect(X + (tx + 2) * unit, Y + ty * unit, unit, unit, BG2);
        rect(X + (tx + 1) * unit, Y + (ty + 1) * unit, unit, unit, BG2);
        n++;
    }
}

/* draws the ground, the dark grass outside and the hedges of p; the view rectangle is in screen pixels */
static void draw_world(const Puzzle *p, SpriteSet *u, int ox, int oy, int vx, int vy, int vw, int vh, unsigned seed)
{
    int ts = SPR * u->scale, half = ts / 2;
    int gx0 = (vx - ox) / ts - 1, gx1 = (vx + vw - ox) / ts + 1;
    int gy0 = (vy - oy) / ts - 1, gy1 = (vy + vh - oy) / ts + 1;
    for (int y = gy0; y <= gy1; y++)
        for (int x = gx0; x <= gx1; x++) {
            int X = ox + x * ts, Y = oy + y * ts;
            unsigned h = cell_hash(x, y, 7);
            int in = x >= 0 && y >= 0 && x < p->w && y < p->h;
            int c = in ? p->cell[y][x] : CL_OUT;
            if (c == CL_FLOOR) {
                int fl = (h % 5 < 2) ? TL_FLOOR_B + (int)(h >> 8) % 2 : TL_FLOOR;
                blit(u->tile[fl], X, Y);
                if (p->goal[y][x]) blit(u->tile[TL_GOAL], X, Y);
                continue;
            }
            draw_outside(p, x, y, X, Y, u->scale);
            if (c != CL_WALL) continue;
            if (is_stump(p, x, y, seed)) {
                int ground = is_floor(p, x - 1, y) || is_floor(p, x + 1, y) || is_floor(p, x, y - 1) || is_floor(p, x, y + 1);
                if (ground) blit(u->tile[TL_FLOOR], X, Y);
                blit(u->tile[TL_STUMP], X, Y);
                continue;
            }
            int gap = p->gap[y][x];               /* end caps only: see fill_diagonal_gaps */
            for (int q = 0; q < 4; q++) {
                int sx = (q & 1) ? 1 : -1, sy = (q & 2) ? 1 : -1;
                int hn = is_wall(p, x + sx, y), vn = is_wall(p, x, y + sy), dn = is_wall(p, x + sx, y + sy);
                int qt, under;
                if (gap && !hn && !vn) continue;                                   /* the open corner: forest */
                if (gap && hn != vn) { qt = hn ? QT_CAP_V : QT_CAP_H; under = 0; }   /* a hedge's rounded end */
                else if (!hn && !vn) { qt = QT_OUTER; under = is_floor(p, x + sx, y) || is_floor(p, x, y + sy) || is_floor(p, x + sx, y + sy); }
                else if (hn && !vn) { qt = QT_EDGE_V; under = is_floor(p, x, y + sy); }
                else if (!hn && vn) { qt = QT_EDGE_H; under = is_floor(p, x + sx, y); }
                else if (!dn) { qt = QT_INNER; under = is_floor(p, x + sx, y + sy); }
                else {
                    unsigned r = cell_hash(x * 2 + (q & 1), y * 2 + (q >> 1), seed + 1) % 23;
                    qt = r == 0 ? QT_FULL_BERRY : r == 1 ? QT_FULL_FLOWER : QT_FULL;
                    under = 0;
                }
                int qx = X + ((q & 1) ? half : 0), qy = Y + ((q & 2) ? half : 0);
                if (under) rect(qx, qy, half, half, BG0);
                blit(u->hq[qt][q], qx, qy);
            }
        }
}

/* fireflies drift over the hedges and the dark grass */
/* pull 0..1: how far they've gathered round the point (cx, cy) (in tiles) */
static void draw_fireflies_at(int ox, int oy, int ts, int unit, int w, int h, unsigned seed, float pull, float cx, float cy);
static void draw_fireflies(int ox, int oy, int ts, int unit, int w, int h, unsigned seed)
{
    draw_fireflies_at(ox, oy, ts, unit, w, h, seed, 0, 0, 0);
}
static void draw_fireflies_at(int ox, int oy, int ts, int unit, int w, int h, unsigned seed, float pull, float cx, float cy)
{
    Uint32 t = now_ms();
    int n = 5 + (w * h) / 60;
    if (n > 14) n = 14;
    if (pull > 0 && n < 7) n = 7;
    for (int i = 0; i < n; i++) {
        unsigned r = cell_hash(i, 99, seed);
        float bx = (r % 1000) / 1000.0f * (w + 2) - 1, by = ((r >> 10) % 1000) / 1000.0f * (h + 2) - 1;
        float ph = (r >> 20) % 628 / 100.0f, sp = 0.00015f + ((r >> 4) % 100) * 0.000002f;
        float fx = bx + sinf(t * sp + ph) * 1.6f + sinf(t * sp * 2.3f + ph * 2) * 0.4f;
        float fy = by + cosf(t * sp * 0.8f + ph) * 1.2f;
        if (pull > 0) {
            /* a lazy loop around the animal, each at its own radius and speed */
            float rad = 0.6f + ((r >> 12) % 100) / 100.0f * 0.7f;
            float ang = t * (0.0004f + (r % 50) * 0.000008f) * ((r & 1) ? 1 : -1) + ph;
            float gx = cx + cosf(ang) * rad, gy = cy + sinf(ang) * rad * 0.7f - 0.35f;
            float u = pull * pull * (3 - 2 * pull);
            fx += (gx - fx) * u;
            fy += (gy - fy) * u;
        }
        float glow = sinf(t * 0.0011f * (1.0f + (r % 7) * 0.1f) + ph * 3);
        if (glow < 0.35f - 0.6f * pull) continue;     /* gathered ones glow more steadily */
        int X = ox + (int)(fx * ts) / unit * unit, Y = oy + (int)(fy * ts) / unit * unit;
        if (pull > 0.3f && glow > 0.2f) {     /* gathered and bright: a little twinkle that shows on light ground */
            rect(X - 2 * unit, Y, unit, unit, BG2); rect(X + 2 * unit, Y, unit, unit, BG2);
            rect(X, Y - 2 * unit, unit, unit, BG2); rect(X, Y + 2 * unit, unit, unit, BG2);
            rect(X - unit, Y, unit, unit, BG1); rect(X + unit, Y, unit, unit, BG1);
            rect(X, Y - unit, unit, unit, BG1); rect(X, Y + unit, unit, unit, BG1);
        } else if (glow > 0.7f || pull > 0.3f) {
            rect(X - unit, Y, unit, unit, BG1); rect(X + unit, Y, unit, unit, BG1);
            rect(X, Y - unit, unit, unit, BG1); rect(X, Y + unit, unit, unit, BG1);
        }
        rect(X, Y, unit, unit, BG0);
    }
}

/* ---------------- squirrels: they collect the acorns when a level is solved ---------------- */

#define MAX_SQ 64
typedef struct { int hx, hy, ax, ay; Uint32 start, out, back; } Squirrel;
static Squirrel g_sq[MAX_SQ];
static int g_nsq;
static Uint32 g_sq_end;

static void squirrels_start(void)
{
    g_nsq = 0;
    Uint32 t0 = now_ms() + 900;
    int n = 0;
    for (int y = 0; y < P.h; y++)
        for (int x = 0; x < P.w; x++) n += P.box[y][x];
    int stagger = n ? 1400 / n : 0;
    if (stagger > 220) stagger = 220;
    for (int y = 0; y < P.h && g_nsq < MAX_SQ; y++)
        for (int x = 0; x < P.w && g_nsq < MAX_SQ; x++) {
            if (!P.box[y][x]) continue;
            /* the nearest bit of hedge */
            int bx = x, by = y, bd = 1 << 30;
            for (int yy = 0; yy < P.h; yy++)
                for (int xx = 0; xx < P.w; xx++) {
                    if (P.cell[yy][xx] != CL_WALL || is_stump(&P, xx, yy, S.seed)) continue;
                    int d = (xx - x) * (xx - x) + (yy - y) * (yy - y);
                    if (d < bd || (d == bd && (cell_hash(xx, yy, g_nsq) & 1))) { bd = d; bx = xx; by = yy; }
                }
            Squirrel *s = &g_sq[g_nsq];
            s->hx = bx; s->hy = by; s->ax = x; s->ay = y;
            float dist = sqrtf((float)bd);
            s->start = t0 + g_nsq * stagger + (cell_hash(x, y, 5) % 120);
            s->out = 250 + (Uint32)(dist * 160);
            s->back = s->out + 100;
            Uint32 end = s->start + s->out + s->back;
            if (end > g_sq_end || g_nsq == 0) g_sq_end = end;
            g_nsq++;
        }
    if (!g_nsq) g_sq_end = t0;
}

static int acorn_taken(int x, int y)
{
    Uint32 t = now_ms();
    for (int i = 0; i < g_nsq; i++)
        if (g_sq[i].ax == x && g_sq[i].ay == y && t >= g_sq[i].start + g_sq[i].out) return 1;
    return 0;
}

static void draw_squirrels(int ox, int oy, int ts, int unit)
{
    Uint32 t = now_ms();
    for (int i = 0; i < g_nsq; i++) {
        Squirrel *s = &g_sq[i];
        if (t < s->start) continue;
        Uint32 rel = t - s->start;
        float fx, fy; int carry = 0, left;
        if (rel < s->out) {
            float u = rel / (float)s->out;
            fx = s->hx + (s->ax - s->hx) * u; fy = s->hy + (s->ay - s->hy) * u;
            left = s->ax < s->hx;
        } else if (rel < s->out + s->back) {
            float u = (rel - s->out) / (float)s->back;
            fx = s->ax + (s->hx - s->ax) * u; fy = s->ay + (s->hy - s->ay) * u;
            left = s->hx < s->ax; carry = 1;
        } else continue;
        int hop = ((rel / 90) % 2) ? unit * 2 : 0;
        int fr = carry ? TL_SQUIRREL_CARRY : ((rel / 90) % 2 ? TL_SQUIRREL_B : TL_SQUIRREL);
        int X = ox + (int)(fx * ts) / unit * unit, Y = oy + (int)(fy * ts) / unit * unit - hop;
        blit(left ? g_spr.tilef[fr] : g_spr.tile[fr], X, Y);
    }
}

enum { OWL_NONE, OWL_COMING, OWL_ARRIVING, OWL_SITTING, OWL_LEAVING };
#define OWL_ARRIVE_MS 2800
#define OWL_LEAVE_MS  3000

/* about one level in four that has a stump gets a visit, somewhere between 8 and 40 s in */
static void barn_owl_plan(void)
{
    S.owl_state = OWL_NONE;
    int n = 0;
    for (int y = 0; y < P.h; y++)
        for (int x = 0; x < P.w; x++)
            if (P.cell[y][x] == CL_WALL && is_stump(&P, x, y, S.seed) && rand() % ++n == 0) { S.owl_x = x; S.owl_y = y; }
#ifdef OWL_TEST
    if (!n) return;
    S.owl_state = OWL_COMING; S.owl_t = now_ms() + 500;
    return;
#endif
    if (!n || rand() % 4) return;
    S.owl_state = OWL_COMING;
    S.owl_t = now_ms() + 8000 + rand() % 32000;
}

static void barn_owl_tick(Uint32 t)
{
    switch (S.owl_state) {
    case OWL_COMING:
        if (t >= S.owl_t) { S.owl_state = OWL_ARRIVING; S.owl_t = t; S.owl_side = rand() % 2 ? 1 : -1; S.dirty = 1; }
        break;
    case OWL_ARRIVING:
        if (t - S.owl_t >= OWL_ARRIVE_MS) { S.owl_state = OWL_SITTING; S.owl_t = t; S.owl_stay = 25000 + rand() % 35000; S.owl_blink = t + 2000; }
        break;
    case OWL_SITTING:
#ifdef OWL_TEST
        S.owl_stay = 1500;
#endif
        if (t - S.owl_t >= S.owl_stay) { S.owl_state = OWL_LEAVING; S.owl_t = t; S.owl_side = -S.owl_side; break; }
        /* it keeps an eye on you: looking left, right or straight ahead, depending on where you are */
        S.owl_look = P.px < S.owl_x ? -1 : P.px > S.owl_x ? 1 : 0;
        if (t > S.owl_blink + 150) S.owl_blink = t + 2500 + rand() % 5000;
        break;
    case OWL_LEAVING:
        if (t - S.owl_t >= OWL_LEAVE_MS) S.owl_state = OWL_NONE;
        break;
    }
}

/* drawn after the animal, so it's in front of anyone passing behind the stump */
static void draw_barn_owl(int ox, int oy, int ts, int unit)
{
    if (S.owl_state < OWL_ARRIVING) return;
    Uint32 t = now_ms();
    SpriteSet *u = &g_spr;
    float px = ox + S.owl_x * ts, py = oy + S.owl_y * ts - 9 * unit;
    int fr;
    if (S.owl_state == OWL_SITTING) {
        fr = (t >= S.owl_blink && t < S.owl_blink + 150) ? TL_BARN_BLINK : S.owl_look ? TL_BARN_LOOK : TL_BARN;
        blit(S.owl_look > 0 ? u->tilef[fr] : u->tile[fr], (int)px / unit * unit, (int)py / unit * unit);
        return;
    }
    /* arriving: a glide in from high off to one side with a swoop, wings held out, to just above the stump;
       then it flaps and settles down onto it. Leaving: it flaps up off the stump, then glides away. */
    float k = S.owl_state == OWL_ARRIVING ? (t - S.owl_t) / (float)OWL_ARRIVE_MS : (t - S.owl_t) / (float)OWL_LEAVE_MS;
    if (k > 1) k = 1;
    float dx, dy;
    int flapping;
    if (S.owl_state == OWL_ARRIVING) {
        if (k < 0.65f) {                                      /* the swoop: wings out, no flapping */
            float u2 = k / 0.65f, far = (1 - u2) * (1 - u2);
            dx = S.owl_side * 9.0f * far * ts;
            dy = (-6.0f * far - 0.8f * u2 + sinf(u2 * 3.1416f) * 1.2f) * ts;
            flapping = 0;
        } else {                                              /* flap and settle onto the stump */
            float u2 = (k - 0.65f) / 0.35f;
            dx = 0; dy = -0.8f * (1 - u2) * ts;
            flapping = 1;
        }
    } else {
        if (k < 0.5f) {                                       /* flap hard to climb off the stump, drifting a little */
            float u2 = k / 0.5f, e = u2 * (2 - u2);
            dx = S.owl_side * 0.6f * u2 * u2 * ts; dy = -2.6f * e * ts;
            flapping = 1;
        } else {                                              /* then glide away, mostly across, sinking a touch */
            float u2 = (k - 0.5f) / 0.5f;
            dx = S.owl_side * (0.6f + 14.0f * u2 * u2 + 2.0f * u2) * ts;
            dy = (-2.6f + 0.5f * sinf(u2 * 3.1416f) - 0.8f * u2) * ts;
            flapping = (u2 > 0.30f && u2 < 0.50f) || (u2 > 0.66f && u2 < 0.80f);   /* a couple of flaps on the way */
            if (flapping) dy -= 0.25f * ts;
        }
    }
    /* the wings: up, mid, down, mid while flapping; held out (mid) while gliding */
    static const int CYCLE[4] = { TL_BARN_WING_UP, TL_BARN_WING_MID, TL_BARN_WING_DOWN, TL_BARN_WING_MID };
    int wing = flapping ? CYCLE[((t - S.owl_t) / 70) % 4] : TL_BARN_WING_MID;
    fr = TL_BARN_FLY;
    int X = (int)(px + dx) / unit * unit, Y = (int)(py + dy) / unit * unit;
    blit(u->tile[wing], X - 14 * unit, Y);        /* left wing, then the right one mirrored */
    blit(u->tilef[wing], X + 13 * unit, Y);
    blit(u->tile[fr], X, Y);
}

static void draw_level(void)
{
    Uint32 t = now_ms();
    float k = S.move_t ? ease((t - S.move_t) / (float)MOVE_MS) : 1.0f;
    if (k >= 1.0f && S.move_t) { S.move_t = 0; S.cmx = -1; }
    float tx, ty;
    camera_target(&tx, &ty);
    if (S.scroll) {
        S.camx += (tx - S.camx) * 0.25f;
        S.camy += (ty - S.camy) * 0.25f;
        if (fabsf(tx - S.camx) < 0.5f) S.camx = tx;
        if (fabsf(ty - S.camy) < 0.5f) S.camy = ty;
    } else { S.camx = tx; S.camy = ty; }
    int unit = S.tscale;
    int ox = S.vx + ((int)-S.camx) / unit * unit, oy = S.vy + ((int)-S.camy) / unit * unit;
    S.ox = ox; S.oy = oy;
    int ts = S.ts;
    set_clip(S.vx, S.vy, S.vw, S.vh);
    draw_world(&P, &g_spr, ox, oy, S.vx, S.vy, S.vw, S.vh, S.seed);
    int x0 = clampi((S.vx - ox) / ts - 1, 0, P.w), x1 = clampi((S.vx + S.vw - ox) / ts + 1, 0, P.w);
    int y0 = clampi((S.vy - oy) / ts - 1, 0, P.h), y1 = clampi((S.vy + S.vh - oy) / ts + 1, 0, P.h);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (!P.box[y][x]) continue;
            if (S.clear && acorn_taken(x, y)) continue;
            int bx = ox + x * ts, by = oy + y * ts;
            if (S.cmx == x && S.cmy == y) {
                bx = ox + (int)((S.cfx + (x - S.cfx) * k) * ts) / unit * unit;
                by = oy + (int)((S.cfy + (y - S.cfy) * k) * ts) / unit * unit;
            }
            int on = P.goal[y][x] && !(S.cmx == x && S.cmy == y && k < 0.6f);
            if (S.wob_t && x == S.wob_x && y == S.wob_y && t >= S.wob_t && t < S.wob_t + 600) {
                static const int W[8] = { 0, 2, 0, -2, 0, 1, 0, -1 };
                bx += W[((t - S.wob_t) / 40) % 8] * unit;
            }
            blit(g_spr.tile[on ? TL_ACORN_GOAL : TL_ACORN], bx, by);
        }
    /* the animal */
    float fx = S.fromx + (P.px - S.fromx) * k, fy = S.fromy + (P.py - S.fromy) * k;
    int frame = FR_IDLE, dy = 0;
    if (S.clear) {
        int ph = ((t - S.clear_t) / 140) % 4;
        frame = FR_HAPPY;
        dy = (ph == 1 || ph == 3) ? -3 * unit : 0;
        S.facing = ((t - S.clear_t) / 280) % 2;
    } else if (k < 1.0f) {
        frame = S.pose;
    } else {
        Uint32 idle = t - S.last_input;
        if (t >= S.blink_at && t < S.blink_at + 140) frame = FR_BLINK;
        else if (S.fidget && t >= S.fidget_at && t < S.fidget_at + 1600) {
            int ph = (t - S.fidget_at) / 200;
            frame = (ph % 2) ? FR_TAIL : FR_IDLE;
            if (S.fidget == 2) frame = (ph % 2) ? FR_HAPPY : FR_IDLE;
        } else if (idle > 1200 && ((t / 700) % 2)) frame = FR_TAIL;   /* slow breathing */
    }
    int ax = ox + (int)(fx * ts) / unit * unit, ay = oy + (int)(fy * ts) / unit * unit + dy;
    ax += lean_dx(g_spr.animal, frame, S.facing, unit);
    blit(g_spr.anim[frame][S.facing], ax, ay);
    if (S.clear) draw_squirrels(ox, oy, ts, unit);
    draw_barn_owl(ox, oy, ts, unit);
    {
        /* after a few quiet seconds the fireflies drift over and circle the animal */
        static Uint32 last;
        float dt = last ? (t - last) / 1000.0f : 0;
        last = t;
        if (dt > 0.2f) dt = 0.2f;
        int idle = !S.clear && !S.move_t && t - S.last_input > 3500;
        float target = idle ? 1.0f : 0.0f;
        S.ff_pull += (target - S.ff_pull) * dt * (idle ? 0.35f : 0.9f);
        draw_fireflies_at(ox, oy, ts, unit, P.w, P.h, S.seed, S.ff_pull, fx + 0.5f, fy + 0.5f);
    }
    /* on small tiles, point the animal out for a moment at the start and after a long pause */
    if (S.ts <= 16 && !S.clear && k >= 1.0f) {
        Uint32 since = t - S.last_input;
        int show = (t - S.level_t < 2400) || since > 6000;
        if (show && ((t / 300) % 2)) {
            int m = 3, L = ts / 3;
            Uint32 c = BG3;
            int x0 = ax - m, y0 = ay - m, x1 = ax + ts + m - 2, y1 = ay + ts + m - 2;
            rect(x0, y0, L, 2, c); rect(x0, y0, 2, L, c);
            rect(x1 - L + 2, y0, L, 2, c); rect(x1, y0, 2, L, c);
            rect(x0, y1, L, 2, c); rect(x0, y1 - L + 2, 2, L, c);
            rect(x1 - L + 2, y1, L, 2, c); rect(x1, y1 - L + 2, 2, L, c);
        }
    }
    no_clip();
}

/* hold Y: the whole level at a glance */
static void draw_map(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, BG3);
    panel(16, 16, SCREEN_W - 32, SCREEN_H - 32);
    char b[96];
    snprintf(b, sizeof b, "%s %d", g_sets[S.set].name, S.level + 1);
    text_c(SCREEN_W / 2, 32, b, 2, BG3);
    int aw = SCREEN_W - 64, ah = SCREEN_H - 112;
    int t = aw / P.w < ah / P.h ? aw / P.w : ah / P.h;
    t = t / 2 * 2;
    if (t > 32) t = 32;
    if (t < 4) t = 4;
    int ox = (SCREEN_W - P.w * t) / 2 / 2 * 2, oy = 64 + (ah - P.h * t) / 2 / 2 * 2;
    Uint32 now = now_ms();
    for (int y = 0; y < P.h; y++)
        for (int x = 0; x < P.w; x++) {
            int X = ox + x * t, Y = oy + y * t, c = P.cell[y][x];
            if (c == CL_WALL) rect(X, Y, t, t, BG2);
            else if (c == CL_FLOOR) {
                rect(X, Y, t, t, BG0);
                if (P.goal[y][x]) rect(X + t / 2 - t / 6, Y + t / 2 - t / 6, (t / 3) / 2 * 2 + 2, (t / 3) / 2 * 2 + 2, BG2);
            }
            if (P.box[y][x]) { rect(X + 2, Y + 2, t - 4, t - 4, BG3); if (t >= 8) rect(X + 4, Y + 4, t - 8, t - 8, P.goal[y][x] ? BG0 : BG1); }
            if (x == P.px && y == P.py && (now / 250) % 2) rect(X, Y, t, t, BG3);
        }
    text_c(SCREEN_W / 2, SCREEN_H - 48, HINT("release Y to return", "release M to return", "lift your finger to return"), 2, BG2);
}

/* the pause menu: what's in it depends on the level (test-playing your own, a best run to watch) */
enum { PI_RESUME, PI_MAKER, PI_RESTART, PI_LIST, PI_BEST, PI_FAV, PI_ZOOM, PI_OPTIONS, PI_TITLE };
static int has_best_run(int s, int lv);
static int pause_items(int *ids)
{
    int n = 0;
    ids[n++] = PI_RESUME;
    if (S.testing) ids[n++] = PI_MAKER;               /* making a level: back to it comes first */
    ids[n++] = PI_RESTART;
    if (!S.testing) ids[n++] = PI_LIST;
    if (has_best_run(S.set, S.level) && !S.watching) ids[n++] = PI_BEST;
    if (!S.testing) ids[n++] = PI_FAV;
    ids[n++] = PI_ZOOM;
    ids[n++] = PI_OPTIONS;
    ids[n++] = PI_TITLE;
    return n;
}
static int pause_id(void) { int ids[12], n = pause_items(ids); return ids[clampi(S.pause_sel, 0, n - 1)]; }

/* Game Boy style window: everything on the 16 px grid, 2 px "pixels" */
static void panel(int x, int y, int w, int h)
{
    rect(x, y, w, h, BG0);
    frame_rect(x, y, w, h, 4, BG3);
    frame_rect(x + 6, y + 6, w - 12, h - 12, 2, BG2);
}

/* a filled ▶ made of 2 px pixels, 14 px tall */
static void cursor(int x, int y, Uint32 c)
{
    for (int i = 0; i < 7; i++) {
        int w = (i < 4 ? i + 1 : 7 - i) * 2;
        rect(x, y + i * 2, w, 2, c);
    }
}

/* rows are 32 px (two tiles) tall; the cursor marks the selection */
static void draw_menu_list(int x, int y, const char *const *items, int n, int sel)
{
    for (int i = 0; i < n; i++) {
        int yy = y + i * 32;
        if (i == sel) cursor(x, yy + 1, BG3);
        text(x + 16, yy, items[i], 2, BG3);
    }
}

/* the rows of a draw_menu_list() */
static void menu_taps(int x, int y, int w, int n, int *sel)
{
    for (int i = 0; i < n; i++) tap_add(x - 16, y + i * 32 - 8, w + 16, 32, TAP_PICK, sel, i);
}

/* holding X (restart) or L1/R1 (change level): a little window with a filling bar, so it's never a surprise */
static void draw_hold_popup(void)
{
    const char *title, *hint;
    char line[64];
    Uint32 since, ms;
    if (S.restart_since && P.hpos > 0) {
        since = S.restart_since; ms = RESTART_HOLD_MS;
        title = "Restart this level?";
        snprintf(line, sizeof line, "Back to the start");
        hint = HINT("Keep holding X", "Keep holding R", "");
    } else if (S.switch_since) {
        since = S.switch_since; ms = SWITCH_HOLD_MS;
        title = S.switch_dir > 0 ? "Next level" : "Previous level";
        int flv, fst = S.fav_play ? fav_ref(S.fav_pos + S.switch_dir, &flv) : -1;
        if (fst >= 0) snprintf(line, sizeof line, "%s %d", g_sets[fst].name, flv + 1);
        else snprintf(line, sizeof line, "%s %d", g_sets[S.set].name, S.level + 1 + S.switch_dir);
        hint = S.switch_dir > 0 ? HINT("Keep holding R1", "Keep holding ]", "") : HINT("Keep holding L1", "Keep holding [", "");
    } else return;
    float t = (now_ms() - since) / (float)ms;
    if (t > 1) t = 1;
    int w = 384, h = 144, x = (SCREEN_W - w) / 2, y = 160;
    panel(x, y, w, h);
    text_c(SCREEN_W / 2, y + 24, title, 2, BG3);
    text_c(SCREEN_W / 2, y + 52, line, 2, BG2);
    rect(x + 32, y + 84, w - 64, 8, BG1);
    rect(x + 32, y + 84, ((int)(t * (w - 64))) / 4 * 4, 8, BG3);
    text_c(SCREEN_W / 2, y + 108, hint, 2, BG2);
}

static void draw_game(void)
{
    if (S.map_held && !S.paused && !S.clear) { draw_map(); return; }
    draw_level();
    draw_hud();
    if (!S.paused && !S.clear) draw_hold_popup();
    Uint32 t = now_ms();
    if (S.clear == 1 && t - S.clear_t > DANCE_MS && t > g_sq_end + 400) { S.clear = 2; }
    if (S.clear == 2) {
        int x = 64, y = 112, w = 512, h = 256;
        panel(x, y, w, h);
        int done_set = S.level + 1 == g_sets[S.set].n && set_solved(S.set) == g_sets[S.set].n;
        text_c(SCREEN_W / 2, y + 32, done_set ? "SET DONE!" : "CLEAR!", 4, BG3);
        char b[96];
        snprintf(b, sizeof b, "Moves %d  Pushes %d", P.hpos, P.pushes);
        text_c(SCREEN_W / 2, y + 96, b, 2, BG3);
        if (S.watching) snprintf(b, sizeof b, "That was the solver's run");
        else if (S.new_best & 1) snprintf(b, sizeof b, "New best! (was %d moves)", S.prev_best_moves);
        else if (S.new_best & 2) snprintf(b, sizeof b, "Fewer pushes! (was %d)", S.prev_best_pushes);
        else snprintf(b, sizeof b, "Best %d moves / %d pushes", g_sets[S.set].best_moves[S.level], g_sets[S.set].best_pushes[S.level]);
        text_c(SCREEN_W / 2, y + 128, b, 2, BG2);
        int fm, fp;
        fewest_of(S.set, S.level, &fm, &fp);
        if (fm || fp) {                                   /* the proven fewest possible */
            int hm = fm && P.hpos == fm, hp = fp && P.pushes == fp;
            if (hm && hp) snprintf(b, sizeof b, "Fewest moves AND pushes!");
            else if (hm) snprintf(b, sizeof b, "Fewest moves possible!");
            else if (hp) snprintf(b, sizeof b, "Fewest pushes possible!");
            else if (fm && fp) snprintf(b, sizeof b, "Fewest: %d moves / %d pushes", fm, fp);
            else if (fm) snprintf(b, sizeof b, "Fewest possible: %d moves", fm);
            else snprintf(b, sizeof b, "Fewest possible: %d pushes", fp);
            if (hm || hp) star(SCREEN_W / 2 - text_w(b, 2) / 2 - 24, y + 160 + 1, 2, BG3);
            text_c(SCREEN_W / 2, y + 160, b, 2, hm || hp ? BG3 : BG2);
        }
        if (g_kbd == 2) {                                 /* touch: buttons (UNDO is up top) */
            tbtn(x + 96, y + 200, 144, 28, S.testing ? "MAKER" : "NEXT", TAP_BTN, NULL, BTN_A);
            tbtn(x + w - 96 - 144, y + 200, 144, 28, "LIST", TAP_BTN, NULL, BTN_Y);
        } else
        text_c(SCREEN_W / 2, y + 208, S.testing ? HINT("A maker  B undo  Y list", "Enter maker  Z undo  Esc list", "") :
               HINT("A next  B undo  Y level list", "Enter next  Z undo  Esc list", ""), 2, BG3);
    }
    if (S.paused) {
        int ids[12], n = pause_items(ids);
        const char *items[12];
        static char zb[32], rb[40];
        if (cfg.zoom) snprintf(zb, sizeof zb, "Zoom  < %dx >", S.tscale);
        else snprintf(zb, sizeof zb, "Zoom  < fit >");
        if (has_best_run(S.set, S.level)) snprintf(rb, sizeof rb, "Best run  %d moves", (int)strlen(g_sets[S.set].best_run[S.level]));
        int fv = fav_find(S.set, S.level) >= 0;
        for (int i = 0; i < n; i++)
            switch (ids[i]) {
            case PI_RESUME: items[i] = "Resume"; break;
            case PI_MAKER: items[i] = "Back to the maker"; break;
            case PI_RESTART: items[i] = "Restart level"; break;
            case PI_LIST: items[i] = "Level list"; break;
            case PI_BEST: items[i] = rb; break;
            case PI_FAV: items[i] = fv ? "Favorite  < yes >" : "Favorite  < no >"; break;
            case PI_ZOOM: items[i] = zb; break;
            case PI_OPTIONS: items[i] = "Options"; break;
            default: items[i] = "Title screen"; break;
            }
        S.pause_sel = clampi(S.pause_sel, 0, n - 1);
        int w = 384, h = 64 + n * 32, x = 128, y = (SCREEN_H - h) / 2 / 16 * 16;
        panel(x, y, w, h);
        modal_taps(x, y, w, h, BTN_B);
        text_c(SCREEN_W / 2, y + 16, "PAUSED", 2, BG3);
        draw_menu_list(x + 32, y + 48, items, n, S.pause_sel);
        menu_taps(x + 32, y + 48, w - 48, n, &S.pause_sel);
    }
}

/* Is the acorn now at (x,y) stuck for good? Either it sits on a dead square, or it is part of a
   2x2 block of hedges/acorns that can never move again (with an acorn not on its spot). */
static int acorn_stuck(int x, int y)
{
    if (P.goal[y][x] == 0 && P.dead[y][x]) return 1;
    for (int oy = -1; oy <= 0; oy++)
        for (int ox = -1; ox <= 0; ox++) {
            int solid = 1, loose = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int cx = x + ox + i, cy = y + oy + j;
                    int wall = cx < 0 || cy < 0 || cx >= P.w || cy >= P.h || P.cell[cy][cx] != CL_FLOOR;
                    if (!wall && !P.box[cy][cx]) solid = 0;
                    if (!wall && P.box[cy][cx] && !P.goal[cy][cx]) loose = 1;
                }
            if (solid && loose) return 1;
        }
    return 0;
}

static void try_move(int d)
{
    if (S.move_t) return;               /* still sliding */
    if (!S.walking) S.wg_from = S.wg_to = -1;     /* anything but a walk ends the walk's turn as one undo */
    music_activity();
    int ox = P.px, oy = P.py;
    int r = do_move(d, 1);
    if (d == 0) S.facing = 1;
    if (d == 1) S.facing = 0;
    if (!r) { sfx_play(SFX_BUMP); return; }
    S.fromx = ox; S.fromy = oy;
    S.move_t = now_ms();
    S.pose = r == 2 ? FR_PUSH : FR_STEP;
    if (r == 2) {
        S.cmx = P.px + DX[d]; S.cmy = P.py + DY[d];
        S.cfx = P.px; S.cfy = P.py;
        sfx_play(P.goal[S.cmy][S.cmx] ? SFX_GOAL : SFX_PUSH);
    } else {
        S.cmx = -1;
        sfx_play(SFX_STEP);
    }
    S.save_due = now_ms() + 3000;
    if (r == 2) report_progress();
    if (r == 2 && solved()) level_won();
    else if (r == 2 && acorn_stuck(S.cmx, S.cmy)) {
        S.wob_x = S.cmx; S.wob_y = S.cmy; S.wob_t = now_ms() + MOVE_MS;
        sfx_play(SFX_STUCK);
        toast(HINT("That acorn is stuck - B to undo", "That acorn is stuck - Z to undo", "That acorn is stuck - tap UNDO"));
    }
}

/* the walk is over (arrived, or something else took over): remember it as one piece for UNDO */
static void walk_end(void)
{
    if (!S.walking) return;
    S.walking = 0; S.nwalk = S.iwalk = 0;
    if (P.hpos - S.wg_start > 1) { S.wg_from = S.wg_start; S.wg_to = P.hpos; }
    else S.wg_from = S.wg_to = -1;
}

/* the shortest way to (tx, ty) without pushing anything; 0 if there's none */
static int walk_to(int tx, int ty)
{
    static short prev[MAXH][MAXW];
    static int qx[MAXW * MAXH], qy[MAXW * MAXH];
    if (tx < 0 || ty < 0 || tx >= P.w || ty >= P.h || P.cell[ty][tx] != CL_FLOOR || P.box[ty][tx]) return 0;
    for (int y = 0; y < P.h; y++) for (int x = 0; x < P.w; x++) prev[y][x] = -1;
    int h = 0, n = 0;
    qx[n] = P.px; qy[n++] = P.py; prev[P.py][P.px] = 4;
    while (h < n && prev[ty][tx] < 0) {
        int x = qx[h], y = qy[h++];
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= P.w || ny >= P.h || P.cell[ny][nx] != CL_FLOOR || P.box[ny][nx] || prev[ny][nx] >= 0) continue;
            prev[ny][nx] = (short)d; qx[n] = nx; qy[n++] = ny;
        }
    }
    if (prev[ty][tx] < 0) return 0;
    int len = 0, x = tx, y = ty;
    while (prev[y][x] != 4) { int d = prev[y][x]; S.walk[len++] = (unsigned char)d; x -= DX[d]; y -= DY[d]; }
    for (int i = 0; i < len / 2; i++) { unsigned char c = S.walk[i]; S.walk[i] = S.walk[len - 1 - i]; S.walk[len - 1 - i] = c; }
    if (!S.walking) S.wg_start = P.hpos;          /* a new tap mid-walk carries on the same walk */
    S.walking = 1; S.nwalk = len; S.iwalk = 0;
    S.wg_from = S.wg_to = -1;
    return 1;
}

static void try_undo(int redo)
{
    music_activity();
    if (S.clear) { S.clear = 0; }
    int ox = P.px, oy = P.py;
    int bx = -1, by = -1;
    int r;
    if (redo) {
        int d = P.hpos < P.hlen ? dir_of(P.hist[P.hpos]) : -1;
        r = redo_move();
        if (r == 2) { bx = P.px + DX[d]; by = P.py + DY[d]; S.cfx = P.px; S.cfy = P.py; }
        if (d == 0) S.facing = 1;
        if (d == 1) S.facing = 0;
    } else {
        char c = P.hpos > 0 ? P.hist[P.hpos - 1] : 0;
        int d = dir_of(c);
        r = undo_move();
        if (r == 2) { bx = ox; by = oy; S.cfx = ox + DX[d]; S.cfy = oy + DY[d]; }
    }
    if (!r) return;
    S.fromx = ox; S.fromy = oy;
    S.move_t = now_ms();
    S.pose = r == 2 ? FR_PUSH : FR_STEP;
    S.cmx = bx; S.cmy = by;
    sfx_play(redo ? SFX_REDO : SFX_UNDO);
    report_progress();
    S.save_due = now_ms() + 3000;
    if (redo && r == 2 && solved()) level_won();
}

/* the stored best run: back to the start with its moves waiting as "redo", and it starts playing itself.
   Any button takes over: A (hold to run) steps on through it, B backs up, and a different move branches off. */
static void play_best_run(void)
{
    const char *r = g_sets[S.set].best_run[S.level];
    if (!r || !*r) return;
    while (P.hpos > 0) undo_move();
    P.hlen = P.hpos = 0;
    for (const char *c = r; *c; c++) if (dir_of(*c) >= 0) hist_push(*c);
    S.fromx = P.px; S.fromy = P.py; S.cmx = -1; S.move_t = 0; S.nqueue = 0;
    S.nwalk = S.walking = 0; S.wg_from = S.wg_to = S.rewind_to = S.replay_to = -1;
    S.clear = 0;
    S.playback = 1;
    S.play_next = now_ms() + 600;
    sfx_play(SFX_RESTART);
    report_progress();
    toast(HINT("Best run - any button takes over", "Best run - any key takes over", "Best run - tap to take over"));
    S.toast_until = now_ms() + 3000;
}

static void restart_level(void)
{
    /* "undo all": the moves stay on the redo stack, so A can replay them */
    if (P.hpos == 0) return;
    while (P.hpos > 0) undo_move();
    S.fromx = P.px; S.fromy = P.py; S.cmx = -1; S.move_t = 0;
    S.clear = 0;
    sfx_play(SFX_RESTART);
    report_progress();
    toast(HINT("Restarted - A replays your moves", "Restarted - X replays your moves", "Restarted - REDO replays moves"));
    S.save_due = now_ms() + 500;
}

/* ======================================================================
 * Menus
 * ==================================================================== */

static const char *const TITLE_ITEMS[] = { "Continue", "Level sets", "Level maker", "Listen", "Options", "Quit" };
static int g_my_from_title;           /* My levels was opened from the title: Back goes there */
#ifdef __EMSCRIPTEN__
#define NTITLE 5          /* no Quit in a browser tab */
#else
#define NTITLE 6
#endif

static void draw_animal_at(int a, int frame, int flip, int x, int y, int scale)
{
    blit(ui_set(scale, a)->anim[frame][flip], x, y);
}

static void draw_bg_pattern(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, BG0);
    /* a faint dot in the middle of every other 16 px cell, like a GB title screen */
    for (int y = 0; y < SCREEN_H; y += 32)
        for (int x = (y / 32 % 2) * 16; x < SCREEN_W; x += 32) rect(x + 7, y + 7, 2, 2, BG1);
}

static void header(const char *title, const char *right)
{
    rect(0, 0, SCREEN_W, 32, BG3);
    rect(0, 28, SCREEN_W, 4, BG2);
    if (g_kbd == 2) {                     /* touch: a BACK button where the title starts */
        tbtn(8, 4, 72, 20, "BACK", TAP_BACK, NULL, 0);
        text(96, 8, title, 2, BG0);
    } else
    text(16, 8, title, 2, BG0);
    if (right) text_r(SCREEN_W - 16, 8, right, 2, BG1);
}

static void footer(const char *hint)
{
    text_c(SCREEN_W / 2, SCREEN_H - 32, hint, 2, BG2);
}

/* a little hand-made clearing for the title and options screens */
static void build_scene(Puzzle *p, const char *const *rows, int h)
{
    g_wall_gen++;
    memset(p, 0, sizeof *p);
    p->h = h;
    for (int y = 0; y < h; y++) {
        int l = (int)strlen(rows[y]);
        if (l > p->w) p->w = l;
        for (int x = 0; x < l; x++) {
            char c = rows[y][x];
            p->cell[y][x] = c == '#' ? CL_WALL : c == ' ' ? CL_OUT : CL_FLOOR;
            if (c == '.') p->goal[y][x] = 1;
        }
    }
}

static int title_animal(void)
{
    if (cfg.animal >= 0) return cfg.animal;
    return (now_ms() / 4400) % NUM_ANIMALS;    /* everybody takes a turn */
}

/* the title logo (tools/logo.py): letters with a vine weaving over and under them. Every few seconds a
   breeze crosses from left to right and the leaves tip in turn; fireflies drift around it. */
static Img *g_logo;
static int g_logo_pal = -1;
static void draw_logo(int ox, int oy)
{
    const int sc = 3;
    if (!g_logo || g_logo_pal != cfg.palette) {
        free(g_logo);
        g_logo = malloc(sizeof(Img) + sizeof(Uint32) * LOGO_W * sc * LOGO_H * sc);
        if (!g_logo) return;
        g_logo->w = LOGO_W * sc; g_logo->h = LOGO_H * sc; g_logo->opaque = 0;
        for (int y = 0; y < g_logo->h; y++)
            for (int x = 0; x < g_logo->w; x++) {
                uint8_t v = LOGO[y / sc][x / sc];
                g_logo->px[y * g_logo->w + x] = v == 255 ? 0 : g_col[v & 3];
            }
        g_logo_pal = cfg.palette;
    }
    blit(g_logo, ox, oy);
    Uint32 t = now_ms(), gust = t % 3500;
    for (int i = 0; i < NUM_LOGO_LEAVES; i++) {
        int lx = LOGO_LEAVES[i].x, ly = LOGO_LEAVES[i].y, dn = LOGO_LEAVES[i].down;
        Uint32 start = 200 + lx * 9;
        int f = gust >= start && gust < start + 260;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 7; c++) {
                uint8_t v = LOGO_LEAF[f][dn][r][c];
                if (v != 255) rect(ox + (lx + c) * sc, oy + (ly + r) * sc, sc, sc, g_col[v & 3]);
            }
    }
    /* fireflies: on the light title ground each one gets a darker ring so it shows */
    for (int i = 0; i < 6; i++) {
        unsigned r = cell_hash(i, 7, 31);
        float bx = (r % 1000) / 1000.0f, by = ((r >> 10) % 1000) / 1000.0f;
        float ph = (r >> 20) % 628 / 100.0f, sp = 0.6f + ((r >> 4) % 50) / 100.0f;
        float fx = ox - 40 + bx * (LOGO_W * sc + 80) + sinf(t * 0.0007f * sp + ph) * 40;
        float fy = oy + by * (LOGO_H * sc - 6) + cosf(t * 0.0005f * sp + ph) * 14;
        float glow = sinf(t * 0.0013f * sp + ph * 3);
        if (glow < 0.2f) continue;
        int X = (int)fx / sc * sc, Y = (int)fy / sc * sc;
        if (glow >= 0.6f) {
            rect(X - 2 * sc, Y, sc, sc, BG2); rect(X + 2 * sc, Y, sc, sc, BG2);
            rect(X, Y - 2 * sc, sc, sc, BG2); rect(X, Y + 2 * sc, sc, sc, BG2);
        }
        rect(X - sc, Y, sc, sc, BG1); rect(X + sc, Y, sc, sc, BG1);
        rect(X, Y - sc, sc, sc, BG1); rect(X, Y + sc, sc, sc, BG1);
        rect(X, Y, sc, sc, BG0);
    }
}

static void draw_title(void)
{
    draw_bg_pattern();
    Uint32 t = now_ms();
    draw_logo((SCREEN_W - LOGO_W * 3) / 2, 6);
    text_c(SCREEN_W / 2, 112, "a forest puzzle", 2, BG2);
    /* little scene: the animal pushes an acorn onto its spot, a squirrel takes it home, repeat */
    static Puzzle scene;
    static const char *const ROWS[] = { "######", "#___.#", "#__###", "######" };
    build_scene(&scene, ROWS, 4);
    int a = title_animal();
    SpriteSet *u = ui_set(3, a);
    int T = 48, sx = 32, sy = 160;
    set_clip(sx, sy, 6 * T, 4 * T);
    draw_world(&scene, u, sx, sy, sx, sy, 6 * T, 4 * T, 3);
    Uint32 cyc = t % 4800;
    float pos, cpos;                            /* tiles from the left wall, row 1 */
    int fr = FR_IDLE, flip = 0, dy = 0, acorn = 1;
    if (cyc < 600) { pos = 0; fr = (cyc / 300) % 2 ? FR_BLINK : FR_IDLE; }
    else if (cyc < 1800) { pos = (cyc - 600) / 600.0f; fr = ((cyc / 150) % 2) ? (pos >= 1 ? FR_PUSH : FR_STEP) : FR_IDLE; }
    else if (cyc < 3400) { pos = 2; fr = FR_HAPPY; dy = ((cyc / 140) % 2) ? -6 : 0; flip = (cyc / 280) % 2; }
    else { pos = 2 - (cyc - 3400) / 700.0f * 2; if (pos < 0) pos = 0; fr = ((cyc / 150) % 2) ? FR_STEP : FR_IDLE; flip = 1; }
    cpos = pos < 1 ? 2 : pos + 1;
    if (cyc >= 3400) cpos = 3;
    if (cyc >= 2900 && cyc < 4400) acorn = 0;   /* the squirrel has it */
    if (cyc >= 4400) cpos = 2;                  /* a new acorn falls into place */
    int ax = sx + T + ((int)(cpos * T)) / 3 * 3;
    if (acorn) blit(u->tile[cpos >= 3 ? TL_ACORN_GOAL : TL_ACORN], ax, sy + T);
    blit(u->anim[fr][flip], sx + T + ((int)(pos * T)) / 3 * 3 + lean_dx(a, fr, flip, 3), sy + T + dy);
    if (cyc >= 2500 && cyc < 3300) {            /* squirrel: down from the hedge, then back up */
        Uint32 r = cyc - 2500;
        float f = r < 400 ? r / 400.0f : 1.0f - (r - 400) / 400.0f;
        int fr2 = r >= 400 ? TL_SQUIRREL_CARRY : ((r / 90) % 2 ? TL_SQUIRREL_B : TL_SQUIRREL);
        blit(u->tile[fr2], sx + 4 * T, sy + ((int)(f * T)) / 3 * 3);
    }
    no_clip();
    /* menu */
    int x = 352, y = 144;
    panel(x, y, 256, 32 + NTITLE * 32);
    draw_menu_list(x + 24, y + 24, TITLE_ITEMS, NTITLE, S.title_sel);
    menu_taps(x + 24, y + 24, 208, NTITLE, &S.title_sel);
    /* progress */
    int tot = 0, done = 0;
    for (int s = 0; s < g_nsets; s++) if (s != g_fav) { tot += g_sets[s].n; done += set_solved(s); }
    char b[96];
    if (g_nsets) snprintf(b, sizeof b, "%d of %d solved", done, tot);
    else snprintf(b, sizeof b, "No levels in levels/");
    text_c(SCREEN_W / 2, 416, b, 2, BG3);
    text_c(SCREEN_W / 2, 448, "Puzzles: David W. Skinner", 2, BG2);
}

#define SET_ROWS 12
/* in a browser the list ends with a row for adding a level set file */
#ifdef __EMSCRIPTEN__
#define SETS_ROWS (g_nsets + 1)
static void web_pick_set_file(void) { EM_ASM({ if (Module.stumpedImport) Module.stumpedImport(); }); }
#else
#define SETS_ROWS g_nsets
#endif
#define SETS_ADD_ROW (S.sets_sel >= g_nsets)
static int set_has_menu(int s) { return s < g_nsets && (progress_ok(s) || g_sets[s].imported); }
static void draw_sets(void)
{
    draw_bg_pattern();
    header("LEVEL SETS", NULL);
    if (S.sets_sel < S.sets_top) S.sets_top = S.sets_sel;
    if (S.sets_sel >= S.sets_top + SET_ROWS) S.sets_top = S.sets_sel - SET_ROWS + 1;
    for (int r = 0; r < SET_ROWS && S.sets_top + r < SETS_ROWS; r++) {
        int s = S.sets_top + r, y = 48 + r * 32;
        tap_add(0, y - 8, SCREEN_W, 32, TAP_PICK, &S.sets_sel, s);
        if (s == S.sets_sel) cursor(16, y + 1, BG3);
        if (s >= g_nsets) { text(32, y, "+ Add a level set", 2, BG2); continue; }
        text(32, y, g_sets[s].name, 2, BG3);
        int done = set_solved(s), n = g_sets[s].n;
        char b[32];
        snprintf(b, sizeof b, "%d/%d", done, n);
        text_r(400, y, b, 2, BG3);
        int bx = 432, bw = g_kbd == 2 && set_has_menu(s) ? 120 : 176;
        if (g_kbd == 2 && set_has_menu(s)) {            /* touch: its menu */
            rect(568, y - 2, 48, 20, BG2);
            for (int d = 0; d < 3; d++) rect(578 + d * 12, y + 6, 4, 4, BG0);
            tap_add(560, y - 8, 64, 32, TAP_MENU, &S.sets_sel, s);
        }
        frame_rect(bx, y, bw, 16, 2, BG2);
        rect(bx + 4, y + 4, ((bw - 8) * done / (n ? n : 1)) / 2 * 2, 8, BG3);
    }
    if (g_kbd == 2) {                                   /* touch: page buttons either side of the hint */
        if (S.sets_top > 0) tbtn(16, 442, 56, 24, "^", TAP_BTN, NULL, BTN_L1);
        if (S.sets_top + SET_ROWS < SETS_ROWS) tbtn(568, 442, 56, 24, "v", TAP_BTN, NULL, BTN_R1);
    } else {
        if (S.sets_top > 0) text_c(SCREEN_W / 2, 32, "^", 2, BG2);
        if (S.sets_top + SET_ROWS < SETS_ROWS) text_c(SCREEN_W / 2, 48 + SET_ROWS * 32 - 6, "v", 2, BG2);   /* above the footer, not on it */
    }
    if (SETS_ADD_ROW) footer(HINT("A add a .txt level file  B back", "Enter add a file  Esc back", "Tap to add a file"));
    else if (set_has_menu(S.sets_sel)) footer(HINT("A open  SELECT menu  B back", "Enter open  M menu  Esc back", "Tap a set to open it"));
    else footer(HINT("A open  B back", "Enter open  Esc back", "Tap a set to open it"));
}

#define LV_COLS 8
#define LV_ROWS 5
static void draw_my_levels(void);
static void ed_open(int idx);
static void draw_thumbnail(const char *map, int x, int y, int w, int h)
{
    Puzzle save = P;            /* parse into P temporarily */
    char *hist = P.hist;
    P.hist = NULL;
    if (!parse_level(map)) { P = save; P.hist = hist; return; }
    int t = w / P.w < h / P.h ? w / P.w : h / P.h;
    t = t / 2 * 2;              /* whole 2 px pixels */
    if (t > 16) t = 16;
    if (t < 2) t = 2;
    int ox = x + (w - P.w * t) / 2 / 2 * 2, oy = y + (h - P.h * t) / 2 / 2 * 2;
    for (int yy = 0; yy < P.h; yy++)
        for (int xx = 0; xx < P.w; xx++) {
            int c = P.cell[yy][xx], X = ox + xx * t, Y = oy + yy * t;
            if (c == CL_WALL) rect(X, Y, t, t, BG2);
            else if (c == CL_FLOOR && P.goal[yy][xx] && t >= 6) rect(X + t / 2 - 2, Y + t / 2 - 2, 4, 4, BG2);
            else if (c == CL_FLOOR && P.goal[yy][xx]) rect(X, Y, t, t, BG1);
            if (P.box[yy][xx]) { rect(X, Y, t, t, BG3); if (t >= 6) rect(X + 2, Y + 2, t - 4, t - 4, P.goal[yy][xx] ? BG0 : BG1); }
            if (xx == P.px && yy == P.py) { rect(X, Y, t, t, BG3); if (t >= 6) rect(X + 2, Y + 2, t - 4, t - 4, BG2); }
        }
    P = save;
    P.hist = hist;
}

static void draw_levels(void)
{
    if (S.sets_sel == g_my) { draw_my_levels(); return; }
    draw_bg_pattern();
    Set *s = &g_sets[S.sets_sel];
    int fav = S.sets_sel == g_fav;
    if (fav && !s->n) {
        header(s->name, NULL);
        panel(16, 96, SCREEN_W - 32, 160);
        text_c(SCREEN_W / 2, 128, "No favorites yet", 2, BG3);
        text_c(SCREEN_W / 2, 168, "While you play a level, open the", 2, BG2);
        text_c(SCREEN_W / 2, 192, "pause menu and set Favorite", 2, BG2);
        footer(HINT("B back", "Esc back", ""));
        return;
    }
    char b[96];
    snprintf(b, sizeof b, "%d/%d", set_solved(S.sets_sel), s->n);
    header(s->name, b);
    if (g_kbd == 2) {                                   /* touch: the menu (Favorites: take it out) */
        const char *ml = fav ? "REMOVE" : "MENU";
        int mw = text_w(ml, 2) + 16, mx = SCREEN_W - 16 - text_w(b, 2) - 16 - mw;
        tbtn(mx, 4, mw, 20, ml, TAP_BTN, NULL, BTN_SELECT);
    }
    int row = S.lv_sel / LV_COLS;
    if (row < S.lv_top) S.lv_top = row;
    if (row >= S.lv_top + LV_ROWS) S.lv_top = row - LV_ROWS + 1;
    int cw = 64, ch = 32, gx = 64, gy = 48;
    for (int r = 0; r < LV_ROWS; r++)
        for (int c = 0; c < LV_COLS; c++) {
            int i = (S.lv_top + r) * LV_COLS + c;
            if (i >= s->n) continue;
            int x = gx + c * cw, y = gy + r * ch;
            tap_add(x - 4, y - 2, cw, ch, g_kbd == 2 ? TAP_PICK2 : TAP_PICK, &S.lv_sel, i);
            int sel = i == S.lv_sel, done = s->best_moves[i] != 0;
            if (sel) rect(x, y, cw - 8, ch - 4, BG3);
            else if (done) rect(x, y, cw - 8, ch - 4, BG1);
            else frame_rect(x, y, cw - 8, ch - 4, 2, BG2);
            if (!done && has_partial(S.sets_sel, i))          /* started: a little bar along the bottom */
                rect(x + 6, y + ch - 4 - 6, cw - 8 - 12, 2, sel ? BG1 : BG2);
            snprintf(b, sizeof b, "%d", i + 1);
            text_c(x + (cw - 8) / 2, y + 6, b, 2, sel ? BG0 : BG3);
            if (!fav && fav_find(S.sets_sel, i) >= 0) heart(x + cw - 8 - 12, y + 2, 2, sel ? BG1 : BG2);
            int fm, fp;
            fewest_of(S.sets_sel, i, &fm, &fp);
            if (s->best_moves[i] > 0 && ((fm && s->best_moves[i] <= fm) || (fp && s->best_pushes[i] <= fp)))   /* hit a fewest: a star */
                mini_star(x + 2, y + 2, 2, sel ? BG1 : BG2);
        }
    if (g_kbd == 2) {                                   /* touch: page buttons down the left */
        if (S.lv_top > 0) tbtn(8, gy, 44, 28, "^", TAP_BTN, NULL, BTN_L1);
        if ((S.lv_top + LV_ROWS) * LV_COLS < s->n) tbtn(8, gy + (LV_ROWS - 1) * ch, 44, 28, "v", TAP_BTN, NULL, BTN_R1);
    } else {
        if (S.lv_top > 0) text(16, gy, "^", 2, BG2);
        if ((S.lv_top + LV_ROWS) * LV_COLS < s->n) text(16, gy + (LV_ROWS - 1) * ch, "v", 2, BG2);
    }
    /* preview + info */
    int py = 224;
    panel(16, py, SCREEN_W - 32, 240);
    if (g_kbd == 2) tap_add(16, py, SCREEN_W - 32, 240, TAP_BTN, NULL, BTN_A);    /* touch: tap the preview to play */
    draw_thumbnail(s->maps[S.lv_sel], 32, py + 16, 304, 208);
    int ix = 352;
    int flv, fst = fav ? fav_ref(S.lv_sel, &flv) : -1;
    if (fst >= 0) {                                   /* Favorites: where it's from */
        snprintf(b, sizeof b, "%.12s %d", g_sets[fst].name, flv + 1);
        text(ix, py + 24, b, 2, BG3);
    } else {
        snprintf(b, sizeof b, "Level %d", S.lv_sel + 1);
        text(ix, py + 24, b, 2, BG3);
    }
    int fm, fp;
    fewest_of(S.sets_sel, S.lv_sel, &fm, &fp);
    if (s->best_moves[S.lv_sel] < 0) {
        text(ix, py + 44, "Solved!", 2, BG3);
        text(ix, py + 64, "(on another", 2, BG2);
        text(ix, py + 84, " device)", 2, BG2);
    } else if (s->best_moves[S.lv_sel]) {
        text(ix, py + 44, "Solved!", 2, BG3);
        snprintf(b, sizeof b, "%d moves", s->best_moves[S.lv_sel]);
        text(ix, py + 64, b, 2, BG2);
        if (fm && s->best_moves[S.lv_sel] <= fm) star(ix + text_w(b, 2) + 8, py + 64 + 1, 2, BG3);
        snprintf(b, sizeof b, "%d pushes", s->best_pushes[S.lv_sel]);
        text(ix, py + 84, b, 2, BG2);
        if (fp && s->best_pushes[S.lv_sel] <= fp) star(ix + text_w(b, 2) + 8, py + 84 + 1, 2, BG3);
    } else if (has_partial(S.sets_sel, S.lv_sel)) {
        text(ix, py + 44, "Started", 2, BG3);
        snprintf(b, sizeof b, "%d moves in", (int)strlen(s->partial[S.lv_sel]));
        text(ix, py + 64, b, 2, BG2);
    } else text(ix, py + 44, "Not yet", 2, BG2);
    if (fm || fp) {                                   /* the proven fewest possible ("?" = not proven) */
        char a1[8], a2[8];
        if (fm) snprintf(a1, sizeof a1, "%d", fm); else snprintf(a1, sizeof a1, "?");
        if (fp) snprintf(a2, sizeof a2, "%d", fp); else snprintf(a2, sizeof a2, "?");
        snprintf(b, sizeof b, "Fewest %s / %s", a1, a2);
        text(ix, py + 104, b, 2, BG2);
    }
    text(ix, py + 128, HINT("A play", "Enter play", "Tap again or"), 2, BG3);
    text(ix, py + 148, HINT("B back", "Esc back", "tap here: play"), 2, BG3);
    text(ix, py + 168, HINT("L1/R1 page", "[ / ] page", "^ v: page"), 2, BG3);
    text(ix, py + 188, HINT("X next unsolved", "N next unsolved", "Next unsolved >"), 2, BG3);
    tap_add(ix - 8, py + 184, SCREEN_W - 32 - ix, 22, TAP_BTN, NULL, BTN_X);   /* touch: tap it */
    if (fav) text(ix, py + 208, HINT("SELECT remove", "M remove", "REMOVE: take out"), 2, BG2);
    else text(ix, py + 208, HINT("SELECT menu", "M menu", "MENU: clearing"), 2, BG2);
}

/* Options. On the web, two more: your save as a file, to keep or to take to another browser
   (on the Miyoo the save is App/Stumped/save.txt on the SD card) */
/* the look (palette, animal), the sound, then saves, clearing, credits */
#ifdef __EMSCRIPTEN__
enum { O_PALETTE, O_ANIMAL, O_MUSIC, O_SFX, O_EXPORT, O_IMPORT, O_CLEAR, O_CREDITS, O_BACK, NOPT };
static const char *const OPT_ITEMS[] = { "Palette", "Animal", "Music", "Effects", "Save to a file", "Load a save file", "Clear all progress", "Credits", "Back" };
#else
enum { O_PALETTE, O_ANIMAL, O_MUSIC, O_SFX, O_CLEAR, O_CREDITS, O_BACK, NOPT, O_EXPORT = 100, O_IMPORT };
static const char *const OPT_ITEMS[] = { "Palette", "Animal", "Music", "Effects", "Clear all progress", "Credits", "Back" };
#endif
#define OPT_ROW (NOPT > 7 ? 28 : 32)        /* rows a little closer when there are more */

static void bar10(int x, int y, int v, Uint32 on, Uint32 off)
{
    for (int i = 0; i < 10; i++) rect(x + i * 16, y, 12, 16, i < v ? on : off);
}

/* the little idle life the animals have in a level: blinking, breathing, a happy wiggle now and then */
static int idle_frame(Uint32 t, int a)
{
    Uint32 ph = (t + a * 1300) % 4200, fid = (t + a * 2900) % 9000;
    if (ph < 140) return FR_BLINK;
    if (fid < 1200) return (fid / 200) % 2 ? FR_HAPPY : FR_IDLE;
    return ((t + a * 450) / 700) % 2 ? FR_TAIL : FR_IDLE;
}

static void draw_options(void)
{
    draw_bg_pattern();
    header("OPTIONS", NULL);
    for (int i = 0; i < NOPT; i++) {
        int y = 56 + i * OPT_ROW;
        tap_add(0, y - 6, SCREEN_W, OPT_ROW, TAP_OPT, &S.opt_sel, i);
        if (i == S.opt_sel) cursor(32, y + 1, BG3);
        text(48, y, OPT_ITEMS[i], 2, BG3);
        char b[64];
        switch (i) {
        case O_PALETTE: snprintf(b, sizeof b, "< %s >", PALETTES[cfg.palette].name); text(304, y, b, 2, BG3); break;
        case O_ANIMAL: snprintf(b, sizeof b, "< %s >", cfg.animal < 0 ? "Random" : ANIMAL_NAMES[cfg.animal]); text(304, y, b, 2, BG3); break;
        case O_MUSIC: bar10(304, y, cfg.music_vol, BG3, BG1); break;
        case O_SFX: bar10(304, y, cfg.sfx_vol, BG3, BG1); break;
        }
    }
    /* palette preview: a tiny scene at 2x, under the list */
    int px = 160, py = 56 + NOPT * OPT_ROW + 24;
    panel(px - 16, py - 16, 352, 128);
    SpriteSet *u = ui_set(2, 0);
    static Puzzle scene;
    static const char *const ROWS[] = { "##########", "#______._#", "##########" };
    build_scene(&scene, ROWS, 3);
    set_clip(px, py, 320, 96);
    draw_world(&scene, u, px, py, px, py, 320, 96, 11);
    Uint32 t = now_ms();
    if (cfg.animal < 0) {             /* Random: all four, idling side by side */
        blit(u->tile[TL_ACORN], px + 6 * 32, py + 32);
        for (int a = 0; a < NUM_ANIMALS; a++) blit(ui_set(2, a)->anim[idle_frame(t, a)][0], px + (1 + a) * 32, py + 32);
    } else {
        blit(u->tile[TL_ACORN], px + 4 * 32, py + 32);
        blit(ui_set(2, cfg.animal)->anim[idle_frame(t, cfg.animal)][0], px + 3 * 32, py + 32);
    }
    draw_fireflies(px, py, 32, 2, 10, 3, 4);
    no_clip();
    footer(HINT("Left/Right change  B back", "Left/Right change  Esc back", "Tap < > to change"));
}

static const char *const CREDITS[] = {
    "STUMPED " APP_VERSION,
    "",
    "Puzzles by David W. Skinner",
    "Microban I-IV, Sasquatch I-XII",
    "\"These sets may be freely",
    "distributed provided they",
    "remain properly credited.\"",
    "",
    "Sokoban (c) 1982 Thinking Rabbit",
    "Font8x8: Daniel Hepper (PD)",
    "Built with Claude",
    "",
    "D-pad move  B undo  A redo",
    "Hold X restart  Hold Y map",
    "L2/R2 zoom  L1/R1 level",
};
#define NCREDITS ((int)(sizeof CREDITS / sizeof CREDITS[0]))

static void draw_credits(void)
{
    draw_bg_pattern();
    panel(16, 16, SCREEN_W - 32, SCREEN_H - 32);
    static const char *const KEYS_HELP[] = {
        "Arrows move  Z undo  Y redo",
        "Hold R restart  Hold M map",
        "-/= zoom  [ / ] level",
    };
    static const char *const TOUCH_HELP[] = {
        "Swipe, or tap where to go",
        "Two fingers undo  Hold: map",
        "Pinch to zoom",
    };
    for (int i = 0; i < NCREDITS; i++)
        text_c(SCREEN_W / 2, 48 + i * 24 / 8 * 8, g_kbd && i >= 12 ? (g_kbd == 2 ? TOUCH_HELP : KEYS_HELP)[i - 12] : CREDITS[i], 2, i >= 12 ? BG2 : BG3);
    text_c(SCREEN_W / 2, SCREEN_H - 64, HINT("B back", "Esc back", "Tap to go back"), 2, BG3);
}

/* ---------------- clearing progress (always asks first) ---------------- */

enum { CF_NONE, CF_CHOOSE, CF_LEVEL, CF_SET, CF_ALL, CF_RESUME, CF_RMSET, CF_SETMENU, CF_IMPORT };
/* Level sets: the menu for one set (what it offers depends on the set) */
enum { SM_CODE, SM_ENTER, SM_REMOVE, SM_CANCEL };
static int setmenu_items(int set, int *ids)
{
    int n = 0;
    if (progress_ok(set)) { ids[n++] = SM_CODE; ids[n++] = SM_ENTER; }
    if (g_sets[set].imported) ids[n++] = SM_REMOVE;
    ids[n++] = SM_CANCEL;
    return n;
}
#ifdef __EMSCRIPTEN__
static void web_remove_set(int s);
static void web_import_apply(void);
#endif

/* open a level; if it was started before, ask whether to continue or start over (Continue first) */
static void go_to_level(int set, int level)
{
    S.fav_play = 0;
    if (set < 0 || set >= g_nsets) return;
    level = clampi(level, 0, g_sets[set].n - 1);
    if (has_partial(set, level)) {
        S.resume_set = set; S.resume_level = level;
        S.confirm = CF_RESUME; S.confirm_sel = 0;
        sfx_play(SFX_MENU);
        S.dirty = 1;
        return;
    }
    start_level(set, level, NULL);
}

/* skip from (set, level) by dir right away; keeps the quick-skip window open */
/* play favorite number pos (from the Favorites set): next / previous then step through Favorites */
static void go_to_fav(int pos)
{
    int lv, st = fav_ref(pos, &lv);
    if (st < 0) return;
    go_to_level(st, lv);
    S.fav_play = 1;
    S.fav_pos = pos;
}

/* where "next level" goes from here: through Favorites, or through the set */
static int nav_pos(void) { return S.fav_play ? S.fav_pos : S.level; }
static int nav_count(void) { return S.fav_play ? g_nfav : g_sets[S.set].n; }

static void quick_skip(int set, int level, int dir)
{
    int fav = S.fav_play, to = (fav ? S.fav_pos : level) + dir;
    if (to < 0 || to >= (fav ? g_nfav : g_sets[set].n)) return;
    S.quick_until = now_ms() + QUICK_SKIP_MS;
    if (S.screen == SC_GAME && !S.confirm) { remember_progress(); save_all(); }
    if (fav) go_to_fav(to); else go_to_level(set, to);
    snprintf(S.toast, sizeof S.toast, "%s", HINT("Tap L1/R1 to keep skipping", "Tap [ or ] to keep skipping", ""));
    S.toast_until = S.quick_until;
}

static void clear_progress(int what)
{
    if (what == CF_LEVEL || what == CF_SET) {
        Set *st = &g_sets[S.sets_sel];
        int lo = what == CF_LEVEL ? S.lv_sel : 0, hi = what == CF_LEVEL ? S.lv_sel : st->n - 1;
        for (int i = lo; i <= hi; i++) { st->best_moves[i] = st->best_pushes[i] = 0; set_partial(S.sets_sel, i, NULL, 0); set_best_run(S.sets_sel, i, NULL, 0); }
        if (cfg.cur_set == S.sets_sel && cfg.cur_level >= lo && cfg.cur_level <= hi) cfg.cur_moves[0] = 0;
        toast(what == CF_LEVEL ? "Level progress cleared" : "Set progress cleared");
    } else {
        for (int s = 0; s < g_nsets; s++)
            for (int i = 0; i < g_sets[s].n && s != g_fav; i++) { g_sets[s].best_moves[i] = g_sets[s].best_pushes[i] = 0; set_partial(s, i, NULL, 0); set_best_run(s, i, NULL, 0); }
        fav_sync();
        cfg.cur_set = cfg.cur_level = 0;
        cfg.cur_moves[0] = 0;
        toast("All progress cleared");
    }
    save_all();
}

static void draw_confirm(void)
{
    if (!S.confirm) return;
    char title[64], b1[64];
    const char *items[6];
    int n;
    if (S.confirm == CF_RESUME) {
        char b2[64];
        snprintf(title, sizeof title, "%s %d", g_sets[S.resume_set].name, S.resume_level + 1);
        snprintf(b1, sizeof b1, "Continue (%d moves)", (int)strlen(g_sets[S.resume_set].partial[S.resume_level]));
        items[0] = b1; items[1] = "Start over"; n = 2;
        int w = 448, h = 96 + 2 * 32 + 32, x = (SCREEN_W - w) / 2 / 16 * 16, y = 128;
        panel(x, y, w, h);
        modal_taps(x, y, w, h, BTN_B);
        text_c(SCREEN_W / 2, y + 24, title, 2, BG3);
        snprintf(b2, sizeof b2, "You've started this one");
        text_c(SCREEN_W / 2, y + 64, b2, 2, BG2);
        draw_menu_list(x + 48, y + 104, items, n, S.confirm_sel);
        menu_taps(x + 48, y + 104, w - 80, n, &S.confirm_sel);
        return;
    }
    if (S.confirm == CF_SETMENU) {
        static const char *const SM[4] = { "Progress code", "Enter a code", "Remove this set", "Cancel" };
        int ids[4];
        snprintf(title, sizeof title, "%.24s", g_sets[S.sets_sel].name);
        n = setmenu_items(S.sets_sel, ids);
        for (int i = 0; i < n; i++) items[i] = SM[ids[i]];
    } else if (S.confirm == CF_CHOOSE) {                     /* the level list's menu: clearing (codes live in Level sets) */
        snprintf(title, sizeof title, "%.24s", g_sets[S.sets_sel].name);
        snprintf(b1, sizeof b1, "Clear level %d", S.lv_sel + 1);
        items[0] = b1; items[1] = "Clear the whole set"; items[2] = "Cancel"; n = 3;
    } else {
        if (S.confirm == CF_LEVEL) snprintf(title, sizeof title, "Clear %s %d?", g_sets[S.sets_sel].name, S.lv_sel + 1);
        else if (S.confirm == CF_SET) snprintf(title, sizeof title, "Clear all of %s?", g_sets[S.sets_sel].name);
        else if (S.confirm == CF_RMSET) snprintf(title, sizeof title, "Remove %s?", g_sets[S.sets_sel].name);
        else if (S.confirm == CF_IMPORT) snprintf(title, sizeof title, "Replace your progress?");
        else snprintf(title, sizeof title, "Clear ALL progress?");
        items[0] = "No"; items[1] = S.confirm == CF_RMSET ? "Yes, remove it" : S.confirm == CF_IMPORT ? "Yes, load the file" : "Yes, clear it"; n = 2;
    }
    int w = ((text_w(title, 2) + 64 + 15) / 16) * 16;
    if (w < 384) w = 384;
    int menu = S.confirm == CF_CHOOSE || S.confirm == CF_SETMENU;
    int h = 96 + n * 32 + (menu ? 0 : 32), x = (SCREEN_W - w) / 2 / 16 * 16, y = 128;
    panel(x, y, w, h);
    modal_taps(x, y, w, h, BTN_B);
    text_c(SCREEN_W / 2, y + 24, title, 2, BG3);
    int ly = y + 64;
    if (!menu) { text_c(SCREEN_W / 2, ly, "Are you sure?", 2, BG2); ly += 40; }
    draw_menu_list(x + 48, ly, items, n, S.confirm_sel);
    menu_taps(x + 48, ly, w - 80, n, &S.confirm_sel);
}

static void progress_show(int set, int lv);
static void code_enter_from(int set, int lv);
static int progress_ok(int set);
/* returns 1 if the dialog took the key */
static int confirm_key(SDLKey k)
{
    if (!S.confirm) return 0;
    int smi[4], n = S.confirm == CF_CHOOSE ? 3 : S.confirm == CF_SETMENU ? setmenu_items(S.sets_sel, smi) : 2;
    if (k == BTN_UP) { S.confirm_sel = (S.confirm_sel + n - 1) % n; sfx_play(SFX_MENU); }
    else if (k == BTN_DOWN) { S.confirm_sel = (S.confirm_sel + 1) % n; sfx_play(SFX_MENU); }
    else if (k == BTN_B) S.confirm = CF_NONE;
    else if ((k == BTN_L1 || k == BTN_R1) && S.confirm == CF_RESUME && now_ms() < S.quick_until) {
        S.confirm = CF_NONE;
        quick_skip(S.resume_set, S.resume_level, k == BTN_R1 ? 1 : -1);
    }
    else if (k == BTN_A && S.confirm == CF_RESUME) {
        int st = S.resume_set, lv = S.resume_level;
        S.confirm = CF_NONE;
        sfx_play(SFX_SELECT);
        if (S.confirm_sel == 0) {
            static char moves[8192];
            snprintf(moves, sizeof moves, "%s", g_sets[st].partial[lv] ? g_sets[st].partial[lv] : "");
            if (cfg.animal < 0 && st == cfg.cur_set && lv == cfg.cur_level) S.animal = cfg.last_animal;
            start_level(st, lv, moves);
        } else {
            set_partial(st, lv, NULL, 0);
            if (st == cfg.cur_set && lv == cfg.cur_level) cfg.cur_moves[0] = 0;
            start_level(st, lv, NULL);
        }
    }
    else if (k == BTN_A) {
        if (S.confirm == CF_SETMENU) {
            int id = smi[S.confirm_sel];
            S.confirm = CF_NONE;
            sfx_play(SFX_MENU);
            if (id == SM_CODE) progress_show(S.sets_sel, -1);
            else if (id == SM_ENTER) code_enter_from(S.sets_sel, -1);
            else if (id == SM_REMOVE) { S.confirm = CF_RMSET; S.confirm_sel = 0; }
        } else if (S.confirm == CF_CHOOSE) {
            int sel = S.confirm_sel;
            sfx_play(SFX_MENU);
            if (sel == 2) S.confirm = CF_NONE;
            else { S.confirm = sel == 0 ? CF_LEVEL : CF_SET; S.confirm_sel = 0; }   /* "No" first */
        } else {
            int what = S.confirm;
            S.confirm = CF_NONE;
#ifdef __EMSCRIPTEN__
            if (S.confirm_sel == 1 && what == CF_RMSET) { web_remove_set(S.sets_sel); sfx_play(SFX_RESTART); return 1; }
            if (what == CF_IMPORT) { if (S.confirm_sel == 1) web_import_apply(); else remove("import.txt"); return 1; }
#endif
            if (S.confirm_sel == 1 && what != CF_RMSET && what != CF_IMPORT) { clear_progress(what); sfx_play(SFX_RESTART); }
        }
    }
    return 1;
}


static void menu_nav(int *sel, int n, int d);

/* ======================================================================
 * Listen: just the music, in a clearing made up on the spot, with all four animals dancing.
 * Each animal follows one part: the frog the bass, the fox the kick, the owl the snare, the raccoon
 * the shaker. When its part drops out it sits and breathes until the part comes back. The buttons
 * play along with the music instead of moving anyone.
 * ==================================================================== */

#define LS_W 13
#define LS_H 10
static struct {
    Puzzle room;
    unsigned seed;
    int ax[NUM_ANIMALS], ay[NUM_ANIMALS];          /* where each animal stands */
    /* moving about: resting animals wander; everyone gathers in the middle for double time and
       scatters afterwards. Steps land on the beat (on eighths in double time). */
    int tx[NUM_ANIMALS], ty[NUM_ANIMALS];          /* where it's heading (= ax/ay when not going anywhere) */
    int fromx[NUM_ANIMALS], fromy[NUM_ANIMALS];    /* the square it's stepping from */
    Uint32 step_t[NUM_ANIMALS], step_ms;           /* when the current step started, and how long steps take */
    int mode[NUM_ANIMALS];                         /* LM_* */
    int rest[NUM_ANIMALS];                         /* beats to stay put before wandering on */
    int stuck[NUM_ANIMALS];                        /* beats spent blocked */
    int face[NUM_ANIMALS];                         /* 1 = facing left */
    int gathered;                                  /* 1 while gathered for double time */
    Uint32 dbl_since;                              /* when double time started (0 = not on) */
    int last_step;                                 /* the music's last sixteenth seen */
    /* guests: an owlet that follows the owl around, and a barn owl perched on a stump */
    int owlet, olx, oly, olfx, olfy; Uint32 ol_t;
    int barn, bx, by, barn_look; Uint32 barn_until, barn_blink; int seen_chord;
    int beats;
    int nac, acx[6], acy[6];                       /* acorns (on their spots) */
    Uint32 ac_gone[6], ac_back[6];                 /* a squirrel took it at / it's back at */
    int sq_on, sq_hx, sq_hy, sq_ac; Uint32 sq_start, sq_out;
    Uint32 seen_section, seen_hit[5]; int hits[5];
    Uint32 help_until;
    int menu, menu_sel;
    /* playing along: Melody (0) or Drums (1), switched with Select */
    int pmode;
    int dmask;                                     /* D-pad directions held (1 up, 2 down, 4 left, 8 right) */
    int kdeg, tdeg, cur_deg;                       /* note held on the keyboard / by touch / now sounding (-1 none) */
    int mods;                                      /* modifiers held: 1 octave, 2 vibrato, 4 arpeggio */
    int tid[5], tkind[5], tval[5];                 /* touches: pointer id, what it's doing (0 none, 1 note, 2 modifier, 3 done) */
    Uint32 pad_flash[NUM_DRUMS];                   /* touch drums: when each pad was last hit */
    int sleep_idx; Uint32 sleep_end, sleep_set; int asleep;
} L;
enum { LM_STAY, LM_WANDER, LM_GATHER, LM_SCATTER };
static const int SLEEP_MIN[] = { 0, 15, 30, 60 };
static const char *const LS_MENU[] = { "Keep listening", "New clearing", "Sleep", "Help", "Back to title" };
#define NLSMENU 5

static int ls_floor(int x, int y) { return x >= 0 && y >= 0 && x < LS_W && y < LS_H && L.room.cell[y][x] == CL_FLOOR; }

/* a small random clearing: two or three overlapping glades ringed by hedge, a stump or two, some acorns */
/* ---- making up a clearing ----
   One of several shapes (a round glade, two glades joined by a path, a ring around a clump of hedge,
   a crescent, an L), roughened at the edges and smoothed, then a few things inside: a hedge clump, lone
   stumps, a short hedge row. Every open square stays reachable. */
static uint8_t G[LS_H][LS_W];          /* the open floor while it's being shaped */
static int g_in(int x, int y) { return x >= 1 && y >= 1 && x < LS_W - 1 && y < LS_H - 1; }
static int g_at(int x, int y) { return x >= 0 && y >= 0 && x < LS_W && y < LS_H && G[y][x]; }
static int g_count(void) { int n = 0; for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) n += G[y][x]; return n; }
static float frnd(void) { return (rand() % 10000) / 10000.0f; }

static void g_ellipse(float cx, float cy, float rx, float ry, int v)
{
    for (int y = 0; y < LS_H; y++)
        for (int x = 0; x < LS_W; x++)
            if (g_in(x, y) && ((x - cx) * (x - cx)) / (rx * rx) + ((y - cy) * (y - cy)) / (ry * ry) <= 1.0f) G[y][x] = (uint8_t)v;
}

/* grow a lumpy blob outward from (cx, cy), favouring squares inside an rx-by-ry oval */
static void g_blob(int cx, int cy, float rx, float ry, int n)
{
    if (!g_in(cx, cy)) return;
    G[cy][cx] = 1;
    int have = 1;
    for (int tries = 0; have < n && tries < 4000; tries++) {
        int x = 1 + rand() % (LS_W - 2), y = 1 + rand() % (LS_H - 2);
        if (G[y][x]) continue;
        if (!g_at(x - 1, y) && !g_at(x + 1, y) && !g_at(x, y - 1) && !g_at(x, y + 1)) continue;
        float d = ((x - cx) * (x - cx)) / (rx * rx) + ((y - cy) * (y - cy)) / (ry * ry);
        if (frnd() < expf(-d * 1.3f)) { G[y][x] = 1; have++; }
    }
}

static void g_path(int x0, int y0, int x1, int y1, int wide)
{
    int x = x0, y = y0, horiz_first = rand() % 2;
    for (int k = 0; k < 2; k++) {
        if ((k == 0) == horiz_first) while (x != x1) { x += x1 > x ? 1 : -1; G[y][x] = 1; if (wide && g_in(x, y + 1)) G[y + 1][x] = 1; }
        else while (y != y1) { y += y1 > y ? 1 : -1; G[y][x] = 1; if (wide && g_in(x + 1, y)) G[y][x + 1] = 1; }
    }
}

/* keep only the biggest connected patch of floor; returns its size */
static int g_keep_largest(void)
{
    static int comp[LS_H][LS_W];
    int qx[LS_W * LS_H], qy[LS_W * LS_H], best = 0, best_id = 0, id = 0;
    memset(comp, 0, sizeof comp);
    for (int y = 0; y < LS_H; y++)
        for (int x = 0; x < LS_W; x++) {
            if (!G[y][x] || comp[y][x]) continue;
            int h = 0, n = 0; id++;
            comp[y][x] = id; qx[n] = x; qy[n++] = y;
            while (h < n) {
                int cx = qx[h], cy = qy[h++];
                for (int d = 0; d < 4; d++) {
                    int nx = cx + DX[d], ny = cy + DY[d];
                    if (g_at(nx, ny) && !comp[ny][nx]) { comp[ny][nx] = id; qx[n] = nx; qy[n++] = ny; }
                }
            }
            if (n > best) { best = n; best_id = id; }
        }
    for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) if (G[y][x] && comp[y][x] != best_id) G[y][x] = 0;
    return best;
}

static void g_roughen(void)
{
    uint8_t T[LS_H][LS_W];
    memcpy(T, G, sizeof T);
    for (int y = 0; y < LS_H; y++)
        for (int x = 0; x < LS_W; x++) {
            int edge = !g_at(x - 1, y) || !g_at(x + 1, y) || !g_at(x, y - 1) || !g_at(x, y + 1);
            int touch = g_at(x - 1, y) || g_at(x + 1, y) || g_at(x, y - 1) || g_at(x, y + 1);
            if (G[y][x] && edge && frnd() < 0.22f) T[y][x] = 0;               /* bays */
            else if (!G[y][x] && g_in(x, y) && touch && frnd() < 0.18f) T[y][x] = 1;   /* points */
        }
    memcpy(G, T, sizeof T);
    for (int pass = 0; pass < 2; pass++)          /* smooth: no one-square spikes or pinholes */
        for (int y = 0; y < LS_H; y++)
            for (int x = 0; x < LS_W; x++) {
                int n = g_at(x - 1, y) + g_at(x + 1, y) + g_at(x, y - 1) + g_at(x, y + 1);
                if (G[y][x] && n <= 1) G[y][x] = 0;
                else if (!G[y][x] && g_in(x, y) && n == 4) G[y][x] = 1;
            }
}

static void g_shape(void)
{
    memset(G, 0, sizeof G);
    float cx = (LS_W - 1) / 2.0f, cy = (LS_H - 1) / 2.0f;
    switch (rand() % 5) {
    case 0:   /* a round glade */
        g_blob((int)cx, (int)cy + rand() % 2, 3.6f, 2.6f, 32 + rand() % 12);
        break;
    case 1: { /* two glades and a path between them */
        int ly = 2 + rand() % 4, ry = 2 + rand() % 4;
        g_blob(3, ly, 1.8f, 1.7f, 10 + rand() % 4);
        g_blob(LS_W - 4, ry, 1.8f, 1.7f, 10 + rand() % 4);
        g_path(3, ly, LS_W - 4, ry, rand() % 3 == 0);
        break;
    }
    case 2:   /* a ring around a clump of hedge */
        g_ellipse(cx, cy, 4.8f + frnd() * 0.4f, 3.4f + frnd() * 0.3f, 1);
        g_ellipse(cx + (rand() % 3 - 1) * 0.5f, cy, 1.6f + frnd() * 0.6f, 0.9f + frnd() * 0.4f, 0);
        break;
    case 3: { /* a crescent */
        int dir = rand() % 2 ? 1 : -1;
        g_ellipse(cx, cy, 4.8f, 3.5f, 1);
        g_ellipse(cx + dir * 2.6f, cy - 0.6f + frnd() * 1.2f, 3.0f, 2.5f, 0);
        break;
    }
    default: { /* an L, turned any way */
        int fx = rand() % 2, fy = rand() % 2;
        for (int y = 2; y <= 4; y++) for (int x = 2; x <= 9; x++) G[fy ? LS_H - 1 - y : y][fx ? LS_W - 1 - x : x] = 1;
        for (int y = 2; y <= 7; y++) for (int x = 7; x <= 9; x++) G[fy ? LS_H - 1 - y : y][fx ? LS_W - 1 - x : x] = 1;
        break;
    }
    }
    g_roughen();
    g_keep_largest();
}

/* is all the floor of the room still one connected piece? */
static int ls_connected(const Puzzle *p)
{
    int qx[LS_W * LS_H], qy[LS_W * LS_H], h = 0, n = 0, total = 0, sx = -1, sy = -1;
    static uint8_t seen[LS_H][LS_W];
    memset(seen, 0, sizeof seen);
    for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) if (p->cell[y][x] == CL_FLOOR) { total++; if (sx < 0) { sx = x; sy = y; } }
    if (sx < 0) return 0;
    seen[sy][sx] = 1; qx[n] = sx; qy[n++] = sy;
    while (h < n) {
        int cx = qx[h], cy = qy[h++];
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= LS_W || ny >= LS_H || seen[ny][nx] || p->cell[ny][nx] != CL_FLOOR) continue;
            seen[ny][nx] = 1; qx[n] = nx; qy[n++] = ny;
        }
    }
    return n == total;
}

/* try to put hedge on these squares (all floor, nothing next to the outer hedge breaks); undo if it cuts the floor in two */
static int ls_try_walls(Puzzle *p, const int *xs, const int *ys, int n)
{
    for (int i = 0; i < n; i++) if (!ls_floor(xs[i], ys[i])) return 0;
    for (int i = 0; i < n; i++) p->cell[ys[i]][xs[i]] = CL_WALL;
    int floor = 0;
    for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) floor += p->cell[y][x] == CL_FLOOR;
    if (floor >= 24 && ls_connected(p)) return 1;
    for (int i = 0; i < n; i++) p->cell[ys[i]][xs[i]] = CL_FLOOR;
    return 0;
}

static void listen_new_room(void)
{
    Puzzle *p = &L.room;
    for (int attempt = 0; attempt < 20; attempt++) {
        g_shape();
        if (g_count() >= 28) break;
    }
    memset(p, 0, sizeof *p);
    g_wall_gen++;
    p->w = LS_W; p->h = LS_H;
    L.seed = (unsigned)rand();
    for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) if (G[y][x]) p->cell[y][x] = CL_FLOOR;
    for (int y = 0; y < LS_H; y++)
        for (int x = 0; x < LS_W; x++) {
            if (p->cell[y][x] == CL_FLOOR) continue;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++)
                if (ls_floor(x + dx, y + dy)) p->cell[y][x] = CL_WALL;
        }
    /* centre it */
    {
        int x0 = LS_W, x1 = -1, y0 = LS_H, y1 = -1;
        for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++)
            if (p->cell[y][x] != CL_OUT) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        int dx = (LS_W - (x1 - x0 + 1)) / 2 - x0, dy = (LS_H - (y1 - y0 + 1)) / 2 - y0;
        uint8_t tmp[LS_H][LS_W];
        for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) tmp[y][x] = p->cell[y][x];
        for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) {
            int sx = x - dx, sy = y - dy;
            p->cell[y][x] = (sx >= 0 && sy >= 0 && sx < LS_W && sy < LS_H) ? tmp[sy][sx] : CL_OUT;
        }
    }
    /* things inside: sometimes a hedge clump, sometimes a short hedge row from the edge, a stump or two */
    if (rand() % 2) {
        for (int tries = 0; tries < 60; tries++) {
            int x = 2 + rand() % (LS_W - 4), y = 2 + rand() % (LS_H - 4);
            static const int SH[3][4][2] = { { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }, { { 0, 0 }, { 1, 0 }, { 2, 0 }, { 1, 1 } }, { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 0, 0 } } };
            int k = rand() % 3, n = k == 2 ? 3 : 2 + rand() % 3, xs[4], ys[4], ok = 1;
            for (int i = 0; i < n; i++) { xs[i] = x + SH[k][i][0]; ys[i] = y + SH[k][i][1]; }
            for (int i = 0; i < n && ok; i++)            /* not touching the outer hedge */
                for (int dy = -1; dy <= 1 && ok; dy++) for (int dx = -1; dx <= 1 && ok; dx++) {
                    int qx = xs[i] + dx, qy = ys[i] + dy, mine = 0;
                    for (int j = 0; j < n; j++) mine |= xs[j] == qx && ys[j] == qy;
                    if (!mine && !ls_floor(qx, qy)) ok = 0;
                }
            if (ok && ls_try_walls(p, xs, ys, n)) break;
        }
    }
    if (rand() % 3 == 0) {
        for (int tries = 0; tries < 60; tries++) {
            int x = 1 + rand() % (LS_W - 2), y = 1 + rand() % (LS_H - 2), d = rand() % 4;
            if (!ls_floor(x, y) || p->cell[y - DY[d]][x - DX[d]] != CL_WALL) continue;   /* starts at the edge, heads in */
            int xs[3], ys[3], n = 2 + rand() % 2;
            for (int i = 0; i < n; i++) { xs[i] = x + DX[d] * i; ys[i] = y + DY[d] * i; }
            if (ls_try_walls(p, xs, ys, n)) break;
        }
    }
    for (int k = 0, want = 1 + rand() % 2, tries = 0; k < want && tries < 80; tries++) {
        int x = 1 + rand() % (LS_W - 2), y = 1 + rand() % (LS_H - 2), ok = ls_floor(x, y);
        for (int dy = -1; dy <= 1 && ok; dy++) for (int dx = -1; dx <= 1 && ok; dx++) ok = ls_floor(x + dx, y + dy);
        if (ok && ls_try_walls(p, &x, &y, 1)) k++;
    }
    {   /* the barn owl needs a stump: if there's no lone square of hedge yet, make one */
        int lone = 0;
        for (int y = 1; y < LS_H - 1; y++) for (int x = 1; x < LS_W - 1; x++)
            if (p->cell[y][x] == CL_WALL && ls_floor(x - 1, y) && ls_floor(x + 1, y) && ls_floor(x, y - 1) && ls_floor(x, y + 1)) lone = 1;
        for (int tries = 0; !lone && tries < 200; tries++) {
            int x = 1 + rand() % (LS_W - 2), y = 1 + rand() % (LS_H - 2);
            if (ls_floor(x, y) && ls_floor(x - 1, y) && ls_floor(x + 1, y) && ls_floor(x, y - 1) && ls_floor(x, y + 1) &&
                ls_try_walls(p, &x, &y, 1)) lone = 1;
        }
    }
    /* acorns on their spots, then the animals, spread out */
    int used[LS_H][LS_W] = { { 0 } };
    L.nac = 3 + rand() % 3;
    for (int k = 0, tries = 0; k < L.nac && tries < 200; tries++) {
        int x = rand() % LS_W, y = rand() % LS_H;
        if (!ls_floor(x, y) || used[y][x]) continue;
        used[y][x] = 1; p->goal[y][x] = 1;
        L.acx[k] = x; L.acy[k] = y; L.ac_gone[k] = L.ac_back[k] = 0; k++;
    }
    for (int a = 0; a < NUM_ANIMALS; a++) {
        int bx = -1, by = -1, bd = -1;
        for (int tries = 0; tries < 60; tries++) {
            int x = rand() % LS_W, y = rand() % LS_H;
            if (!ls_floor(x, y) || used[y][x]) continue;
            int d = 99;
            for (int o = 0; o < a; o++) { int dd = abs(L.ax[o] - x) + abs(L.ay[o] - y); if (dd < d) d = dd; }
            if (d > bd) { bd = d; bx = x; by = y; }
        }
        if (bx < 0) { bx = LS_W / 2 - 2 + a; by = LS_H / 2; }
        L.ax[a] = bx; L.ay[a] = by; used[by][bx] = 1;
        L.tx[a] = L.fromx[a] = bx; L.ty[a] = L.fromy[a] = by;
        L.mode[a] = LM_STAY; L.rest[a] = 2 + rand() % 6; L.stuck[a] = 0; L.step_t[a] = 0; L.face[a] = a & 1;
    }
    L.gathered = 0; L.dbl_since = 0;
    /* the barn owl wants a stump: nudge the look of the clearing until a lone wall is one */
    L.barn = 0;
    for (int tries = 0; tries < 200 && !L.barn; tries++) {
        for (int y = 0; y < LS_H && !L.barn; y++)
            for (int x = 0; x < LS_W && !L.barn; x++)
                if (p->cell[y][x] == CL_WALL && is_stump(p, x, y, L.seed)) { L.barn = 1; L.bx = x; L.by = y; }
        if (!L.barn) L.seed = (unsigned)rand();
    }
    L.barn_look = 0; L.barn_until = 0; L.barn_blink = now_ms() + 3000;
    /* the owlet starts next to the owl */
    L.owlet = 0;
    for (int d = 0; d < 4 && !L.owlet; d++) {
        int x = L.ax[0] + DX[d], y = L.ay[0] + DY[d];
        if (ls_floor(x, y) && !used[y][x] && !p->goal[y][x]) { L.owlet = 1; L.olx = L.olfx = x; L.oly = L.olfy = y; L.ol_t = 0; used[y][x] = 1; }
    }
    L.sq_on = 0;
}

static void listen_enter(void)
{
    memset(&L, 0, sizeof L);
    L.kdeg = L.tdeg = L.cur_deg = -1;
    lead_reset();
    listen_new_room();
    L.help_until = now_ms() + 7000;
    L.seen_section = g_section_ms;
    S.screen = SC_LISTEN;
    music_listen(1);
    S.dirty = 1;
}

static void listen_leave(void)
{
    for (int i = LH_GATE; i <= LH_DOUBLE; i++) music_hold(i, 0);
    lead_reset();
    music_listen(0);
    S.screen = SC_TITLE;
}

/* ---- playing along: Melody or Drums (Select switches) ---- */

/* let go of everything you're playing: the lead note and its modifiers */
static void listen_quiet(void)
{
    lead_reset();
    L.dmask = 0; L.kdeg = L.tdeg = L.cur_deg = -1; L.mods = 0;
    for (int i = 0; i < 5; i++) L.tkind[i] = 0;
}

/* Melody: the D-pad as a circle of eight notes. Right is the current chord's root, then counterclockwise
   one scale step at a time (right-up, up, left-up, left, left-down, down) to right-down, the octave. */
static int dpad_deg(int m)
{
    int dx = ((m & 8) ? 1 : 0) - ((m & 4) ? 1 : 0), dy = ((m & 1) ? 1 : 0) - ((m & 2) ? 1 : 0);
    static const int DEG[3][3] = { { 5, 6, 7 }, { 4, -1, 0 }, { 3, 2, 1 } };   /* [dy + 1][dx + 1] */
    return DEG[dy + 1][dx + 1];
}

static void listen_update_note(void)
{
    int d = -1;
    if (L.pmode == 0) {
        d = dpad_deg(L.dmask);
        if (d < 0) d = L.tdeg;
        if (d < 0) d = L.kdeg;
    }
    if (d == L.cur_deg) return;
    L.cur_deg = d;
    if (d >= 0) lead_on(d); else lead_off();
}

static void listen_set_mode(int m)
{
    listen_quiet();
    L.pmode = m;
    if (g_kbd != 2) toast(m ? "Drums" : "Melody");     /* touch shows the mode on its button */
}

static void listen_mod(int which, int on)
{
    if (on) L.mods |= 1 << which; else L.mods &= ~(1 << which);
    lead_mod(which, on);
}

static void listen_voice(void)
{
    char b[32];
    snprintf(b, sizeof b, "Voice: %s", LV_NAMES[lead_next_voice()]);
    toast(b);
}

/* Drums: B kick, A snare, Y shaker, X clap; Up rim, Down low tom, Left woodblock, Right high tom */
static int drum_for_key(SDLKey k)
{
    switch (k) {
    case BTN_B: return DR_KICK;
    case BTN_A: return DR_SNARE;
    case BTN_Y: return DR_SHAKER;
    case BTN_X: return DR_CLAP;
    case BTN_UP: return DR_RIM;
    case BTN_DOWN: return DR_LOTOM;
    case BTN_LEFT: return DR_BLOCK;
    case BTN_RIGHT: return DR_HITOM;
    default: return -1;
    }
}
static int dpad_bit(SDLKey k) { return k == BTN_UP ? 1 : k == BTN_DOWN ? 2 : k == BTN_LEFT ? 4 : k == BTN_RIGHT ? 8 : 0; }

/* which part each animal dances to (index into g_hit_ms) and how present that part is right now */
static const int LS_PART[NUM_ANIMALS] = { 1, 4, 0, 2 };          /* owl: snare, frog: bass, fox: kick, raccoon: shaker */
static float listen_part_level(int part)
{
    float live = 1.0f - M.wind;
    if (M.duck_t > 0) return 1.0f;                                /* you're drumming: everyone dances */
    switch (part) {
    case 4: return M.wb_gain * live;
    default: return M.layer_gain[part] * live;
    }
}


/* ---- getting around the clearing ---- */

static int ls_acorn_at(int x, int y)
{
    for (int k = 0; k < L.nac; k++) if (L.acx[k] == x && L.acy[k] == y) return 1;
    return 0;
}
/* floor with nothing on it (animals aside) */
static int ls_open(int x, int y) { return ls_floor(x, y) && !ls_acorn_at(x, y); }
static int ls_animal_at(int x, int y, int except)
{
    for (int a = 0; a < NUM_ANIMALS; a++) if (a != except && L.ax[a] == x && L.ay[a] == y) return 1;
    if (L.owlet && except != 0 && L.olx == x && L.oly == y) return 1;    /* the owl can swap places with its owlet */
    return 0;
}
static int ls_claimed(int x, int y, int except)
{
    for (int a = 0; a < NUM_ANIMALS; a++) if (a != except && L.tx[a] == x && L.ty[a] == y) return 1;
    return 0;
}

/* distances (in steps) to (x, y) over open floor; with_animals: other animals block too */
static void ls_bfs(int x, int y, int self, int with_animals, int dist[LS_H][LS_W])
{
    int qx[LS_W * LS_H], qy[LS_W * LS_H], h = 0, n = 0;
    for (int j = 0; j < LS_H; j++) for (int i = 0; i < LS_W; i++) dist[j][i] = -1;
    dist[y][x] = 0; qx[n] = x; qy[n++] = y;
    while (h < n) {
        int cx = qx[h], cy = qy[h++];
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (!ls_open(nx, ny) || dist[ny][nx] >= 0) continue;
            if (with_animals && ls_animal_at(nx, ny, self) && !(nx == L.ax[self] && ny == L.ay[self])) continue;
            dist[ny][nx] = dist[cy][cx] + 1;
            qx[n] = nx; qy[n++] = ny;
        }
    }
}

/* a random open square this animal can reach, not claimed by anyone, a little apart from the others */
static void ls_pick_spot(int a)
{
    int dist[LS_H][LS_W];
    ls_bfs(L.ax[a], L.ay[a], a, 0, dist);
    int best = -1, bx = L.ax[a], by = L.ay[a];
    for (int tries = 0; tries < 80; tries++) {
        int x = rand() % LS_W, y = rand() % LS_H;
        if (dist[y][x] <= 0 || ls_claimed(x, y, a)) continue;
        int near = 99;
        for (int o = 0; o < NUM_ANIMALS; o++) if (o != a) { int d = abs(L.tx[o] - x) + abs(L.ty[o] - y); if (d < near) near = d; }
        int score = (near >= 2 ? 100 : 0) + (dist[y][x] <= 8 ? 10 : 0) + rand() % 10;
        if (score > best) { best = score; bx = x; by = y; }
    }
    L.tx[a] = bx; L.ty[a] = by; L.stuck[a] = 0;
}

/* the four open squares nearest the middle of the clearing, a 2x2 block if there is one */
static void ls_gather_spots(int gx[4], int gy[4])
{
    float cx = 0, cy = 0; int n = 0;
    for (int y = 0; y < LS_H; y++) for (int x = 0; x < LS_W; x++) if (ls_floor(x, y)) { cx += x; cy += y; n++; }
    if (n) { cx /= n; cy /= n; }
    float bd = 1e9f; int bx = -1, by = -1;
    for (int y = 0; y + 1 < LS_H; y++)
        for (int x = 0; x + 1 < LS_W; x++)
            if (ls_open(x, y) && ls_open(x + 1, y) && ls_open(x, y + 1) && ls_open(x + 1, y + 1)) {
                float d = (x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy);
                if (d < bd) { bd = d; bx = x; by = y; }
            }
    if (bx >= 0) {
        for (int k = 0; k < 4; k++) { gx[k] = bx + (k & 1); gy[k] = by + (k >> 1); }
        return;
    }
    int taken = 0;
    while (taken < 4) {                 /* otherwise the nearest open squares, one at a time */
        float best = 1e9f; int px = L.ax[taken], py = L.ay[taken];
        for (int y = 0; y < LS_H; y++)
            for (int x = 0; x < LS_W; x++) {
                if (!ls_open(x, y)) continue;
                int dup = 0;
                for (int k = 0; k < taken; k++) if (gx[k] == x && gy[k] == y) dup = 1;
                if (dup) continue;
                float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (d < best) { best = d; px = x; py = y; }
            }
        gx[taken] = px; gy[taken] = py; taken++;
    }
}

static void ls_gather(void)
{
    int gx[4], gy[4], used[4] = { 0 };
    ls_gather_spots(gx, gy);
    for (int a = 0; a < NUM_ANIMALS; a++) {          /* each to the nearest spot still free */
        int best = -1, bd = 1 << 30;
        for (int k = 0; k < 4; k++) {
            if (used[k]) continue;
            int d = abs(gx[k] - L.ax[a]) + abs(gy[k] - L.ay[a]);
            if (d < bd) { bd = d; best = k; }
        }
        if (best < 0) best = a;
        used[best] = 1;
        L.tx[a] = gx[best]; L.ty[a] = gy[best]; L.mode[a] = LM_GATHER; L.stuck[a] = 0;
    }
    L.gathered = 1;
#ifdef LS_DEBUG
    fprintf(stderr, "GATHER");
    for (int i = 0; i < NUM_ANIMALS; i++) fprintf(stderr, " (%d,%d)->(%d,%d)", L.ax[i], L.ay[i], L.tx[i], L.ty[i]);
    fprintf(stderr, "\n");
#endif
}

static void ls_scatter(void)
{
#ifdef LS_DEBUG
    { int n = 0; for (int i = 0; i < NUM_ANIMALS; i++) n += L.ax[i] == L.tx[i] && L.ay[i] == L.ty[i];
      fprintf(stderr, "SCATTER arrived %d/4\n", n); }
#endif
    for (int a = 0; a < NUM_ANIMALS; a++) { L.tx[a] = L.ax[a]; L.ty[a] = L.ay[a]; }
    for (int a = 0; a < NUM_ANIMALS; a++) { ls_pick_spot(a); L.mode[a] = LM_SCATTER; }
    L.gathered = 0;
}

/* one step toward the target, if the way is clear; returns 1 if it moved */
static int ls_step(int a, Uint32 t)
{
    if (L.ax[a] == L.tx[a] && L.ay[a] == L.ty[a]) return 0;
    int dist[LS_H][LS_W];
    ls_bfs(L.tx[a], L.ty[a], a, 1, dist);
    int best = -1, bd = 1 << 30;
    for (int d = 0; d < 4; d++) {
        int nx = L.ax[a] + DX[d], ny = L.ay[a] + DY[d];
        if (!ls_open(nx, ny) || ls_animal_at(nx, ny, a) || dist[ny][nx] < 0) continue;
        if (dist[ny][nx] < bd) { bd = dist[ny][nx]; best = d; }
    }
    if (best < 0 || (bd >= dist[L.ay[a]][L.ax[a]] && dist[L.ay[a]][L.ax[a]] >= 0)) {
        /* blocked (someone in the way): wait, and after a few beats go somewhere else */
        if (++L.stuck[a] >= (L.mode[a] == LM_GATHER ? 8 : 3)) {
            if (L.mode[a] == LM_GATHER) {          /* settle for the nearest open square to where we are */
                L.tx[a] = L.ax[a]; L.ty[a] = L.ay[a];
            } else ls_pick_spot(a);
        }
        return 0;
    }
    L.stuck[a] = 0;
    L.fromx[a] = L.ax[a]; L.fromy[a] = L.ay[a];
    L.ax[a] += DX[best]; L.ay[a] += DY[best];
    if (DX[best]) L.face[a] = DX[best] < 0;
    L.step_t[a] = t;
    if (a == 0 && L.owlet) {           /* the owlet hops into the square the owl just left */
        L.olfx = L.olx; L.olfy = L.oly;
        L.olx = L.fromx[0]; L.oly = L.fromy[0];
        L.ol_t = t;
    }
    return 1;
}

/* called on every beat (every eighth in double time) */
static void ls_move(Uint32 t, int eighth)
{
    for (int a = 0; a < NUM_ANIMALS; a++) {
        int resting = listen_part_level(LS_PART[a]) <= 0.25f;
        switch (L.mode[a]) {
        case LM_GATHER:
            ls_step(a, t);
            break;
        case LM_SCATTER:
            if (eighth) break;
            if (!ls_step(a, t) && L.ax[a] == L.tx[a] && L.ay[a] == L.ty[a]) { L.mode[a] = LM_STAY; L.rest[a] = 2 + rand() % 5; }
            break;
        case LM_WANDER:
            if (eighth) break;
            if (!resting) { L.tx[a] = L.ax[a]; L.ty[a] = L.ay[a]; L.mode[a] = LM_STAY; break; }   /* its part is back: dance here */
            if (!ls_step(a, t) && L.ax[a] == L.tx[a] && L.ay[a] == L.ty[a]) { L.mode[a] = LM_STAY; L.rest[a] = 2 + rand() % 5; }
            break;
        default:   /* staying put; a resting animal wanders off after a few beats */
            if (eighth) break;
            if (resting && --L.rest[a] <= 0) { ls_pick_spot(a); L.mode[a] = LM_WANDER; }
            break;
        }
    }
}

static void ls_moves_tick(Uint32 t)
{
    int st = M.step;
    if (st == L.last_step) return;
    L.last_step = st;
    int dbl = M.dbl_left > 0;
    /* gather once double time has lasted a bar; scatter when it ends */
    if (dbl && !L.dbl_since) L.dbl_since = t;
    if (!dbl) L.dbl_since = 0;
    Uint32 bar_ms = (Uint32)(16 * STEP_SEC * 1000);
    if (dbl && !L.gathered && t - L.dbl_since >= bar_ms) ls_gather();
    if (!dbl && L.gathered) ls_scatter();
    if (L.asleep) return;
    if ((st % 4) == 0) { L.step_ms = L.gathered ? 180 : 260; ls_move(t, 0); }
    else if ((st % 2) == 0 && L.gathered) { L.step_ms = 180; ls_move(t, 1); }
#ifdef LS_DEBUG
    for (int i = 0; i < NUM_ANIMALS; i++) {
        if (!ls_open(L.ax[i], L.ay[i])) fprintf(stderr, "animal %d on a blocked square %d,%d\n", i, L.ax[i], L.ay[i]);
        for (int j = i + 1; j < NUM_ANIMALS; j++)
            if (L.ax[i] == L.ax[j] && L.ay[i] == L.ay[j]) fprintf(stderr, "OVERLAP %d %d at %d,%d\n", i, j, L.ax[i], L.ay[i]);
    }
    for (int i = 0; L.owlet && i < NUM_ANIMALS; i++)
        if (L.ax[i] == L.olx && L.ay[i] == L.oly) fprintf(stderr, "OWLET OVERLAP %d at %d,%d\n", i, L.olx, L.oly);
    if (L.owlet && !ls_open(L.olx, L.oly)) fprintf(stderr, "owlet on blocked square\n");
    if ((st % 16) == 0) {
        fprintf(stderr, "bar g=%d dbl=%d", L.gathered, dbl);
        for (int i = 0; i < NUM_ANIMALS; i++) fprintf(stderr, "  %d:(%d,%d)->(%d,%d) m%d s%d", i, L.ax[i], L.ay[i], L.tx[i], L.ty[i], L.mode[i], L.stuck[i]);
        fprintf(stderr, "\n");
    }
#endif
}

static void listen_tick(void)
{
    Uint32 t = now_ms();
    for (int k = 0; k < 5; k++) if (g_hit_ms[k] != L.seen_hit[k]) { L.seen_hit[k] = g_hit_ms[k]; L.hits[k]++; }
    /* sleep timer: fade over the last minute */
    if (L.sleep_end && !L.asleep) {
        Sint32 rem = (Sint32)(L.sleep_end - t);
        if (rem <= 0) { L.asleep = 1; listen_quiet(); music_sleep_gain(0); music_sleep_wind(1); }
        else if (rem < 60000) music_sleep_gain(rem / 60000.0f);
    }
    /* once the last drum has faded, the Miyoo switches itself off (saving first) */
    if (L.asleep && g_sleep_done) {
        g_sleep_done = 0;
#ifndef __EMSCRIPTEN__
        if (g_on_device) {
            save_all();
            sync();
            if (system("poweroff") != 0) g_quit = 1;   /* if that's not allowed, at least go back to Onion */
        }
#endif
    }
    /* a new section: a squirrel pops out for an acorn */
    if (g_section_ms != L.seen_section) {
        L.seen_section = g_section_ms;
        if (!L.sq_on && !L.asleep && L.nac) {
            int k = rand() % L.nac;
            if (!L.ac_gone[k]) {
                int bx = L.acx[k], by = L.acy[k], bd = 1 << 30;
                for (int y = 0; y < LS_H; y++)
                    for (int x = 0; x < LS_W; x++) {
                        if (L.room.cell[y][x] != CL_WALL || is_stump(&L.room, x, y, L.seed)) continue;
                        int d = (x - L.acx[k]) * (x - L.acx[k]) + (y - L.acy[k]) * (y - L.acy[k]);
                        if (d < bd) { bd = d; bx = x; by = y; }
                    }
                L.sq_on = 1; L.sq_ac = k; L.sq_hx = bx; L.sq_hy = by;
                L.sq_start = t; L.sq_out = 300 + (Uint32)(sqrtf((float)bd) * 200);
                L.ac_gone[k] = t + L.sq_out;
                L.ac_back[k] = t + L.sq_out * 2 + 6000 + rand() % 6000;
            }
        }
    }
    if (L.sq_on && t > L.sq_start + L.sq_out * 2 + 100) L.sq_on = 0;
    ls_moves_tick(t);
    if (L.barn) {
        int ck = M.chord * 100 + M.key;
        if (ck != L.seen_chord) {                     /* a new chord: the barn owl looks around */
            L.seen_chord = ck;
            L.barn_look = (rand() % 3) - 1;           /* left, front or right */
            L.barn_until = t + 2500 + rand() % 3000;
        }
        if (L.barn_look && t > L.barn_until) L.barn_look = 0;
        if (t > L.barn_blink + 150) L.barn_blink = t + 2500 + rand() % 5000;
    }
    for (int k = 0; k < L.nac; k++) if (L.ac_back[k] && t >= L.ac_back[k]) L.ac_gone[k] = L.ac_back[k] = 0;
}

/* held rain: short streaks that fall and splash, fading in and out with it */
static void listen_rain(int ox, int oy, Uint32 t)
{
    static struct { float x, y, v; int land; Uint32 splash; } drop[80];
    static int init;
    static Uint32 last;
    float dt = last ? (t - last) / 1000.0f : 0; last = t;
    if (dt > 0.1f) dt = 0.1f;
    if (!init) {
        for (int i = 0; i < 80; i++) { drop[i].x = rand() % SCREEN_W; drop[i].y = rand() % SCREEN_H; drop[i].v = 380 + rand() % 160; drop[i].land = 40 + rand() % (SCREEN_H - 40); }
        init = 1;
    }
    int n = (int)(M.rain * 80);
    for (int i = 0; i < 80; i++) {
        if (drop[i].splash) {
            if (t - drop[i].splash < 120) {
                if (i < n) {
                    int X = (int)drop[i].x / 3 * 3, Y = drop[i].land / 3 * 3;
                    rect(X - 6, Y, 3, 3, BG1); rect(X + 6, Y, 3, 3, BG1); rect(X, Y - 3, 3, 3, BG1);
                }
                continue;
            }
            drop[i].splash = 0; drop[i].x = rand() % SCREEN_W; drop[i].y = -(float)(rand() % 120);
            drop[i].land = 40 + rand() % (SCREEN_H - 40); drop[i].v = 380 + rand() % 160;
        }
        drop[i].y += drop[i].v * dt; drop[i].x -= drop[i].v * dt * 0.12f;      /* a little slant */
        if (drop[i].x < 0) drop[i].x += SCREEN_W;
        if (drop[i].y >= drop[i].land) { drop[i].splash = t; continue; }
        if (i < n && drop[i].y > -12) {
            int X = (int)drop[i].x / 3 * 3, Y = (int)drop[i].y / 3 * 3;
            rect(X, Y, 3, 9, BG1); rect(X + 3, Y - 6, 3, 6, BG1);
        }
    }
}

/* ---- touch in Listen: a mode button, and Melody's modifier buttons or Drums' eight pads ---- */
#define LT_MODX (SCREEN_W - 104)
#define LT_MODY 104
#define LT_MODW 96
#define LT_MODH 56
#define LT_MODGAP 72
#define LT_PADY 56
static const char *const LT_MOD_NAMES[4] = { "OCT", "VIB", "ARP", "VOICE" };
static const int LT_PADS[8] = { DR_LOTOM, DR_HITOM, DR_RIM, DR_BLOCK, DR_KICK, DR_SNARE, DR_SHAKER, DR_CLAP };
static const char *const LT_PAD_NAMES[8] = { "LO TOM", "HI TOM", "RIM", "BLOCK", "KICK", "SNARE", "SHAKER", "CLAP" };

static int lt_mod_at(int x, int y)
{
    if (x < LT_MODX || x >= LT_MODX + LT_MODW) return -1;
    for (int i = 0; i < 4; i++) if (y >= LT_MODY + i * LT_MODGAP && y < LT_MODY + i * LT_MODGAP + LT_MODH) return i;
    return -1;
}
static int lt_pad_at(int x, int y)
{
    if (y < LT_PADY) return -1;
    int c = x * 4 / SCREEN_W, r = (y - LT_PADY) * 2 / (SCREEN_H - LT_PADY);
    if (c < 0 || c > 3 || r < 0 || r > 1) return -1;
    return r * 4 + c;
}
static int lt_deg_at(int y)                    /* higher up, higher note */
{
    int d = (SCREEN_H - y) * 8 / (SCREEN_H - LT_PADY);
    return d < 0 ? 0 : d > 7 ? 7 : d;
}

static void listen_draw_touch(Uint32 t)
{
    /* the mode, top right: tap to switch */
    const char *mname = L.pmode ? "DRUMS" : "MELODY";
    int bw = 128, bx = SCREEN_W - 8 - bw;
    rect(bx, 8, bw, 32, BG3);
    text_c(bx + bw / 2, 16, mname, 2, BG0);
    tap_add(bx - 8, 0, bw + 16, 56, TAP_BTN, NULL, BTN_SELECT);
    if (L.pmode == 0) {
        for (int i = 0; i < 4; i++) {
            int y = LT_MODY + i * LT_MODGAP, on = i < 3 && (L.mods & (1 << i));
            rect(LT_MODX, y, LT_MODW, LT_MODH, on ? BG0 : BG3);
            text_c(LT_MODX + LT_MODW / 2, y + 20, LT_MOD_NAMES[i], 2, on ? BG3 : BG0);
        }
    } else {
        int pw = SCREEN_W / 4, ph = (SCREEN_H - LT_PADY) / 2;
        for (int p = 0; p < 8; p++) {
            int x = (p % 4) * pw, y = LT_PADY + (p / 4) * ph;
            int hit = L.pad_flash[p] && t - L.pad_flash[p] < 160;
            Uint32 c = hit ? BG0 : BG3;
            int b = hit ? 6 : 2;
            rect(x + 4, y + 4, pw - 8, b, c); rect(x + 4, y + ph - 4 - b, pw - 8, b, c);
            rect(x + 4, y + 4, b, ph - 8, c); rect(x + pw - 4 - b, y + 4, b, ph - 8, c);
            int lw = text_w(LT_PAD_NAMES[p], 2) + 16;
            rect(x + (pw - lw) / 2, y + ph - 44, lw, 28, BG3);
            text_c(x + pw / 2, y + ph - 38, LT_PAD_NAMES[p], 2, BG0);
        }
    }
}

static void draw_listen(void)
{
    Uint32 t = now_ms();
    const int sc = 3, ts = SPR * sc;
    int ox = (SCREEN_W - LS_W * ts) / 2, oy = (SCREEN_H - LS_H * ts) / 2;
    SpriteSet *u = ui_set(sc, 0);
    rect(0, 0, SCREEN_W, SCREEN_H, BG3);
    draw_world(&L.room, u, ox, oy, 0, 0, SCREEN_W, SCREEN_H, L.seed);
    for (int k = 0; k < L.nac; k++) {
        if (L.ac_gone[k] && t >= L.ac_gone[k]) continue;
        int drop = 0;
        if (L.ac_back[k] == 0 && L.ac_gone[k] == 0) drop = 0;
        blit(u->tile[TL_ACORN_GOAL], ox + L.acx[k] * ts, oy + L.acy[k] * ts - drop);
    }
    /* the owlet, trailing after the owl */
    if (L.owlet) {
        float k = L.ol_t && L.step_ms ? (t - L.ol_t) / (float)L.step_ms : 1.0f;
        if (k > 1) k = 1;
        float fx = L.olfx + (L.olx - L.olfx) * k, fy = L.olfy + (L.oly - L.olfy) * k;
        int dy = k < 1 ? -(int)(sinf(k * 3.1416f) * 3) * sc : 0;
        int owl_dancing = listen_part_level(LS_PART[0]) > 0.25f;
        if (!L.asleep && k >= 1 && (t - g_hit_ms[3] < 140 || (owl_dancing && t - g_hit_ms[LS_PART[0]] < 140))) dy = -2 * sc;
        int fr = L.asleep || ((t + 900) % 3700) < 140 ? TL_OWLET_BLINK : TL_OWLET;
        blit(L.face[0] ? u->tilef[fr] : u->tile[fr], ox + (int)(fx * ts) / sc * sc, oy + (int)(fy * ts) / sc * sc + dy);
    }
    /* the dancers */
    for (int a = 0; a < NUM_ANIMALS; a++) {
        SpriteSet *ua = ui_set(sc, a);
        int part = LS_PART[a], frame = FR_IDLE, flip = (a & 1), dy = 0;
        float lvl = listen_part_level(part);
        Uint32 since = t - g_hit_ms[part];
        float k = L.step_t[a] && L.step_ms ? (t - L.step_t[a]) / (float)L.step_ms : 1.0f;
        if (k > 1) k = 1;
        int walking = k < 1;
        int arrived = L.ax[a] == L.tx[a] && L.ay[a] == L.ty[a];
        if (L.asleep) frame = FR_BLINK;                                   /* asleep, eyes closed */
        else if (walking) { frame = FR_STEP; flip = L.face[a]; dy = -(int)(sinf(k * 3.1416f) * 2) * sc; }
        else if (lvl > 0.25f && (arrived || L.mode[a] == LM_STAY)) {
            flip = (L.hits[part] / 2) % 2;                                /* turn every other hit */
            if (since < 150) { frame = FR_HAPPY; dy = -2 * sc; }
            else if (since < 260) frame = FR_TAIL;
        } else if (L.gathered && arrived) {                               /* gathered: bounce on the beat anyway */
            Uint32 kb = t - g_hit_ms[0];
            flip = (L.hits[0] / 2 + a) % 2;
            if (kb < 150) { frame = FR_HAPPY; dy = -2 * sc; }
        } else {
            Uint32 ph = (t + a * 1777) % 4200;
            flip = L.face[a];
            frame = ph < 140 ? FR_BLINK : ((t + a * 500) / 900) % 2 ? FR_TAIL : FR_IDLE;
        }
        float fx = L.fromx[a] + (L.ax[a] - L.fromx[a]) * k, fy = L.fromy[a] + (L.ay[a] - L.fromy[a]) * k;
        blit(ua->anim[frame][flip], ox + (int)(fx * ts) / sc * sc, oy + (int)(fy * ts) / sc * sc + dy);
    }
    /* a squirrel fetching an acorn */
    if (L.sq_on) {
        Uint32 rel = t - L.sq_start;
        int k = L.sq_ac, carry = rel >= L.sq_out;
        float u2 = carry ? (rel - L.sq_out) / (float)L.sq_out : rel / (float)L.sq_out;
        if (u2 > 1) u2 = 1;
        float fx = carry ? L.acx[k] + (L.sq_hx - L.acx[k]) * u2 : L.sq_hx + (L.acx[k] - L.sq_hx) * u2;
        float fy = carry ? L.acy[k] + (L.sq_hy - L.acy[k]) * u2 : L.sq_hy + (L.acy[k] - L.sq_hy) * u2;
        int left = carry ? L.sq_hx < L.acx[k] : L.acx[k] < L.sq_hx;
        int fr = carry ? TL_SQUIRREL_CARRY : ((rel / 90) % 2 ? TL_SQUIRREL_B : TL_SQUIRREL);
        int hop = ((rel / 90) % 2) ? 2 * sc : 0;
        blit(left ? u->tilef[fr] : u->tile[fr], ox + (int)(fx * ts) / sc * sc, oy + (int)(fy * ts) / sc * sc - hop);
    }
    /* the barn owl on its stump: drawn last, so anyone walking behind it passes behind */
    if (L.barn) {
        int fr = L.asleep || (t >= L.barn_blink && t < L.barn_blink + 150) ? TL_BARN_BLINK : L.barn_look ? TL_BARN_LOOK : TL_BARN;
        Img *im = L.barn_look > 0 ? u->tilef[fr] : u->tile[fr];
        blit(im, ox + L.bx * ts, oy + L.by * ts - 9 * sc);
    }
    listen_rain(ox, oy, t);
    draw_fireflies(ox, oy, ts, sc, LS_W, LS_H, L.seed);
    if (L.asleep) text_c(SCREEN_W / 2, 16, "Goodnight", 2, BG1);
    if (g_kbd == 2) {                                   /* touch: a menu button in the corner */
        rect(8, 8, 40, 32, BG3);
        for (int i = 0; i < 3; i++) rect(16, 14 + i * 8, 24, 4, BG1);
        tap_add(0, 0, 64, 56, TAP_BTN, NULL, BTN_START);
        if (!L.menu && !L.asleep) listen_draw_touch(t);
    }
    if (t < L.help_until && !L.menu) {
        static const char *const PAD[2][7] = {
            { "D-pad: notes in a circle,", "  right = the chord's root", "A octave     B vibrato", "X arpeggio   Y voice",
              "L1 chop      R1 warble", "L2 rain      R2 double", "SELECT drums  START menu" },
            { "B kick       A snare", "Y shaker     X clap", "Up rim       Down low tom", "Left block   Right high tom",
              "L1 chop      R1 warble", "L2 rain      R2 double", "SELECT melody  START menu" } };
        static const char *const KEYS[2][7] = {
            { "1-8 notes, or the arrows", "  (two at once between)", "Z octave     X vibrato", "C arpeggio   V voice",
              "9 chop       0 warble", "- rain       = double", "M drums      Esc menu" },
            { "1 kick   2 snare  3 shaker", "4 clap   5 rim", "6 low tom   7 block", "8 high tom",
              "9 chop       0 warble", "- rain       = double", "M melody     Esc menu" } };
        static const char *const TOUCH[2][7] = {
            { "Touch: a note (higher up,", "  a higher note)", "Slide to change notes", "Hold OCT, VIB or ARP",
              "  while you play", "VOICE: another sound", "Top right: drums" },
            { "Tap the pads to drum", "", "Your drumming steps in", "  for the band's drums", "", "", "Top right: melody" } };
        const char *const *h = g_kbd == 1 ? KEYS[L.pmode] : g_kbd == 2 ? TOUCH[L.pmode] : PAD[L.pmode];
        int w = 544, x = (SCREEN_W - w) / 2, y = 128;
        if (g_kbd == 2) { w = 496; x = 16; }           /* touch: leave the buttons down the right side showing */
        panel(x, y, w, 7 * 24 + 48);
        for (int i = 0; i < 7; i++) text(x + 32, y + 24 + i * 24, h[i], 2, BG3);
    }
    if (L.menu) {
        char sb[32];
        const char *items[NLSMENU];
        for (int i = 0; i < NLSMENU; i++) items[i] = LS_MENU[i];
        if (L.sleep_end && !L.asleep && t - L.sleep_set >= 1000) {
            Uint32 rem = (L.sleep_end - t + 999) / 1000;
            snprintf(sb, sizeof sb, "Sleep  < %d:%02d >", rem / 60, rem % 60);
        } else snprintf(sb, sizeof sb, "Sleep  < %s >", SLEEP_MIN[L.sleep_idx] ? "on" : "off");
        if (L.sleep_idx && (!L.sleep_end || t - L.sleep_set < 1000)) snprintf(sb, sizeof sb, "Sleep  < %d min >", SLEEP_MIN[L.sleep_idx]);
        items[2] = sb;
        int w = 384, h = 64 + NLSMENU * 32, x = (SCREEN_W - w) / 2, y = 112;
        panel(x, y, w, h);
        modal_taps(x, y, w, h, BTN_B);
        text_c(SCREEN_W / 2, y + 20, "LISTENING", 2, BG3);
        draw_menu_list(x + 32, y + 52, items, NLSMENU, L.menu_sel);
        menu_taps(x + 32, y + 52, w - 64, NLSMENU, &L.menu_sel);
    }
}

static void listen_menu_key(SDLKey k)
{
    if (k == BTN_UP) menu_nav(&L.menu_sel, NLSMENU, -1);
    else if (k == BTN_DOWN) menu_nav(&L.menu_sel, NLSMENU, 1);
    else if (k == BTN_B || k == BTN_START) L.menu = 0;
    else if (L.menu_sel == 2 && (k == BTN_LEFT || k == BTN_RIGHT || k == BTN_A)) {
        L.sleep_idx = (L.sleep_idx + (k == BTN_LEFT ? 3 : 1)) % 4;
        /* it shows "15 min" for a second (so you can step past it), then starts counting down */
        L.sleep_set = now_ms();
        L.sleep_end = SLEEP_MIN[L.sleep_idx] ? now_ms() + 1000 + SLEEP_MIN[L.sleep_idx] * 60000u : 0;
        L.asleep = 0; music_sleep_gain(1); music_sleep_wind(0);
        sfx_play(SFX_MENU);
    } else if (k == BTN_A) {
        sfx_play(SFX_SELECT);
        switch (L.menu_sel) {
        case 0: L.menu = 0; break;
        case 1: L.menu = 0; listen_new_room(); break;
        case 3: L.menu = 0; L.help_until = now_ms() + 9000; break;
        case 4: L.menu = 0; listen_leave(); break;
        }
    }
}

static void listen_key(SDLKey k)
{
    if (L.asleep) { L.asleep = 0; L.sleep_end = 0; L.sleep_idx = 0; music_sleep_gain(1); music_sleep_wind(0); return; }   /* wake up */
    if (L.menu) { listen_menu_key(k); return; }
    switch (k) {                                   /* the same in both modes */
    case BTN_L1: music_hold(LH_GATE, 1); return;
    case BTN_R1: music_hold(LH_WARBLE, 1); return;
    case BTN_L2: music_hold(LH_RAIN, 1); return;
    case BTN_R2: music_hold(LH_DOUBLE, 1); return;
    case BTN_SELECT: listen_set_mode(!L.pmode); return;
    case BTN_START: case BTN_MENU: listen_quiet(); L.menu = 1; L.menu_sel = 0; L.help_until = 0; sfx_play(SFX_MENU); return;
    default: break;
    }
    int bit = dpad_bit(k);
    if (bit) L.dmask |= bit;
    if (L.pmode == 1) { int d = drum_for_key(k); if (d >= 0) music_drum(d); return; }
    if (bit) { listen_update_note(); return; }
    switch (k) {
    case BTN_A: listen_mod(LMOD_OCT, 1); break;    /* glide up an octave (and back when let go) */
    case BTN_B: listen_mod(LMOD_VIB, 1); break;    /* vibrato */
    case BTN_X: listen_mod(LMOD_ARP, 1); break;    /* arpeggio from the note you're playing */
    case BTN_Y: listen_voice(); break;             /* next voice */
    default: break;
    }
}

static void listen_key_up(SDLKey k)
{
    if (k == BTN_L1) music_hold(LH_GATE, 0);
    if (k == BTN_R1) music_hold(LH_WARBLE, 0);
    if (k == BTN_L2) music_hold(LH_RAIN, 0);
    if (k == BTN_R2) music_hold(LH_DOUBLE, 0);
    int bit = dpad_bit(k);
    if (bit) { L.dmask &= ~bit; if (L.pmode == 0 && !L.menu) listen_update_note(); }
    if (L.pmode == 0 && !L.menu) {
        if (k == BTN_A && (L.mods & 1)) listen_mod(LMOD_OCT, 0);
        if (k == BTN_B && (L.mods & 2)) listen_mod(LMOD_VIB, 0);
        if (k == BTN_X && (L.mods & 4)) listen_mod(LMOD_ARP, 0);
    }
}

static void draw_edit(void);
static void draw_code(void);
static void draw_screen(void)
{
    fav_sync();
    g_ntaps = 0;
    switch (S.screen) {
    case SC_TITLE: draw_title(); break;
    case SC_SETS: draw_sets(); break;
    case SC_LEVELS: draw_levels(); break;
    case SC_GAME: draw_game(); break;
    case SC_OPTIONS: draw_options(); break;
    case SC_CREDITS: draw_credits(); break;
    case SC_LISTEN: draw_listen(); break;
    case SC_EDIT: draw_edit(); break;
    case SC_CODE: draw_code(); break;
    }
    draw_confirm();
    Uint32 t = now_ms();
    if (S.toast[0] && t < S.toast_until) {
        int w = ((text_w(S.toast, 2) + 32 + 15) / 16) * 16;
        rect((SCREEN_W - w) / 2, SCREEN_H - 48, w, 32, BG3);
        text_c(SCREEN_W / 2, SCREEN_H - 40, S.toast, 2, BG0);
    }
}

/* ======================================================================
 * Input
 * ==================================================================== */

static int key_dir(SDLKey k)
{
    switch (k) {
    case BTN_LEFT: return 0;
    case BTN_RIGHT: return 1;
    case BTN_UP: return 2;
    case BTN_DOWN: return 3;
    default: return -1;
    }
}

#include "maker.c"

static void open_levels(int set, int sel)
{
    S.sets_sel = set;
    S.lv_sel = clampi(sel, 0, g_sets[set].n - 1 + (set == g_my));
    S.lv_top = 0;
    S.screen = SC_LEVELS;
}

/* the level list: jump to the next level you haven't solved, wrapping around the set */
static void level_list_next_unsolved(void)
{
    Set *s = &g_sets[S.sets_sel];
    for (int k = 1; k <= s->n; k++) {
        int i = (S.lv_sel + k) % s->n;
        if (!s->best_moves[i]) { S.lv_sel = i; sfx_play(SFX_MENU); S.dirty = 1; return; }
    }
    sfx_play(SFX_BUMP);
    toast("Every level here is solved");
}

static void open_options(int ret)
{
    S.options_return = ret;
    S.opt_sel = 0;
    S.screen = SC_OPTIONS;
}

static void leave_game(int to)
{
    remember_progress();
    save_all();
    S.paused = 0;
    S.screen = to;
}

/* Random -> Owl -> Frog -> Fox -> Raccoon -> Random ...; in a level the new animal takes over at once */
static void change_animal(int d)
{
    cfg.animal = (cfg.animal + 1 + d + NUM_ANIMALS + 1) % (NUM_ANIMALS + 1) - 1;
    if (S.screen == SC_GAME && cfg.animal >= 0) { S.animal = cfg.animal; spriteset_get(&g_spr, S.tscale, S.animal); }
    sfx_play(SFX_MENU);
    save_all();
}

/* back to the level list this level came from (Favorites, if that's where you were playing from) */
static void open_level_list(void)
{
    if (S.fav_play) { fav_sync(); open_levels(g_fav, S.fav_pos); }
    else open_levels(S.set, S.level);
}

static void pause_favorite(void)
{
    int was = fav_find(S.set, S.level) >= 0;
    fav_toggle(S.set, S.level);
    sfx_play(was ? SFX_UNDO : SFX_SELECT);
    if (S.fav_play && was) S.fav_play = 0;       /* it's not in Favorites any more */
}

static void on_key_game(SDLKey k)
{
    if (S.paused) {
        int ids[12], n = pause_items(ids), id = pause_id();
        if (k == BTN_UP) { S.pause_sel = (S.pause_sel + n - 1) % n; sfx_play(SFX_MENU); }
        else if (k == BTN_DOWN) { S.pause_sel = (S.pause_sel + 1) % n; sfx_play(SFX_MENU); }
        else if (k == BTN_B || k == BTN_START) S.paused = 0;
        else if (k == BTN_A) {
            sfx_play(SFX_SELECT);
            switch (id) {
            case PI_RESUME: S.paused = 0; break;
            case PI_MAKER: { int lv = S.level; leave_game(SC_EDIT); ed_open(lv); break; }
            case PI_RESTART: S.paused = 0; restart_level(); break;
            case PI_LIST: leave_game(SC_LEVELS); open_level_list(); break;
            case PI_BEST: S.paused = 0; play_best_run(); break;
            case PI_FAV: pause_favorite(); break;
            case PI_ZOOM: set_zoom(cfg.zoom + 1 > 4 - S.fit_scale ? 0 : cfg.zoom + 1); break;
            case PI_OPTIONS: leave_game(SC_GAME); open_options(SC_GAME); break;
            case PI_TITLE: leave_game(SC_TITLE); break;
            }
        }
        else if (id == PI_ZOOM && (k == BTN_LEFT || k == BTN_RIGHT)) set_zoom(cfg.zoom + (k == BTN_RIGHT ? 1 : -1));
        else if (id == PI_FAV && (k == BTN_LEFT || k == BTN_RIGHT)) pause_favorite();
        return;
    }
    if (S.playback) {                   /* watching the best run: any button takes over from here */
        S.playback = 0;
        S.toast_until = 0;
        toast(HINT("Your turn - A steps on through it", "Your turn - X steps on through it", "Your turn - REDO steps on"));
        return;
    }
    if (S.walking) walk_end();          /* any button stops a walk where it is */
    if (S.rewind_to >= 0 || S.replay_to >= 0) { S.rewind_to = S.replay_to = -1; if (k == BTN_A || k == BTN_B) return; }
    if (S.clear == 1) return;           /* let the dance play */
    if (S.clear == 2) {
        if (k == BTN_A && S.testing) { int lv = S.level, w = S.watching; leave_game(SC_EDIT); ed_open(lv); toast(w ? "Back in the level maker" : "Solved! It has a code now"); return; }
        if (k == BTN_A && S.fav_play) {               /* the next unsolved favorite */
            sfx_play(SFX_SELECT);
            int n = g_nfav, to = (S.fav_pos + 1) % (n ? n : 1);
            for (int d = 1; d <= n; d++) {
                int lv, st = fav_ref((S.fav_pos + d) % n, &lv);
                if (st >= 0 && !g_sets[st].best_moves[lv]) { to = (S.fav_pos + d) % n; break; }
            }
            go_to_fav(to);
            return;
        }
        if (k == BTN_A) { sfx_play(SFX_SELECT); go_to_level(S.set, next_level_in_set(S.set, S.level)); return; }
        if (k == BTN_Y) { leave_game(SC_LEVELS); open_level_list(); return; }
        if (k != BTN_B) return;
        /* B: take back the last move and carry on (undo, and redo, as far as you like) */
    }
    int d = key_dir(k);
    if (d >= 0) {
        if (S.move_t || S.nqueue) { if (S.nqueue < 4) S.queue[S.nqueue++] = d; }
        else try_move(d);
        S.held_dir = d;
        S.dir_next = now_ms() + REPEAT_FIRST;
        return;
    }
    switch (k) {
    case BTN_B:
        if (S.wg_to >= 0 && P.hpos == S.wg_to) { S.rewind_to = S.wg_from; try_undo(0); break; }      /* a walk: all of it */
        try_undo(0); S.undo_held = 1; S.undo_next = now_ms() + REPEAT_FIRST + 60; break;
    case BTN_A:
        if (S.wg_to >= 0 && P.hpos == S.wg_from && P.hlen >= S.wg_to && !S.clear) { S.replay_to = S.wg_to; try_undo(1); break; }
        try_undo(1); S.redo_held = 1; S.redo_next = now_ms() + REPEAT_FIRST + 60; break;
    case BTN_X: S.restart_since = now_ms(); break;
    case BTN_L2: set_zoom(cfg.zoom - 1); break;
    case BTN_R2: set_zoom(cfg.zoom + 1); break;
    case BTN_Y: S.map_held = 1; break;
    case BTN_START: S.paused = 1; S.pause_sel = 0; sfx_play(SFX_MENU); break;
    case BTN_L1: case BTN_R1: {
        int dir = k == BTN_R1 ? 1 : -1;
        if (nav_pos() + dir < 0 || nav_pos() + dir >= nav_count()) break;
        if (now_ms() < S.quick_until) quick_skip(S.set, S.level, dir);     /* still skipping: no hold needed */
        else { S.switch_since = now_ms(); S.switch_dir = dir; }
        break;
    }
    case BTN_SELECT: leave_game(SC_LEVELS); open_level_list(); break;
    default: break;
    }
}

static void menu_nav(int *sel, int n, int d)
{
    *sel = (*sel + d + n) % n;
    sfx_play(SFX_MENU);
}

static void on_key(SDLKey k)
{
    S.dirty = 1;
    fav_sync();
    if (confirm_key(k)) return;
    switch (S.screen) {
    case SC_GAME: on_key_game(k); break;
    case SC_LISTEN: listen_key(k); break;
    case SC_EDIT: ed_key(k); break;
    case SC_CODE: code_key(k); break;
    case SC_TITLE:
        if (k == BTN_UP) menu_nav(&S.title_sel, NTITLE, -1);
        else if (k == BTN_DOWN) menu_nav(&S.title_sel, NTITLE, 1);
        else if (k == BTN_A || k == BTN_START) {
            sfx_play(SFX_SELECT);
            switch (S.title_sel) {
            case 0: if (g_nsets) { S.fav_play = 0; if (cfg.animal < 0) S.animal = cfg.last_animal; start_level(cfg.cur_set, cfg.cur_level, cfg.cur_moves); } break;
            case 1: S.screen = SC_SETS; S.sets_sel = cfg.cur_set; break;
            case 2: g_my_from_title = 1; open_levels(g_my, g_sets[g_my].n); break;   /* My levels, on the + */
            case 3: listen_enter(); break;
            case 4: open_options(SC_TITLE); break;
            case 5: g_quit = 1; break;
            }
        }
        break;
    case SC_SETS:
        if (k == BTN_UP) menu_nav(&S.sets_sel, SETS_ROWS, -1);
        else if (k == BTN_DOWN) menu_nav(&S.sets_sel, SETS_ROWS, 1);
        else if (k == BTN_B) S.screen = SC_TITLE;
        else if (k == BTN_L1 || k == BTN_R1) {           /* a page at a time */
            S.sets_sel = clampi(S.sets_sel + (k == BTN_R1 ? SET_ROWS : -SET_ROWS), 0, SETS_ROWS - 1);
            S.sets_top = clampi(S.sets_top + (k == BTN_R1 ? SET_ROWS : -SET_ROWS), 0, SETS_ROWS > SET_ROWS ? SETS_ROWS - SET_ROWS : 0);
            sfx_play(SFX_MENU);
        }
        else if (k == BTN_SELECT && set_has_menu(S.sets_sel)) { S.confirm = CF_SETMENU; S.confirm_sel = 0; sfx_play(SFX_MENU); }
#ifdef __EMSCRIPTEN__
        else if (k == BTN_A && SETS_ADD_ROW) { sfx_play(SFX_SELECT); web_pick_set_file(); }
#endif
        else if (k == BTN_A) {
            sfx_play(SFX_SELECT);
            g_my_from_title = 0;
            int first = 0;
            while (first < g_sets[S.sets_sel].n && g_sets[S.sets_sel].best_moves[first]) first++;
            if (first >= g_sets[S.sets_sel].n) first = 0;
            if (S.sets_sel == cfg.cur_set) first = cfg.cur_level;
            open_levels(S.sets_sel, first);
        }
        break;
    case SC_LEVELS: {
        if (S.sets_sel == g_my) { my_levels_key(k); break; }
        int n = g_sets[S.sets_sel].n;
        if (S.sets_sel == g_fav) {
            if (k == BTN_B) { S.screen = SC_SETS; return; }
            if (!n) return;
            if (k == BTN_X) { level_list_next_unsolved(); return; }
            if (k == BTN_A || k == BTN_START) { sfx_play(SFX_SELECT); go_to_fav(S.lv_sel); return; }
            if (k == BTN_SELECT) {                     /* take it out of Favorites */
                int lv, st = fav_ref(S.lv_sel, &lv);
                if (st >= 0) fav_toggle(st, lv);
                S.lv_sel = clampi(S.lv_sel, 0, g_nfav - 1 < 0 ? 0 : g_nfav - 1);
                sfx_play(SFX_UNDO);
                toast("Taken out of Favorites");
                return;
            }
        }
        if (k == BTN_LEFT) S.lv_sel = (S.lv_sel + n - 1) % n;
        else if (k == BTN_RIGHT) S.lv_sel = (S.lv_sel + 1) % n;
        else if (k == BTN_UP) S.lv_sel = S.lv_sel >= LV_COLS ? S.lv_sel - LV_COLS : S.lv_sel;
        else if (k == BTN_DOWN) S.lv_sel = S.lv_sel + LV_COLS < n ? S.lv_sel + LV_COLS : n - 1;
        else if (k == BTN_L1 || k == BTN_R1) {          /* a whole page: the view moves with it */
            int maxtop = (n - 1) / LV_COLS - LV_ROWS + 1;
            if (maxtop < 0) maxtop = 0;
            if (k == BTN_L1) { S.lv_sel = S.lv_sel >= LV_COLS * LV_ROWS ? S.lv_sel - LV_COLS * LV_ROWS : 0; S.lv_top = clampi(S.lv_top - LV_ROWS, 0, maxtop); }
            else { S.lv_sel = S.lv_sel + LV_COLS * LV_ROWS < n ? S.lv_sel + LV_COLS * LV_ROWS : n - 1; S.lv_top = clampi(S.lv_top + LV_ROWS, 0, maxtop); }
        }
        else if (k == BTN_B) { S.screen = SC_SETS; return; }
        else if (k == BTN_SELECT) { S.confirm = CF_CHOOSE; S.confirm_sel = 0; sfx_play(SFX_MENU); return; }
        else if (k == BTN_X) { level_list_next_unsolved(); return; }
        else if (k == BTN_A || k == BTN_START) {
            sfx_play(SFX_SELECT);
            go_to_level(S.sets_sel, S.lv_sel);
            return;
        }
        if (k != BTN_B) sfx_play(SFX_MENU);
        break;
    }
    case SC_OPTIONS:
        if (k == BTN_UP) menu_nav(&S.opt_sel, NOPT, -1);
        else if (k == BTN_DOWN) menu_nav(&S.opt_sel, NOPT, 1);
        else if (k == BTN_B) { save_all(); S.screen = S.options_return; }
        else if (k == BTN_LEFT || k == BTN_RIGHT || k == BTN_A) {
            int d = k == BTN_LEFT ? -1 : 1;
            switch (S.opt_sel) {
            case O_PALETTE:
                cfg.palette = (cfg.palette + d + NUM_PALETTES) % NUM_PALETTES;
                apply_palette();
                sfx_play(SFX_MENU);
                break;
            case O_MUSIC: cfg.music_vol = clampi(cfg.music_vol + d, 0, 10); music_set_volume(cfg.music_vol); break;
            case O_SFX: cfg.sfx_vol = clampi(cfg.sfx_vol + d, 0, 10); sfx_set_volume(cfg.sfx_vol); sfx_play(SFX_PUSH); break;
            case O_ANIMAL: cfg.animal = (cfg.animal + 1 + d + NUM_ANIMALS + 1) % (NUM_ANIMALS + 1) - 1; sfx_play(SFX_MENU); break;
#ifdef __EMSCRIPTEN__
            case O_EXPORT:
                if (k == BTN_A) {
                    if (S.options_return == SC_GAME) remember_progress();
                    save_all();
                    EM_ASM({ if (Module.stumpedExportSave) Module.stumpedExportSave(); });
                    sfx_play(SFX_SELECT); toast("Save file downloaded");
                }
                break;
            case O_IMPORT: if (k == BTN_A) { EM_ASM({ if (Module.stumpedPickSave) Module.stumpedPickSave(); }); sfx_play(SFX_MENU); } break;
#endif
            case O_CLEAR: if (k == BTN_A) { S.confirm = CF_ALL; S.confirm_sel = 0; sfx_play(SFX_MENU); } break;
            case O_CREDITS: if (k == BTN_A) S.screen = SC_CREDITS; break;
            case O_BACK: if (k == BTN_A) { save_all(); S.screen = S.options_return; } break;
            }
            if (S.options_return == SC_GAME && cfg.animal >= 0) S.animal = cfg.animal;
            if (S.options_return == SC_GAME) spriteset_get(&g_spr, S.tscale, S.animal);
        }
        break;
    case SC_CREDITS:
        if (k == BTN_B || k == BTN_A) S.screen = SC_OPTIONS;      /* Credits lives in Options */
        break;
    }
}

static void on_key_up(SDLKey k)
{
    if (S.screen == SC_LISTEN) { listen_key_up(k); return; }
    if (S.screen == SC_EDIT) { ed_key_up(k); return; }
    int d = key_dir(k);
    if (d >= 0 && d == S.held_dir) S.held_dir = -1;
    if (k == BTN_B) S.undo_held = 0;
    if (k == BTN_A) S.redo_held = 0;
    if (k == BTN_X) { if (S.restart_since) S.dirty = 1; S.restart_since = 0; }
    if (k == BTN_Y) S.map_held = 0;
    if ((k == BTN_L1 && S.switch_dir < 0) || (k == BTN_R1 && S.switch_dir > 0)) { if (S.switch_since) S.dirty = 1; S.switch_since = 0; }
}

static void game_tick(void)
{
    Uint32 t = now_ms();
    if (S.screen == SC_GAME) barn_owl_tick(t);
    if (S.screen != SC_GAME || S.paused) return;
    if (S.move_t && t - S.move_t >= MOVE_MS) { S.move_t = 0; S.cmx = -1; S.dirty = 1; }
    if (!S.move_t && S.nqueue) {
        int d = S.queue[0];
        memmove(S.queue, S.queue + 1, (--S.nqueue) * sizeof S.queue[0]);
        if (!S.clear) { try_move(d); S.dir_next = t + REPEAT_FIRST; }
        else S.nqueue = 0;
    }
    if (S.walking && !S.move_t) {
        if (S.clear || S.iwalk >= S.nwalk) walk_end();
        else {
            int hp = P.hpos;
            try_move(S.walk[S.iwalk++]);
            if (P.hpos == hp) walk_end();          /* blocked (it shouldn't be): stop */
        }
    }
    if (S.rewind_to >= 0 && !S.move_t) {
        if (P.hpos > S.rewind_to) try_undo(0); else S.rewind_to = -1;
    }
    if (S.replay_to >= 0 && !S.move_t) {
        if (P.hpos < S.replay_to && !S.clear) { int hp = P.hpos; try_undo(1); if (P.hpos == hp) S.replay_to = -1; }
        else S.replay_to = -1;
    }
    if (S.held_dir >= 0 && !S.clear && t >= S.dir_next && !S.move_t) {
        try_move(S.held_dir);
        S.dir_next = t + REPEAT_MS;
    }
    if (S.undo_held && t >= S.undo_next && !S.move_t) { try_undo(0); S.undo_next = t + UNDO_REPEAT; }
    if (S.redo_held && t >= S.redo_next && !S.move_t && !S.clear) { try_undo(1); S.redo_next = t + UNDO_REPEAT; }
    if (S.restart_since && t - S.restart_since >= RESTART_HOLD_MS) { S.restart_since = 0; restart_level(); }
    if (S.playback && !S.move_t && t >= S.play_next) {
        if (S.clear || P.hpos >= P.hlen) S.playback = 0;
        else { try_undo(1); S.play_next = t + MOVE_MS + 110; if (S.clear) S.playback = 0; }
    }
    if (S.switch_since && t - S.switch_since >= SWITCH_HOLD_MS) {
        S.switch_since = 0;
        quick_skip(S.set, S.level, S.switch_dir);   /* this level's moves are kept for next time */
    }
    /* idle life: blink every few seconds, now and then a little fidget */
    if (t > S.blink_at + 200) S.blink_at = t + 2200 + rand() % 3500;
    if (t > S.fidget_at + 1700) { S.fidget_at = t + 7000 + rand() % 9000; S.fidget = 1 + rand() % 2; }
    if (S.save_due && t >= S.save_due) { S.save_due = 0; remember_progress(); save_all(); }
}

static SDLKey key_from_name(const char *n)
{
    static const struct { const char *n; SDLKey k; } map[] = {
        {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L1", BTN_L1}, {"R1", BTN_R1},
        {"L2", BTN_L2}, {"R2", BTN_R2}, {"SELECT", BTN_SELECT}, {"START", BTN_START}, {"MENU", BTN_MENU},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (!strcasecmp(n, map[i].n)) return map[i].k;
    return SDLK_UNKNOWN;
}

/* ======================================================================
 * main
 * ==================================================================== */

/* one pass of the loop: game logic, then a redraw when something changed or the clock says so */
static Uint32 g_last_draw;
static void frame_update(Uint32 t)
{
    game_tick();
    if (S.screen == SC_LISTEN) listen_tick();
    if (S.screen == SC_EDIT) ed_tick();
    /* ~30 fps while something slides or dances; the title scene at 20 fps; otherwise a calm
       10 fps is plenty for blinking and breathing (and kinder to the battery) */
    Uint32 interval = 100;
    if (S.screen == SC_GAME && !S.paused && (S.move_t || S.clear == 1 || S.restart_since || S.switch_since || S.playback)) interval = 33;
    else if (S.screen == SC_GAME && S.scroll) {
        float tx, ty;
        camera_target(&tx, &ty);
        if (fabsf(tx - S.camx) > 0.5f || fabsf(ty - S.camy) > 0.5f) interval = 33;
    } else if (S.screen == SC_TITLE || S.screen == SC_LISTEN) interval = S.screen == SC_LISTEN ? 33 : 50;
    else if (S.screen == SC_EDIT || S.screen == SC_CODE) interval = Ed.checking || (S.screen == SC_EDIT && t < Ed.shake_until) ? 33 : 100;
    if (S.screen == SC_GAME && (S.owl_state == OWL_ARRIVING || S.owl_state == OWL_LEAVING)) interval = 33;
    if (S.dirty || t - g_last_draw >= interval) {
        draw_screen();
        present();
        S.dirty = 0;
        g_last_draw = t;
    }
}

static void game_init(void)
{
    apply_palette();
    audio_open();
    music_set_volume(cfg.music_vol);
    sfx_set_volume(cfg.sfx_vol);
    S.screen = SC_TITLE;
    S.animal = cfg.last_animal;
    S.held_dir = -1;
    S.nqueue = 0;
    S.cmx = -1;
    S.dirty = 1;
}

#ifdef __EMSCRIPTEN__
/* ---- browser: the page sends button presses (keyboard or touch) as indexes into this table ---- */
static const SDLKey WEB_BTN[] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y,
                                  BTN_L1, BTN_R1, BTN_L2, BTN_R2, BTN_SELECT, BTN_START, BTN_MENU };
static struct { unsigned char btn, down; } g_webq[64];
static int g_webq_n;

EMSCRIPTEN_KEEPALIVE void web_key(int btn, int down)
{
    if (btn < 0 || btn >= (int)(sizeof WEB_BTN / sizeof WEB_BTN[0]) || g_webq_n >= 64) return;
    g_webq[g_webq_n].btn = (unsigned char)btn;
    g_webq[g_webq_n].down = (unsigned char)(down != 0);
    g_webq_n++;
}

/* Keyboard: keys name actions (Enter = choose, Esc = back or pause, Z = undo, ...). Each key turns
   into whichever button does that action on the current screen, and remembers it for the key-up. */
enum { KA_UP, KA_DOWN, KA_LEFT, KA_RIGHT, KA_OK, KA_ESC, KA_BACK, KA_UNDO, KA_REDO, KA_RESTART, KA_MAP,
       KA_ZOOM_OUT, KA_ZOOM_IN, KA_PREV, KA_NEXT, KA_PAUSE, KA_LEVELS, KA_CLEAR,
       KA_D0, KA_D1, KA_D2, KA_D3, KA_D4, KA_D5, KA_D6, KA_D7, KA_D8, KA_D9, KA_RIM, KA_NEW, KA_KX, KA_KC, KA_KV, KA_PGUP, KA_PGDN, KA_TEST, KA_NEXTU, NUM_KA };
static int g_ka_btn[NUM_KA];          /* WEB_BTN index pressed by each held action, or -1 */

static int ka_resolve(int a)
{
    enum { UP, DOWN, LEFT, RIGHT, A, B, X, Y, L1, R1, L2, R2, SELECT, START };
    if (a <= KA_RIGHT) return a;
    int play = S.screen == SC_GAME && !S.paused && !S.clear && !S.confirm;   /* walking around a level */
    if (S.confirm == CF_RESUME && now_ms() < S.quick_until && (a == KA_PREV || a == KA_NEXT)) return a == KA_PREV ? L1 : R1;
    if (S.confirm) return a == KA_OK ? A : (a == KA_ESC || a == KA_BACK) ? B : -1;
    if (S.screen == SC_EDIT) {
        if (Ed.menu || Ed.clearing || Ed.checking || Ed.result) return a == KA_OK ? A : (a == KA_ESC || a == KA_BACK) ? B : -1;
        switch (a) {
        case KA_OK: return A;                       /* Space or Enter paints (hold it and move: a line) */
        case KA_BACK: case KA_CLEAR: return B;      /* Backspace / Delete erase */
        case KA_UNDO: return SELECT;
        case KA_PREV: return L1;
        case KA_NEXT: return R1;
        case KA_ZOOM_IN: case KA_ZOOM_OUT: return X;
        case KA_TEST: return Y;                     /* T: test play */
        case KA_REDO: case KA_KX: return R2;        /* Y, X or Shift+Z: redo */
        case KA_ESC: case KA_PAUSE: return START;
        }
        return -1;
    }
    if (S.screen == SC_CODE) {
        if (Cd.enter) return a == KA_OK ? A : a == KA_ESC ? B : -1;   /* typing goes through web_code_char */
        return a == KA_OK ? A : (a == KA_ESC || a == KA_BACK) ? B : (a == KA_LEVELS || a == KA_KX) ? X : a == KA_REDO ? Y : -1;   /* L (or X): copy a link */
    }
    if (S.screen == SC_LISTEN) {
        if (L.menu) return a == KA_OK ? A : (a == KA_ESC || a == KA_BACK) ? B : a == KA_PAUSE ? START : -1;
        /* the number keys (top row or ten-key) are handled in web_kbd: Melody 1-8 notes, Drums 1-8 hits.
           Arrows (two at once for the in-between notes) are the D-pad, Z X C V are A B X Y,
           9 0 - = hold the effects, M or Tab switches mode, Esc is the menu. */
        switch (a) {
        case KA_UNDO: return A;
        case KA_KX: return B;
        case KA_KC: return X;
        case KA_KV: return Y;
        case KA_D9: return L1;
        case KA_D0: return R1;
        case KA_ZOOM_OUT: return L2;
        case KA_ZOOM_IN: return R2;
        case KA_MAP: return SELECT;
        case KA_ESC: case KA_PAUSE: return START;
        }
        return -1;
    }
    if (S.screen == SC_GAME && S.clear == 1) return -1;                       /* the dance */
    if (S.screen == SC_GAME && S.clear == 2)
        return a == KA_OK ? A : (a == KA_UNDO || a == KA_BACK) ? B : (a == KA_ESC || a == KA_LEVELS) ? Y : -1;
    switch (a) {
    case KA_OK: return play ? -1 : A;
    case KA_ESC: return play ? START : B;
    case KA_BACK: return B;
    case KA_PAUSE: return S.screen == SC_GAME ? START : -1;
    case KA_UNDO: return play ? B : -1;
    case KA_REDO: case KA_KX: return play ? A : -1;                 /* Y or X: redo */
    case KA_RESTART: return play ? X : -1;
    case KA_MAP: return play ? Y : S.screen == SC_LEVELS || S.screen == SC_SETS ? SELECT : -1;   /* M: map, or the list's menu */
    case KA_ZOOM_OUT: return play ? L2 : -1;
    case KA_ZOOM_IN: return play ? R2 : -1;
    case KA_PREV: return play || S.screen == SC_LEVELS ? L1 : -1;
    case KA_NEXT: return play || S.screen == SC_LEVELS ? R1 : -1;
    case KA_NEXTU: return S.screen == SC_LEVELS ? X : -1;              /* N: the next unsolved level */
    case KA_PGUP: return S.screen == SC_LEVELS ? L1 : -1;               /* lists: page up / down */
    case KA_PGDN: return S.screen == SC_LEVELS ? R1 : -1;
    case KA_LEVELS: return play ? SELECT : -1;
    case KA_CLEAR: return S.screen == SC_LEVELS || S.screen == SC_SETS ? SELECT : -1;
    }
    return -1;
}

EMSCRIPTEN_KEEPALIVE void web_kbd(int a, int down)
{
    if (a < 0 || a >= NUM_KA) return;
    if (down && S.screen == SC_EDIT && !Ed.menu && !Ed.clearing && !Ed.checking && a >= KA_D1 && a < KA_D1 + NTOOLS) {
        Ed.tool = a - KA_D1; Ed.help_until = 0; S.dirty = 1; sfx_play(SFX_MENU);
        return;
    }
    if (S.screen == SC_LISTEN && !L.menu && !L.asleep && a >= KA_D1 && a <= KA_D8) {
        static const int KEY_DRUM[8] = { DR_KICK, DR_SNARE, DR_SHAKER, DR_CLAP, DR_RIM, DR_LOTOM, DR_BLOCK, DR_HITOM };
        int n = a - KA_D1;
        S.last_input = now_ms(); S.dirty = 1;
        if (L.pmode == 1) { if (down) music_drum(KEY_DRUM[n]); }
        else if (down) { L.kdeg = n; listen_update_note(); }               /* 1 = the chord's root ... 8 = its octave */
        else if (L.kdeg == n) { L.kdeg = -1; listen_update_note(); }
        return;
    }
    if (down) { g_ka_btn[a] = ka_resolve(a); if (g_ka_btn[a] >= 0) web_key(g_ka_btn[a], 1); }
    else if (g_ka_btn[a] >= 0) { web_key(g_ka_btn[a], 0); g_ka_btn[a] = -1; }
}

/* ---- touch gestures (the page works out taps, swipes, two-finger taps, long presses and pinches) ---- */
static void web_press(SDLKey k) { on_key(k); on_key_up(k); }
static int in_play(void) { return S.screen == SC_GAME && !S.paused && !S.clear && !S.confirm; }

/* in a level, a tap (or a click) on a square: right next to you, one step (pushing what's there);
   further away, walk there the shortest way without pushing anything */
static void tap_walk(int x, int y)
{
    if (S.ts <= 0) return;
    int fx = x - S.ox, fy = y - S.oy;
    int tx = fx >= 0 ? fx / S.ts : -1 - (-fx - 1) / S.ts, ty = fy >= 0 ? fy / S.ts : -1 - (-fy - 1) / S.ts;
    if (S.walking && tx == P.px && ty == P.py) { walk_end(); return; }   /* tap yourself: stop */
    int dx = tx - P.px, dy = ty - P.py;
    if (!dx && !dy) return;
    if (abs(dx) + abs(dy) == 1) {
        web_press(dx < 0 ? BTN_LEFT : dx > 0 ? BTN_RIGHT : dy < 0 ? BTN_UP : BTN_DOWN);
        return;
    }
    S.held_dir = -1; S.nqueue = 0;
    if (!walk_to(tx, ty)) {
        if (S.walking) walk_end();
        if (!S.move_t) {              /* can't get there: a little bump the way you tapped */
            if (abs(dx) > abs(dy)) S.facing = dx < 0;
            sfx_play(SFX_BUMP);
        }
    }
    music_activity();
}

/* a mouse click on the game with the keyboard or on-screen buttons in use: walking only */
EMSCRIPTEN_KEEPALIVE void web_click(int x, int y)
{
    S.last_input = now_ms(); S.dirty = 1;
    if (in_play()) tap_walk(x, y);
}

/* a tap at (x, y) in screen pixels */
EMSCRIPTEN_KEEPALIVE void web_tap(int x, int y)
{
    S.last_input = now_ms(); S.dirty = 1;
    for (int i = g_ntaps - 1; i >= 0; i--) {
        Tap *t = &g_taps[i];
        if (x < t->x || y < t->y || x >= t->x + t->w || y >= t->y + t->h) continue;
        if (t->kind == TAP_NONE) return;
        if (t->kind == TAP_BTN) web_press((SDLKey)t->val);
        else if (t->kind == TAP_BACK) { if (S.screen == SC_CODE) code_back(); else web_press(BTN_B); }
        else if (t->kind == TAP_MENU) { if (t->sel) *t->sel = t->val; web_press(BTN_SELECT); }
        else if (t->kind == TAP_PICK) { if (t->sel) *t->sel = t->val; web_press(BTN_A); }
        else if (t->kind == TAP_PICK2) { if (*t->sel != t->val) { *t->sel = t->val; sfx_play(SFX_MENU); } else web_press(BTN_A); }
        else if (*t->sel != t->val) { *t->sel = t->val; sfx_play(SFX_MENU); }
        else web_press(t->val >= 4 ? BTN_A : x < 360 ? BTN_LEFT : BTN_RIGHT);
        return;
    }
    if (in_play()) tap_walk(x, y);
    else if (S.screen == SC_GAME && S.clear == 2) web_press(BTN_A);
    else if (S.screen == SC_CREDITS) web_press(BTN_B);
    else if (S.screen == SC_LISTEN && !L.menu && L.asleep) listen_key(BTN_B);   /* wake up */
}

/* a swipe starts (down = 1) or the finger lifts (0); dir 0 up, 1 down, 2 left, 3 right.
   Walking: the step keeps repeating while the finger stays down, like holding the D-pad. */
static int g_swipe_key = -1;
EMSCRIPTEN_KEEPALIVE void web_swipe(int dir, int down)
{
    static const SDLKey D[4] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
    S.last_input = now_ms(); S.dirty = 1;
    if (!down) { if (g_swipe_key >= 0) on_key_up((SDLKey)g_swipe_key); g_swipe_key = -1; return; }
    if (g_swipe_key >= 0) on_key_up((SDLKey)g_swipe_key);
    int k = -1;
    if (in_play()) k = D[dir];
    else if (S.screen == SC_LISTEN && !L.menu) k = L.asleep ? BTN_B : -1;   /* playing goes through web_ltouch */
    /* menus don't swipe: BACK, MENU and the page buttons are drawn on the screen */
    g_swipe_key = k;
    if (k >= 0) on_key((SDLKey)k);
}

/* kind 0: two-finger tap, 1: long press (on/off), 2: pinch in, 3: pinch out */
EMSCRIPTEN_KEEPALIVE void web_gesture(int kind, int x, int y, int on)
{
    S.last_input = now_ms(); S.dirty = 1;
    if (kind == 0) { if (in_play() || (S.screen == SC_GAME && S.clear == 2)) web_press(BTN_B); }
    else if (kind == 1) {
        if (in_play() || (S.screen == SC_GAME && S.map_held)) { if (on) on_key(BTN_Y); else on_key_up(BTN_Y); }
    } else if (in_play()) web_press(kind == 2 ? BTN_L2 : BTN_R2);
}

/* Listen, while playing: the page sends each finger straight here instead of working out gestures */
EMSCRIPTEN_KEEPALIVE int web_listen_raw(void)
{
    if (S.screen == SC_EDIT) return !Ed.menu && !Ed.clearing && !Ed.checking && !Ed.result;      /* the level maker paints with every touch too */
    return S.screen == SC_LISTEN && !L.menu && !L.asleep && !S.confirm;
}
/* the on-screen D-pad reports diagonals too (Melody's in-between notes) */
EMSCRIPTEN_KEEPALIVE int web_dpad8(void) { return S.screen == SC_LISTEN && !L.menu && L.pmode == 0; }

/* phase 0 down, 1 move, 2 up; x, y in screen pixels */
EMSCRIPTEN_KEEPALIVE void web_ltouch(int id, int x, int y, int phase)
{
    S.last_input = now_ms(); S.dirty = 1;
    if (S.screen == SC_EDIT) {
        if (phase == 0 && web_listen_raw())
            for (int i = g_ntaps - 1; i >= 0; i--) {          /* undo / zoom / test / menu */
                Tap *t = &g_taps[i];
                if (t->kind == TAP_BTN && x >= t->x && y >= t->y && x < t->x + t->w && y < t->y + t->h) { web_press((SDLKey)t->val); return; }
            }
        ed_touch(id, x, y, phase);
        return;
    }
    if (S.screen != SC_LISTEN) return;
    int slot = -1;
    for (int i = 0; i < 5; i++) if (L.tkind[i] && L.tid[i] == id) slot = i;
    if (phase == 0) {
        if (!web_listen_raw()) return;
        if (slot < 0) for (int i = 0; i < 5; i++) if (!L.tkind[i]) { slot = i; break; }
        if (slot < 0) return;
        L.tid[slot] = id; L.tkind[slot] = 3;
        for (int i = g_ntaps - 1; i >= 0; i--) {          /* the menu and mode buttons */
            Tap *t = &g_taps[i];
            if (t->kind == TAP_BTN && x >= t->x && y >= t->y && x < t->x + t->w && y < t->y + t->h) { web_press((SDLKey)t->val); return; }
        }
        if (L.pmode == 1) {
            int p = lt_pad_at(x, y);
            if (p >= 0) { music_drum(LT_PADS[p]); L.pad_flash[p] = now_ms(); }
            return;
        }
        int m = lt_mod_at(x, y);
        if (m == 3) { listen_voice(); return; }
        if (m >= 0) { L.tkind[slot] = 2; L.tval[slot] = m; listen_mod(m, 1); return; }
        L.tkind[slot] = 1; L.tval[slot] = L.tdeg = lt_deg_at(y);
        listen_update_note();
        return;
    }
    if (slot < 0) return;
    if (phase == 1) {
        if (L.tkind[slot] == 1) {
            int d = lt_deg_at(y);
            if (d != L.tval[slot]) { L.tval[slot] = L.tdeg = d; listen_update_note(); }
        }
        return;
    }
    int kind = L.tkind[slot];
    L.tkind[slot] = 0;
    if (kind == 1) {                                      /* back to another finger still down, if any */
        L.tdeg = -1;
        for (int i = 0; i < 5; i++) if (L.tkind[i] == 1) L.tdeg = L.tval[i];
        listen_update_note();
    } else if (kind == 2) listen_mod(L.tval[slot], 0);
}

/* typing a code: the page sends letters and digits straight here (and pasted text) */
EMSCRIPTEN_KEEPALIVE int web_text_mode(void) { return S.screen == SC_CODE && Cd.enter && !S.confirm; }
EMSCRIPTEN_KEEPALIVE void web_code_char(int c) { code_char(c); }
/* a link with a code after the # : open it as if it were typed in (start = 1 begins, then chars, then 13) */
EMSCRIPTEN_KEEPALIVE void web_code_link(int start) { if (start) code_enter(); }

/* which prompts to show: 1 = keyboard, 0 = buttons, 2 = touch */
EMSCRIPTEN_KEEPALIVE void web_input_mode(int kbd)
{
    if (kbd != g_kbd) { g_kbd = kbd; S.dirty = 1; }
}

/* the page is being hidden or closed: keep the exact position, like leaving the game on the device */
EMSCRIPTEN_KEEPALIVE void web_save(void)
{
    if (S.screen == SC_GAME) remember_progress();
    save_all();
}

/* which buttons do something right now, as bits in WEB_BTN order; the page dims the others */
enum { WB_UP = 1, WB_DOWN = 2, WB_LEFT = 4, WB_RIGHT = 8, WB_A = 16, WB_B = 32, WB_X = 64, WB_Y = 128,
       WB_L1 = 256, WB_R1 = 512, WB_L2 = 1024, WB_R2 = 2048, WB_SELECT = 4096, WB_START = 8192 };
#define WB_DPAD (WB_UP | WB_DOWN | WB_LEFT | WB_RIGHT)
static int web_buttons(void)
{
    if (S.confirm) return WB_UP | WB_DOWN | WB_A | WB_B | (S.confirm == CF_RESUME && now_ms() < S.quick_until ? WB_L1 | WB_R1 : 0);
    switch (S.screen) {
    case SC_TITLE: return WB_UP | WB_DOWN | WB_A | WB_START;
    case SC_SETS: return WB_UP | WB_DOWN | WB_A | WB_B | (set_has_menu(S.sets_sel) ? WB_SELECT : 0);
    case SC_LEVELS: return WB_DPAD | WB_A | WB_B | WB_L1 | WB_R1 | WB_SELECT | WB_START | (S.sets_sel != g_my ? WB_X : 0);
    case SC_OPTIONS: return WB_UP | WB_DOWN | WB_A | WB_B | (S.opt_sel <= 3 ? WB_LEFT | WB_RIGHT : 0);
    case SC_CREDITS: return WB_A | WB_B;
    case SC_EDIT:
        if (Ed.checking) return WB_B;
        if (Ed.menu || Ed.clearing || Ed.result) return WB_UP | WB_DOWN | WB_A | WB_B | WB_START;
        return WB_DPAD | WB_A | WB_B | WB_X | WB_Y | WB_L1 | WB_R1 | WB_L2 | WB_R2 | WB_SELECT | WB_START;
    case SC_CODE: return Cd.enter ? WB_DPAD | WB_A | WB_B | WB_START : WB_A | WB_B | WB_X | WB_Y | WB_UP | WB_DOWN;
    case SC_LISTEN:
        if (L.menu && !L.asleep) return WB_UP | WB_DOWN | WB_A | WB_B | WB_START | (L.menu_sel == 2 ? WB_LEFT | WB_RIGHT : 0);
        return 0x3FFF;
    case SC_GAME:
        if (S.paused) return WB_UP | WB_DOWN | WB_A | WB_B | WB_START | (pause_id() == PI_ZOOM || pause_id() == PI_FAV ? WB_LEFT | WB_RIGHT : 0);
        if (S.clear == 1) return 0;
        if (S.clear == 2) return WB_A | WB_B | WB_Y;
        {
            int m = WB_DPAD | WB_B | WB_A | WB_X | WB_Y | WB_SELECT | WB_START;
            if (nav_pos() > 0) m |= WB_L1;
            if (nav_pos() + 1 < nav_count()) m |= WB_R1;
            if (cfg.zoom > 0) m |= WB_L2;
            if (cfg.zoom < 4 - S.fit_scale) m |= WB_R2;
            return m;
        }
    }
    return 0;
}

static void web_frame(void)
{
    Uint32 t = now_ms();
    for (int i = 0; i < g_webq_n; i++) {
        SDLKey k = WEB_BTN[g_webq[i].btn];
        if (g_webq[i].down) { S.last_input = t; on_key(k); }
        else on_key_up(k);
    }
    g_webq_n = 0;
    frame_update(t);
    static int shown = -1;
    int m = web_buttons();
    if (m != shown) { shown = m; EM_ASM({ if (Module.stumpedButtons) Module.stumpedButtons($0); }, m); }
}

int main(void)
{
    srand((unsigned)time(NULL));
    for (int i = 0; i < NUM_KA; i++) g_ka_btn[i] = -1;
    EM_ASM({ if (Module.stumpedLoad) Module.stumpedLoad(); });   /* localStorage -> save.txt */
    EM_ASM({ if (Module.stumpedLoadSets) Module.stumpedLoadSets(); });   /* level sets you've added */
    load_sets();
    load_save();
    game_init();
    if (g_save_restored) toast("Your save was damaged: the backup is back");
    emscripten_set_main_loop(web_frame, 0, 1);
    return 0;
}

#else
int main(int argc, char **argv)
{
    const char *keys_arg = NULL, *shot = NULL, *wav = NULL, *rooms_out = NULL;
    int rotate_arg = -1, start_set = -1, start_lv = 0, wav_secs = 60, pal_arg = -1, animal_arg = -2, delay_ms = 250;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys_arg = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) { sscanf(argv[++i], "%d:%d", &start_set, &start_lv); start_set--; start_lv--; }
        else if (!strcmp(argv[i], "--palette") && i + 1 < argc) pal_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--animal") && i + 1 < argc) animal_arg = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--delay") && i + 1 < argc) delay_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 2 < argc) { wav = argv[++i]; wav_secs = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--wav-listen") && i + 2 < argc) { wav = argv[++i]; wav_secs = atoi(argv[++i]); g_wav_listen = 1; }
        else if (!strcmp(argv[i], "--rooms") && i + 1 < argc) rooms_out = argv[++i];
        else if (!strcmp(argv[i], "--rotate")) rotate_arg = 1;
        else if (!strcmp(argv[i], "--no-rotate")) rotate_arg = 0;
    }
    srand((unsigned)time(NULL));
    if (wav) { audio_render_wav(wav, wav_secs); return 0; }

    g_on_device = dir_exists("/mnt/SDCARD") && dir_exists("/sys/class/pwm/pwmchip0");
    g_rotate = rotate_arg >= 0 ? rotate_arg : g_on_device;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);

    load_sets();
    load_save();
    if (pal_arg >= 0) cfg.palette = clampi(pal_arg, 0, NUM_PALETTES - 1);
    if (animal_arg >= -1) cfg.animal = clampi(animal_arg, -1, NUM_ANIMALS - 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_WM_SetCaption(APP_NAME, APP_NAME);
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    if (!video) { fprintf(stderr, "SetVideoMode: %s\n", SDL_GetError()); return 1; }
    scr = SDL_CreateRGBSurfaceFrom(g_fb, SCREEN_W, SCREEN_H, 32, SCREEN_W * 4, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (video->format->BitsPerPixel != 32)
        rotbuf = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, SCREEN_H, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    if (!scr) { fprintf(stderr, "surface: %s\n", SDL_GetError()); return 1; }

    game_init();
    if (g_save_restored) toast("Your save was damaged: the backup is back");
    if (rooms_out) {          /* a sheet of nine made-up Listen clearings, for checking the generator */
        rect(0, 0, SCREEN_W, SCREEN_H, BG3);
        for (int i = 0; i < 9; i++) {
            listen_new_room();
            int ox = (i % 3) * 213, oy = (i / 3) * 160;
            set_clip(ox, oy, 208, 160);
            draw_world(&L.room, ui_set(1, 0), ox, oy, ox, oy, 208, 160, L.seed);
            for (int k = 0; k < L.nac; k++) blit(ui_set(1, 0)->tile[TL_ACORN_GOAL], ox + L.acx[k] * 16, oy + L.acy[k] * 16);
            for (int a = 0; a < NUM_ANIMALS; a++) blit(ui_set(1, a)->anim[FR_IDLE][0], ox + L.ax[a] * 16, oy + L.ay[a] * 16);
            if (L.barn) blit(ui_set(1, 0)->tile[TL_BARN], ox + L.bx * 16, oy + L.by * 16 - 9);
            if (L.owlet) blit(ui_set(1, 0)->tile[TL_OWLET], ox + L.olx * 16, oy + L.oly * 16);
            no_clip();
        }
        SDL_SaveBMP(scr, rooms_out);
        return 0;
    }
    if (start_set >= 0 && start_set < g_nsets) start_level(start_set, start_lv, NULL);

    char *keybuf = keys_arg ? strdup(keys_arg) : NULL;
    char *keytok = keybuf ? strtok(keybuf, " ,") : NULL;
    Uint32 next_key = now_ms() + 300, script_done_at = 0;

    while (!g_quit) {
        Uint32 t = now_ms();
        SDL_Event ev;
        if (keybuf && t >= next_key) {
            if (keytok) {
                if (!strncasecmp(keytok, "WAIT", 4)) {
                    next_key = t + (keytok[4] ? atoi(keytok + 4) : delay_ms);
                } else {
                    int hold = !strncasecmp(keytok, "PRESS:", 6), rel = !strncasecmp(keytok, "RELEASE:", 8);
                    SDL_Event e;
                    memset(&e, 0, sizeof e);
                    e.key.keysym.sym = key_from_name(hold || rel ? strchr(keytok, ':') + 1 : keytok);
                    e.type = rel ? SDL_KEYUP : SDL_KEYDOWN;
                    SDL_PushEvent(&e);
                    if (!hold && !rel) { e.type = SDL_KEYUP; SDL_PushEvent(&e); }
                    next_key = t + delay_ms;
                }
                keytok = strtok(NULL, " ,");
            } else if (!script_done_at) script_done_at = t + 400;
        }
        if (script_done_at && t >= script_done_at) {
            draw_screen();
            if (shot) SDL_SaveBMP(scr, shot);
            break;
        }
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) g_quit = 1;
            else if (ev.type == SDL_KEYDOWN) { S.last_input = t; on_key(ev.key.keysym.sym); }
            else if (ev.type == SDL_KEYUP) on_key_up(ev.key.keysym.sym);
        }
        frame_update(t);
        SDL_Delay(S.move_t || S.held_dir >= 0 ? 4 : 10);
    }

    if (S.screen == SC_GAME) remember_progress();
    save_all();
    sync();
    free(keybuf);
    SDL_CloseAudio();
    SDL_Quit();
    return 0;
}
#endif
