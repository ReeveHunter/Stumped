/*
 * Stumped — audio (included from main.c)
 *
 * Everything runs on a slow, hidden pulse (70 BPM, 4/4, sixteenth-note steps).
 *
 * Pads: five soft 4-bit "wave channel" voices plus a bass, voice-led from chord to chord with slow
 *   glides, a little tape wow, a warp as each chord slides in, and per-voice warble in pitch AND
 *   volume. Each voice's pan wanders slowly. The low-pass opens as more acorns reach their spots.
 * Chords wander a graph of related chords and change on bar lines. About every minute and a half
 *   the walk takes a colourful turn: a borrowed chord (bVII, iv, bVI, Lydian IV) or a key change.
 * Plinks & runs belong to the player: an acorn landing on its spot plays a run up the chord, a blocked
 *   step plinks, menus plink and selections run, undo sings a step down and redo a step up.
 *   Every note gets its own wandering pan and goes through a tempo-synced ping-pong echo.
 *   A rare idle plink keeps things from going completely still.
 * Rhythm: acorns on their spots bring in a rhythm section — first a sparse walking bass (it knows
 *   the next chord a bar ahead and leads into it), then a kick with the pads articulating (short lifts
 *   off the keys before beats, with a medium release), a brushy snare, a shaker, and finally a ghost kick
 *   and rim ticks. They fade out as acorns leave.
 * Breathing: every 6-14 bars the pads thin out to two voices, or go very quiet, for two bars.
 * Double time: with the full groove in and the player on a roll, the drums and bass switch to a
 *   double-time feel for four bars (at most once every 40 s).
 * Idle wind-down: after 20 s with no moves, drums and bass fade out over ~12 s; any move brings them back.
 * Clearing a level plays a longer run and the music moves to a fresh chord (often a new key).
 * Underneath: faint vinyl crackle.
 * Listen mode (the title menu's "Listen"): nobody is solving anything, so the music runs its own slow arc,
 *   bringing the rhythm section in and out over a few minutes, with double-time stretches and new keys.
 *   You play along in two modes: Melody (a sustained lead on the D-pad, with an octave swoop, vibrato,
 *   arpeggio and four voices) or Drums (eight hits; the band's drums duck while you play), plus held
 *   effects on the shoulder buttons (chopped pads, heavy warble, rain, double time). A sleep timer
 *   can fade it all out.
 */

#define TWO_PI 6.28318530718f
/* use the long-standing double-precision libm entry points: the float ones need glibc 2.27 */
#define exp2f(x) ((float)exp2((double)(x)))
#define expf(x)  ((float)exp((double)(x)))
#define powf(x, y) ((float)pow((double)(x), (double)(y)))
#define CTRL_BLOCK 32
#define NPAD 5
#define NPLINK 16
#define NSFX 16
#define DELAY_LEN 48000
#define BPM 70.0f
#define STEP_SEC (60.0f / BPM / 4.0f)     /* a sixteenth note */

static float frand01(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return (*s >> 8) * (1.0f / 16777216.0f); }
static float frandpm(uint32_t *s) { return frand01(s) * 2.0f - 1.0f; }
static float midi_hz(float m) { return 440.0f * exp2f((m - 69.0f) / 12.0f); }

/* ---------------- chord graph ---------------- */

typedef struct {
    const char *name;
    int tones[5];   /* semitones above the key's tonic */
    int nt;
    int bass;       /* chord root, semitones above tonic */
    int scale[7];   /* scale for runs and undo/redo steps */
    int special;    /* a colourful borrowed chord: only reached occasionally */
} Chord;

#define IONIAN  { 0, 2, 4, 5, 7, 9, 11 }
#define MIXO    { 0, 2, 4, 5, 7, 9, 10 }
#define AEOL    { 0, 2, 3, 5, 7, 8, 10 }
#define LYDIAN  { 0, 2, 4, 6, 7, 9, 11 }

enum { CH_I, CH_ii, CH_iii, CH_IV, CH_V, CH_vi, CH_bVII, CH_iv, CH_bVI, CH_IVlyd, CH_Isus, NCHORDS };
static const Chord CHORDS[NCHORDS] = {
    [CH_I]     = { "Imaj9",     { 0, 4, 7, 11, 14 }, 5, 0,  IONIAN, 0 },
    [CH_ii]    = { "ii9",       { 2, 5, 9, 12, 16 }, 5, 2,  IONIAN, 0 },
    [CH_iii]   = { "iii7",      { 4, 7, 11, 14, 0 }, 4, 4,  IONIAN, 0 },
    [CH_IV]    = { "IVmaj9",    { 5, 9, 12, 16, 19 }, 5, 5, IONIAN, 0 },
    [CH_V]     = { "V9sus",     { 7, 12, 14, 17, 21 }, 5, 7, IONIAN, 0 },
    [CH_vi]    = { "vi9",       { 9, 12, 16, 19, 23 }, 5, 9, IONIAN, 0 },
    [CH_bVII]  = { "bVIImaj7",  { 10, 14, 17, 21, 0 }, 4, 10, MIXO, 1 },
    [CH_iv]    = { "iv6",       { 5, 8, 12, 14, 0 }, 4, 5,  AEOL, 1 },
    [CH_bVI]   = { "bVImaj7",   { 8, 12, 15, 19, 0 }, 4, 8, AEOL, 1 },
    [CH_IVlyd] = { "IVmaj7#11", { 5, 9, 12, 16, 23 }, 5, 5, IONIAN, 1 },   /* Lydian on IV is the key's own scale */
    [CH_Isus]  = { "Isus2",     { 0, 7, 14, 16, 19 }, 5, 0, IONIAN, 0 },
};

typedef struct { int to; float w; } Edge;
static const Edge GRAPH[NCHORDS][7] = {
    [CH_I]     = { {CH_IV, 3}, {CH_vi, 3}, {CH_ii, 2}, {CH_iii, 1}, {CH_Isus, 1}, {CH_bVII, 1}, {CH_bVI, .6f} },
    [CH_ii]    = { {CH_V, 3}, {CH_IV, 1}, {CH_vi, 1.5f}, {CH_iii, 1}, {CH_I, 1}, {-1, 0} },
    [CH_iii]   = { {CH_vi, 3}, {CH_IV, 2}, {CH_ii, 1}, {CH_IVlyd, .8f}, {-1, 0} },
    [CH_IV]    = { {CH_I, 3}, {CH_ii, 1}, {CH_V, 1}, {CH_vi, 1.5f}, {CH_iv, 1}, {CH_IVlyd, .7f}, {-1, 0} },
    [CH_V]     = { {CH_I, 3}, {CH_vi, 2}, {CH_IV, 1}, {CH_Isus, 1}, {-1, 0} },
    [CH_vi]    = { {CH_IV, 3}, {CH_ii, 2}, {CH_iii, 1}, {CH_V, 1}, {CH_I, 1}, {CH_bVI, .5f}, {-1, 0} },
    [CH_bVII]  = { {CH_I, 2}, {CH_IV, 2}, {CH_Isus, 1}, {-1, 0} },
    [CH_iv]    = { {CH_I, 3}, {CH_Isus, 1}, {-1, 0} },
    [CH_bVI]   = { {CH_bVII, 2}, {CH_I, 1.5f}, {CH_iv, .8f}, {-1, 0} },
    [CH_IVlyd] = { {CH_I, 2}, {CH_iii, 1}, {CH_vi, 1}, {-1, 0} },
    [CH_Isus]  = { {CH_IV, 2}, {CH_vi, 2}, {CH_I, 1}, {CH_ii, 1}, {-1, 0} },
};

/* ---------------- voices ---------------- */

typedef struct {
    float pitch, from, to;      /* MIDI note numbers (fractional while gliding) */
    float gt, glen;             /* glide time / length (s) */
    float phase, amp;
    float vib, vib_rate, detune;
    float trem, trem_rate;      /* volume warble */
    float pan, pan_to, pan_t;   /* wandering pan */
    float lp;
} Pad;

typedef struct {
    int on;
    float phase, freq, env, decay, duty, pan, pan_drift, lp, start_in;
} Plink;

typedef struct {               /* one sound-effect note (thuds, ticks, drums) */
    int on, wave;              /* 0 square, 1 triangle, 2 noise, 3 sine */
    float t, dur, f0, f1, vol, duty, delay, phase, send, pan, hp;
    uint32_t nseed; float nval, ncount, lp, hpz;
    int mus;                   /* part of the music (drums): follows the music volume */
} SfxNote;

static struct {
    uint32_t rng;
    int key, home;
    int chord;
    int bars_left;              /* chord changes on a bar line when this hits 0 */
    int force_change;           /* level cleared: move to a fresh chord at the next bar */
    int changes_since_special;
    int bars_since_key;
    Pad pad[NPAD], bass;
    float wow_phase, flutter_phase, warp, warp_vel;
    float pad_lp_phase;
    float bright, bright_target;           /* 0..1: filter opening with progress */
    float layer_gain[4], layer_target[4];  /* drum layers: kick, snare, shaker, ghost kick + rim */
    float wb_gain, wb_target;              /* walking bass */
    float pump, pump_target;               /* how much the pads "lift off the keys" between beats (0..1) */
    float gate, gate_target;               /* pad articulation envelope: 1 = held, lower = released */
    float vmul[NPAD], vmul_target[NPAD];   /* breathing: per-voice level */
    int breath_left, breath_wait;          /* bars left in a breath / bars until the next one */
    float act;                             /* recent player activity (moves, decays over a few seconds) */
    float idle_s;                          /* seconds since the player last did anything */
    float wind, wind_target;               /* idle wind-down: 1 = drums and bass have faded out */
    int dbl_left;                          /* bars of double-time feel left */
    float since_dbl;                       /* seconds since the last double-time stretch */
    int pend_chord, pend_key, has_pend;    /* the next chord, decided a bar early so the bass can lead into it */
    int wb_pat[16];                        /* this bar's bass rhythm */
    float wb_last;                         /* last bass note (MIDI) */
    struct { float phase, freq, env, decay, lp; } wb;
    Plink pl[NPLINK];
    float idle_plink;                      /* seconds until a rare idle plink */
    /* tempo */
    float step_t; int step;                /* time into the current sixteenth, step 0..15 */
    /* queued notes (runs): played on a 32nd-note grid from now */
    float qtime[24]; float qnote[24]; float qvol[24]; int nq;
    float dl[DELAY_LEN], dr[DELAY_LEN]; int dpos; float dlp_l, dlp_r;
    int dlen_l, dlen_r;
    float comb[8][1700]; int clen[8], cpos[8]; float cfilt[8];
    float ap[4][600]; int alen[4], apos[4];
    float hiss_lp, pop, pop_lp, next_pop;
    float music_gain, music_target, sfx_gain;
    SfxNote sfx[NSFX];
    float time;
    /* listen mode */
    int listen, l_stage, l_bars;           /* its own arc: the current stage (0..5) and bars until it moves */
    int hold_gate, hold_dbl;               /* buttons held: chop the pads / double time */
    float warble, warble_target;           /* held: heavy pitch & volume warble (0..1) */
    float rain, rain_target;               /* held: rain (0..1): a big wash of reverb with rain falling */
    float sleep_gain;                      /* sleep timer fade (1 = full) */
    int sleeping, sleep_bars;              /* the timer ran out: the beat thins out, a layer every two bars */
    float rain_lp, rain_hp, drip_t;        /* the rain's hiss and drips */
    /* listen, Melody mode: one sustained lead voice played from the D-pad */
    struct {
        int deg;                           /* scale step held (0 = the chord's root .. 7 = its octave), -1 = none */
        int voice;                         /* LV_* */
        float pitch, target, glide;        /* MIDI; glide = time constant (s) */
        float env, env_target;
        float phase, phase2, lp;
        float oct, oct_target;             /* held: glides up an octave */
        float vib, vib_target, vib_ph;     /* held: vibrato */
        int arp, arp_i, arp_tick;          /* held: arpeggio from the note being played */
        float pluck, bell;                 /* re-strike accents (arpeggio notes, bell) */
        int seen_chord, seen_key;
    } ld;
    float duck, duck_t;                    /* listen, Drums mode: your drumming ducks the band's drums */
} M;

/* when each part last played (now_ms clock), so the listening animals can dance to them:
   0 kick, 1 snare, 2 shaker, 3 rim, 4 bass note */
static volatile Uint32 g_hit_ms[5];
static volatile Uint32 g_section_ms;    /* listen mode: a new section or key just started */
static volatile int g_sleep_done;       /* the sleep timer has run out and the last drum has faded */

static int nearest_free(float cur, const int *pcs, int npc, int key, int lo, int hi, const int *taken, int ntaken)
{
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < npc; i++)
        for (int n = lo; n <= hi; n++) {
            if (((n - 50 - key - pcs[i]) % 12 + 12) % 12) continue;   /* D3 = 50 */
            int dup = 0;
            for (int k = 0; k < ntaken; k++) if (taken[k] == n) dup = 1;
            float d = fabsf(n - cur) + (dup ? 6.0f : 0.0f);
            if (d < bd) { bd = d; best = n; }
        }
    return best;
}

static void voice_lead(int chord, float glide)
{
    const Chord *c = &CHORDS[chord];
    int taken[NPAD], nt = 0, order[NPAD];
    for (int i = 0; i < NPAD; i++) order[i] = i;
    for (int i = 0; i < NPAD; i++)
        for (int j = i + 1; j < NPAD; j++)
            if (M.pad[order[j]].to < M.pad[order[i]].to) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    int used_pc[5] = { 0 };
    for (int k = 0; k < NPAD; k++) {
        Pad *p = &M.pad[order[k]];
        int pcs[5], np = 0;
        for (int i = 0; i < c->nt; i++) if (!used_pc[i]) pcs[np++] = c->tones[i];
        if (!np) for (int i = 0; i < c->nt; i++) pcs[np++] = c->tones[i];
        int n = nearest_free(p->to, pcs, np, M.key, 52, 77, taken, nt);
        if (n < 0) n = (int)p->to;
        for (int i = 0; i < c->nt; i++) if (((n - 50 - M.key - c->tones[i]) % 12 + 12) % 12 == 0) used_pc[i] = 1;
        taken[nt++] = n;
        p->from = p->pitch; p->to = (float)n; p->gt = 0;
        p->glen = glide * (0.75f + 0.5f * frand01(&M.rng));   /* voices arrive at slightly different times */
    }
    int root = 50 + M.key + c->bass;
    while (root > 50) root -= 12;
    while (root < 38) root += 12;
    if (fabsf(root + 12 - M.bass.to) < fabsf(root - M.bass.to) && root + 12 <= 50) root += 12;
    M.bass.from = M.bass.pitch; M.bass.to = (float)root; M.bass.gt = 0; M.bass.glen = glide * 1.2f;
    M.warp_vel = (frand01(&M.rng) < 0.5f ? -1.0f : 1.0f) * (0.12f + 0.12f * frand01(&M.rng));
}

static int pick_next_chord(void)
{
    const Edge *e = GRAPH[M.chord];
    int allow_special = M.changes_since_special >= 4;
    float tot = 0;
    for (int i = 0; i < 7 && e[i].to >= 0 && e[i].w > 0; i++)
        if (!CHORDS[e[i].to].special || allow_special) tot += e[i].w * (CHORDS[e[i].to].special ? 1.2f : 1.0f);
    float r = frand01(&M.rng) * tot;
    for (int i = 0; i < 7 && e[i].to >= 0 && e[i].w > 0; i++) {
        if (CHORDS[e[i].to].special && !allow_special) continue;
        r -= e[i].w * (CHORDS[e[i].to].special ? 1.2f : 1.0f);
        if (r <= 0) return e[i].to;
    }
    return CH_I;
}

static int new_key(void)
{
    static const int MOVES[] = { 4, -4, 5, -5, 3, -3, -4 };   /* mediants, fourths, minor thirds */
    int mv = MOVES[(int)(frand01(&M.rng) * 7) % 7];
    int nk = M.key + mv;
    if (nk - M.home > 6) nk -= 12;
    if (nk - M.home < -6) nk += 12;
    if (nk - M.home > 5 || nk - M.home < -5) { nk = M.key - mv; if (abs(nk - M.home) > 6) nk = M.home; }
    return nk;
}

/* decide the next chord (and maybe key) now; it takes effect at the next chord change. Deciding a
   bar early lets the walking bass lead into it. */
static void plan_next(int fresh)
{
    M.changes_since_special++;
    int nc, nk = M.key;
    int stable = M.chord == CH_I || M.chord == CH_Isus || M.chord == CH_vi;
    int may_mod = M.bars_since_key >= 8;
    if (may_mod && (fresh ? frand01(&M.rng) < 0.6f : (M.changes_since_special >= 6 && stable && frand01(&M.rng) < 0.15f))) {
        nk = new_key();
        M.bars_since_key = 0;
        nc = frand01(&M.rng) < 0.6f ? CH_I : CH_IV;
        M.changes_since_special = 0;
    } else {
        nc = pick_next_chord();
        if (fresh) while (nc == M.chord) nc = pick_next_chord();
        if (CHORDS[nc].special) M.changes_since_special = 0;
    }
    M.pend_chord = nc;
    M.pend_key = nk;
    M.has_pend = 1;
}

static void apply_next(void)
{
    if (!M.has_pend) plan_next(0);
    M.chord = M.pend_chord;
    M.key = M.pend_key;
    M.has_pend = 0;
    M.bars_left = 2 + (int)(frand01(&M.rng) * 3);           /* 2-4 bars, about 7-14 s */
    voice_lead(M.chord, 1.6f + 0.8f * frand01(&M.rng));
}

/* MIDI note of a chord tone in a given register */
static float chord_tone(int idx, int lo)
{
    const Chord *c = &CHORDS[M.chord];
    int n = 50 + M.key + c->tones[((idx % c->nt) + c->nt) % c->nt];
    while (n < lo) n += 12;
    while (n >= lo + 12) n -= 12;
    return (float)n;
}

/* scale degree -> MIDI note, degree 0 = tonic of the current chord's scale in octave `base` */
static float scale_note(int deg, int base)
{
    const Chord *c = &CHORDS[M.chord];
    int oct = deg >= 0 ? deg / 7 : -((-deg + 6) / 7);
    int d7 = ((deg % 7) + 7) % 7;
    return (float)(base + M.key + c->scale[d7] + 12 * oct);
}

/* the scale degree nearest a note (for starting runs / steps on a chord tone) */
static int degree_of(float midi, int base)
{
    int best = 0; float bd = 1e9f;
    for (int d = -14; d <= 28; d++) { float x = fabsf(scale_note(d, base) - midi); if (x < bd) { bd = x; best = d; } }
    return best;
}

static void plink_note(float midi, float vol, float delay, float decay_s)
{
    int slot = -1; float quiet = 1e9f;
    for (int i = 0; i < NPLINK; i++) {
        Plink *p = &M.pl[i];
        if (!p->on) { slot = i; break; }
        if (p->env < quiet) { quiet = p->env; slot = i; }
    }
    Plink *p = &M.pl[slot];
    p->on = 1;
    p->phase = 0;
    p->freq = midi_hz(midi);
    p->env = vol;
    p->decay = expf(-1.0f / (OUT_RATE * decay_s));
    p->duty = frand01(&M.rng) < 0.5f ? 0.125f : 0.25f;
    p->pan = 0.15f + 0.7f * frand01(&M.rng);
    p->pan_drift = frandpm(&M.rng) * 0.35f;        /* the note glides across the stereo field as it rings */
    p->start_in = delay;
}

/* queue a run: notes on a 32nd-note grid starting now */
static void queue_run(const float *notes, int n, float vol)
{
    float step = STEP_SEC / 2;
    for (int i = 0; i < n && M.nq < 24; i++) {
        M.qtime[M.nq] = i * step;
        M.qnote[M.nq] = notes[i];
        M.qvol[M.nq] = vol * (1.0f - 0.06f * i);
        M.nq++;
    }
}

/* ascending chord tones from `lo` upward */
static int chord_notes(int lo, float *out, int max)
{
    const Chord *c = &CHORDS[M.chord];
    int k = 0;
    for (int m = lo; m < lo + 40 && k < max; m++)
        for (int i = 0; i < c->nt; i++)
            if (((m - 50 - M.key - c->tones[i]) % 12 + 12) % 12 == 0) { out[k++] = (float)m; break; }
    return k;
}

static void run_chord_up(int len, int start_lo, float vol)
{
    float n[12];
    int k = chord_notes(start_lo, n, len < 12 ? len : 12);
    queue_run(n, k, vol);
}

static void run_scale(int from_deg, int dir, int len, int base, float vol)
{
    float n[12];
    for (int i = 0; i < len && i < 12; i++) n[i] = scale_note(from_deg + i * dir, base);
    queue_run(n, len, vol);
}

static void music_init(void)
{
    memset(&M, 0, sizeof M);
    M.rng = 0x1234567u ^ (uint32_t)time(NULL);
    M.home = M.key = 0;        /* D */
    M.chord = CH_I;
    float start[NPAD] = { 54, 57, 61, 64, 66 };
    for (int i = 0; i < NPAD; i++) {
        Pad *p = &M.pad[i];
        p->pitch = p->from = p->to = start[i];
        p->amp = 0.085f;
        p->pan = p->pan_to = 0.2f + 0.6f * i / (NPAD - 1);
        p->vib_rate = 0.18f + 0.25f * frand01(&M.rng);
        p->vib = frand01(&M.rng) * TWO_PI;
        p->trem_rate = 0.06f + 0.14f * frand01(&M.rng);
        p->trem = frand01(&M.rng) * TWO_PI;
        p->detune = frandpm(&M.rng) * 0.06f;
    }
    M.bass.pitch = M.bass.from = M.bass.to = 38;
    M.bass.amp = 0.11f;
    M.bass.pan = M.bass.pan_to = 0.5f;
    voice_lead(CH_I, 0.1f);
    M.bars_left = 3;
    M.idle_plink = 8.0f;
    M.gate = M.gate_target = 1.0f;
    for (int i = 0; i < NPAD; i++) M.vmul[i] = M.vmul_target[i] = 1.0f;
    M.breath_wait = 6 + (int)(frand01(&M.rng) * 6);
    M.since_dbl = 30.0f;
    M.dlen_l = (int)(3 * STEP_SEC * OUT_RATE);   /* dotted eighth */
    M.dlen_r = (int)(2 * STEP_SEC * OUT_RATE);   /* eighth */
    static const int CL[8] = { 1116, 1188, 1277, 1356, 1139, 1211, 1300, 1379 };
    static const int AL[4] = { 556, 441, 579, 464 };
    for (int i = 0; i < 8; i++) M.clen[i] = CL[i] * OUT_RATE / 44100;
    for (int i = 0; i < 4; i++) M.alen[i] = AL[i] * OUT_RATE / 44100;
    M.next_pop = 0.5f;
    M.sleep_gain = 1.0f;
    M.ld.deg = -1;
    M.ld.glide = 0.03f;
    M.ld.pitch = M.ld.target = 62;
}

/* 4-bit triangle, like the NES/GB wave channel: soft but a little crunchy */
static float tri4(float ph)
{
    float t = ph < 0.5f ? ph * 4.0f - 1.0f : 3.0f - ph * 4.0f;
    return floorf(t * 7.5f + 0.5f) / 7.5f;
}
static float pulse(float ph, float duty) { return ph < duty ? 1.0f : -1.0f; }

/* ---------------- drums (chill) ---------------- */

static void sfx_note_pan(int wave, float delay, float dur, float f0, float f1, float vol, float duty, float send, float pan, float hp);

static int g_music_note;   /* set while the drums add notes, so they follow the music volume */

/* 0 kick, 1 snare, 2 shaker, 3 rim; and for playing along: 4 clap, 5 low tom, 6 high tom, 7 woodblock */
enum { DR_KICK, DR_SNARE, DR_SHAKER, DR_RIM, DR_CLAP, DR_LOTOM, DR_HITOM, DR_BLOCK, NUM_DRUMS };
static void drum(int which, float vol)
{
    static const int HIT[NUM_DRUMS] = { 0, 1, 2, 3, 1, 4, 4, 3 };   /* which listening animal dances to it */
    g_music_note = 1;
    g_hit_ms[HIT[which]] = now_ms();
    switch (which) {
    case 0: /* kick: a round sine thump with a soft click on top */
        sfx_note_pan(3, 0, 0.26f, 120, 42, 0.85f * vol, 0, 0.04f, 0.5f, 0);
        sfx_note_pan(2, 0, 0.012f, 3000, 2000, 0.05f * vol, 0, 0, 0.5f, 0.3f);
        break;
    case 1: /* brushy snare: noise + a little body, into the reverb/echo */
        sfx_note_pan(2, 0, 0.18f, 2600, 1900, 0.17f * vol, 0, 0.4f, 0.42f, 0.25f);
        sfx_note_pan(1, 0, 0.08f, 200, 165, 0.16f * vol, 0, 0.1f, 0.42f, 0);
        break;
    case 2: /* shaker */
        sfx_note_pan(2, 0, 0.05f, 9000, 9000, 0.05f * vol, 0, 0.1f, 0.6f + 0.1f * frandpm(&M.rng), 0.8f);
        break;
    case 3: /* rim tick */
        sfx_note_pan(0, 0, 0.025f, 1200, 1100, 0.04f * vol, 0.125f, 0.4f, 0.35f, 0.5f);
        break;
    case 4: /* clap: three quick bursts of noise and a short tail */
        for (int i = 0; i < 3; i++) sfx_note_pan(2, i * 0.011f, 0.018f, 3200, 2800, 0.13f * vol, 0, 0.2f, 0.55f, 0.45f);
        sfx_note_pan(2, 0.033f, 0.14f, 3000, 2400, 0.10f * vol, 0, 0.45f, 0.55f, 0.45f);
        break;
    case 5: /* low tom: a falling sine with a little triangle body */
        sfx_note_pan(3, 0, 0.34f, 150, 88, 0.55f * vol, 0, 0.15f, 0.36f, 0);
        sfx_note_pan(1, 0, 0.05f, 300, 180, 0.06f * vol, 0, 0, 0.36f, 0);
        break;
    case 6: /* high tom */
        sfx_note_pan(3, 0, 0.26f, 230, 150, 0.45f * vol, 0, 0.15f, 0.64f, 0);
        sfx_note_pan(1, 0, 0.04f, 460, 300, 0.05f * vol, 0, 0, 0.64f, 0);
        break;
    case 7: /* woodblock: a short hollow knock */
        sfx_note_pan(3, 0, 0.07f, 1050, 980, 0.22f * vol, 0, 0.35f, 0.7f, 0);
        sfx_note_pan(1, 0, 0.03f, 2100, 1900, 0.04f * vol, 0, 0.2f, 0.7f, 0);
        break;
    }
    g_music_note = 0;
}

/* ---------------- walking bass ---------------- */

/* sparse rhythms (sixteenth steps). Busier ones come in as more acorns land. */
static const int WB_PATS[][6] = {
    { 0, 8, -1 },                 /* half notes */
    { 0, 10, -1 },                /* one, and-of-three */
    { 0, 6, 8, -1 },
    { 0, 8, 14, -1 },             /* with a pickup into the next bar */
    { 0, 4, 8, 12, -1 },          /* a proper walk, but short notes */
    { 0, 6, 8, 12, 14, -1 },
    { 0, 3, 8, 11, 14, -1 },
};

static void wb_new_bar(void)
{
    memset(M.wb_pat, 0, sizeof M.wb_pat);
    int busy = M.layer_target[1] > 0.5f ? 7 : M.layer_target[0] > 0.5f ? 5 : 3;   /* more choices as it fills in */
    const int *p = WB_PATS[(int)(frand01(&M.rng) * busy) % busy];
    if (M.dbl_left > 0) p = WB_PATS[4 + (int)(frand01(&M.rng) * 3) % 3];        /* double time: walk */
    for (int i = 0; i < 6 && p[i] >= 0; i++) M.wb_pat[p[i]] = 1;
}

/* pitch class (0..11, relative to D) of a chord/scale tone */
static int pc_of(int key, int semis) { return ((key + semis) % 12 + 12) % 12; }

/* nearest MIDI note with pitch class pc to `near`, kept inside the bass register */
static float bass_near(int pc, float near)
{
    int best = 0; float bd = 1e9f;
    for (int n = 38; n <= 55; n++) {
        if (((n - 50) % 12 + 12) % 12 != pc) continue;
        float d = fabsf(n - near);
        if (d < bd) { bd = d; best = n; }
    }
    return (float)best;
}

static void wb_play(float midi, float vol)
{
    M.wb.freq = midi_hz(midi);
    M.wb.env = vol;
    M.wb.decay = expf(-1.0f / (OUT_RATE * (0.10f + 0.06f * frand01(&M.rng))));   /* short, plucky */
    M.wb_last = midi;
    g_hit_ms[4] = now_ms();
}

/* jazz-ish note choice: the root on the one, chord tones on strong beats, stepwise passing tones between,
   and a chromatic or scale-step approach into the next chord's root */
static void wb_step(int st)
{
    if (!M.wb_pat[st] || M.wb_gain < 0.02f) return;
    const Chord *c = &CHORDS[M.chord];
    float last = M.wb_last > 0 ? M.wb_last : 45;
    float n;
    /* is this the last note of the bar before a chord change? */
    int last_in_bar = 1;
    for (int k = st + 1; k < 16; k++) if (M.wb_pat[k]) last_in_bar = 0;
    if (st == 0) {
        n = bass_near(pc_of(M.key, c->bass), last);
    } else if (last_in_bar && M.has_pend && st >= 8) {
        float target = bass_near(pc_of(M.pend_key, CHORDS[M.pend_chord].bass), last);
        n = target + (frand01(&M.rng) < 0.5f ? 1 : -1);          /* chromatic approach */
        if (frand01(&M.rng) < 0.35f) n = target + (target > last ? -2 : 2);   /* or a whole step */
    } else if (st == 8 || st == 4 || st == 12) {
        int pick = 1 + (int)(frand01(&M.rng) * (c->nt > 3 ? 3 : c->nt - 1));    /* 3rd, 5th or 7th */
        n = bass_near(pc_of(M.key, c->tones[pick % c->nt]), last);
        if (fabsf(n - last) < 0.5f) n = bass_near(pc_of(M.key, c->tones[(pick + 1) % c->nt]), last);
    } else {
        /* a scale step toward the root, or away if we're on it */
        float root = bass_near(pc_of(M.key, c->bass), last);
        int dir = root > last ? 1 : root < last ? -1 : (frand01(&M.rng) < 0.5f ? 1 : -1);
        int d = degree_of(last, 50) + dir;
        n = scale_note(d, 50);
        while (n > 55) n -= 12;
        while (n < 38) n += 12;
    }
    float vol = 0.42f * (st == 0 ? 1.0f : st == 8 ? 0.85f : 0.7f);
    wb_play(n, vol);
}

/* one sixteenth-note step of the sequencer */
/* pads lift off the keys just before these steps' following beat (sixteenth-note gaps) */
static int gap_step(int st, int stage_deep)
{
    if (M.dbl_left > 0) return (st % 4) == 3 || st == 13;           /* busier: before every beat and the and-of-4 */
    if (stage_deep) return (st % 4) == 3;                              /* before every beat */
    return st == 7 || st == 15;                                        /* before beats 3 and 1 */
}

static void start_breath(void)
{
    /* thin out to two voices, or (sometimes) let all of them go very quiet */
    int quiet = frand01(&M.rng) < 0.35f;
    int keep1 = (int)(frand01(&M.rng) * NPAD), keep2 = (keep1 + 1 + (int)(frand01(&M.rng) * (NPAD - 1))) % NPAD;
    for (int i = 0; i < NPAD; i++)
        M.vmul_target[i] = quiet ? 0.18f : (i == keep1 || i == keep2) ? 1.0f : 0.06f;
    M.breath_left = 2;
}

static void set_stage(int stage)
{
    M.wb_target = stage >= 1 ? 1.0f : 0.0f;
    M.layer_target[0] = stage >= 2 ? 1.0f : 0.0f;
    M.layer_target[1] = stage >= 3 ? 1.0f : 0.0f;
    M.layer_target[2] = stage >= 4 ? 1.0f : 0.0f;
    M.layer_target[3] = stage >= 5 ? 1.0f : 0.0f;
    M.pump_target = stage >= 3 ? 1.0f : stage >= 2 ? 0.5f : 0.0f;
}

/* listen mode: every 8-24 bars the arrangement drifts up or down a stage or two (mostly staying busy),
   and sometimes a fresh chord or key marks the new section */
static void listen_bar(void)
{
    if (--M.l_bars > 0) return;
    /* mostly busy (3-5), sometimes sparser, rarely just the pads; at most two stages from here */
    static const float W[6] = { 0.4f, 1.0f, 2.0f, 3.0f, 3.0f, 2.5f };
    float tot = 0;
    for (int k = 0; k < 6; k++) if (k != M.l_stage && abs(k - M.l_stage) <= 2) tot += W[k];
    float r = frand01(&M.rng) * tot;
    int st = M.l_stage;
    for (int k = 0; k < 6; k++) {
        if (k == M.l_stage || abs(k - M.l_stage) > 2) continue;
        r -= W[k];
        if (r <= 0) { st = k; break; }
    }
    M.l_stage = st;
    set_stage(st);
    M.bright_target = 0.25f + 0.15f * st + 0.1f * frand01(&M.rng);
    M.l_bars = 8 + (int)(frand01(&M.rng) * 17);
    if (frand01(&M.rng) < 0.4f) { M.force_change = 1; plan_next(1); }
    g_section_ms = now_ms();
}

static void sequencer_step(int st)
{
    if (st == 0 && M.sleeping) {
        /* falling asleep: shaker and rim go, then the snare, then the kick and bass, two bars apart */
        M.dbl_left = 0; M.hold_dbl = 0;
        if (--M.sleep_bars <= 0) {
            if (M.l_stage > 0) { M.l_stage--; set_stage(M.l_stage); M.sleep_bars = 2; }
            else if (M.layer_gain[0] < 0.02f && M.wb_gain < 0.02f) g_sleep_done = 1;
        }
    } else if (st == 0 && M.listen) listen_bar();
    if (st == 0) {
        M.bars_since_key++;
        if (M.force_change) { M.force_change = 0; if (!M.has_pend) plan_next(1); apply_next(); }
        else if (--M.bars_left <= 0) apply_next();
        if (M.bars_left == 1 && !M.has_pend) plan_next(0);     /* last bar of this chord: know where we're going */
        /* breathing sections: every so often the pads thin out or go quiet for two bars */
        if (M.breath_left > 0) {
            if (--M.breath_left == 0) {
                for (int i = 0; i < NPAD; i++) M.vmul_target[i] = 1.0f;
                M.breath_wait = 6 + (int)(frand01(&M.rng) * 8);
            }
        } else if (--M.breath_wait <= 0) start_breath();
        /* double-time feel: when the full groove is in and you're on a roll */
        if (M.dbl_left > 0) M.dbl_left--;
        else if (M.layer_target[2] > 0.5f && M.act > 5.0f && M.since_dbl > 40.0f && M.wind < 0.1f) {
            M.dbl_left = 4;
            M.since_dbl = 0;
        } else if (M.listen && M.layer_target[1] > 0.5f && M.since_dbl > 45.0f && frand01(&M.rng) < 0.12f) {
            M.dbl_left = 4;                                    /* listen mode finds its own double-time stretches */
            M.since_dbl = 0;
        }
        if (M.hold_dbl && M.dbl_left < 1) M.dbl_left = 1;      /* held: double time for as long as you hold */
        wb_new_bar();
    }
    wb_step(st);
    float live = 1.0f - M.wind;                                   /* idle wind-down fades drums and bass */
    float dk = live * (1.0f - 0.85f * M.duck);                    /* you're drumming: the band's drums step back */
    float g0 = M.layer_gain[0] * dk, g1 = M.layer_gain[1] * dk, g2 = M.layer_gain[2] * dk, g3 = M.layer_gain[3] * dk;
    if (M.dbl_left > 0) {
        /* double time: kick on 1, the and-of-2 and 3; snare on 2 and 4; shaker eighths */
        if ((st == 0 || st == 6 || st == 8) && g0 > 0.01f) drum(0, g0 * (st == 0 ? 1.0f : 0.7f));
        if ((st == 4 || st == 12) && g1 > 0.01f) drum(1, g1 * 0.9f);
        if ((st % 2) == 0 && g2 > 0.01f) drum(2, g2 * ((st % 4) == 2 ? 1.0f : 0.6f));
        if ((st == 7 || st == 15) && g3 > 0.01f) drum(3, g3);
    } else {
        if (st == 0 && g0 > 0.01f) drum(0, g0);
        if (st == 10 && g3 > 0.01f) drum(0, g3 * 0.55f);                       /* ghost kick */
        if (st == 8 && g1 > 0.01f) drum(1, g1);
        if ((st % 4) == 2 && g2 > 0.01f) drum(2, g2 * (st == 6 || st == 14 ? 1.0f : 0.7f));
        if ((st == 7 || st == 13) && g3 > 0.01f) drum(3, g3);
    }
    /* pad articulation: lift off for this sixteenth, press again on the beat */
    float depth = M.pump * live;
    M.gate_target = gap_step(st, M.pump > 0.75f) ? 1.0f - 0.9f * depth : 1.0f;
    if (M.hold_gate) M.gate_target = (st % 2) ? 0.05f : 1.0f;    /* held: chop the pads on every sixteenth */
    /* rare idle plink, on the grid */
    if (M.idle_plink <= 0 && (st % 2) == 0) {
        plink_note(chord_tone((int)(frand01(&M.rng) * 5), 74 + (frand01(&M.rng) < 0.3f ? 7 : 0)), 0.045f, 0, 0.35f);
        M.idle_plink = 12.0f + 18.0f * frand01(&M.rng);
    }
}

/* ---------------- the lead (Listen, Melody mode) ----------------
   Eight steps of the scale starting on the current chord's root (right = root, then around the D-pad
   to the octave). Notes sustain while held and slide into each other; if the chord changes under a held
   note, the note moves with it. */
enum { LV_FLUTE, LV_SQUARE, LV_REED, LV_BELL, NUM_LV };
static const char *const LV_NAMES[NUM_LV] = { "Flute", "Square", "Reed", "Bell" };
enum { LMOD_OCT, LMOD_VIB, LMOD_ARP };

static int chord_root_deg(void)
{
    const Chord *c = &CHORDS[M.chord];
    for (int d = 0; d < 7; d++) if (c->scale[d] == c->bass % 12) return d;
    return 0;
}

/* MIDI note of scale step i above the chord's root, the root kept between C4 and B4 */
static float lead_note(int i)
{
    int r = chord_root_deg();
    float root = scale_note(r, 50), off = 0;
    while (root + off < 60) off += 12;
    while (root + off >= 72) off -= 12;
    return scale_note(r + i, 50) + off;
}

static const int ARP_PAT[6] = { 0, 2, 4, 7, 4, 2 };   /* up the triad to the octave and back, in the scale */

static void lead_strike(float note, float glide)
{
    M.ld.target = note;
    if (M.ld.env < 0.02f || glide <= 0) M.ld.pitch = note;
    M.ld.glide = glide > 0 ? glide : 0.001f;
    M.ld.bell = 1.0f;
}

static void lead_control(float dt)
{
    /* the chord moved under a held note: follow it */
    if ((M.ld.seen_chord != M.chord || M.ld.seen_key != M.key) && M.ld.deg >= 0 && !M.ld.arp)
        lead_strike(lead_note(M.ld.deg), 0.08f);
    M.ld.seen_chord = M.chord; M.ld.seen_key = M.key;
    /* arpeggio: a new note every 32nd, locked to the beat */
    if (M.ld.arp && M.ld.deg >= 0) {
        int tick = M.step * 2 + (M.step_t >= STEP_SEC / 2);
        if (tick != M.ld.arp_tick) {
            M.ld.arp_tick = tick;
            M.ld.arp_i = (M.ld.arp_i + 1) % 6;
            lead_strike(lead_note(M.ld.deg + ARP_PAT[M.ld.arp_i]), 0);
            M.ld.pluck = 1.0f;
        }
    }
    float k = dt / M.ld.glide; M.ld.pitch += (M.ld.target - M.ld.pitch) * (k > 1 ? 1 : k);
    k = M.ld.env_target > M.ld.env ? dt / 0.012f : dt / 0.14f;
    M.ld.env += (M.ld.env_target - M.ld.env) * (k > 1 ? 1 : k);
    k = dt / 0.05f; M.ld.oct += (M.ld.oct_target - M.ld.oct) * (k > 1 ? 1 : k);      /* a quick swoop up, and back */
    k = dt / 0.18f; M.ld.vib += (M.ld.vib_target - M.ld.vib) * (k > 1 ? 1 : k);
    M.ld.vib_ph += TWO_PI * 5.5f * dt;
    if (M.ld.vib_ph > TWO_PI) M.ld.vib_ph -= TWO_PI;
    M.ld.pluck *= expf(-dt / 0.09f);
    M.ld.bell *= expf(-dt / 0.6f);
}

static void lead_on(int deg)
{
    AUDIO_LOCK();
    M.act += 1.0f;
    M.ld.deg = deg;
    M.ld.env_target = 1.0f;
    if (M.ld.arp) { M.ld.arp_i = 0; M.ld.arp_tick = M.step * 2 + (M.step_t >= STEP_SEC / 2); lead_strike(lead_note(deg), 0); M.ld.pluck = 1.0f; }
    else lead_strike(lead_note(deg), 0.03f);
    M.idle_plink = 10.0f + 10.0f * frand01(&M.rng);
    AUDIO_UNLOCK();
}

static void lead_off(void)
{
    AUDIO_LOCK();
    M.ld.deg = -1;
    M.ld.env_target = 0;
    AUDIO_UNLOCK();
}

static void lead_mod(int which, int on)
{
    AUDIO_LOCK();
    switch (which) {
    case LMOD_OCT: M.ld.oct_target = on ? 1.0f : 0.0f; break;
    case LMOD_VIB: M.ld.vib_target = on ? 1.0f : 0.0f; break;
    case LMOD_ARP:
        M.ld.arp = on;
        if (on && M.ld.deg >= 0) { M.ld.arp_i = 0; M.ld.arp_tick = M.step * 2 + (M.step_t >= STEP_SEC / 2); M.ld.pluck = 1.0f; }
        if (!on && M.ld.deg >= 0) lead_strike(lead_note(M.ld.deg), 0.03f);
        break;
    }
    AUDIO_UNLOCK();
}

static int lead_next_voice(void)
{
    AUDIO_LOCK();
    M.ld.voice = (M.ld.voice + 1) % NUM_LV;
    if (M.ld.deg >= 0) M.ld.bell = 1.0f;
    int v = M.ld.voice;
    AUDIO_UNLOCK();
    return v;
}

static void lead_reset(void)
{
    AUDIO_LOCK();
    M.ld.deg = -1; M.ld.env_target = 0;
    M.ld.oct_target = 0; M.ld.vib_target = 0; M.ld.arp = 0;
    AUDIO_UNLOCK();
}

/* Drums mode: one hit, and the band's own drums duck for a couple of bars */
static void music_drum(int which)
{
    static const float VOL[NUM_DRUMS] = { 1.0f, 1.1f, 1.8f, 2.2f, 1.0f, 1.0f, 1.0f, 1.0f };
    if (which < 0 || which >= NUM_DRUMS) return;
    AUDIO_LOCK();
    M.act += 1.0f;
    M.duck_t = 2 * 16 * STEP_SEC;
    drum(which, VOL[which]);
    M.idle_plink = 10.0f + 10.0f * frand01(&M.rng);
    AUDIO_UNLOCK();
}

static void music_control(float dt)
{
    M.time += dt;
    M.idle_plink -= dt;
    /* tempo grid */
    M.step_t += dt;
    while (M.step_t >= STEP_SEC) {
        M.step_t -= STEP_SEC;
        M.step = (M.step + 1) % 16;
        sequencer_step(M.step);
    }
    /* queued run notes */
    for (int i = 0; i < M.nq; i++) {
        M.qtime[i] -= dt;
        if (M.qtime[i] <= 0) {
            plink_note(M.qnote[i], M.qvol[i], 0, 0.3f);
            M.qtime[i] = M.qtime[M.nq - 1]; M.qnote[i] = M.qnote[M.nq - 1]; M.qvol[i] = M.qvol[M.nq - 1];
            M.nq--; i--;
        }
    }
    /* pads: glides, warble, wandering pan */
    for (int i = 0; i <= NPAD; i++) {
        Pad *p = i < NPAD ? &M.pad[i] : &M.bass;
        if (p->gt < p->glen) {
            p->gt += dt;
            float u = p->gt / p->glen; if (u > 1) u = 1;
            u = u * u * (3 - 2 * u);
            p->pitch = p->from + (p->to - p->from) * u;
        } else p->pitch = p->to;
        p->vib += TWO_PI * p->vib_rate * dt;
        p->trem += TWO_PI * p->trem_rate * dt;
        if (i < NPAD) {
            p->pan_t -= dt;
            if (p->pan_t <= 0) { p->pan_to = 0.1f + 0.8f * frand01(&M.rng); p->pan_t = 4.0f + 8.0f * frand01(&M.rng); }
            p->pan += (p->pan_to - p->pan) * dt * 0.15f;
        }
    }
    M.warp += M.warp_vel * dt * 3.0f;
    M.warp_vel -= M.warp * dt * 4.0f;
    M.warp_vel *= expf(-dt * 1.8f);
    M.wow_phase += TWO_PI * 0.11f * dt;
    M.flutter_phase += TWO_PI * 4.7f * dt;
    M.pad_lp_phase += TWO_PI * 0.03f * dt;
    M.bright += (M.bright_target - M.bright) * dt * 0.4f;
    for (int k = 0; k < 4; k++) M.layer_gain[k] += (M.layer_target[k] - M.layer_gain[k]) * dt * 0.5f;
    M.wb_gain += (M.wb_target - M.wb_gain) * dt * 0.6f;
    M.pump += (M.pump_target - M.pump) * dt * 0.4f;
    /* the pad "keys": a quick press, a medium release */
    if (M.gate_target < M.gate) M.gate += (M.gate_target - M.gate) * (dt / 0.10f > 1 ? 1 : dt / 0.10f);
    else M.gate += (M.gate_target - M.gate) * (dt / 0.025f > 1 ? 1 : dt / 0.025f);
    for (int i = 0; i < NPAD; i++) M.vmul[i] += (M.vmul_target[i] - M.vmul[i]) * dt * 0.7f;
    M.act *= expf(-dt / 6.0f);
    M.idle_s += dt;
    M.since_dbl += dt;
    if (M.listen) M.idle_s = 0;                                 /* listening is never idle */
    M.wind_target = M.idle_s < 20.0f ? 0.0f : (M.idle_s - 20.0f) / 12.0f > 1 ? 1.0f : (M.idle_s - 20.0f) / 12.0f;
    M.warble += (M.warble_target - M.warble) * (dt * 3.0f > 1 ? 1 : dt * 3.0f);
    M.rain += (M.rain_target - M.rain) * (dt * (M.rain_target > M.rain ? 4.0f : 0.8f) > 1 ? 1 : dt * (M.rain_target > M.rain ? 4.0f : 0.8f));
    M.wind += (M.wind_target - M.wind) * dt * (M.wind_target < M.wind ? 1.5f : 0.5f);
    /* drumming along: duck quickly, come back slowly a couple of bars after the last hit */
    if (M.duck_t > 0) M.duck_t -= dt;
    {
        float dtgt = M.duck_t > 0 ? 1.0f : 0.0f, rate = dtgt > M.duck ? dt / 0.05f : dt / 1.5f;
        M.duck += (dtgt - M.duck) * (rate > 1 ? 1 : rate);
    }
    lead_control(dt);
}

/* render n stereo frames of music (added into L/R); sendL/R feed the echo */
static void music_render(float *L, float *R, const float *sendL, const float *sendR, int n)
{
    float dt = 1.0f / OUT_RATE;
    float pl_step[NPAD + 1], tr[NPAD + 1];
    for (int base = 0; base < n; base += CTRL_BLOCK) {
        int m = n - base < CTRL_BLOCK ? n - base : CTRL_BLOCK;
        music_control(dt * m);
        float wow = (sinf(M.wow_phase * (1.0f + 5.0f * M.warble)) * (0.10f + 0.9f * M.warble) + sinf(M.flutter_phase) * 0.012f) + M.warp;   /* semitones */
        for (int i = 0; i <= NPAD; i++) {
            Pad *p = i < NPAD ? &M.pad[i] : &M.bass;
            pl_step[i] = midi_hz(p->pitch + p->detune + sinf(p->vib * (1.0f + 6.0f * M.warble)) * (0.05f + 0.35f * M.warble) + wow) / OUT_RATE;
            tr[i] = 1.0f + (0.16f + 0.5f * M.warble) * sinf(p->trem * (1.0f + 8.0f * M.warble));   /* volume warble */
        }
        float cutoff = 800.0f + 1500.0f * M.bright + 300.0f * sinf(M.pad_lp_phase);
        float a = 1.0f - expf(-TWO_PI * cutoff / OUT_RATE);
        float ab = 1.0f - expf(-TWO_PI * 420.0f / OUT_RATE);
        float awb = 1.0f - expf(-TWO_PI * 1700.0f / OUT_RATE);
        /* the lead: pitch with the octave swoop, vibrato and a touch of the tape wow */
        float ld_step = 0, ld_amp = 0, ald = 0;
        int ld_on = M.ld.env > 0.0005f;
        if (ld_on) {
            float lp_pitch = M.ld.pitch + 12.0f * M.ld.oct + sinf(M.ld.vib_ph) * 0.45f * M.ld.vib + wow * 0.5f;
            ld_step = midi_hz(lp_pitch) / OUT_RATE;
            static const float AMP[NUM_LV] = { 0.10f, 0.045f, 0.05f, 0.09f };
            static const float CUT[NUM_LV] = { 2400, 2600, 3800, 5000 };
            float strike = M.ld.arp ? 0.55f + 0.45f * M.ld.pluck : 1.0f;
            float bell = M.ld.voice == LV_BELL ? 0.2f + 0.8f * M.ld.bell : 1.0f;
            ld_amp = AMP[M.ld.voice] * M.ld.env * strike * bell;
            ald = 1.0f - expf(-TWO_PI * CUT[M.ld.voice] / OUT_RATE);
        }
        float duck = M.gate;                                               /* pad articulation */
        float sus_bass = 1.0f - 0.9f * M.wb_gain;                         /* the drone bass steps aside */
        for (int j = 0; j < m; j++) {
            int s = base + j;
            float l = 0, r = 0;
            M.music_gain += (M.music_target - M.music_gain) * 0.00005f;
            float g = M.music_gain * M.sleep_gain;
            for (int i = 0; i < NPAD; i++) {
                Pad *p = &M.pad[i];
                p->phase += pl_step[i]; if (p->phase >= 1) p->phase -= 1;
                float x = tri4(p->phase) * 0.8f + pulse(p->phase, 0.5f) * 0.12f;
                p->lp += (x - p->lp) * a;
                float y = p->lp * p->amp * tr[i] * duck * M.vmul[i];
                l += y * (1 - p->pan); r += y * p->pan;
            }
            Pad *b = &M.bass;
            b->phase += pl_step[NPAD]; if (b->phase >= 1) b->phase -= 1;
            b->lp += (tri4(b->phase) - b->lp) * ab;
            float sb = 1.0f - (1.0f - sus_bass) * (1.0f - M.wind);              /* drone bass returns when it winds down */
            l += b->lp * b->amp * 0.5f * tr[NPAD] * sb * duck; r += b->lp * b->amp * 0.5f * tr[NPAD] * sb * duck;
            /* walking bass: 4-bit triangle with a little pulse edge so it speaks on small speakers */
            if (M.wb.env > 0.0005f) {
                M.wb.phase += M.wb.freq / OUT_RATE; if (M.wb.phase >= 1) M.wb.phase -= 1;
                float x = tri4(M.wb.phase) * 0.8f + pulse(M.wb.phase, 0.25f) * 0.35f;
                M.wb.lp += (x - M.wb.lp) * awb;
                float y = M.wb.lp * M.wb.env * M.wb_gain * (1.0f - M.wind);
                M.wb.env *= M.wb.decay;
                l += y; r += y;
            }
            /* plinks -> echo send */
            float pl = 0, pr = 0;
            for (int i = 0; i < NPLINK; i++) {
                Plink *q = &M.pl[i];
                if (!q->on) continue;
                if (q->start_in > 0) { q->start_in -= dt; continue; }
                q->phase += q->freq / OUT_RATE; if (q->phase >= 1) q->phase -= 1;
                float x = pulse(q->phase, q->duty) * q->env;
                q->lp += (x - q->lp) * 0.25f;
                q->env *= q->decay;
                q->pan += q->pan_drift * dt;
                if (q->pan < 0.05f) { q->pan = 0.05f; q->pan_drift = -q->pan_drift; }
                if (q->pan > 0.95f) { q->pan = 0.95f; q->pan_drift = -q->pan_drift; }
                if (q->env < 0.0005f) q->on = 0;
                pl += q->lp * (1 - q->pan); pr += q->lp * q->pan;
            }
            /* the lead: dry in the middle, and a little into the echo */
            float lds = 0;
            if (ld_on) {
                M.ld.phase += ld_step; if (M.ld.phase >= 1) M.ld.phase -= 1;
                float x;
                switch (M.ld.voice) {
                case LV_FLUTE: x = tri4(M.ld.phase) * 0.9f + pulse(M.ld.phase, 0.5f) * 0.08f; break;
                case LV_SQUARE: x = pulse(M.ld.phase, 0.5f); break;
                case LV_REED: x = pulse(M.ld.phase, 0.125f); break;
                default:      /* bell: a sine with an inharmonic partial that fades first */
                    M.ld.phase2 += ld_step * 2.76f; if (M.ld.phase2 >= 1) M.ld.phase2 -= 1;
                    x = sinf(TWO_PI * M.ld.phase) + 0.45f * M.ld.bell * sinf(TWO_PI * M.ld.phase2);
                    break;
                }
                M.ld.lp += (x - M.ld.lp) * ald;
                lds = M.ld.lp * ld_amp;
                l += lds * 0.55f; r += lds * 0.45f;
            }
            /* tempo-synced ping-pong echo */
            int rd = (M.dpos + DELAY_LEN - M.dlen_l) % DELAY_LEN;
            int rd2 = (M.dpos + DELAY_LEN - M.dlen_r) % DELAY_LEN;
            float el = M.dl[rd], er = M.dr[rd2];
            M.dlp_l += (er - M.dlp_l) * 0.35f;
            M.dlp_r += (el - M.dlp_r) * 0.35f;
            M.dl[M.dpos] = pl + sendL[s] + lds * 0.3f + M.dlp_l * 0.48f;
            M.dr[M.dpos] = pr + sendR[s] + lds * 0.2f + M.dlp_r * 0.48f;
            M.dpos = (M.dpos + 1) % DELAY_LEN;
            l += pl + el * 0.55f; r += pr + er * 0.55f;
            /* small reverb */
            float in = (l + r) * 0.5f, wl = 0, wr = 0;
            for (int c = 0; c < 8; c++) {
                float o = M.comb[c][M.cpos[c]];
                M.cfilt[c] += (o - M.cfilt[c]) * 0.6f;
                M.comb[c][M.cpos[c]] = in + M.cfilt[c] * (0.80f + 0.15f * M.rain);
                if (++M.cpos[c] >= M.clen[c]) M.cpos[c] = 0;
                if (c < 4) wl += o; else wr += o;
            }
            for (int k = 0; k < 4; k++) {
                float *w = k < 2 ? &wl : &wr;
                float bo = M.ap[k][M.apos[k]];
                float v = *w + bo * 0.5f;
                M.ap[k][M.apos[k]] = v;
                *w = bo - v * 0.5f;
                if (++M.apos[k] >= M.alen[k]) M.apos[k] = 0;
            }
            /* held rain: more reverb wash, a little less dry, and kept at about the usual loudness so it
               doesn't drive the soft clipper (it peaked at 2x before) */
            float dry = 1.0f - 0.3f * M.rain, wet = 0.05f + 0.16f * M.rain;
            l = l * dry + wl * wet; r = r * dry + wr * wet;
            /* crackle: faint hiss + sparse soft pops */
            float nz = frandpm(&M.rng);
            M.hiss_lp += (nz - M.hiss_lp) * 0.08f;
            float cr = M.hiss_lp * 0.0035f;
            M.next_pop -= dt;
            if (M.next_pop <= 0) {
                M.pop = (0.006f + 0.02f * frand01(&M.rng) * frand01(&M.rng)) * (frand01(&M.rng) < 0.5f ? -1 : 1);
                M.next_pop = frand01(&M.rng) < 0.25f ? 0.02f + 0.05f * frand01(&M.rng) : 0.12f + 0.6f * frand01(&M.rng);
            }
            M.pop_lp += (M.pop - M.pop_lp) * 0.3f;
            M.pop *= 0.82f;
            cr += M.pop_lp;
            /* held rain: a soft filtered hiss with the odd drip */
            if (M.rain > 0.01f) {
                float rn = frandpm(&M.rng);
                M.rain_lp += (rn - M.rain_lp) * 0.35f;
                float hp = M.rain_lp - M.rain_hp; M.rain_hp += (M.rain_lp - M.rain_hp) * 0.02f;
                cr += hp * 0.018f * M.rain;
                M.drip_t -= dt;
                if (M.drip_t <= 0) {
                    M.pop += (0.004f + 0.006f * frand01(&M.rng)) * M.rain * (frand01(&M.rng) < 0.5f ? -1 : 1);
                    M.drip_t = 0.03f + 0.15f * frand01(&M.rng);
                }
            }
            L[s] += (l + cr) * g;
            R[s] += (r + cr * 0.9f) * g;
        }
    }
}

/* ---------------- game events ---------------- */

enum { SFX_STEP, SFX_PUSH, SFX_GOAL, SFX_BUMP, SFX_UNDO, SFX_REDO, SFX_RESTART, SFX_CLEAR, SFX_MENU, SFX_SELECT, SFX_STUCK };

static void sfx_note_pan(int wave, float delay, float dur, float f0, float f1, float vol, float duty, float send, float pan, float hp)
{
    int slot = -1;
    for (int i = 0; i < NSFX; i++) if (!M.sfx[i].on) { slot = i; break; }
    if (slot < 0) return;
    SfxNote *s = &M.sfx[slot];
    memset(s, 0, sizeof *s);
    s->on = 1; s->wave = wave; s->delay = delay; s->dur = dur;
    s->f0 = f0; s->f1 = f1; s->vol = vol; s->duty = duty; s->send = send; s->pan = pan; s->hp = hp;
    s->nseed = 0xACE1u + slot * 77 + (uint32_t)(M.time * 1000);
    s->mus = g_music_note;
}

static float g_last_note = 76;   /* where the last plink was, for undo/redo steps */

static void sfx_play(int id)
{
    AUDIO_LOCK();
    float r = frand01(&M.rng);
    float pv = M.music_target > 0 ? 1.0f : 0.0f;     /* musical events follow the music volume */
    switch (id) {
    case SFX_STEP:   sfx_note_pan(1, 0, 0.03f, 300 + 30 * r, 240, 0.07f, 0.5f, 0, 0.5f, 0); break;
    case SFX_PUSH:   sfx_note_pan(0, 0, 0.07f, 140, 90, 0.07f, 0.25f, 0, 0.5f, 0); sfx_note_pan(2, 0, 0.05f, 2000, 1500, 0.035f, 0, 0, 0.5f, 0); break;
    case SFX_BUMP:   /* a plink instead of a buzz */
        if (pv) { g_last_note = chord_tone((int)(r * 5), 62); plink_note(g_last_note, 0.06f, 0, 0.25f); }
        break;
    case SFX_MENU:
        if (pv) { g_last_note = chord_tone((int)(r * 5), 76); plink_note(g_last_note, 0.045f, 0, 0.22f); }
        break;
    case SFX_SELECT: if (pv) run_chord_up(4, 72, 0.055f); break;
    case SFX_GOAL:
        sfx_note_pan(0, 0, 0.07f, 140, 90, 0.06f, 0.25f, 0, 0.5f, 0);
        if (pv) run_chord_up(5, 67 + (int)(r * 5), 0.06f);
        break;
    case SFX_UNDO: case SFX_REDO:
        if (pv) {
            int d = degree_of(g_last_note, 50) + (id == SFX_UNDO ? -1 : 1);
            if (d < 8) d += 7;
            if (d > 24) d -= 7;
            g_last_note = scale_note(d, 50);
            plink_note(g_last_note, 0.045f, 0, 0.18f);
        } else sfx_note_pan(1, 0, 0.04f, id == SFX_UNDO ? 300 : 220, id == SFX_UNDO ? 200 : 330, 0.06f, 0.5f, 0, 0.5f, 0);
        break;
    case SFX_STUCK: {
        /* a soft "uh-oh": two falling notes, the second sagging a little */
        float hi = chord_tone(0, 64), lo = hi - 3;
        sfx_note_pan(1, 0, 0.16f, midi_hz(hi), midi_hz(hi), 0.11f, 0.5f, 0.2f, 0.5f, 0);
        sfx_note_pan(1, 0.17f, 0.30f, midi_hz(lo), midi_hz(lo - 0.6f), 0.11f, 0.5f, 0.3f, 0.5f, 0);
        break;
    }
    case SFX_RESTART:
        if (pv) run_scale(degree_of(84, 50), -1, 7, 50, 0.05f);
        break;
    case SFX_CLEAR:
        if (pv) {
            float n[12];
            int k = chord_notes(64, n, 8);
            float top = chord_tone(0, (int)n[k - 1] + 1);    /* land on the root above */
            n[k++] = top;
            queue_run(n, k, 0.06f);
        }
        M.force_change = 1;                 /* a fresh chord (often a new key) at the next bar */
        plan_next(1);                       /* decided now, so the bass leads into it */
        break;
    }
    if (id == SFX_BUMP || id == SFX_MENU || id == SFX_SELECT || id == SFX_GOAL) M.idle_plink = 12.0f + 18.0f * frand01(&M.rng);
    AUDIO_UNLOCK();
}

/* the player did something: keeps the groove awake and feeds the double-time feel */
static void music_activity(void)
{
    AUDIO_LOCK();
    M.act += 1.0f;
    M.idle_s = 0;
    AUDIO_UNLOCK();
}

/* acorns on spots -> brightness and the rhythm section.
   Stages (levels with more than one acorn): 1 walking bass, 2 + kick and the pads lifting off before
   beats 1 and 3, 3 + brushy snare and lifting before every beat, 4 + shaker, 5 + ghost kick and rim. */
static void music_progress(int on_goal, int total)
{
    AUDIO_LOCK();
    float f = total > 0 ? (float)on_goal / total : 0;
    M.bright_target = f;
    int stage = 0;
    if (total > 1 && on_goal > 0) {
        int span = total > 2 ? total - 2 : 1;               /* every layer is in by the last acorn */
        stage = 1 + ((on_goal - 1) * 4 + span / 2) / span;
        if (stage > 1 + (on_goal - 1) * 2) stage = 1 + (on_goal - 1) * 2;   /* at most two new layers per acorn */
        if (stage > 5) stage = 5;
    }
    set_stage(stage);
    AUDIO_UNLOCK();
}

/* ---------------- listen mode ---------------- */

static void music_listen(int on)
{
    AUDIO_LOCK();
    M.listen = on;
    M.sleeping = 0; g_sleep_done = 0;
    M.hold_gate = M.hold_dbl = 0;
    M.warble_target = M.rain_target = 0;
    M.sleep_gain = 1.0f;
    if (on) {
        M.l_stage = 2; M.l_bars = 6;               /* start with bass and a kick, then wander */
        set_stage(2);
        M.bright_target = 0.55f;
        M.idle_s = 0;
        M.force_change = 1; plan_next(1);
    } else {
        set_stage(0);
        M.bright_target = 0;
        M.dbl_left = 0;
    }
    AUDIO_UNLOCK();
}

enum { LH_GATE, LH_WARBLE, LH_RAIN, LH_DOUBLE };
static void music_hold(int what, int on)
{
    AUDIO_LOCK();
    switch (what) {
    case LH_GATE: M.hold_gate = on; if (!on) M.gate_target = 1.0f; break;
    case LH_WARBLE: M.warble_target = on ? 1.0f : 0.0f; break;
    case LH_RAIN: M.rain_target = on ? 1.0f : 0.0f; break;
    case LH_DOUBLE: M.hold_dbl = on; if (on && M.dbl_left < 1) M.dbl_left = 1; break;
    }
    AUDIO_UNLOCK();
}

static void music_sleep_wind(int on)
{
    AUDIO_LOCK();
    M.sleeping = on;
    M.sleep_bars = 1;
    g_sleep_done = 0;
    if (!on && M.listen) M.l_bars = 1;       /* awake: the arrangement picks itself back up */
    AUDIO_UNLOCK();
}

static void music_sleep_gain(float g)
{
    AUDIO_LOCK();
    M.sleep_gain = g < 0 ? 0 : g > 1 ? 1 : g;
    AUDIO_UNLOCK();
}

static void sfx_render(float *L, float *R, float *sendL, float *sendR, int n)
{
    float dt = 1.0f / OUT_RATE;
    for (int i = 0; i < NSFX; i++) {
        SfxNote *s = &M.sfx[i];
        if (!s->on) continue;
        for (int j = 0; j < n; j++) {
            if (s->delay > 0) { s->delay -= dt; continue; }
            float u = s->t / s->dur;
            if (u >= 1) { s->on = 0; break; }
            float f = s->f0 * powf(s->f1 / s->f0, u);
            s->phase += f / OUT_RATE; if (s->phase >= 1) s->phase -= 1;
            float x;
            if (s->wave == 0) x = pulse(s->phase, s->duty);
            else if (s->wave == 1) x = tri4(s->phase);
            else if (s->wave == 3) x = sinf(TWO_PI * s->phase);
            else {
                s->ncount += f / OUT_RATE * 8;
                if (s->ncount >= 1) { s->ncount -= 1; s->nseed = s->nseed * 1103515245u + 12345u; s->nval = ((s->nseed >> 16) & 1) ? 1.f : -1.f; }
                x = s->nval;
                s->lp += (x - s->lp) * 0.35f;      /* soften the noise */
                x = s->lp;
            }
            if (s->hp > 0) { float y = x - s->hpz; s->hpz += (x - s->hpz) * (1.0f - s->hp); x = y; }
            float env = (u < 0.03f ? u / 0.03f : 1.0f) * (1.0f - u) * (1.0f - u);
            float y = x * env * s->vol * (s->mus ? M.music_gain * 0.9f : M.sfx_gain);
            L[j] += y * (1.0f - s->pan) * 1.6f;
            R[j] += y * s->pan * 1.6f;
            if (s->send > 0) { sendL[j] += y * s->send * 0.5f; sendR[j] += y * s->send * 0.3f; }
            s->t += dt;
        }
    }
}

/* ---------------- SDL glue ---------------- */

#ifdef PEAK_DEBUG
static float g_peak;
#endif
static float g_mixL[4096], g_mixR[4096], g_sendL[4096], g_sendR[4096];

static void audio_mix(int16_t *out, int frames)
{
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        memset(g_mixL, 0, n * sizeof(float));
        memset(g_mixR, 0, n * sizeof(float));
        memset(g_sendL, 0, n * sizeof(float));
        memset(g_sendR, 0, n * sizeof(float));
        sfx_render(g_mixL, g_mixR, g_sendL, g_sendR, n);
        music_render(g_mixL, g_mixR, g_sendL, g_sendR, n);
        for (int i = 0; i < n; i++) {
            float l = g_mixL[i], r = g_mixR[i];
#ifdef PEAK_DEBUG
            if (fabsf(l) > g_peak) g_peak = fabsf(l);
            if (fabsf(r) > g_peak) g_peak = fabsf(r);
#endif
            l = l / (1.0f + fabsf(l)); r = r / (1.0f + fabsf(r));   /* soft clip */
            out[2 * i] = (int16_t)(l * 32000);
            out[2 * i + 1] = (int16_t)(r * 32000);
        }
        out += 2 * n;
        frames -= n;
    }
}

#ifndef __EMSCRIPTEN__
static void audio_cb(void *ud, Uint8 *stream, int len) { (void)ud; audio_mix((int16_t *)stream, len / 4); }
#else
/* the page's Web Audio node asks for samples: fills g_web_audio with `frames` stereo frames */
static int16_t g_web_audio[2 * 4096];
EMSCRIPTEN_KEEPALIVE int16_t *web_audio_fill(int frames)
{
    if (frames > 4096) frames = 4096;
    audio_mix(g_web_audio, frames);
    return g_web_audio;
}
#endif

/* the music volume also scales the musical events (plinks, runs, drums are part of the music) */
static void music_set_volume(int v) { AUDIO_LOCK(); M.music_target = (v / 10.0f) * (v / 10.0f) * 2.2f; AUDIO_UNLOCK(); }
static void sfx_set_volume(int v) { AUDIO_LOCK(); M.sfx_gain = (v / 10.0f) * (v / 10.0f) * 1.4f; AUDIO_UNLOCK(); }

static void audio_open(void)
{
    music_init();
#ifdef __EMSCRIPTEN__
    return;                       /* the page starts Web Audio on the first click or tap */
#else
    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_SAMPLES;
    want.callback = audio_cb;
    if (SDL_OpenAudio(&want, NULL) < 0) { fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError()); return; }
    SDL_PauseAudio(0);
#endif
}

/* offline render for testing: --wav out.wav seconds. Simulates a little play so events and
   rhythm layers are heard: acorns land, a level is cleared, menus are browsed. */
static int g_wav_listen;      /* --wav-listen: render listen mode with some play-along instead */
static void audio_render_wav(const char *path, int secs)
{
    music_init();
    if (g_wav_listen) music_listen(1);
    M.music_target = M.music_gain = 0.36f * 2.2f;
    M.sfx_gain = 0.49f * 1.4f;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int frames = secs * OUT_RATE;
    uint32_t data = frames * 4, riff = 36 + data, sr = OUT_RATE, br = OUT_RATE * 4;
    uint16_t fmt = 1, ch = 2, ba = 4, bits = 16;
    uint32_t fl = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fl, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    static int16_t buf[2 * 1024];
    int last_chord = -1, last_key = 99;
    /* script: a 4-acorn level over ~70 s with steps, bumps, undos; then clear; then menus */
    /* script: a 6-acorn level. Acorns land at 15, 29, 43 and 57 s (bass, kick, snare, shaker); a burst of
       quick moves from 60 s brings on the double-time feel; the 5th acorn at 76 s; then nothing until 122 s
       so the idle wind-down is heard; a move brings the groove back; the last acorn at 128 s, then menus. */
    const int total = 6;
    int placed = 0, next_ev = 0;
    float ev_t[64]; int ev_id[64]; int ne = 0;
    float base_t[] = { 3, 5, 8, 11, 15, 22, 26, 29, 36, 40, 43, 50, 54, 57 };
    int base_id[] = { SFX_STEP, SFX_PUSH, SFX_BUMP, SFX_STEP, SFX_GOAL, SFX_STEP, SFX_PUSH, SFX_GOAL, SFX_STEP, SFX_UNDO, SFX_GOAL,
                      SFX_STEP, SFX_REDO, SFX_GOAL };
    for (int i = 0; i < 14; i++) { ev_t[ne] = base_t[i]; ev_id[ne++] = base_id[i]; }
    for (float tt = 60; tt < 75 && ne < 50; tt += 0.45f) { ev_t[ne] = tt; ev_id[ne++] = ((int)(tt * 10) % 3) ? SFX_STEP : SFX_PUSH; }
    float tail_t[] = { 76, 122, 124, 128, 134, 135, 136, 137 };
    int tail_id[] = { SFX_GOAL, SFX_STEP, SFX_PUSH, SFX_GOAL, SFX_CLEAR, SFX_MENU, SFX_MENU, SFX_SELECT };
    for (int i = 0; i < 8; i++) { ev_t[ne] = tail_t[i]; ev_id[ne++] = tail_id[i]; }
    int nev = g_wav_listen ? 0 : ne;
    /* listen script: a melody (with the octave swoop, vibrato, an arpeggio and each voice), some drumming,
       then each held effect for ~8 s */
    static const struct { float t; int what, arg, on; } LS[] = {
        { 10, 1, 0, 1 }, { 10.5f, 1, 2, 1 }, { 11, 1, 4, 1 }, { 11.5f, 1, 7, 1 }, { 12.5f, 2, LMOD_OCT, 1 }, { 13.5f, 2, LMOD_OCT, 0 },
        { 14, 1, 5, 1 }, { 14.5f, 2, LMOD_VIB, 1 }, { 16, 2, LMOD_VIB, 0 }, { 16.5f, 2, LMOD_ARP, 1 }, { 19, 2, LMOD_ARP, 0 },
        { 19.5f, 3, 0, 0 }, { 20, 1, 0, 1 }, { 21, 1, 3, 1 }, { 22, 3, 0, 0 }, { 22.5f, 1, 6, 1 }, { 23.5f, 3, 0, 0 }, { 24, 1, 2, 1 },
        { 25, 1, 7, 1 }, { 26, 1, -1, 0 },
        { 27, 4, DR_KICK, 0 }, { 27.43f, 4, DR_SNARE, 0 }, { 27.86f, 4, DR_KICK, 0 }, { 28.07f, 4, DR_KICK, 0 }, { 28.29f, 4, DR_CLAP, 0 },
        { 28.7f, 4, DR_HITOM, 0 }, { 28.9f, 4, DR_LOTOM, 0 }, { 29.1f, 4, DR_BLOCK, 0 }, { 29.3f, 4, DR_RIM, 0 }, { 29.5f, 4, DR_SHAKER, 0 },
        { 32, 0, LH_GATE, 1 }, { 40, 0, LH_GATE, 0 },
        { 44, 0, LH_WARBLE, 1 }, { 52, 0, LH_WARBLE, 0 }, { 56, 0, LH_RAIN, 1 }, { 64, 0, LH_RAIN, 0 },
        { 70, 0, LH_DOUBLE, 1 }, { 78, 0, LH_DOUBLE, 0 },
        { 100, -2, 0, 0 },     /* the sleep timer runs out */
    };
    int li = 0;
    for (int done = 0; done < frames; done += 1024) {
        float now = done / (float)OUT_RATE;
        while (g_wav_listen && li < (int)(sizeof LS / sizeof LS[0]) && now >= LS[li].t) {
            switch (LS[li].what) {
            case -2: music_sleep_gain(0); music_sleep_wind(1); break;
            case 0: music_hold(LS[li].arg, LS[li].on); break;
            case 1: if (LS[li].arg >= 0) lead_on(LS[li].arg); else lead_off(); break;
            case 2: lead_mod(LS[li].arg, LS[li].on); break;
            case 3: lead_next_voice(); break;
            case 4: music_drum(LS[li].arg); break;
            }
            fprintf(stderr, "%6.1fs  listen: %d %d %d\n", now, LS[li].what, LS[li].arg, LS[li].on);
            li++;
        }
#ifdef PEAK_DEBUG
        if (((done / 1024) % 47) == 0) { fprintf(stderr, "%6.1fs  peak before soft clip %.2f  rain %.2f\n", now, g_peak, M.rain); g_peak = 0; }
#endif
        if (g_wav_listen) {
            static int lst = -1;
            if (M.l_stage != lst) { fprintf(stderr, "%6.1fs  listen stage %d\n", now, M.l_stage); lst = M.l_stage; }
            static int sd;
            if (g_sleep_done && !sd) { fprintf(stderr, "%6.1fs  asleep: last drum gone\n", now); sd = 1; }
        }
        while (next_ev < nev && now >= ev_t[next_ev]) {
            int id = ev_id[next_ev++];
            sfx_play(id);
            if (id != SFX_MENU && id != SFX_SELECT && id != SFX_CLEAR) music_activity();
            if (id == SFX_GOAL) { placed++; if (placed > total) placed = total; music_progress(placed, total); }
            if (id == SFX_RESTART) { placed = 0; music_progress(0, total); }
            if (id == SFX_CLEAR) { placed = 0; music_progress(0, total); }
            if (id == SFX_GOAL && placed == total) { sfx_play(SFX_CLEAR); placed = 0; music_progress(0, total); }
            if (id == SFX_GOAL || id == SFX_RESTART || id == SFX_CLEAR || id == SFX_MENU)
                fprintf(stderr, "%6.1fs  event %d  (%d/%d on goals)\n", now, id, placed, total);
        }
        audio_mix(buf, 1024);
        fwrite(buf, 4, 1024, f);
        {
            static int ldbl = -1, lbr = -1, lw = -1;
            if ((M.dbl_left > 0) != ldbl) { fprintf(stderr, "%6.1fs  double time %s\n", now, M.dbl_left > 0 ? "ON" : "off"); ldbl = M.dbl_left > 0; }
            if ((M.breath_left > 0) != lbr) { fprintf(stderr, "%6.1fs  breath %s\n", now, M.breath_left > 0 ? "in" : "out"); lbr = M.breath_left > 0; }
            int wq = M.wind > 0.5f;
            if (wq != lw) { fprintf(stderr, "%6.1fs  wind-down %s\n", now, wq ? "(drums+bass faded)" : "(awake)"); lw = wq; }
        }
        if (M.chord != last_chord || M.key != last_key) {
            static const char *KEYS[12] = { "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B", "C", "C#" };
            fprintf(stderr, "%6.1fs  key %-2s  %s\n", now, KEYS[((M.key % 12) + 12) % 12], CHORDS[M.chord].name);
            last_chord = M.chord; last_key = M.key;
        }
    }
    fclose(f);
}
