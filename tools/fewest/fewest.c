/* fewest: proves the fewest moves and the fewest pushes for Sokoban levels, offline.
 *
 * Two searches per level, each appending to its own file, one line per level:
 *
 *     data/fewest-pushes.txt   fewest pushes (run first: it's much quicker)
 *     data/fewest.txt          fewest moves (and, among those, the fewest pushes)
 *
 *     Set name|level|moves|pushes|solution
 *
 * The solution is in LURD notation (lowercase = walk, uppercase = push). In the pushes file the
 * moves are those of the solution found, not the fewest possible. Levels already in a file are
 * skipped, so it can be stopped with Ctrl-C at any time and run again to pick up where it left off.
 * Levels that run out of memory or time are listed in data/fewest-failed.txt and only retried when
 * given more memory or time. tools/make_fewest.py turns the two files into src/fewest.h for the game.
 *
 * Fewest pushes: A* over pushes, with the player standing for the whole area it can reach, the
 * matching bound below, and PI-corral pruning (see pi_corral).
 * Known solutions (our other file, and data/online-best.txt from tools/check_online.py) cap the
 * search: only cheaper solutions are looked for, and if there are none the known one is proven.
 * Fewest moves: A* over push states (cost = moves, then pushes). The lower bound is the best of: a
 * minimum-cost matching of acorns to spots by push distance plus the walk to the nearest acorn, and
 * exact move counts for every pair and some triples of acorns played alone (tables built per level).
 * Dead squares and frozen acorns are pruned. States are packed into a few bytes each, and once half
 * the memory is used it switches to partial expansion (children are stored only when needed), which
 * is slower but fits more of the search. Every solution is replayed and checked before it is saved.
 *
 * Build and run from the repository root:  make fewest && ./fewest
 * Options:  -s "Microban II"   only this set (repeatable; default: the four Microban sets)
 *           -l 5  or  -l 5-20  only these level numbers
 *           -m 4000            memory limit in MB, tables included (default 3000)
 *           -t 600             time limit in seconds per level (default 1800; 0 = none)
 *           -a                 all sets in levels/ (Sasquatch too)
 *           -r                 retry levels listed as failed, even with the same limits
 *           -p / -M            only fewest pushes / only fewest moves (default: both)
 *           -noknown / -nopi   don't use known solutions / PI-corral pruning
 *           -c 0               partial expansion slack in moves once memory is half full (default 0;
 *                              -c 100000 turns it off)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#define INF 0x3fffffff
#define K 1000                    /* cost = moves * K + pushes */
#define MAXB 64

/* ---------- level ---------- */
static int W, H, N, NB, NG;
static char *wall, *goal, *dead, *floor_;
static int *goals;                /* goal cells */
static int start_pl;
static uint16_t start_boxes[MAXB];
static int dir4[4];               /* L U R D */
static const char LURD[] = "lurd";
static int **gdist;               /* gdist[g][c]: pushes to bring an acorn from c to goal g */
static uint16_t *pdist;
static uint16_t *d1;                /* d1[box*N+player]: one acorn alone, fewest moves to a spot */
static void build_d1(void);
static uint16_t *d2; static int NF;    /* d2[(a*NF+b)*NF+p]: two acorns alone (floor indexes), fewest moves */
static void build_d2(void);
static uint16_t *d3;                /* d3[((a*NF+b)*NF+c)*NF+p]: three acorns alone */
static void build_d3(void);
#define IX3(a, b, c, p) ((((size_t)(a) * NF + (b)) * NF + (c)) * NF + (p))           /* pdist[a*N+b]: walking distance, walls only */

/* ---------- search storage ---------- */
static uint8_t *st;               /* node i: SB bytes at st[i*SB]: sorted acorns, then player (packed) */
static int SB, byte_mode;         /* byte_mode: squares stored as 1-byte floor indexes */
static int *fidx, *fcell;         /* cell -> floor index, floor index -> cell */
static int pea_c = 0;                    /* partial expansion: keep children up to f + pea_c */
static uint32_t *gcost, *parent;
static uint32_t nnodes, cap_nodes;
static int S;
static uint32_t *tab; static uint64_t tabmask;
static size_t mem_limit, mem_total, tables_bytes;   /* search limit = total - lookup tables */
static int push_mode;                       /* 0: fewest moves (then pushes); 1: fewest pushes */
#define SOLVER_TAG (push_mode ? "p2" : "v3") /* failures from older versions are retried */
#define U (push_mode ? 1 : K)                /* cost of one move or push in the current mode */
typedef struct { uint32_t f, g, i; } HE;
static HE *heap; static size_t hn, hcap;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }


static void free_level(void)
{
    free(d1); d1 = NULL; free(d2); d2 = NULL; free(d3); d3 = NULL;
    free(wall); free(goal); free(dead); free(floor_); free(goals); free(pdist); free(fidx); free(fcell);
    fidx = fcell = NULL;
    if (gdist) { for (int g = 0; g < NG; g++) free(gdist[g]); free(gdist); }
    gdist = NULL;
}

/* parse a map; returns 0 if unusable */
static int load_map(const char *m)
{
    int w = 0, h = 0, x = 0;
    for (const char *p = m; *p; p++) { if (*p == '\n') { if (x > w) w = x; x = 0; h++; } else x++; }
    if (x) { if (x > w) w = x; h++; }
    W = w + 2; H = h + 2; N = W * H;
    wall = calloc(N, 1); goal = calloc(N, 1); dead = calloc(N, 1); floor_ = calloc(N, 1);
    goals = malloc(N * sizeof *goals);
    memset(wall, 1, N);
    NB = NG = 0; start_pl = -1;
    int boxes[MAXB + 1], nb = 0;
    x = 0; int y = 0;
    for (const char *p = m; *p; p++) {
        if (*p == '\n') { x = 0; y++; continue; }
        int c = (y + 1) * W + x + 1;
        char ch = *p;
        if (ch != '#') wall[c] = 0;
        if (ch == '.' || ch == '*' || ch == '+') goal[c] = 1;
        if (ch == '$' || ch == '*') { if (nb < MAXB) boxes[nb] = c; nb++; }
        if (ch == '@' || ch == '+') start_pl = c;
        x++;
    }
    if (start_pl < 0 || nb > MAXB || nb == 0) return 0;
    dir4[0] = -1; dir4[1] = -W; dir4[2] = 1; dir4[3] = W;
    /* squares the player can reach (ignoring acorns); everything else is wall */
    int *q = malloc(N * sizeof *q), qh = 0, qt = 0;
    floor_[start_pl] = 1; q[qt++] = start_pl;
    while (qh < qt) { int c = q[qh++]; for (int d = 0; d < 4; d++) { int n = c + dir4[d]; if (!wall[n] && !floor_[n]) { floor_[n] = 1; q[qt++] = n; } } }
    for (int c = 0; c < N; c++) if (!floor_[c]) { wall[c] = 1; goal[c] = 0; }
    for (int c = 0; c < N; c++) if (goal[c]) goals[NG++] = c;
    fidx = malloc(N * sizeof *fidx); fcell = malloc(N * sizeof *fcell);
    int nf = 0;
    for (int c = 0; c < N; c++) { fidx[c] = -1; if (floor_[c]) { fidx[c] = nf; fcell[nf++] = c; } }
    byte_mode = nf <= 256;
    NB = 0;
    for (int i = 0; i < nb; i++) if (floor_[boxes[i]]) start_boxes[NB++] = boxes[i];
    if (NB == 0 || NG != NB) { free(q); return 0; }   /* sealed-off acorns on spots are scenery */
    /* push distances to each goal (reverse BFS: pull the acorn away from the goal) */
    gdist = malloc(NG * sizeof *gdist);
    for (int g = 0; g < NG; g++) {
        int *d = gdist[g] = malloc(N * sizeof(int));
        for (int c = 0; c < N; c++) d[c] = INF;
        qh = qt = 0; d[goals[g]] = 0; q[qt++] = goals[g];
        while (qh < qt) {
            int c = q[qh++];
            for (int k = 0; k < 4; k++) {         /* acorn at c came from c-dir, player stood at c-2dir */
                int from = c - dir4[k], pl = from - dir4[k];
                if (from < 0 || pl < 0 || from >= N || pl >= N) continue;
                if (floor_[from] && floor_[pl] && d[from] == INF) { d[from] = d[c] + 1; q[qt++] = from; }
            }
        }
    }
    for (int c = 0; c < N; c++) {
        dead[c] = 1;
        if (floor_[c]) for (int g = 0; g < NG; g++) if (gdist[g][c] < INF) { dead[c] = 0; break; }
    }
    /* walking distances */
    pdist = malloc((size_t)N * N * sizeof *pdist);
    for (size_t i = 0; i < (size_t)N * N; i++) pdist[i] = 0xffff;
    for (int s = 0; s < N; s++) if (floor_[s]) {
        uint16_t *d = pdist + (size_t)s * N;
        qh = qt = 0; d[s] = 0; q[qt++] = s;
        while (qh < qt) { int c = q[qh++]; for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (floor_[n] && d[n] == 0xffff) { d[n] = d[c] + 1; q[qt++] = n; } } }
    }
    free(q);
    for (int i = 0; i < NB; i++) if (dead[start_boxes[i]] && !goal[start_boxes[i]]) return 0;
    NF = nf; d1 = d2 = d3 = NULL;
    tables_bytes = (size_t)N * N * 2;
    if (push_mode) return 1;              /* pushes only need the matching bound */
    build_d1();
    size_t table_limit = mem_total / 4;
    tables_bytes = (size_t)N * N * 4;
    if (NB >= 2 && (size_t)nf * nf * nf * 6 <= table_limit) { build_d2(); tables_bytes += (size_t)nf * nf * nf * 2; }
    if (NB >= 3 && (size_t)nf * nf * nf * nf * 6 <= table_limit) { build_d3(); tables_bytes += (size_t)nf * nf * nf * nf * 2; }
    return 1;
}

/* ---------- lower bound ---------- */
static int hung_a[MAXB + 1][MAXB + 1];
static int assignment_cost(const uint16_t *b)      /* min-cost perfect matching acorns -> goals */
{
    int n = NB;
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) {
        int v = gdist[j][b[i]];
        hung_a[i + 1][j + 1] = v >= INF ? 100000 : v;
    }
    static int u[MAXB + 1], v[MAXB + 1], p[MAXB + 1], way[MAXB + 1], minv[MAXB + 1];
    static char used[MAXB + 1];
    for (int i = 0; i <= n; i++) u[i] = v[i] = p[i] = way[i] = 0;
    for (int i = 1; i <= n; i++) {
        p[0] = i; int j0 = 0;
        for (int j = 0; j <= n; j++) { minv[j] = INF; used[j] = 0; }
        do {
            used[j0] = 1; int i0 = p[j0], delta = INF, j1 = 0;
            for (int j = 1; j <= n; j++) if (!used[j]) {
                int cur = hung_a[i0][j] - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int j = 0; j <= n; j++) { if (used[j]) { u[p[j]] += delta; v[j] -= delta; } else minv[j] -= delta; }
            j0 = j1;
        } while (p[j0] != 0);
        do { int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
    }
    int cost = 0;
    for (int j = 1; j <= n; j++) cost += hung_a[p[j]][j];
    return cost >= 100000 ? INF : cost;
}

/* d1[box*N+player]: fewest moves to get this one acorn onto any spot with no other acorns around */
static void build_d1(void)
{
    size_t n = (size_t)N * N;
    d1 = malloc(n * 2);
    for (size_t i = 0; i < n; i++) d1[i] = 0xffff;
    uint32_t *q = malloc(n * 4); size_t qh = 0, qt = 0;
    for (int g = 0; g < NG; g++) for (int p = 0; p < N; p++)
        if (floor_[p] && p != goals[g]) { d1[(size_t)goals[g] * N + p] = 0; q[qt++] = goals[g] * N + p; }
    while (qh < qt) {
        uint32_t v = q[qh++]; int b = v / N, p = v % N; uint16_t c = d1[v] + 1;
        for (int k = 0; k < 4; k++) {
            int pp = p - dir4[k];                         /* player came from pp */
            if (!floor_[pp]) continue;
            if (pp != b) {                                /* walked */
                size_t u = (size_t)b * N + pp;
                if (d1[u] == 0xffff) { d1[u] = c; q[qt++] = u; }
            }
            if (b == p + dir4[k]) {                       /* pushed: acorn was at p */
                size_t u = (size_t)p * N + pp;
                if (d1[u] == 0xffff) { d1[u] = c; q[qt++] = u; }
            }
        }
    }
    free(q);
}
static void build_d2(void)
{
    size_t n = (size_t)NF * NF * NF;
    d2 = malloc(n * 2);
    for (size_t i = 0; i < n; i++) d2[i] = 0xffff;
    uint32_t *q = malloc(n * 4); size_t qh = 0, qt = 0;
    int *isg = calloc(NF, sizeof(int));
    for (int i = 0; i < NF; i++) isg[i] = goal[fcell[i]];
    for (int a = 0; a < NF; a++) if (isg[a]) for (int b = 0; b < NF; b++) if (b != a && isg[b])
        for (int p = 0; p < NF; p++) if (p != a && p != b) { size_t v = ((size_t)a * NF + b) * NF + p; d2[v] = 0; q[qt++] = v; }
    while (qh < qt) {
        uint32_t v = q[qh++]; int p = v % NF, b = (v / NF) % NF, a = v / NF / NF; uint16_t c = d2[v] + 1;
        int pc = fcell[p];
        for (int k = 0; k < 4; k++) {
            int ppc = pc - dir4[k];
            if (!floor_[ppc]) continue;
            int pp = fidx[ppc];
            if (pp != a && pp != b) {                             /* walked */
                size_t u = ((size_t)a * NF + b) * NF + pp;
                if (d2[u] == 0xffff) { d2[u] = c; q[qt++] = u; }
            }
            int fwd = pc + dir4[k];
            if (floor_[fwd] && pp != a && pp != b) {
                int f = fidx[fwd];
                if (f == a) { size_t u = ((size_t)p * NF + b) * NF + pp; if (d2[u] == 0xffff) { d2[u] = c; q[qt++] = u; } }
                if (f == b) { size_t u = ((size_t)a * NF + p) * NF + pp; if (d2[u] == 0xffff) { d2[u] = c; q[qt++] = u; } }
            }
        }
    }
    free(q); free(isg);
}
static void build_d3(void)
{
    size_t n = (size_t)NF * NF * NF * NF;
    d3 = malloc(n * 2);
    for (size_t i = 0; i < n; i++) d3[i] = 0xffff;
    uint32_t *q = malloc(n * 4); size_t qh = 0, qt = 0;
    for (int a = 0; a < NF; a++) if (goal[fcell[a]]) for (int b = 0; b < NF; b++) if (b != a && goal[fcell[b]])
        for (int c = 0; c < NF; c++) if (c != a && c != b && goal[fcell[c]])
            for (int p = 0; p < NF; p++) if (p != a && p != b && p != c) { size_t v = IX3(a, b, c, p); d3[v] = 0; q[qt++] = v; }
    while (qh < qt) {
        uint32_t v = q[qh++]; int p = v % NF, c = (v / NF) % NF, b = (v / NF / NF) % NF, a = v / NF / NF / NF;
        uint16_t cost = d3[v] + 1; int pc = fcell[p];
        for (int k = 0; k < 4; k++) {
            int ppc = pc - dir4[k];
            if (!floor_[ppc]) continue;
            int pp = fidx[ppc];
            if (pp == a || pp == b || pp == c) continue;
            size_t u = IX3(a, b, c, pp);
            if (d3[u] == 0xffff) { d3[u] = cost; q[qt++] = u; }
            int fwd = pc + dir4[k];
            if (!floor_[fwd]) continue;
            int f = fidx[fwd];
            if (f == a) { u = IX3(p, b, c, pp); if (d3[u] == 0xffff) { d3[u] = cost; q[qt++] = u; } }
            if (f == b) { u = IX3(a, p, c, pp); if (d3[u] == 0xffff) { d3[u] = cost; q[qt++] = u; } }
            if (f == c) { u = IX3(a, b, p, pp); if (d3[u] == 0xffff) { d3[u] = cost; q[qt++] = u; } }
        }
    }
    free(q);
}
static int heuristic(const uint16_t *b, int pl)    /* INF = deadlock */
{
    int m = assignment_cost(b);
    if (m >= INF) return INF;
    if (m == 0 || push_mode) return m;
    int walk = 0xffff, one = 0;
    for (int i = 0; i < NB; i++) if (!goal[b[i]]) {
        int d = pdist[(size_t)pl * N + b[i]]; if (d < walk) walk = d;
        int e = d1[(size_t)b[i] * N + pl]; if (e > one) one = e;
    }
    walk = walk == 0xffff ? 0 : walk - 1;
    if (d2) {
        int pf = fidx[pl], bi = -1, bj = -1, best = -1;
        for (int i = 0; i < NB; i++) for (int j = i + 1; j < NB; j++) {
            if (goal[b[i]] && goal[b[j]]) continue;
            int e = d2[((size_t)fidx[b[i]] * NF + fidx[b[j]]) * NF + pf];
            if (e > best) { best = e; bi = i; bj = j; }
        }
        if (best > one) one = best;
        if (d3 && bi >= 0) for (int k = 0; k < NB; k++) if (k != bi && k != bj) {
            int e = d3[IX3(fidx[b[bi]], fidx[b[bj]], fidx[b[k]], pf)];
            if (e > one) one = e;
        }
    }
    int h1 = m * (K + 1) + walk * K, h2 = one * K;
    return h1 > h2 ? h1 : h2;
}

/* ---------- frozen acorns ---------- */
static char *isbox, *tmpwall;
static int frozen(int c);
static int blocked_axis(int c, int d)
{
    int a = c - d, b = c + d;
    if (wall[a] || tmpwall[a] || wall[b] || tmpwall[b]) return 1;
    if (dead[a] && dead[b]) return 1;
    if (isbox[a] && frozen(a)) return 1;
    if (isbox[b] && frozen(b)) return 1;
    return 0;
}
static int frozen(int c)                           /* acorn at c can never move again */
{
    tmpwall[c] = 1;
    int r = blocked_axis(c, 1) && blocked_axis(c, W);
    tmpwall[c] = 0;
    return r;
}
static int freeze_deadlock(int c)                  /* after pushing an acorn to c */
{
    if (!goal[c] && frozen(c)) return 1;
    for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (isbox[n] && !goal[n] && frozen(n)) return 1; }
    if (goal[c] && frozen(c)) {                    /* frozen on a goal is fine unless it froze a neighbour */
        for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (isbox[n] && !goal[n]) { tmpwall[c] = 1; int f = frozen(n); tmpwall[c] = 0; if (f) return 1; } }
    }
    return 0;
}

/* ---------- PI-corrals (fewest pushes only) ----------
   A corral is an area the player can't reach, closed off by walls and acorns (its barrier). It's a
   PI-corral when every push of a barrier acorn that could ever happen before the barrier changes is
   a push into the corral that the player can make right now. A corral that still has work in it
   (an empty spot inside, or a barrier acorn off its spot) has to be opened by one of those pushes,
   and making it first never costs an extra push (the other pushes can follow it unchanged), so the
   search only needs those pushes. With none possible, the position is a dead end. */
static int *clab, *cq; static char *cbnd; static int corral_on = 1;
static long stat_pi, stat_pidead;
static int pi_corral(const uint16_t *cur, const int *wd, char *allowed)   /* -1 dead end, 0 none, 1 restricted */
{
    int best = -1, best_n = 1 << 30, nreg = 0;
    static char cand[4 * (MAXB + 1)];
    for (int c = 0; c < N; c++) clab[c] = -1;
    for (int c0 = 0; c0 < N; c0++) {
        if (!floor_[c0] || isbox[c0] || wd[c0] >= 0 || clab[c0] >= 0) continue;
        int r = nreg++, qh = 0, qt = 0, unfinished = 0;
        cq[qt++] = c0; clab[c0] = r;
        while (qh < qt) {
            int c = cq[qh++];
            if (goal[c]) unfinished = 1;                     /* an empty spot inside */
            for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (floor_[n] && !isbox[n] && clab[n] < 0) { clab[n] = r; cq[qt++] = n; } }
        }
        /* the barrier: acorns next to the area */
        for (int i = 0; i < NB; i++) {
            int b = cur[i]; cbnd[b] = 0;
            for (int k = 0; k < 4; k++) if (clab[b + dir4[k]] == r) cbnd[b] = 1;
        }
        int ok = 1, n = 0;
        memset(cand, 0, (size_t)4 * NB);
        for (int i = 0; i < NB && ok; i++) {
            int b = cur[i];
            if (!cbnd[b]) continue;
            if (!goal[b]) unfinished = 1;
            for (int k = 0; k < 4 && ok; k++) {
                int x = b - dir4[k], y = b + dir4[k];
                if (!floor_[x] || clab[x] == r) continue;         /* no player square outside the corral */
                if (isbox[x] && cbnd[x]) continue;                /* blocked by the barrier itself */
                if (!floor_[y] || dead[y] || (isbox[y] && cbnd[y])) continue;   /* can't (or never should) */
                if (clab[y] == r && wd[x] >= 0) { cand[i * 4 + k] = 1; n++; continue; }   /* inward, possible now */
                ok = 0;                                           /* some other push could come first */
            }
        }
        for (int i = 0; i < NB; i++) cbnd[cur[i]] = 0;
        if (!ok || !unfinished) continue;
        if (n == 0) return -1;
        if (n < best_n) { best_n = n; best = r; memcpy(allowed, cand, 4 * NB); }
    }
    return best >= 0;
}

/* ---------- hash table ---------- */
static void pack(const uint16_t *x, uint8_t *out)
{
    if (byte_mode) for (int i = 0; i < S; i++) out[i] = (uint8_t)fidx[x[i]];
    else memcpy(out, x, S * 2);
}
static void unpack(uint32_t i, uint16_t *x)
{
    const uint8_t *p = st + (size_t)i * SB;
    if (byte_mode) for (int k = 0; k < S; k++) x[k] = (uint16_t)fcell[p[k]];
    else memcpy(x, p, S * 2);
}
static uint64_t hash_state(const uint8_t *p)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < SB; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h ^ (h >> 31);
}
static uint32_t *find_slot(const uint8_t *p)
{
    uint64_t h = hash_state(p) & tabmask;
    for (;;) {
        uint32_t v = tab[h];
        if (!v || !memcmp(st + (size_t)(v - 1) * SB, p, SB)) return &tab[h];
        h = (h + 1) & tabmask;
    }
}
static void heap_push(uint32_t f, uint32_t g, uint32_t i)
{
    if (hn == hcap) { hcap = hcap ? hcap * 2 : 1 << 16; heap = realloc(heap, hcap * sizeof *heap); }
    size_t k = hn++;
    HE e = { f, g, i };
    while (k) {
        size_t p = (k - 1) / 2;
        if (heap[p].f < f || (heap[p].f == f && heap[p].g >= g)) break;
        heap[k] = heap[p]; k = p;
    }
    heap[k] = e;
}
static HE heap_pop(void)
{
    HE top = heap[0], e = heap[--hn];
    size_t k = 0;
    for (;;) {
        size_t c = 2 * k + 1;
        if (c >= hn) break;
        if (c + 1 < hn && (heap[c + 1].f < heap[c].f || (heap[c + 1].f == heap[c].f && heap[c + 1].g > heap[c].g))) c++;
        if (e.f < heap[c].f || (e.f == heap[c].f && e.g >= heap[c].g)) break;
        heap[k] = heap[c]; k = c;
    }
    heap[k] = e;
    return top;
}
static size_t peak_mem;
static size_t mem_used(void)
{
    size_t m = (size_t)cap_nodes * (SB + 8) + (tabmask + 1) * 4 + hcap * sizeof(HE);
    if (m > peak_mem) peak_mem = m;
    return m;
}
static int grow(void)
{
    size_t per = (size_t)SB + 8;
    if (nnodes + 1 >= cap_nodes) {
        uint32_t nc = cap_nodes ? cap_nodes * 2 : 1 << 16;
        if ((size_t)nc * per + (tabmask + 1) * 4 + hcap * sizeof(HE) > mem_limit) nc = cap_nodes + cap_nodes / 8;
        if ((size_t)nc * per + (tabmask + 1) * 4 + hcap * sizeof(HE) > mem_limit) return 0;
        uint8_t *a = realloc(st, (size_t)nc * SB); if (!a) return 0; st = a;
        uint32_t *b = realloc(gcost, (size_t)nc * 4); if (!b) return 0; gcost = b;
        uint32_t *c = realloc(parent, (size_t)nc * 4); if (!c) return 0; parent = c;
        cap_nodes = nc;
    }
    if ((uint64_t)nnodes * 4 >= tabmask * 3) {            /* keep the table under 75% full */
        uint64_t ns = (tabmask + 1) * 2;
        if ((size_t)cap_nodes * per + ns * 4 + hcap * sizeof(HE) <= mem_limit) {
            uint32_t *t = calloc(ns, 4);
            if (t) {
                free(tab); tab = t; tabmask = ns - 1;
                for (uint32_t i = 0; i < nnodes; i++) *find_slot(st + (size_t)i * SB) = i + 1;
            }
        }
        if ((uint64_t)nnodes * 20 >= tabmask * 19) return 0;   /* table too full */
    }
    return 1;
}

static int cmp_u16(const void *a, const void *b) { return *(const uint16_t *)a - *(const uint16_t *)b; }

/* ---------- solution ---------- */
static int walk_path(int from, int to, const char *blocked, char *out)   /* BFS; returns length */
{
    if (from == to) return 0;
    int *prev = malloc(N * sizeof(int)), *q = malloc(N * sizeof(int)), qh = 0, qt = 0;
    for (int i = 0; i < N; i++) prev[i] = -1;
    prev[from] = from; q[qt++] = from;
    while (qh < qt && prev[to] < 0) {
        int c = q[qh++];
        for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (floor_[n] && !blocked[n] && prev[n] < 0) { prev[n] = c; q[qt++] = n; } }
    }
    int len = 0;
    if (prev[to] >= 0) {
        char tmp[8192]; int c = to;
        while (c != from) { int p = prev[c]; for (int k = 0; k < 4; k++) if (p + dir4[k] == c) tmp[len++] = LURD[k]; c = p; }
        for (int i = 0; i < len; i++) out[i] = tmp[len - 1 - i];
    } else len = -1;
    free(prev); free(q);
    return len;
}
static char *build_solution(uint32_t goal_i)
{
    int n = 0; uint32_t i = goal_i;
    while (i) { n++; i = parent[i]; }
    uint32_t *chain = malloc((n + 1) * sizeof *chain);
    i = goal_i; for (int k = n; k >= 0; k--) { chain[k] = i; if (!i) break; i = parent[i]; }
    char *out = malloc(1 << 20); int len = 0, pl = start_pl;
    char *blocked = calloc(N, 1), *inb = calloc(N, 1);
    for (int k = 0; k < n; k++) {
        uint16_t a[MAXB + 1], b[MAXB + 1];
        unpack(chain[k], a); unpack(chain[k + 1], b);
        memset(blocked, 0, N); memset(inb, 0, N);
        for (int j = 0; j < NB; j++) { blocked[a[j]] = 1; inb[b[j]] = 1; }
        int gone = -1, came = -1;                 /* the acorn that moved: from `gone` to `came` */
        for (int j = 0; j < NB; j++) { if (!inb[a[j]]) gone = a[j]; if (!blocked[b[j]]) came = b[j]; }
        int dirk = -1;
        for (int d = 0; d < 4; d++) if (gone >= 0 && gone + dir4[d] == came) dirk = d;
        int l = dirk < 0 ? -1 : walk_path(pl, gone - dir4[dirk], blocked, out + len);
        if (l < 0) { free(out); out = NULL; break; }
        len += l; out[len++] = toupper(LURD[dirk]); pl = gone;
    }
    if (out) out[len] = 0;
    free(blocked); free(inb); free(chain);
    return out;
}
static int replay(const char *sol, int *moves, int *pushes)   /* 1 if it solves the level */
{
    char *box = calloc(N, 1); int pl = start_pl;
    for (int i = 0; i < NB; i++) box[start_boxes[i]] = 1;
    *moves = *pushes = 0;
    for (const char *p = sol; *p; p++) {
        const char *k = strchr(LURD, tolower(*p)); if (!k) { free(box); return 0; }
        int d = dir4[k - LURD], n = pl + d;
        if (!floor_[n]) { free(box); return 0; }
        if (box[n]) {
            if (!isupper(*p) || !floor_[n + d] || box[n + d]) { free(box); return 0; }
            box[n] = 0; box[n + d] = 1; (*pushes)++;
        } else if (isupper(*p)) { free(box); return 0; }
        pl = n; (*moves)++;
    }
    int ok = 1;
    for (int c = 0; c < N; c++) if (box[c] && !goal[c]) ok = 0;
    free(box);
    return ok;
}

/* ---------- search ---------- */
enum { R_SOLVED, R_UNSOLVABLE, R_MEMORY, R_TIME };
static int *nq;
static int norm_player(int p)          /* fewest pushes: the player stands for the whole area it can reach */
{
    static char *seen; static int seen_n;
    if (seen_n != N) { free(seen); seen = calloc(N, 1); seen_n = N; }
    int qh = 0, qt = 0, best = p; nq[qt++] = p; seen[p] = 1;
    while (qh < qt) { int c = nq[qh++]; if (c < best) best = c; for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (floor_[n] && !isbox[n] && !seen[n]) { seen[n] = 1; nq[qt++] = n; } } }
    for (int i = 0; i < qt; i++) seen[nq[i]] = 0;
    return best;
}
static long stat_expanded; static uint32_t stat_bound;
static uint32_t ub_cost = 0xffffffffu;    /* a known solution's cost: only cheaper ones are searched for */
static int solve(double tlimit, char **solution)
{
    S = NB + 1; SB = byte_mode ? S : 2 * S;
    mem_limit = mem_total > tables_bytes + ((size_t)256 << 20) ? mem_total - tables_bytes : (size_t)256 << 20;
    nnodes = 0; cap_nodes = 0; hn = 0; peak_mem = 0; stat_expanded = 0; stat_bound = 0;
    tabmask = (1 << 20) - 1; tab = calloc(tabmask + 1, 4);
    st = NULL; gcost = parent = NULL;
    isbox = calloc(N, 1); tmpwall = calloc(N, 1);
    int res = R_UNSOLVABLE; *solution = NULL;
    double t0 = now(), tlast = t0;
    uint16_t s0[MAXB + 1]; uint8_t pk[2 * (MAXB + 1)];
    memcpy(s0, start_boxes, NB * 2); qsort(s0, NB, 2, cmp_u16); s0[NB] = start_pl;
    nq = malloc(N * sizeof(int)); clab = malloc(N * sizeof(int)); cq = malloc(N * sizeof(int)); cbnd = calloc(N, 1);
    stat_pi = stat_pidead = 0;
    if (push_mode) { for (int i = 0; i < NB; i++) isbox[s0[i]] = 1; s0[NB] = norm_player(start_pl); for (int i = 0; i < NB; i++) isbox[s0[i]] = 0; }
    grow();
    pack(s0, pk);
    memcpy(st, pk, SB); gcost[0] = 0; parent[0] = 0; nnodes = 1;
    *find_slot(pk) = 1;
    int h0 = heuristic(s0, start_pl);
    if (h0 < INF && (uint32_t)h0 < ub_cost) heap_push(h0, 0, 0);
    uint32_t pea_late = pea_c >= (1 << 20) ? 0xffffffffu : (uint32_t)pea_c * U, pea = 0xffffffffu;
    int *wd = malloc(N * sizeof(int)), *q = malloc(N * sizeof(int));
    while (hn) {
        HE e = heap_pop();
        if (e.g != gcost[e.i]) continue;                    /* stale entry */
        uint16_t cur[MAXB + 1]; unpack(e.i, cur);
        int unsolved = 0;
        for (int i = 0; i < NB; i++) if (!goal[cur[i]]) { unsolved = 1; break; }
        if (!unsolved) { *solution = build_solution(e.i); res = R_SOLVED; break; }
        stat_expanded++;
        if (pea != pea_late && mem_used() > mem_limit / 2) pea = pea_late;   /* memory getting full: store fewer children */
        if (e.f > stat_bound) stat_bound = e.f;
        if ((stat_expanded & 4095) == 0) {
            double t = now();
            if (tlimit > 0 && t - t0 > tlimit) { res = R_TIME; break; }
            if (t - tlast > 10) {
                tlast = t;
                fprintf(stderr, "\r    %.0fs  %u states  %zu MB  bound %u %s   ", t - t0, nnodes, mem_used() >> 20, e.f / U, push_mode ? "pushes" : "moves");
                fflush(stderr);
            }
        }
        for (int i = 0; i < NB; i++) isbox[cur[i]] = 1;
        /* walking distances for the player */
        for (int c = 0; c < N; c++) wd[c] = -1;
        int qh = 0, qt = 0; wd[cur[NB]] = 0; q[qt++] = cur[NB];
        while (qh < qt) { int c = q[qh++]; for (int k = 0; k < 4; k++) { int n = c + dir4[k]; if (floor_[n] && !isbox[n] && wd[n] < 0) { wd[n] = wd[c] + 1; q[qt++] = n; } } }
        int stop = 0;
        uint32_t later = 0xffffffff;                         /* smallest f of children left for later */
        static char allowed[4 * (MAXB + 1)];
        int pic = push_mode && corral_on ? pi_corral(cur, wd, allowed) : 0;
        if (pic < 0) { stat_pidead++; for (int i = 0; i < NB; i++) isbox[cur[i]] = 0; continue; }
        if (pic > 0) stat_pi++;
        for (int i = 0; i < NB && !stop; i++) {
            int b = cur[i];
            for (int k = 0; k < 4; k++) {
                int from = b - dir4[k], to = b + dir4[k];
                if (pic > 0 && !allowed[i * 4 + k]) continue;
                if (wd[from] < 0 || !floor_[to] || isbox[to] || dead[to]) continue;
                isbox[b] = 0; isbox[to] = 1;
                int dl = freeze_deadlock(to);
                isbox[to] = 0; isbox[b] = 1;
                if (dl) continue;
                uint16_t nx[MAXB + 1]; memcpy(nx, cur, S * 2);
                nx[i] = to; nx[NB] = b;
                if (push_mode) { isbox[b] = 0; isbox[to] = 1; nx[NB] = norm_player(b); isbox[to] = 0; isbox[b] = 1; }
                /* keep the acorns sorted */
                for (int j = i; j > 0 && nx[j] < nx[j - 1]; j--) { uint16_t t = nx[j]; nx[j] = nx[j - 1]; nx[j - 1] = t; }
                for (int j = i; j < NB - 1 && nx[j] > nx[j + 1]; j++) { uint16_t t = nx[j]; nx[j] = nx[j + 1]; nx[j + 1] = t; }
                uint32_t g = push_mode ? e.g + 1 : e.g + (wd[from] + 1) * K + 1;
                pack(nx, pk);
                uint32_t *slot = find_slot(pk);
                if (*slot && gcost[*slot - 1] <= g) continue;   /* already known, as cheap */
                int h = heuristic(nx, nx[NB]);
                if (h >= INF) continue;
                uint32_t f = g + h;
                if (f >= ub_cost) continue;                      /* can't beat the known solution */
                if (pea != 0xffffffffu && f > e.f + pea) { if (f < later) later = f; continue; }   /* not yet: parent stays open */
                if (*slot) {
                    uint32_t j = *slot - 1;
                    gcost[j] = g; parent[j] = e.i;
                    heap_push(f, g, j);
                    continue;
                }
                if (!grow()) { stop = 1; res = R_MEMORY; break; }
                slot = find_slot(pk);
                memcpy(st + (size_t)nnodes * SB, pk, SB);
                gcost[nnodes] = g; parent[nnodes] = e.i;
                *slot = nnodes + 1;
                heap_push(f, g, nnodes);
                nnodes++;
                if (mem_used() > mem_limit) { stop = 1; res = R_MEMORY; break; }
            }
        }
        for (int i = 0; i < NB; i++) isbox[cur[i]] = 0;
        if (stop) break;
        if (later != 0xffffffff) heap_push(later, e.g, e.i);
    }
    mem_used();
    if (tlast > t0) fprintf(stderr, "\r%70s\r", "");
    free(wd); free(q); free(nq); free(clab); free(cq); free(cbnd); free(st); free(gcost); free(parent); free(tab); free(heap); free(isbox); free(tmpwall);
    heap = NULL; hcap = 0;
    return res;
}

/* ---------- level files ---------- */
typedef struct { char name[128]; char **maps; int n; } Set;

static int is_map_line(const char *s)
{
    int hash = 0;
    if (!*s) return 0;
    for (; *s; s++) { if (!strchr(" #@$.*+-_", *s)) return 0; if (*s == '#') hash = 1; }
    return hash;
}
static int load_set(const char *path, const char *fname, Set *s)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    memset(s, 0, sizeof *s);
    const char *nm = strchr(fname, '-') ? strchr(fname, '-') + 1 : fname;
    snprintf(s->name, sizeof s->name, "%.*s", (int)(strlen(nm) - 4), nm);
    char line[1024], buf[16384]; int blen = 0, first = 1, cap = 0;
    for (;;) {
        char *ok = fgets(line, sizeof line, f);
        if (ok) {
            line[strcspn(line, "\r\n")] = 0;
            size_t ll = strlen(line);
            while (ll && line[ll - 1] == ' ') line[--ll] = 0;
        }
        if (ok && first && line[0] == ';') {
            const char *p = line + 1; while (*p == ' ') p++;
            if (*p && !isdigit((unsigned char)*p)) snprintf(s->name, sizeof s->name, "%.120s", p);
            first = 0; continue;
        }
        if (ok && is_map_line(line)) {
            for (char *p = line; *p; p++) if (*p == '-' || *p == '_') *p = ' ';
            if (blen + (int)strlen(line) + 2 < (int)sizeof buf) blen += sprintf(buf + blen, "%s\n", line);
            continue;
        }
        if (blen) {
            if (s->n == cap) { cap = cap ? cap * 2 : 64; s->maps = realloc(s->maps, cap * sizeof *s->maps); }
            buf[blen] = 0; s->maps[s->n++] = strdup(buf); blen = 0;
        }
        if (!ok) break;
    }
    fclose(f);
    return s->n > 0;
}

/* data/fewest.txt and data/fewest-failed.txt */
#define OUT (push_mode ? "data/fewest-pushes.txt" : "data/fewest.txt")
#define FAILED "data/fewest-failed.txt"
static int in_file(const char *file, const char *set, int lv, int *mem, int *secs)
{
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    char line[70000]; int found = 0;
    size_t sl = strlen(set);
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || strncmp(line, set, sl) || line[sl] != '|') continue;
        if (mem) { char tg[8]; snprintf(tg, sizeof tg, "|%s ", SOLVER_TAG); if (!strstr(line, tg)) continue; }   /* other mode or older solver */
        int l = 0, a = 0, b = 0;
        if (sscanf(line + sl + 1, "%d|%d|%d", &l, &a, &b) >= 1 && l == lv) {
            found = 1;
            if (mem && a > *mem) *mem = a;
            if (secs && b > *secs) *secs = b;
        }
    }
    fclose(f);
    return found;
}

/* the best known solution for this level in the current mode, from our other file and from the
   checked online solutions (data/online-best.txt); replayed here before it's trusted */
static char *known_solution(const char *set, int lv, int *km, int *kp)
{
    const char *files[2] = { push_mode ? "data/fewest.txt" : "data/fewest-pushes.txt", "data/online-best.txt" };
    char *best = NULL; size_t sl = strlen(set);
    static char line[1 << 17];
    for (int fi = 0; fi < 2; fi++) {
        FILE *f = fopen(files[fi], "r");
        if (!f) continue;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || strncmp(line, set, sl) || line[sl] != '|') continue;
            int l, m, pu; char *sol;
            if (sscanf(line + sl + 1, "%d|%d|%d|", &l, &m, &pu) != 3 || l != lv) continue;
            sol = line + sl + 1;
            for (int k = 0; k < 3; k++) sol = strchr(sol, '|') + 1;
            sol[strcspn(sol, "|\r\n")] = 0;
            int rm, rp;
            if (!replay(sol, &rm, &rp)) continue;
            int better = !best || (push_mode ? (rp < *kp || (rp == *kp && rm < *km)) : (rm < *km || (rm == *km && rp < *kp)));
            if (better) { free(best); best = strdup(sol); *km = rm; *kp = rp; }
        }
        fclose(f);
    }
    return best;
}

int main(int argc, char **argv)
{
    const char *only[32]; int nonly = 0, all = 0, retry = 0, lv_from = 1, lv_to = 100000;
    int mem_mb = 3000, secs = 1800, modes = 3, use_known = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc && nonly < 32) only[nonly++] = argv[++i];
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) { i++; if (sscanf(argv[i], "%d-%d", &lv_from, &lv_to) == 1) lv_to = lv_from; }
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mem_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a")) all = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) pea_c = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r")) retry = 1;
        else if (!strcmp(argv[i], "-p")) modes = 2;
        else if (!strcmp(argv[i], "-M")) modes = 1;
        else if (!strcmp(argv[i], "-nopi")) corral_on = 0;
        else if (!strcmp(argv[i], "-noknown")) use_known = 0;
        else { fprintf(stderr, "usage: %s [-s set] [-l 5 | -l 5-20] [-m MB] [-t seconds] [-a] [-r] [-c moves] [-p | -M]\n", argv[0]); return 1; }
    }
    mem_total = (size_t)mem_mb << 20;
    mkdir("data", 0755);
    for (push_mode = 0; push_mode < 2; push_mode++) {   /* headers so the files explain themselves */
        FILE *f = fopen(OUT, "r");
        if (!f) {
            f = fopen(OUT, "w");
            if (f && !push_mode) fprintf(f, "# Fewest moves for each level, proven by tools/fewest (A* search). Among the fewest-move\n"
                              "# solutions, the one with the fewest pushes. Format: set|level|moves|pushes|solution (LURD;\n"
                              "# lowercase walks, uppercase pushes). Levels not listed have not been proven.\n");
            if (f && push_mode) fprintf(f, "# Fewest pushes for each level, proven by tools/fewest (A* search). The moves are those of\n"
                              "# the solution found (shortest walks between its pushes), not the fewest possible for that push\n"
                              "# count. Format: set|level|moves|pushes|solution (LURD). Levels not listed have not been proven.\n");
        }
        if (f) fclose(f);
    }
    DIR *d = opendir("levels");
    if (!d) { fprintf(stderr, "Run this from the Stumped folder (the one with levels/ in it).\n"); return 1; }
    char *names[64]; int nn = 0; struct dirent *e;
    while ((e = readdir(d)) && nn < 64) { size_t l = strlen(e->d_name); if (l > 4 && !strcmp(e->d_name + l - 4, ".txt") && strcmp(e->d_name, "CREDITS.txt")) names[nn++] = strdup(e->d_name); }
    closedir(d);
    for (int i = 0; i < nn; i++) for (int j = i + 1; j < nn; j++) if (strcmp(names[j], names[i]) < 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; }
    int n_solved = 0, n_failed = 0, n_skipped = 0;
    double t_all = now();
    for (push_mode = 1; push_mode >= 0; push_mode--) {   /* fewest pushes first: it's much quicker */
    if (!(modes & (push_mode ? 2 : 1))) continue;
    printf("\n== Fewest %s ==\n", push_mode ? "pushes" : "moves");
    for (int si = 0; si < nn; si++) {
        char path[512]; snprintf(path, sizeof path, "levels/%s", names[si]);
        Set s;
        if (!load_set(path, names[si], &s)) continue;
        int want = 0;
        if (nonly) { for (int k = 0; k < nonly; k++) if (!strcasecmp(only[k], s.name)) want = 1; }
        else want = all || !strncmp(s.name, "Microban", 8);
        if (!want) continue;
        printf("%s (%d levels)\n", s.name, s.n); fflush(stdout);
        for (int lv = 1; lv <= s.n; lv++) {
            if (lv < lv_from || lv > lv_to) continue;
            if (in_file(OUT, s.name, lv, NULL, NULL)) { n_skipped++; continue; }
            int fm = 0, fs = 0;
            if (in_file(FAILED, s.name, lv, &fm, &fs) && !retry && mem_mb <= fm && (secs && secs <= fs)) {
                printf("  %3d  skipped (failed before with %d MB, %d s; use -r or more -m/-t)\n", lv, fm, fs);
                n_skipped++; continue;
            }
            if (!load_map(s.maps[lv - 1])) { printf("  %3d  can't read this level\n", lv); free_level(); continue; }
            printf("  %3d  %d acorns ... ", lv, NB); fflush(stdout);
            double t0 = now(); char *sol = NULL;
            int km = 0, kp = 0;
            char *known = use_known ? known_solution(s.name, lv, &km, &kp) : NULL;
            ub_cost = known ? (push_mode ? (uint32_t)kp : (uint32_t)km * K + (uint32_t)kp) : 0xffffffffu;
            if (known) { printf("(known: %d %s) ", push_mode ? kp : km, push_mode ? "pushes" : "moves"); fflush(stdout); }
            int r = solve(secs, &sol);
            if (r == R_UNSOLVABLE && known) { r = R_SOLVED; sol = known; known = NULL; }   /* nothing cheaper: the known one is the fewest */
            free(known);
            long exp = stat_expanded;
            double t = now() - t0;
            int moves = 0, pushes = 0;
            if (r == R_SOLVED && sol && replay(sol, &moves, &pushes)) {
                printf("%d moves, %d pushes  (%.1fs, %ld expanded, %u stored, %zu MB)\n", moves, pushes, t, exp, nnodes, peak_mem >> 20);
                if (getenv("FW_STATS")) printf("        PI-corral %ld, corral dead ends %ld\n", stat_pi, stat_pidead);
                FILE *f = fopen(OUT, "a");
                if (f) { fprintf(f, "%s|%d|%d|%d|%s\n", s.name, lv, moves, pushes, sol); fclose(f); }
                n_solved++;
            } else {
                const char *why = r == R_MEMORY ? "out of memory" : r == R_TIME ? "out of time" : r == R_UNSOLVABLE ? "no solution found (?)" : "solution failed its check (bug!)";
                printf("%s; it's at least %u %s  (%.0fs, %ld expanded, %u stored, %zu MB)\n", why, stat_bound / U, push_mode ? "pushes" : "moves", t, exp, nnodes, peak_mem >> 20);
                FILE *f = fopen(FAILED, "a");
                if (f) { fprintf(f, "%s|%d|%d|%d|%s %s (at least %u %s)\n", s.name, lv, mem_mb, secs ? secs : 999999, SOLVER_TAG, why, stat_bound / U, push_mode ? "pushes" : "moves"); fclose(f); }
                n_failed++;
            }
            fflush(stdout);
            free(sol); free_level();
        }
        for (int i = 0; i < s.n; i++) free(s.maps[i]);
        free(s.maps);
    }
    }
    printf("\nDone in %.0f minutes: %d newly proven, %d not proven, %d already done or skipped.\nResults are in data/fewest.txt and data/fewest-pushes.txt.\n",
           (now() - t_all) / 60, n_solved, n_failed, n_skipped);
    return 0;
}
