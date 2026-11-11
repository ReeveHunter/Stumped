/*
 * Stumped — the level maker, level codes, the solver, and adding level sets (included from main.c)
 *
 * My levels: levels you make (or enter as codes) live in a set of their own, "My levels", always the
 *   last set. A level gets a code once it's been solved (the clear check), so every code is solvable.
 * The maker: a 20 x 16 canvas. Pick a tool (hedge, acorn, spot, your animal, eraser) with L1/R1 and paint
 *   with A (hold A and move to paint a line); B erases; Select undoes; X zooms; Y test-plays; Start: menu.
 * The solver: before a test play it looks for the fewest moves (a search over pushes, with the walking
 *   between them as the cost), a few seconds at most. It can prove a level can't be solved.
 * Codes: the level packed into letters and digits (Crockford base 32: no I, L, O or U), shown in rows of
 *   ten as two groups of five, with a checksum at the end that catches typos.
 * Adding sets: on the Miyoo, drop a .txt file into the levels folder. On the web, "Add a level set" reads
 *   a .txt file you pick; it's kept in the browser.
 */

static void open_levels(int set, int sel);

/* QR codes, from Project Nayuki's QR Code generator library (MIT licence, see src/qrcodegen.c) */
#include "qrcodegen.c"

#define ED_W 20
#define ED_H 16

/* ---------------- a level as a grid of characters: ' ' # $ . * @ + ---------------- */
typedef struct { int w, h; char c[ED_H][ED_W]; } Grid;

static void grid_clear(Grid *g, int w, int h) { g->w = w; g->h = h; memset(g->c, ' ', sizeof g->c); }

/* map text -> grid; 0 if it doesn't fit 20 x 16. With canvas = 1 it's centred on the full 20 x 16 canvas. */
static int grid_from_map(const char *map, Grid *g, int canvas)
{
    char rows[ED_H][ED_W + 1];
    int h = 0, w = 0, n = 0;
    const char *p = map;
    while (*p) {
        const char *e = strchr(p, '\n');
        int l = e ? (int)(e - p) : (int)strlen(p);
        while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\r')) l--;
        if (l < 0) l = 0;
        if (h >= ED_H || l > ED_W) {
            /* only empty rows may follow */
            if (l == 0) { if (!e) break; p = e + 1; continue; }
            return 0;
        }
        memcpy(rows[h], p, l); rows[h][l] = 0;
        if (l > w) w = l;
        h++;
        if (l) n = h;
        if (!e) break;
        p = e + 1;
    }
    h = n;                                   /* drop trailing empty rows */
    if (!h || !w) return 0;
    grid_clear(g, canvas ? ED_W : w, canvas ? ED_H : h);
    int ox = canvas ? (ED_W - w) / 2 : 0, oy = canvas ? (ED_H - h) / 2 : 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; rows[y][x]; x++) {
            char c = rows[y][x];
            if (c == '-' || c == '_') c = ' ';
            if (!strchr(" #$.*@+", c)) c = ' ';
            g->c[oy + y][ox + x] = c;
        }
    return 1;
}

/* grid -> map text, cropped to what's there; "" if the grid is empty */
static void grid_to_map(const Grid *g, char *out, int cap)
{
    int x0 = g->w, y0 = g->h, x1 = -1, y1 = -1;
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++)
            if (g->c[y][x] != ' ') { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    int n = 0;
    out[0] = 0;
    if (x1 < 0) return;
    for (int y = y0; y <= y1; y++) {
        int e = x1;
        while (e >= x0 && g->c[y][e] == ' ') e--;
        for (int x = x0; x <= e && n < cap - 2; x++) out[n++] = g->c[y][x];
        if (n < cap - 1) out[n++] = '\n';
    }
    out[n] = 0;
}

/* what's wrong with a level, or NULL if it's ready to play */
static const char *grid_problem(const Grid *g)
{
    static char msg[64];
    int np = 0, nb = 0, ng = 0, loose = 0, px = 0, py = 0;
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            char c = g->c[y][x];
            if (c == '@' || c == '+') { np++; px = x; py = y; }
            if (c == '$' || c == '*') nb++;
            if (c == '.' || c == '*' || c == '+') ng++;
            if (c == '$') loose++;
        }
    if (!np) return "Put your animal in the level";
    if (!nb) return "It needs at least one acorn";
    if (nb != ng) { snprintf(msg, sizeof msg, "%d acorn%s but %d spot%s", nb, nb == 1 ? "" : "s", ng, ng == 1 ? "" : "s"); return msg; }
    if (!loose) return "Every acorn is already on a spot";
    static uint8_t in[ED_H][ED_W];
    static int q[ED_W * ED_H];
    memset(in, 0, sizeof in);
    int qh = 0, qt = 0, open = 0;
    in[py][px] = 1; q[qt++] = py * ED_W + px;
    while (qh < qt) {
        int v = q[qh++], x = v % ED_W, y = v / ED_W;
        if (x == 0 || y == 0 || x == g->w - 1 || y == g->h - 1) open = 1;
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= g->w || ny >= g->h || in[ny][nx] || g->c[ny][nx] == '#') continue;
            in[ny][nx] = 1; q[qt++] = ny * ED_W + nx;
        }
    }
    if (open) return "Close the hedge all the way round";
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++)
            if (strchr("$.*", g->c[y][x]) && !in[y][x]) return "Something's outside the hedge";
    return NULL;
}

static const char *map_problem(const char *map)
{
    static Grid g;
    if (!grid_from_map(map, &g, 0)) return "It's bigger than 20 x 16";
    return grid_problem(&g);
}

/* ---------------- the solver ----------------
   Dijkstra over "push states": where the acorns are, and where the animal stands just after a push.
   The cost is moves: the walk to the push (a breadth-first search) plus the push itself, so the first
   solved state that comes off the queue is a solution with the fewest moves. Acorns are never pushed
   onto dead squares (where they could never reach a spot) or into a frozen 2 x 2 block, which keeps it
   quick and still exact: if the queue runs dry, the level can't be solved. */
#define SV_MAXN 256
#define SV_MAXB 24
#define SV_MAXSTATES 1000000
#define SV_HASH (1 << 21)
#define SV_TIME_MS 6000
enum { SV_IDLE, SV_RUNNING, SV_SOLVED, SV_IMPOSSIBLE, SV_GAVE_UP };

static struct {
    int state;
    int n, nb, k, w, h;
    int nbr[SV_MAXN][4];
    uint8_t goal[SV_MAXN], dead[SV_MAXN];
    int id[ED_H][ED_W], cx[SV_MAXN], cy[SV_MAXN];
    uint8_t *st; uint16_t *cost; int32_t *parent; int ns, cap;
    uint32_t *ht;
    int32_t *hidx; uint16_t *hcost; int hn, hcap;
    int moves, pushes;
    char *sol;                  /* the solution, as moves (l r u d; capitals push), when solved */
    Uint32 spent;
    char map[512];
} SV;

static void sv_free(void)
{
    /* (SV.sol is kept: it's the answer, and goes with the next sv_start) */
    free(SV.st); free(SV.cost); free(SV.parent); free(SV.ht); free(SV.hidx); free(SV.hcost);
    SV.st = NULL; SV.cost = NULL; SV.parent = NULL; SV.ht = NULL; SV.hidx = NULL; SV.hcost = NULL;
}

static uint32_t sv_hash(const uint8_t *s)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < SV.k; i++) { h ^= s[i]; h *= 16777619u; }
    return h;
}

static void sv_heap_push(int idx, int c)
{
    if (SV.hn == SV.hcap) {
        SV.hcap *= 2;
        SV.hidx = realloc(SV.hidx, SV.hcap * sizeof *SV.hidx);
        SV.hcost = realloc(SV.hcost, SV.hcap * sizeof *SV.hcost);
    }
    int i = SV.hn++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (SV.hcost[p] <= c) break;
        SV.hidx[i] = SV.hidx[p]; SV.hcost[i] = SV.hcost[p]; i = p;
    }
    SV.hidx[i] = idx; SV.hcost[i] = (uint16_t)c;
}

static int sv_heap_pop(int *c)
{
    int top = SV.hidx[0];
    *c = SV.hcost[0];
    int li = SV.hidx[--SV.hn], lc = SV.hcost[SV.hn], i = 0;
    for (;;) {
        int a = 2 * i + 1, b = a + 1, m = i;
        int mc = lc;
        if (a < SV.hn && SV.hcost[a] < mc) { m = a; mc = SV.hcost[a]; }
        if (b < SV.hn && SV.hcost[b] < mc) { m = b; }
        if (m == i) break;
        SV.hidx[i] = SV.hidx[m]; SV.hcost[i] = SV.hcost[m]; i = m;
    }
    if (SV.hn) { SV.hidx[i] = li; SV.hcost[i] = (uint16_t)lc; }
    return top;
}

/* find (or add) a state; returns its index, *fresh = 1 if new. -1 when full. */
static int sv_lookup(const uint8_t *s, int *fresh)
{
    uint32_t h = sv_hash(s) & (SV_HASH - 1);
    for (;;) {
        uint32_t e = SV.ht[h];
        if (!e) break;
        if (!memcmp(SV.st + (size_t)(e - 1) * SV.k, s, SV.k)) { *fresh = 0; return (int)e - 1; }
        h = (h + 1) & (SV_HASH - 1);
    }
    if (SV.ns >= SV_MAXSTATES) return -1;
    if (SV.ns == SV.cap) {                      /* grow as needed (most levels need very little) */
        int nc = SV.cap * 2 > SV_MAXSTATES ? SV_MAXSTATES : SV.cap * 2;
        uint8_t *a = realloc(SV.st, (size_t)nc * SV.k);
        uint16_t *b = realloc(SV.cost, nc * sizeof *b);
        int32_t *c = realloc(SV.parent, nc * sizeof *c);
        if (a) SV.st = a;
        if (b) SV.cost = b;
        if (c) SV.parent = c;
        if (!a || !b || !c) return -1;
        SV.cap = nc;
    }
    int i = SV.ns++;
    memcpy(SV.st + (size_t)i * SV.k, s, SV.k);
    SV.ht[h] = (uint32_t)i + 1;
    *fresh = 1;
    return i;
}

static int cmp_u8(const void *a, const void *b) { return *(const uint8_t *)a - *(const uint8_t *)b; }

/* start solving a map; the work happens in sv_run() a slice at a time */
static void sv_start(const char *map)
{
    sv_free();
    free(SV.sol);
    memset(&SV, 0, sizeof SV);
    snprintf(SV.map, sizeof SV.map, "%s", map);
    static Grid g;
    if (!grid_from_map(map, &g, 0)) { SV.state = SV_GAVE_UP; return; }
    SV.w = g.w; SV.h = g.h;
    int px = -1, py = -1;
    for (int y = 0; y < g.h; y++) for (int x = 0; x < g.w; x++) { SV.id[y][x] = -1; if (g.c[y][x] == '@' || g.c[y][x] == '+') { px = x; py = y; } }
    if (px < 0) { SV.state = SV_GAVE_UP; return; }
    /* number the floor the animal can reach (acorns don't block this) */
    static int q[ED_W * ED_H];
    int qh = 0, qt = 0;
    SV.id[py][px] = SV.n; SV.cx[SV.n] = px; SV.cy[SV.n] = py; SV.n++;
    q[qt++] = py * ED_W + px;
    while (qh < qt) {
        int v = q[qh++], x = v % ED_W, y = v / ED_W;
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= g.w || ny >= g.h || SV.id[ny][nx] >= 0 || g.c[ny][nx] == '#') continue;
            if (SV.n >= SV_MAXN) { SV.state = SV_GAVE_UP; return; }
            SV.id[ny][nx] = SV.n; SV.cx[SV.n] = nx; SV.cy[SV.n] = ny; SV.n++;
            q[qt++] = ny * ED_W + nx;
        }
    }
    uint8_t start[SV_MAXB + 1];
    for (int i = 0; i < SV.n; i++) {
        int x = SV.cx[i], y = SV.cy[i];
        char c = g.c[y][x];
        SV.goal[i] = c == '.' || c == '*' || c == '+';
        if (c == '$' || c == '*') { if (SV.nb >= SV_MAXB) { SV.state = SV_GAVE_UP; return; } start[SV.nb++] = (uint8_t)i; }
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            SV.nbr[i][d] = nx >= 0 && ny >= 0 && nx < g.w && ny < g.h ? SV.id[ny][nx] : -1;
        }
    }
    /* dead squares: pull an acorn backwards from every spot; where it can't get to is dead */
    static uint8_t live[SV_MAXN];
    memset(live, 0, sizeof live);
    qh = qt = 0;
    for (int i = 0; i < SV.n; i++) if (SV.goal[i]) { live[i] = 1; q[qt++] = i; }
    while (qh < qt) {
        int b = q[qh++];
        for (int d = 0; d < 4; d++) {
            int a = SV.nbr[b][d], p = a >= 0 ? SV.nbr[a][d] : -1;      /* acorn came from a, pushed from p */
            if (a < 0 || p < 0 || live[a]) continue;
            live[a] = 1; q[qt++] = a;
        }
    }
    for (int i = 0; i < SV.n; i++) SV.dead[i] = !live[i];
    SV.k = SV.nb + 1;
    qsort(start, SV.nb, 1, cmp_u8);
    start[SV.nb] = (uint8_t)SV.id[py][px];
    SV.cap = 65536;
    SV.st = malloc((size_t)SV.cap * SV.k);
    SV.cost = malloc(SV.cap * sizeof *SV.cost);
    SV.parent = malloc(SV.cap * sizeof *SV.parent);
    SV.ht = calloc(SV_HASH, sizeof *SV.ht);
    SV.hcap = 4096;
    SV.hidx = malloc(SV.hcap * sizeof *SV.hidx);
    SV.hcost = malloc(SV.hcap * sizeof *SV.hcost);
    if (!SV.st || !SV.cost || !SV.parent || !SV.ht || !SV.hidx || !SV.hcost) { sv_free(); SV.state = SV_GAVE_UP; return; }
    int fresh;
    int s0 = sv_lookup(start, &fresh);
    SV.cost[s0] = 0; SV.parent[s0] = -1;
    sv_heap_push(s0, 0);
    SV.state = SV_RUNNING;
}

/* is the 2 x 2 block around cell i, with acorns in occ, frozen with an acorn off its spot? */
static int sv_frozen(const uint8_t *occ, int i)
{
    int x = SV.cx[i], y = SV.cy[i];
    for (int oy = -1; oy <= 0; oy++)
        for (int ox = -1; ox <= 0; ox++) {
            int solid = 1, loose = 0;
            for (int j = 0; j < 4 && solid; j++) {
                int xx = x + ox + (j & 1), yy = y + oy + (j >> 1);
                int c = xx >= 0 && yy >= 0 && xx < SV.w && yy < SV.h ? SV.id[yy][xx] : -1;
                if (c < 0) continue;                 /* hedge or outside */
                if (!occ[c]) solid = 0;
                else if (!SV.goal[c]) loose = 1;
            }
            if (solid && loose) return 1;
        }
    return 0;
}

/* the moves from the start to state `end`: for each push, the walk to it, then the push */
static void sv_solution(int end)
{
    static const char LR[4] = { 'l', 'r', 'u', 'd' };
    int n = 0;
    for (int p = end; p >= 0; p = SV.parent[p]) n++;
    int *chain = malloc(n * sizeof *chain);
    SV.sol = malloc(SV.moves + 2);
    if (!chain || !SV.sol) { free(chain); free(SV.sol); SV.sol = NULL; return; }
    for (int p = end, k = n - 1; p >= 0; p = SV.parent[p]) chain[k--] = p;
    static uint8_t occ[SV_MAXN];
    static int prev[SV_MAXN], pdir[SV_MAXN], q[SV_MAXN];
    static char path[SV_MAXN];
    int len = 0;
    for (int k = 0; k + 1 < n; k++) {
        const uint8_t *a = SV.st + (size_t)chain[k] * SV.k, *b = SV.st + (size_t)chain[k + 1] * SV.k;
        memset(occ, 0, SV.n);
        for (int i = 0; i < SV.nb; i++) occ[a[i]] = 1;
        int was = b[SV.nb], to = -1, d = -1;           /* the acorn at `was` went one square to `to` */
        for (int i = 0; i < SV.nb; i++) if (!occ[b[i]]) to = b[i];
        for (int j = 0; j < 4; j++) if (SV.nbr[was][j] == to) d = j;
        int from = d >= 0 ? SV.nbr[was][d ^ 1] : -1;
        if (from < 0) break;
        /* walk from where the animal is to `from` */
        for (int i = 0; i < SV.n; i++) prev[i] = -2;
        int qh = 0, qt = 0, pl = a[SV.nb];
        prev[pl] = -1; q[qt++] = pl;
        while (qh < qt && prev[from] == -2) {
            int v = q[qh++];
            for (int j = 0; j < 4; j++) {
                int u = SV.nbr[v][j];
                if (u < 0 || occ[u] || prev[u] != -2) continue;
                prev[u] = v; pdir[u] = j; q[qt++] = u;
            }
        }
        if (prev[from] == -2) break;
        int m = 0;
        for (int v = from; v != pl; v = prev[v]) path[m++] = LR[pdir[v]];
        while (m && len < SV.moves) SV.sol[len++] = path[--m];
        if (len < SV.moves) SV.sol[len++] = (char)toupper(LR[d]);
    }
    SV.sol[len] = 0;
    free(chain);
}

/* work for up to `ms` milliseconds */
static void sv_run(int ms)
{
    if (SV.state != SV_RUNNING) return;
    Uint32 t0 = now_ms();
    static uint8_t occ[SV_MAXN];
    static int dist[SV_MAXN], q[SV_MAXN];
    uint8_t ns[SV_MAXB + 1];
    int iter = 0;
    for (;;) {
        if ((++iter & 63) == 0 && now_ms() - t0 >= (Uint32)ms) break;
        if (!SV.hn) { SV.state = SV_IMPOSSIBLE; sv_free(); return; }
        int c, s = sv_heap_pop(&c);
        if (c > SV.cost[s]) continue;                    /* a stale entry: found cheaper since */
        const uint8_t *cur = SV.st + (size_t)s * SV.k;
        int done = 1;
        for (int i = 0; i < SV.nb; i++) if (!SV.goal[cur[i]]) { done = 0; break; }
        if (done) {
            SV.moves = c;
            SV.pushes = 0;
            for (int p = s; SV.parent[p] >= 0; p = SV.parent[p]) SV.pushes++;
            sv_solution(s);
            SV.state = SV_SOLVED;
            sv_free();
            return;
        }
        memset(occ, 0, SV.n);
        for (int i = 0; i < SV.nb; i++) occ[cur[i]] = 1;
        for (int i = 0; i < SV.n; i++) dist[i] = -1;
        int qh = 0, qt = 0, pl = cur[SV.nb];
        dist[pl] = 0; q[qt++] = pl;
        while (qh < qt) {
            int v = q[qh++];
            for (int d = 0; d < 4; d++) {
                int u = SV.nbr[v][d];
                if (u < 0 || occ[u] || dist[u] >= 0) continue;
                dist[u] = dist[v] + 1; q[qt++] = u;
            }
        }
        for (int i = 0; i < SV.nb; i++) {
            int b = cur[i];
            for (int d = 0; d < 4; d++) {
                int t = SV.nbr[b][d], from = SV.nbr[b][d ^ 1];   /* DX/DY pairs: 0/1 left/right, 2/3 up/down */
                if (t < 0 || from < 0 || occ[t] || SV.dead[t] || dist[from] < 0) continue;
                occ[b] = 0; occ[t] = 1;
                int frozen = sv_frozen(occ, t);
                occ[t] = 0; occ[b] = 1;
                if (frozen) continue;
                /* the new state: acorn b -> t (kept sorted), the animal where the acorn was */
                int k = 0, put = 0;
                for (int j = 0; j < SV.nb; j++) {
                    int v = j == i ? -1 : cur[j];
                    if (v < 0) continue;
                    if (!put && t < v) { ns[k++] = (uint8_t)t; put = 1; }
                    ns[k++] = (uint8_t)v;
                }
                if (!put) ns[k++] = (uint8_t)t;
                ns[SV.nb] = (uint8_t)b;
                int nc = c + dist[from] + 1;
                if (nc > 65000) continue;
                int fresh, idx = sv_lookup(ns, &fresh);
                if (idx < 0) { SV.state = SV_GAVE_UP; sv_free(); return; }
                if (fresh || nc < SV.cost[idx]) {
                    SV.cost[idx] = (uint16_t)nc; SV.parent[idx] = s;
                    sv_heap_push(idx, nc);
                }
                cur = SV.st + (size_t)s * SV.k;          /* the table may have grown (and moved) */
            }
        }
    }
    SV.spent += now_ms() - t0;
    if (SV.spent > SV_TIME_MS) { SV.state = SV_GAVE_UP; sv_free(); }
}

/* ---------------- level codes ----------------
   5 bits version (1), 5 bits width - 1, 4 bits height - 1, then the squares row by row: a space is 0 and
   a hedge 10, each followed by 3 bits (run length - 1, up to 8); an acorn 110, a spot 1110, an acorn on a
   spot 11110, the animal 111110, the animal on a spot 111111. Padded to whole letters, then two letters of
   checksum. */
static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
#define CODE_MAX 520

static int b32_val(int c)
{
    c = toupper(c);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char *p = c ? strchr(B32, c) : NULL;
    return p ? (int)(p - B32) : -1;
}

static int code_sum(const int *sym, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++) s = (s * 33 + sym[i] + 1) % 1021;
    return s;
}

/* map -> code (no spaces); 0 if the map doesn't fit */
static int code_from_map(const char *map, char *out)
{
    static Grid g;
    if (!grid_from_map(map, &g, 0)) return 0;
    static uint8_t bits[ED_W * ED_H * 7 + 64];
    int nb = 0;
#define PUT(v, n) do { for (int _b = (n) - 1; _b >= 0; _b--) bits[nb++] = ((v) >> _b) & 1; } while (0)
    PUT(1, 5); PUT(g.w - 1, 5); PUT(g.h - 1, 4);
    int total = g.w * g.h;
    for (int i = 0; i < total;) {
        char c = g.c[i / g.w][i % g.w];
        if (c == ' ' || c == '#') {
            int r = 1;
            while (i + r < total && r < 8 && g.c[(i + r) / g.w][(i + r) % g.w] == c) r++;
            if (c == ' ') PUT(0, 1); else PUT(2, 2);
            PUT(r - 1, 3);
            i += r;
            continue;
        }
        switch (c) {
        case '$': PUT(6, 3); break;
        case '.': PUT(14, 4); break;
        case '*': PUT(30, 5); break;
        case '@': PUT(62, 6); break;
        default:  PUT(63, 6); break;   /* '+' */
        }
        i++;
    }
#undef PUT
    while (nb % 5) bits[nb++] = 0;
    static int sym[CODE_MAX];
    int ns = 0;
    for (int i = 0; i < nb && ns < CODE_MAX - 3; i += 5) {
        int v = 0;
        for (int j = 0; j < 5; j++) v = v * 2 + bits[i + j];
        sym[ns++] = v;
    }
    int s = code_sum(sym, ns);
    for (int i = 0; i < ns; i++) out[i] = B32[sym[i]];
    out[ns] = B32[s >> 5]; out[ns + 1] = B32[s & 31]; out[ns + 2] = 0;
    return 1;
}

/* code (spaces, dashes and case don't matter) -> map; 0 if it doesn't check out */
static int code_to_map(const char *code, char *map, int cap)
{
    static int sym[CODE_MAX];
    int ns = 0;
    for (const char *p = code; *p; p++) {
        if (*p == ' ' || *p == '-' || *p == '\n') continue;
        int v = b32_val((unsigned char)*p);
        if (v < 0 || ns >= CODE_MAX) return 0;
        sym[ns++] = v;
    }
    if (ns < 5) return 0;
    if (code_sum(sym, ns - 2) != sym[ns - 2] * 32 + sym[ns - 1]) return 0;
    int nbits = (ns - 2) * 5, pos = 0;
#define GET(n) ({ int _v = 0; for (int _b = 0; _b < (n); _b++) { if (pos >= nbits) return 0; _v = _v * 2 + ((sym[pos / 5] >> (4 - pos % 5)) & 1); pos++; } _v; })
    if (GET(5) != 1) return 0;
    static Grid g;
    int w = GET(5) + 1, h = GET(4) + 1;
    if (w > ED_W || h > ED_H) return 0;
    grid_clear(&g, w, h);
    for (int i = 0; i < w * h;) {
        char c;
        if (!GET(1)) c = ' ';
        else if (!GET(1)) c = '#';
        else if (!GET(1)) c = '$';
        else if (!GET(1)) c = '.';
        else if (!GET(1)) c = '*';
        else c = GET(1) ? '+' : '@';
        int r = 1;
        if (c == ' ' || c == '#') r = GET(3) + 1;
        for (int k = 0; k < r && i < w * h; k++, i++) g.c[i / w][i % w] = c;
    }
#undef GET
    grid_to_map(&g, map, cap);
    return map[0] != 0;
}

/* the code as rows of ten, in two groups of five */
static int code_rows(const char *code, char rows[][12], int maxrows)
{
    int n = (int)strlen(code), r = 0;
    for (int i = 0; i < n && r < maxrows; i += 10, r++) {
        int k = 0;
        for (int j = 0; j < 10 && i + j < n; j++) {
            if (j == 5) rows[r][k++] = ' ';
            rows[r][k++] = code[i + j];
        }
        rows[r][k] = 0;
    }
    return r;
}

/* ---------------- progress codes ----------------
   Which levels of a set are solved, to carry over to another device (no records, replays or games in
   progress). Same letters, layout and checksum as level codes, but version 2: the set's fingerprint
   (20 bits, from all its levels, so a code only fits the same set), its number of levels (11 bits),
   then either one bit per level, or (when shorter) the lengths of the runs of unsolved and solved
   levels, unsolved first, each written as its length + 1 in Elias gamma code. */
#define PROG_VERSION 2
static uint32_t set_fingerprint(int set)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < g_sets[set].n; i++) {
        for (const char *p = g_sets[set].maps[i]; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
        h ^= 0xff; h *= 16777619u;
    }
    return (h ^ (h >> 20)) & 0xFFFFF;
}
static int progress_ok(int set) { return set >= 0 && set != g_fav && set != g_my && g_sets[set].n > 0 && g_sets[set].n < 2048; }

static int progress_code(int set, char *out)
{
    if (!progress_ok(set)) return 0;
    Set *st = &g_sets[set];
    int n = st->n;
    static uint8_t bm[2048 + 64], rl[2048 * 24 + 64];
    int nb = 0, nr = 0;
    /* the runs version */
#define PUTR(v, k) do { for (int _b = (k) - 1; _b >= 0; _b--) rl[nr++] = ((v) >> _b) & 1; } while (0)
    for (int i = 0, want = 0; i < n; want = !want) {     /* only the first run can be empty */
        int r = 0;
        while (i + r < n && (st->best_moves[i + r] != 0) == want) r++;
        int v = r + 1, len = 0;
        while ((v >> len) > 1) len++;
        PUTR(0, len); PUTR(v, len + 1);
        i += r;
    }
#undef PUTR
    for (int i = 0; i < n; i++) bm[nb++] = st->best_moves[i] != 0;
    static uint8_t bits[2048 * 24 + 128];
    int k = 0, runs = nr < nb;
#define PUT(v, m) do { for (int _b = (m) - 1; _b >= 0; _b--) bits[k++] = ((v) >> _b) & 1; } while (0)
    PUT(PROG_VERSION, 5); PUT(set_fingerprint(set), 20); PUT(n, 11); PUT(runs, 1);
#undef PUT
    memcpy(bits + k, runs ? rl : bm, runs ? nr : nb); k += runs ? nr : nb;
    while (k % 5) bits[k++] = 0;
    static int sym[CODE_MAX];
    int ns = 0;
    for (int i = 0; i < k; i += 5) {
        if (ns >= CODE_MAX - 3) return 0;
        int v = 0;
        for (int j = 0; j < 5; j++) v = v * 2 + bits[i + j];
        sym[ns++] = v;
    }
    int sum = code_sum(sym, ns);
    for (int i = 0; i < ns; i++) out[i] = B32[sym[i]];
    out[ns] = B32[sum >> 5]; out[ns + 1] = B32[sum & 31]; out[ns + 2] = 0;
    return 1;
}

/* the code's version (1 level, 2 progress), or 0 if it doesn't check out */
static int code_version(const char *code, int *sym, int *ns_out)
{
    int ns = 0;
    for (const char *p = code; *p; p++) {
        if (*p == ' ' || *p == '-' || *p == '\n') continue;
        int v = b32_val((unsigned char)*p);
        if (v < 0 || ns >= CODE_MAX) return 0;
        sym[ns++] = v;
    }
    if (ns < 3 || code_sum(sym, ns - 2) != sym[ns - 2] * 32 + sym[ns - 1]) return 0;
    *ns_out = ns;
    return sym[0];
}

/* applies a progress code: marks its solved levels solved (never unmarks any).
   Returns 1 and the set and how many were new; 0 bad code; -1 no set here matches it */
static int progress_apply(const char *code, int *set_out, int *added, int *total)
{
    static int sym[CODE_MAX];
    int ns;
    if (code_version(code, sym, &ns) != PROG_VERSION) return 0;
    int nbits = (ns - 2) * 5, pos = 5;
#define GETB() (pos < nbits ? (sym[pos / 5] >> (4 - pos % 5)) & 1 : (pos = nbits + 1, 0)); pos++
    uint32_t fp = 0; int n = 0, runs;
    for (int i = 0; i < 20; i++) { int b = GETB(); fp = fp * 2 + b; }
    for (int i = 0; i < 11; i++) { int b = GETB(); n = n * 2 + b; }
    { int b = GETB(); runs = b; }
    if (pos > nbits || n <= 0) return 0;
    static uint8_t solved[2048];
    if (!runs) {
        for (int i = 0; i < n; i++) { int b = GETB(); solved[i] = b; }
    } else {
        int i = 0, want = 0;
        while (i < n) {
            int len = 0, v = 1;
            for (;;) { int b = GETB(); if (pos > nbits || len > 12) return 0; if (b) break; len++; }
            for (int j = 0; j < len; j++) { int b = GETB(); v = v * 2 + b; }
            int r = v - 1;
            if (r > n - i) return 0;
            for (int j = 0; j < r; j++) solved[i++] = want;
            want = !want;
        }
    }
#undef GETB
    if (pos > nbits + 1) return 0;
    int set = -1;
    for (int s2 = 0; s2 < g_nsets; s2++) if (progress_ok(s2) && g_sets[s2].n == n && set_fingerprint(s2) == fp) { set = s2; break; }
    if (set < 0) return -1;
    *set_out = set; *added = 0; *total = 0;
    for (int i = 0; i < n; i++) if (solved[i]) {
        (*total)++;
        if (!g_sets[set].best_moves[i]) { g_sets[set].best_moves[i] = SOLVED_BY_CODE; g_sets[set].best_pushes[i] = 0; (*added)++; }
    }
    return 1;
}

/* ---------------- My levels ---------------- */

static int my_find(const char *map)
{
    for (int i = 0; i < g_sets[g_my].n; i++) if (!strcmp(g_sets[g_my].maps[i], map)) return i;
    return -1;
}

static int my_add(const char *map, int flags)
{
    Set *s = &g_sets[g_my];
    if (s->n >= MY_MAX) return -1;
    int i = s->n++;
    s->maps[i] = strdup(map);
    s->best_moves[i] = s->best_pushes[i] = 0;
    s->partial[i] = NULL; s->best_run[i] = NULL;
    g_my_flags[i] = (uint8_t)flags;
    g_my_par[i] = 0;
    return i;
}

static void my_delete(int i)
{
    Set *s = &g_sets[g_my];
    if (i < 0 || i >= s->n) return;
    free(s->maps[i]); free(s->partial[i]); free(s->best_run[i]);
    for (int j = i; j + 1 < s->n; j++) {
        s->maps[j] = s->maps[j + 1]; s->partial[j] = s->partial[j + 1]; s->best_run[j] = s->best_run[j + 1];
        s->best_moves[j] = s->best_moves[j + 1]; s->best_pushes[j] = s->best_pushes[j + 1];
        g_my_flags[j] = g_my_flags[j + 1]; g_my_par[j] = g_my_par[j + 1];
    }
    s->n--;
    s->maps[s->n] = NULL; s->partial[s->n] = NULL; s->best_run[s->n] = NULL;
    s->best_moves[s->n] = s->best_pushes[s->n] = 0;
    if (cfg.cur_set == g_my) {
        if (cfg.cur_level == i) { cfg.cur_level = 0; cfg.cur_moves[0] = 0; if (!s->n) cfg.cur_set = 0; }
        else if (cfg.cur_level > i) cfg.cur_level--;
    }
    /* favorites: drop this one, and the ones after it move up a number */
    int k = 0;
    for (int f = 0; f < g_nfav; f++) {
        if (!strcmp(g_favs[f].set, MY_NAME)) {
            if (g_favs[f].lv == i) continue;
            if (g_favs[f].lv > i) g_favs[f].lv--;
        }
        g_favs[k++] = g_favs[f];
    }
    g_nfav = k;
    fav_sync();
}

/* a level was edited: a new map means a new level (its records and its code have to be earned again) */
static void my_set_map(int i, const char *map)
{
    Set *s = &g_sets[g_my];
    if (!strcmp(s->maps[i], map)) return;
    free(s->maps[i]);
    s->maps[i] = strdup(map);
    s->best_moves[i] = s->best_pushes[i] = 0;
    set_partial(g_my, i, NULL, 0);
    set_best_run(g_my, i, NULL, 0);
    g_my_flags[i] &= ~MY_OK;
    g_my_par[i] = 0;
    if (cfg.cur_set == g_my && cfg.cur_level == i) cfg.cur_moves[0] = 0;
}

/* ---------------- the maker ---------------- */

enum { T_HEDGE, T_ACORN, T_SPOT, T_ANIMAL, T_ERASE, NTOOLS };
static const char *const TOOL_NAMES[NTOOLS] = { "Hedge", "Acorn", "Spot", "Animal", "Erase" };
#define ED_UNDO 48
static const char *const ED_MENU[] = { "Keep editing", "Test play", "Solve it for me", "Rotate", "Flip", "Help", "Clear everything", "Save and leave" };
enum { EM_KEEP, EM_TEST, EM_SOLVE, EM_ROTATE, EM_FLIP, EM_HELP, EM_CLEAR, EM_LEAVE, NED_MENU };
static const char *const ED_CLEAR[] = { "No", "Yes, clear it" };
static const char *const ED_RESULT[] = { "Watch it", "Keep editing" };

static struct {
    int idx;                    /* which of My levels (-1: new, not saved yet) */
    Grid g;
    int cx, cy, tool, big;      /* cursor, tool, 2x view */
    int paint, erase;           /* A / B held: painting / erasing as you move */
    int held_dir; Uint32 dir_next;
    Grid undo[ED_UNDO]; int nundo;
    Grid redo[ED_UNDO]; int nredo;
    int menu, menu_sel;
    int clearing, clear_sel;    /* "Clear everything": are you sure? */
    Uint32 help_until;
    int checking;               /* the solver is working: 1 before a test play, 2 "Solve it for me" */
    int result, result_sel;     /* "Solve it for me" found it: Watch it / Keep editing */
    int touch_mode;             /* touch: this drag places (1) or removes (-1) */
    Uint32 stuck_t[ED_H][ED_W]; /* stuck acorns: when the next shake starts (0: not stuck) */
    Uint32 shake_until;         /* draw quickly until then (a shake is playing) */
    int touch_id, touching, top;   /* touch: the finger painting; the first row in view (2x) */
} Ed;

static void ed_open(int idx)
{
    memset(&Ed, 0, sizeof Ed);
    Ed.idx = idx;
    grid_clear(&Ed.g, ED_W, ED_H);
    if (idx >= 0 && idx < g_sets[g_my].n) grid_from_map(g_sets[g_my].maps[idx], &Ed.g, 1);
    else Ed.idx = -1;
    Ed.cx = ED_W / 2; Ed.cy = ED_H / 2;
    Ed.big = 1;
    Ed.held_dir = -1;
    Ed.help_until = now_ms() + 7000;
    S.screen = SC_EDIT;
    S.dirty = 1;
}

static void ed_snapshot(void)
{
    if (Ed.nundo == ED_UNDO) { memmove(Ed.undo, Ed.undo + 1, sizeof Ed.undo[0] * (ED_UNDO - 1)); Ed.nundo--; }
    Ed.undo[Ed.nundo++] = Ed.g;
    Ed.nredo = 0;                                  /* a new change: nothing to redo */
}

static void ed_undo(void)
{
    if (!Ed.nundo) return;
    if (Ed.nredo < ED_UNDO) Ed.redo[Ed.nredo++] = Ed.g;
    Ed.g = Ed.undo[--Ed.nundo];
    sfx_play(SFX_UNDO);
}

static void ed_redo(void)
{
    if (!Ed.nredo) return;
    if (Ed.nundo == ED_UNDO) { memmove(Ed.undo, Ed.undo + 1, sizeof Ed.undo[0] * (ED_UNDO - 1)); Ed.nundo--; }
    Ed.undo[Ed.nundo++] = Ed.g;
    Ed.g = Ed.redo[--Ed.nredo];
    sfx_play(SFX_MENU);
}

/* does this square already have what the tool puts there? (then pressing again takes it away) */
static int ed_has(int tool, char c)
{
    switch (tool) {
    case T_HEDGE: return c == '#';
    case T_ACORN: return c == '$' || c == '*';
    case T_SPOT: return c == '.' || c == '*' || c == '+';
    case T_ANIMAL: return c == '@' || c == '+';
    }
    return 0;
}

/* take away just what the tool puts there, leaving the rest */
static void ed_unpaint(int tool, int x, int y)
{
    char *c = &Ed.g.c[y][x], n = *c;
    if (!ed_has(tool, n)) return;
    switch (tool) {
    case T_HEDGE: n = ' '; break;
    case T_ACORN: n = n == '*' ? '.' : ' '; break;
    case T_SPOT: n = n == '*' ? '$' : n == '+' ? '@' : ' '; break;
    case T_ANIMAL: n = n == '+' ? '.' : ' '; break;
    }
    if (n != *c) { *c = n; S.dirty = 1; }
}

/* rotate a quarter turn clockwise (rot = 1) or flip left to right (rot = 0); the result is centred */
static void ed_transform(int rot)
{
    static char map[1024], out[1024];
    grid_to_map(&Ed.g, map, sizeof map);
    if (!map[0]) return;
    char rows[ED_H][ED_W + 1];
    int h = 0, w = 0;
    for (char *p = map; *p && h < ED_H; h++) {
        char *e = strchr(p, '\n');
        int l = e ? (int)(e - p) : (int)strlen(p);
        memset(rows[h], ' ', ED_W); rows[h][ED_W] = 0;
        memcpy(rows[h], p, l);
        if (l > w) w = l;
        if (!e) { h++; break; }
        p = e + 1;
    }
    if (rot && w > ED_H) { toast("Too wide to turn: 16 squares at most"); sfx_play(SFX_BUMP); return; }
    int n = 0, ow = rot ? h : w, oh = rot ? w : h;
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) out[n++] = rot ? rows[h - 1 - x][y] : rows[y][w - 1 - x];
        out[n++] = '\n';
    }
    out[n] = 0;
    ed_snapshot();
    grid_clear(&Ed.g, ED_W, ED_H);
    grid_from_map(out, &Ed.g, 1);
    sfx_play(SFX_SELECT);
    S.dirty = 1;
}

static void ed_paint(int tool, int x, int y)
{
    char *c = &Ed.g.c[y][x], o = *c, n = o;
    int spot = o == '.' || o == '*' || o == '+';
    switch (tool) {
    case T_HEDGE: n = '#'; break;
    case T_ACORN: n = spot ? '*' : '$'; break;
    case T_SPOT:  n = o == '$' || o == '*' ? '*' : o == '@' || o == '+' ? '+' : '.'; break;
    case T_ANIMAL:
        for (int yy = 0; yy < ED_H; yy++)          /* only one of you */
            for (int xx = 0; xx < ED_W; xx++) {
                if (Ed.g.c[yy][xx] == '@') Ed.g.c[yy][xx] = ' ';
                if (Ed.g.c[yy][xx] == '+') Ed.g.c[yy][xx] = '.';
            }
        o = *c; spot = o == '.' || o == '*' || o == '+';
        n = spot ? '+' : '@';
        break;
    default: n = ' '; break;
    }
    if (n != *c) { *c = n; S.dirty = 1; }
}

/* save into My levels; returns the index, or -1 if there was nothing to save */
static int ed_save(void)
{
    static char map[1024];
    grid_to_map(&Ed.g, map, sizeof map);
    if (!map[0]) return Ed.idx;
    if (Ed.idx < 0) {
        Ed.idx = my_add(map, 0);
        if (Ed.idx < 0) { toast("My levels is full"); return -1; }
    } else my_set_map(Ed.idx, map);
    save_all();
    return Ed.idx;
}

static void ed_leave(void)
{
    int i = ed_save();
    open_levels(g_my, i >= 0 ? i : g_sets[g_my].n);
}

static void ed_start_test(int idx, const char *note)
{
    Ed.checking = 0;
    S.fav_play = 0;
    start_level(g_my, idx, NULL);
    S.testing = 1;
    if (note) { toast(note); S.toast_until = now_ms() + 3500; }
}

static void ed_solve_for_me(void)
{
    const char *pr = grid_problem(&Ed.g);
    if (pr) { toast(pr); sfx_play(SFX_BUMP); return; }
    int i = ed_save();
    if (i < 0) return;
    sv_start(g_sets[g_my].maps[i]);
    Ed.checking = 2;
}

/* "Watch it": play the level with the solver's moves, like a best run */
static void ed_watch(void)
{
    if (!SV.sol || Ed.idx < 0) return;
    ed_start_test(Ed.idx, NULL);
    S.watching = 1;
    for (const char *c = SV.sol; *c; c++) if (dir_of(*c) >= 0) hist_push(*c);
    S.playback = 1;
    S.play_next = now_ms() + 700;
    toast(HINT("The solver's run - any button takes over", "The solver's run - any key takes over", "The solver's run - tap to take over"));
    S.toast_until = now_ms() + 3000;
}

static void ed_test(void)
{
    const char *pr = grid_problem(&Ed.g);
    if (pr) { toast(pr); sfx_play(SFX_BUMP); return; }
    int i = ed_save();
    if (i < 0) return;
    sfx_play(SFX_SELECT);
    if (g_my_par[i]) { static char b[64]; snprintf(b, sizeof b, "Fewest moves possible: %d", g_my_par[i]); ed_start_test(i, b); return; }
    sv_start(g_sets[g_my].maps[i]);
    Ed.checking = 1;
}

/* the solver finished (or was skipped with B) */
static void ed_check_done(int skipped)
{
    static char b[64];
    int i = Ed.idx;
    if (Ed.checking == 2) {                     /* "Solve it for me" */
        Ed.checking = 0;
        if (skipped) { SV.state = SV_IDLE; sv_free(); toast("Stopped the solver"); return; }
        if (SV.state == SV_SOLVED) {
            g_my_par[i] = SV.moves;
            g_my_flags[i] |= MY_OK;             /* proven solvable: it has a code now */
            save_all();
            Ed.result = 1; Ed.result_sel = 0;
            sfx_play(SFX_CLEAR);
        } else if (SV.state == SV_IMPOSSIBLE) { toast("The solver says it can't be solved"); S.toast_until = now_ms() + 3500; sfx_play(SFX_STUCK); }
        else { toast("Too big for the solver - solve it yourself"); S.toast_until = now_ms() + 3500; }
        return;
    }
    if (skipped) { SV.state = SV_IDLE; sv_free(); ed_start_test(i, NULL); return; }
    switch (SV.state) {
    case SV_SOLVED:
        g_my_par[i] = SV.moves;
        save_all();
        snprintf(b, sizeof b, "Fewest moves possible: %d", SV.moves);
        ed_start_test(i, b);
        break;
    case SV_IMPOSSIBLE:
        Ed.checking = 0;
        toast("The solver says it can't be solved");
        S.toast_until = now_ms() + 3500;
        sfx_play(SFX_STUCK);
        break;
    default:
        ed_start_test(i, "Too big for the solver - can you solve it?");
        break;
    }
}

static void ed_move(int d)
{
    int nx = clampi(Ed.cx + DX[d], 0, ED_W - 1), ny = clampi(Ed.cy + DY[d], 0, ED_H - 1);
    if (nx == Ed.cx && ny == Ed.cy) return;
    Ed.cx = nx; Ed.cy = ny;
    if (Ed.paint > 0) ed_paint(Ed.tool, nx, ny);
    else if (Ed.paint < 0) ed_unpaint(Ed.tool, nx, ny);
    else if (Ed.erase) ed_paint(T_ERASE, nx, ny);
    S.dirty = 1;
}

static void ed_key(SDLKey k)
{
    S.dirty = 1;
    if (Ed.checking) { if (k == BTN_B) ed_check_done(1); return; }
    if (Ed.result) {
        if (k == BTN_UP || k == BTN_DOWN) { Ed.result_sel = !Ed.result_sel; sfx_play(SFX_MENU); }
        else if (k == BTN_B || k == BTN_START || (k == BTN_A && Ed.result_sel == 1)) Ed.result = 0;
        else if (k == BTN_A) { Ed.result = 0; sfx_play(SFX_SELECT); ed_watch(); }
        return;
    }
    if (Ed.clearing) {
        if (k == BTN_UP || k == BTN_DOWN) { Ed.clear_sel = !Ed.clear_sel; sfx_play(SFX_MENU); }
        else if (k == BTN_B || k == BTN_START) Ed.clearing = 0;
        else if (k == BTN_A) {
            Ed.clearing = 0;
            if (Ed.clear_sel == 1) { ed_snapshot(); grid_clear(&Ed.g, ED_W, ED_H); sfx_play(SFX_RESTART); }
        }
        return;
    }
    if (Ed.menu) {
        if (k == BTN_UP) menu_nav(&Ed.menu_sel, NED_MENU, -1);
        else if (k == BTN_DOWN) menu_nav(&Ed.menu_sel, NED_MENU, 1);
        else if (k == BTN_B || k == BTN_START) Ed.menu = 0;
        else if (k == BTN_A) {
            sfx_play(SFX_SELECT);
            Ed.menu = 0;
            switch (Ed.menu_sel) {
            case EM_TEST: ed_test(); break;
            case EM_SOLVE: ed_solve_for_me(); break;
            case EM_ROTATE: ed_transform(1); break;
            case EM_FLIP: ed_transform(0); break;
            case EM_CLEAR: Ed.clearing = 1; Ed.clear_sel = 0; break;      /* "No" first */
            case EM_HELP: Ed.help_until = now_ms() + 9000; break;
            case EM_LEAVE: ed_leave(); break;
            }
        }
        return;
    }
    Ed.help_until = 0;
    int d = key_dir(k);
    if (d >= 0) { ed_move(d); Ed.held_dir = d; Ed.dir_next = now_ms() + REPEAT_FIRST; return; }
    switch (k) {
    case BTN_A:                                    /* place it, or (if it's already there) take it away */
        ed_snapshot();
        Ed.paint = Ed.tool != T_ERASE && ed_has(Ed.tool, Ed.g.c[Ed.cy][Ed.cx]) ? -1 : 1;
        if (Ed.paint > 0) ed_paint(Ed.tool, Ed.cx, Ed.cy); else ed_unpaint(Ed.tool, Ed.cx, Ed.cy);
        break;
    case BTN_B: ed_snapshot(); Ed.erase = 1; ed_paint(T_ERASE, Ed.cx, Ed.cy); break;
    case BTN_L1: Ed.tool = (Ed.tool + NTOOLS - 1) % NTOOLS; sfx_play(SFX_MENU); break;
    case BTN_R1: Ed.tool = (Ed.tool + 1) % NTOOLS; sfx_play(SFX_MENU); break;
    case BTN_SELECT: ed_undo(); break;
    case BTN_R2: ed_redo(); break;
    case BTN_X: case BTN_L2: Ed.big = !Ed.big; break;
    case BTN_Y: ed_test(); break;
    case BTN_START: case BTN_MENU: Ed.menu = 1; Ed.menu_sel = 0; sfx_play(SFX_MENU); break;
    default: break;
    }
}

static void ed_key_up(SDLKey k)
{
    int d = key_dir(k);
    if (d >= 0 && d == Ed.held_dir) Ed.held_dir = -1;
    if (k == BTN_A) Ed.paint = 0;
    if (k == BTN_B) Ed.erase = 0;
}

static void ed_tick(void)
{
    Uint32 t = now_ms();
    if (Ed.checking) {
        sv_run(20);
        if (SV.state != SV_RUNNING) ed_check_done(0);
        S.dirty = 1;
        return;
    }
    if (Ed.held_dir >= 0 && !Ed.menu && !Ed.clearing && !Ed.result && t >= Ed.dir_next) { ed_move(Ed.held_dir); Ed.dir_next = t + REPEAT_MS; }
}

/* where the canvas sits on screen */
static void ed_view(int *ts, int *ox, int *oy)
{
    if (Ed.big) {
        *ts = 32;
        if (!Ed.touching) {                        /* follow the cursor (2 rows don't fit) */
            int vis = (SCREEN_H - HUD_H) / 32;
            if (Ed.cy < Ed.top + 1) Ed.top = Ed.cy - 1;
            if (Ed.cy > Ed.top + vis - 2) Ed.top = Ed.cy - vis + 2;
            Ed.top = clampi(Ed.top, 0, ED_H - vis);
        }
        *ox = 0; *oy = HUD_H - Ed.top * 32;
    } else {
        *ts = 16;
        *ox = (SCREEN_W - ED_W * 16) / 2;
        *oy = HUD_H + (SCREEN_H - HUD_H - ED_H * 16) / 2;
    }
}

/* the tool's picture, at 1x */
static void ed_tool_icon(int tool, int x, int y)
{
    SpriteSet *u = ui_set(1, cfg.animal < 0 ? 0 : cfg.animal);
    switch (tool) {
    case T_HEDGE: for (int q = 0; q < 4; q++) blit(u->hq[QT_OUTER][q], x + (q & 1) * 8, y + (q >> 1) * 8); break;
    case T_ACORN: blit(u->tile[TL_ACORN], x, y); break;
    case T_SPOT: blit(u->tile[TL_FLOOR], x, y); blit(u->tile[TL_GOAL], x, y); break;
    case T_ANIMAL: blit(u->anim[FR_IDLE][0], x, y); break;
    default: blit(u->tile[TL_FLOOR], x, y); rect(x + 3, y + 7, 10, 2, BG3); break;
    }
}

#define ED_TBX(i) (8 + (i) * 40)
static const char *const ED_TBTN[5] = { "UNDO", "REDO", "ZOOM", "TEST", "MENU" };
static const int ED_TKEY[5] = { BTN_SELECT, BTN_R2, BTN_X, BTN_Y, BTN_START };

static void draw_edit(void)
{
    Uint32 t = now_ms();
    rect(0, 0, SCREEN_W, SCREEN_H, BG3);
    int ts, ox, oy;
    ed_view(&ts, &ox, &oy);
    SpriteSet *u = ui_set(ts / 16, cfg.animal < 0 ? 0 : cfg.animal);
    /* the canvas as a level: inside the hedge (from the animal) is ground, outside is the dark;
       with no animal yet it's all ground */
    static Puzzle ep;
    memset(ep.cell, 0, sizeof ep.cell); memset(ep.goal, 0, sizeof ep.goal); memset(ep.gap, 0, sizeof ep.gap);
    ep.w = ED_W; ep.h = ED_H;
    int px = -1, py = -1, na = 0, ns = 0;
    for (int y = 0; y < ED_H; y++)
        for (int x = 0; x < ED_W; x++) {
            char c = Ed.g.c[y][x];
            if (c == '#') ep.cell[y][x] = CL_WALL;
            if (c == '.' || c == '*' || c == '+') { ep.goal[y][x] = 1; ns++; }
            if (c == '$' || c == '*') na++;
            if (c == '@' || c == '+') { px = x; py = y; }
        }
    if (px < 0) { for (int y = 0; y < ED_H; y++) for (int x = 0; x < ED_W; x++) if (!ep.cell[y][x]) ep.cell[y][x] = CL_FLOOR; }
    else {
        static int q[ED_W * ED_H];
        int qh = 0, qt = 0;
        ep.cell[py][px] = CL_FLOOR; q[qt++] = py * ED_W + px;
        while (qh < qt) {
            int v = q[qh++], x = v % ED_W, y = v / ED_W;
            for (int d = 0; d < 4; d++) {
                int nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= ED_W || ny >= ED_H || ep.cell[ny][nx] != CL_OUT) continue;
                ep.cell[ny][nx] = CL_FLOOR; q[qt++] = ny * ED_W + nx;
            }
        }
        fill_diagonal_gaps(&ep, ED_W, ED_H);
    }
    /* stuck acorns: squares an acorn could never be pushed from onto a spot (pulled back from the spots) */
    static uint8_t live[ED_H][ED_W];
    memset(live, 0, sizeof live);
    if (ns) {
        static int lq[ED_W * ED_H];
        int qh = 0, qt = 0;
        for (int y = 0; y < ED_H; y++) for (int x = 0; x < ED_W; x++)
            if (ep.goal[y][x] && Ed.g.c[y][x] != '#') { live[y][x] = 1; lq[qt++] = y * ED_W + x; }
        while (qh < qt) {
            int v = lq[qh++], x = v % ED_W, y = v / ED_W;
            for (int d = 0; d < 4; d++) {
                int ax = x + DX[d], ay = y + DY[d], bx = ax + DX[d], by = ay + DY[d];   /* from a, the animal at b */
                if (bx < 0 || by < 0 || bx >= ED_W || by >= ED_H) continue;
                if (Ed.g.c[ay][ax] == '#' || Ed.g.c[by][bx] == '#' || live[ay][ax]) continue;
                live[ay][ax] = 1; lq[qt++] = ay * ED_W + ax;
            }
        }
    }
    set_clip(0, HUD_H, SCREEN_W, SCREEN_H - HUD_H);
    draw_world(&ep, u, ox, oy, 0, HUD_H, SCREEN_W, SCREEN_H - HUD_H, 1234);
    for (int y = 0; y < ED_H; y++)
        for (int x = 0; x < ED_W; x++) {
            char c = Ed.g.c[y][x];
            int X = ox + x * ts, Y = oy + y * ts;
            if ((c == '.' || c == '*' || c == '+') && ep.cell[y][x] != CL_FLOOR) blit(u->tile[TL_GOAL], X, Y);
            /* an acorn that's stuck where it is shakes, like in the game: once when it gets stuck, then
               now and then, but only once there are as many spots as acorns (not while you're still
               placing them) */
            int stuck = c == '$' && ns && !live[y][x], sx = 0;
            Uint32 *st = &Ed.stuck_t[y][x];
            if (!stuck) *st = 0;
            else {
                if (!*st) *st = t;                                       /* just got stuck: shake now */
                else if (na == ns && t >= *st + 600 + 3600) *st = t + (x * 7 + y * 13) % 900;   /* again, staggered */
                if (t >= *st && t < *st + 600) {
                    static const int W[8] = { 0, 2, 0, -2, 0, 1, 0, -1 };
                    sx = W[((t - *st) / 40) % 8] * (ts / 16);
                }
                if (t + 100 >= *st && *st + 600 > Ed.shake_until) Ed.shake_until = *st + 600;   /* smooth while it plays */
            }
            if (c == '$' || c == '*') blit(u->tile[c == '*' ? TL_ACORN_GOAL : TL_ACORN], X + sx, Y);
            if (c == '@' || c == '+') blit(u->anim[((t / 700) % 2) ? FR_TAIL : FR_IDLE][0], X, Y);
        }
    /* the edge of the 20 x 16 canvas */
    if (!Ed.big) frame_rect(ox - 4, oy - 4, ED_W * ts + 8, ED_H * ts + 8, 2, BG1);
    else {
        if (Ed.top == 0) rect(0, oy, SCREEN_W, 2, BG1);
        if (Ed.top + (SCREEN_H - HUD_H) / 32 >= ED_H) rect(0, oy + ED_H * ts - 2, SCREEN_W, 2, BG1);
    }
    /* the cursor */
    if (g_kbd != 2 && (t / 400) % 3) {
        int X = ox + Ed.cx * ts, Y = oy + Ed.cy * ts, L = ts / 3;
        Uint32 c = (t / 400) % 3 == 1 ? BG0 : BG1;
        rect(X, Y, L, 2, c); rect(X, Y, 2, L, c); rect(X + ts - L, Y, L, 2, c); rect(X + ts - 2, Y, 2, L, c);
        rect(X, Y + ts - 2, L, 2, c); rect(X, Y + ts - L, 2, L, c); rect(X + ts - L, Y + ts - 2, L, 2, c); rect(X + ts - 2, Y + ts - L, 2, L, c);
    }
    no_clip();
    /* the bar along the top */
    rect(0, 0, SCREEN_W, HUD_H, BG3);
    rect(0, HUD_H - 4, SCREEN_W, 4, BG2);
    char b[64];
    if (g_kbd == 2) {                         /* touch: the tools, then undo / zoom / test / menu */
        for (int i = 0; i < NTOOLS; i++) {
            int x = ED_TBX(i);
            if (i == Ed.tool) rect(x - 4, 2, 32, 24, BG1);
            ed_tool_icon(i, x + 4, 6);
        }
        for (int i = 0; i < 5; i++) {
            int x = 212 + i * 84;
            rect(x, 4, 78, 20, BG2);
            text_c(x + 39, 6, ED_TBTN[i], 2, BG0);
            tap_add(x - 3, 0, 84, HUD_H + 4, TAP_BTN, NULL, ED_TKEY[i]);
        }
    } else {
        ed_tool_icon(Ed.tool, 16, 6);
        snprintf(b, sizeof b, "< %s >", TOOL_NAMES[Ed.tool]);
        text(40, 8, b, 2, BG0);
        snprintf(b, sizeof b, "%d acorn%s  %d spot%s", na, na == 1 ? "" : "s", ns, ns == 1 ? "" : "s");
        text_r(SCREEN_W - 16, 8, b, 2, na == ns ? BG1 : BG0);
    }
    if (g_kbd == 2 && !Ed.checking && !Ed.menu && !Ed.clearing && !Ed.result) {     /* touch: the counts, small, bottom right */
        snprintf(b, sizeof b, "%d acorn%s  %d spot%s", na, na == 1 ? "" : "s", ns, ns == 1 ? "" : "s");
        int tw = text_w(b, 2);
        rect(SCREEN_W - tw - 24, SCREEN_H - 28, tw + 16, 24, BG3);
        text(SCREEN_W - tw - 16, SCREEN_H - 24, b, 2, na == ns ? BG1 : BG0);
    }
    if (Ed.checking) {
        int w = 448, x = (SCREEN_W - w) / 2, y = 176;
        panel(x, y, w, 128);
        text_c(SCREEN_W / 2, y + 24, Ed.checking == 2 ? "The solver is working on it" : "Checking it can be solved", 2, BG3);
        static const char dots[4][4] = { "", ".", "..", "..." };
        text_c(SCREEN_W / 2, y + 56, dots[(t / 300) % 4], 2, BG3);
        text_c(SCREEN_W / 2, y + 88, Ed.checking == 2 ? HINT("B stop", "Esc stop", "Tap to stop") : HINT("B skip", "Esc skip", "Tap to skip"), 2, BG2);
        if (g_kbd == 2) tap_add(0, 0, SCREEN_W, SCREEN_H, TAP_BTN, NULL, BTN_B);
    } else if (Ed.result) {
        int w = 512, x = (SCREEN_W - w) / 2, y = 144;
        panel(x, y, w, 192);
        modal_taps(x, y, w, 192, BTN_B);
        snprintf(b, sizeof b, "Solved: %d moves, %d pushes", SV.moves, SV.pushes);
        text_c(SCREEN_W / 2, y + 20, b, 2, BG3);
        text_c(SCREEN_W / 2, y + 48, "It has a code now", 2, BG2);
        draw_menu_list(x + 96, y + 96, ED_RESULT, 2, Ed.result_sel);
        menu_taps(x + 96, y + 96, w - 128, 2, &Ed.result_sel);
    } else if (Ed.clearing) {
        int w = 384, x = (SCREEN_W - w) / 2, y = 144;
        panel(x, y, w, 192);
        modal_taps(x, y, w, 192, BTN_B);
        text_c(SCREEN_W / 2, y + 24, "Clear everything?", 2, BG3);
        text_c(SCREEN_W / 2, y + 64, "Are you sure?", 2, BG2);
        draw_menu_list(x + 48, y + 112, ED_CLEAR, 2, Ed.clear_sel);
        menu_taps(x + 48, y + 112, w - 80, 2, &Ed.clear_sel);
    } else if (Ed.menu) {
        int w = 384, h = 64 + NED_MENU * 32, x = (SCREEN_W - w) / 2, y = 96;
        panel(x, y, w, h);
        modal_taps(x, y, w, h, BTN_B);
        text_c(SCREEN_W / 2, y + 16, "LEVEL MAKER", 2, BG3);
        draw_menu_list(x + 32, y + 48, ED_MENU, NED_MENU, Ed.menu_sel);
        menu_taps(x + 32, y + 48, w - 48, NED_MENU, &Ed.menu_sel);
    } else if (t < Ed.help_until) {
        static const char *const PAD[] = { "D-pad move  A place/remove", "  (hold A and move: a line)", "B erase  SELECT undo  R2 redo",
            "L1/R1 tool    X zoom", "Y test play   START menu" };
        static const char *const KEYS[] = { "Arrows move  Space place", "  (on the same: remove)", "Bksp erase  Z undo  X redo",
            "1-5 or [ ] tool  - zoom", "T test play   Esc menu" };
        static const char *const TOUCH[] = { "Pick a tool up top, then", "  touch and drag to paint", "  (on the same: remove)", "UNDO REDO ZOOM TEST",
            "MENU: solve, turn, save" };
        const char *const *h = g_kbd == 1 ? KEYS : g_kbd == 2 ? TOUCH : PAD;
        int w = 512, x = (SCREEN_W - w) / 2, y = 144;
        panel(x, y, w, 5 * 24 + 48);
        for (int i = 0; i < 5; i++) text(x + 32, y + 24 + i * 24, h[i], 2, BG3);
    }
}

/* touch: paint where the finger goes (the first finger only); the top bar picks tools */
static void ed_touch(int id, int x, int y, int phase)
{
    if (Ed.checking || Ed.menu || Ed.clearing || Ed.result) return;
    if (phase == 0 && now_ms() < Ed.help_until) { Ed.help_until = 0; S.dirty = 1; return; }   /* first touch: just close the help */
    Ed.help_until = 0;
    if (phase == 0 && y < HUD_H) {
        for (int i = 0; i < NTOOLS; i++) if (x >= ED_TBX(i) - 4 && x < ED_TBX(i) + 36) { Ed.tool = i; sfx_play(SFX_MENU); }
        return;
    }
    if (phase == 0) { if (Ed.touching) return; Ed.touching = 1; Ed.touch_id = id; Ed.touch_mode = 0; ed_snapshot(); }
    else if (!Ed.touching || id != Ed.touch_id) return;
    if (phase == 2) { Ed.touching = 0; S.dirty = 1; return; }
    int ts, ox, oy;
    ed_view(&ts, &ox, &oy);
    if (y < HUD_H) return;
    int cx = (x - ox) / ts, cy = (y - oy) / ts;
    if (x < ox || y < oy || cx >= ED_W || cy >= ED_H) return;
    Ed.cx = cx; Ed.cy = cy;
    if (!Ed.touch_mode) Ed.touch_mode = Ed.tool != T_ERASE && ed_has(Ed.tool, Ed.g.c[cy][cx]) ? -1 : 1;   /* the first square decides */
    if (Ed.touch_mode > 0) ed_paint(Ed.tool, cx, cy); else ed_unpaint(Ed.tool, cx, cy);
}

/* ---------------- codes: show one, or type one in ---------------- */

#define KB_COLS 8
#define KB_KEYS 34            /* 32 letters and digits, then Delete and Done */
static struct {
    int enter;                /* 0 showing a level's code, 1 typing one in */
    int idx;
    char code[CODE_MAX + 4];
    int n, sel;
    char msg[64]; Uint32 msg_until;
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)]; int qr_ok;   /* the code as a QR code: just the text, no web address */
    int page;                 /* long codes: 0 the QR code, 1 the code as text (short ones show both) */
    int prog;                 /* showing a progress code: 1 (Cd.idx is the set) */
    int ret_set, ret_lv;      /* where Back goes */
} Cd;

static void code_qr(void);
/* Back from a code screen: the level list it came from, or Level sets (ret_lv < 0) */
static void code_back(void)
{
    if (Cd.ret_lv < 0) { S.sets_sel = Cd.ret_set; S.screen = SC_SETS; S.dirty = 1; }
    else open_levels(Cd.ret_set, Cd.ret_lv);
}
static void code_show(int idx)
{
    memset(&Cd, 0, sizeof Cd);
    Cd.idx = idx; Cd.ret_set = g_my; Cd.ret_lv = idx;
    if (!code_from_map(g_sets[g_my].maps[idx], Cd.code)) { toast("This level is too big for a code"); return; }
    code_qr();
}

/* a set's progress code */
static void progress_show(int set, int lv)
{
    memset(&Cd, 0, sizeof Cd);
    Cd.idx = set; Cd.prog = 1; Cd.ret_set = set; Cd.ret_lv = lv;
    if (!progress_code(set, Cd.code)) { toast("This set can't have a progress code"); return; }
    code_qr();
}

static void code_qr(void)
{
    /* the QR code holds the code in groups of five, so it reads the same on a phone */
    static char spaced[CODE_MAX * 2];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
    int k = 0;
    for (int i = 0; Cd.code[i]; i++) { if (i && i % 5 == 0) spaced[k++] = ' '; spaced[k++] = Cd.code[i]; }
    spaced[k] = 0;
    Cd.qr_ok = qrcodegen_encodeText(spaced, tmp, Cd.qr, qrcodegen_Ecc_MEDIUM, 1, 15, qrcodegen_Mask_AUTO, 1);
    S.screen = SC_CODE;
    S.dirty = 1;
}

static void code_enter_from(int set, int lv)
{
    memset(&Cd, 0, sizeof Cd);
    Cd.enter = 1; Cd.ret_set = set; Cd.ret_lv = lv;
    S.screen = SC_CODE;
    S.dirty = 1;
}
static void code_enter(void)
{
    memset(&Cd, 0, sizeof Cd);
    Cd.enter = 1; Cd.ret_set = g_my; Cd.ret_lv = g_sets[g_my].n;
    S.screen = SC_CODE;
    S.dirty = 1;
}

static void code_type(int c)
{
    int v = b32_val(c);
    if (v < 0 || Cd.n >= CODE_MAX) return;
    Cd.code[Cd.n++] = B32[v];
    Cd.code[Cd.n] = 0;
    S.dirty = 1;
}

static void code_msg(const char *m) { snprintf(Cd.msg, sizeof Cd.msg, "%s", m); Cd.msg_until = now_ms() + 3000; S.dirty = 1; }

static void code_submit(void)
{
    static char map[1024];
    if (!Cd.n) { code_msg("Type a code first"); return; }
    {   /* a progress code: mark its levels solved */
        static int sym[CODE_MAX];
        int ns, set, added, total;
        if (code_version(Cd.code, sym, &ns) == PROG_VERSION) {
            int r = progress_apply(Cd.code, &set, &added, &total);
            if (r == 0) { code_msg("That code doesn't check out"); sfx_play(SFX_BUMP); return; }
            if (r < 0) { code_msg("No set here matches that code"); sfx_play(SFX_BUMP); return; }
            save_all(); fav_sync();
            sfx_play(SFX_SELECT);
            int first = 0;
            while (first < g_sets[set].n && g_sets[set].best_moves[first]) first++;
            open_levels(set, first < g_sets[set].n ? first : 0);
            static char t[64];
            if (added) snprintf(t, sizeof t, "%d more solved (%d in all)", added, total);
            else snprintf(t, sizeof t, "Nothing new: all %d were solved", total);
            toast(t);
            return;
        }
    }
    if (!code_to_map(Cd.code, map, sizeof map)) { code_msg("That code doesn't check out"); sfx_play(SFX_BUMP); return; }
    if (map_problem(map)) { code_msg("That code makes a broken level"); sfx_play(SFX_BUMP); return; }
    int i = my_find(map);
    if (i >= 0) { open_levels(g_my, i); toast("That one's already in My levels"); return; }
    i = my_add(map, MY_OK | MY_CODE);
    if (i < 0) { code_msg("My levels is full"); return; }
    save_all();
    sfx_play(SFX_SELECT);
    open_levels(g_my, i);
    toast("Added to My levels");
}

#ifdef __EMSCRIPTEN__
static void code_copy(int link)
{
    EM_ASM({
        var c = UTF8ToString($0); var t = c;
        if ($1) t = location.origin + location.pathname + '#' + c;
        try { navigator.clipboard.writeText(t); } catch (e) {}
    }, Cd.code, link);
    toast(link ? "Link copied" : "Code copied");
}
#endif

static void code_key(SDLKey k)
{
    S.dirty = 1;
    if (!Cd.enter) {
#ifdef __EMSCRIPTEN__
        if (k == BTN_A) { code_copy(0); return; }
        if (k == BTN_X) { code_copy(1); return; }
#endif
        if (k == BTN_Y || k == BTN_UP || k == BTN_DOWN || k == BTN_A) { Cd.page = !Cd.page; return; }   /* long codes: QR / text */
        if (k == BTN_B || k == BTN_START) code_back();
        return;
    }
    int r = Cd.sel < 32 ? Cd.sel / KB_COLS : 4, c = Cd.sel < 32 ? Cd.sel % KB_COLS : (Cd.sel == 32 ? 0 : 4);
    switch (k) {
    case BTN_LEFT:  if (r == 4) Cd.sel = Cd.sel == 32 ? 33 : 32; else Cd.sel = r * KB_COLS + (c + KB_COLS - 1) % KB_COLS; break;
    case BTN_RIGHT: if (r == 4) Cd.sel = Cd.sel == 32 ? 33 : 32; else Cd.sel = r * KB_COLS + (c + 1) % KB_COLS; break;
    case BTN_UP:    Cd.sel = r == 0 ? (c < 4 ? 32 : 33) : r == 4 ? 3 * KB_COLS + c + (Cd.sel == 33 ? 1 : 0) : Cd.sel - KB_COLS; break;
    case BTN_DOWN:  Cd.sel = r == 3 ? (c < 4 ? 32 : 33) : r == 4 ? c + (Cd.sel == 33 ? 1 : 0) : Cd.sel + KB_COLS; break;
    case BTN_A:
        if (Cd.sel < 32) { code_type(B32[Cd.sel]); sfx_play(SFX_MENU); }
        else if (Cd.sel == 32) { if (Cd.n) Cd.code[--Cd.n] = 0; sfx_play(SFX_UNDO); }
        else code_submit();
        return;
    case BTN_B:
        if (Cd.n) { Cd.code[--Cd.n] = 0; sfx_play(SFX_UNDO); }
        else code_back();
        return;
    case BTN_START: code_submit(); return;
    default: return;
    }
    sfx_play(SFX_MENU);
}

/* the web keyboard (and pasting) types straight in: letters and digits, 8 backspace, 13 done, 27 back */
static void code_char(int c)
{
    if (S.screen != SC_CODE || !Cd.enter) return;
    if (c == 8) code_key(BTN_B);
    else if (c == 13) code_submit();
    else if (c == 27) code_back();
    else code_type(c);
    S.dirty = 1;
}

static void draw_code(void)
{
    draw_bg_pattern();
    static char rows[32][12];
    if (!Cd.enter) {
        char t[64];
        if (Cd.prog) snprintf(t, sizeof t, "%.24s", g_sets[Cd.idx].name);
        else snprintf(t, sizeof t, "My levels %d", Cd.idx + 1);
        header(Cd.prog ? "PROGRESS CODE" : "LEVEL CODE", t);
        int nr = code_rows(Cd.code, rows, 32);
        int longc = nr > 13 && Cd.qr_ok;          /* too long for both at once: one page each */
        int show_text = !longc || Cd.page == 1, show_qr = Cd.qr_ok && (!longc || Cd.page == 0);
        int colw = text_w("XXXXX XXXXX", 2), gap = 32, x0 = 32;
        if (show_text) {
            int cols = nr <= 13 ? 1 : 2, per = (nr + cols - 1) / cols;
            if (longc) x0 = (SCREEN_W - cols * colw - gap) / 2;
            for (int i = 0; i < nr; i++) text(x0 + (i / per) * (colw + gap), 64 + (i % per) * 28, rows[i], 2, BG3);
        }
        /* the QR code: dark squares on the lightest shade, with a quiet border of four */
        if (show_qr) {
            int n = qrcodegen_getSize(Cd.qr);
            int ax = longc ? 24 : x0 + colw + 32, aw = SCREEN_W - 24 - ax, ah = 336;
            int m = (aw < ah ? aw : ah) / (n + 8);
            if (m > 10) m = 10;
            int sz = (n + 8) * m, qx = ax + (aw - sz) / 2, qy = 52 + (ah - sz) / 2;
            rect(qx, qy, sz, sz, BG0);
            for (int y = 0; y < n; y++)
                for (int x = 0; x < n; x++)
                    if (qrcodegen_getModule(Cd.qr, x, y)) rect(qx + (x + 4) * m, qy + (y + 4) * m, m, m, BG3);
            text_c(qx + sz / 2, qy + sz + 12, "Scan with a phone", 2, BG2);
        }
        if (longc && g_kbd == 2) tbtn(SCREEN_W - 32 - 152, SCREEN_H - 76, 152, 24, Cd.page ? "QR CODE" : "AS TEXT", TAP_BTN, NULL, BTN_Y);
        else if (longc) text_r(SCREEN_W - 32, SCREEN_H - 72, Cd.page ? HINT("Y: QR code", "Down: QR code", "") : HINT("Y: as text", "Down: as text", ""), 2, BG3);
        char b[48];
        if (Cd.prog) {                            /* under the code: what it holds and how to use it */
            int y = 64 + (nr < 13 ? nr : 13) * 28 + 12;
            if (longc && Cd.page == 0) y = SCREEN_H - 72;
            snprintf(b, sizeof b, "%d of %d solved", set_solved(Cd.idx), g_sets[Cd.idx].n);
            text(x0, y, b, 2, BG2);
            if (!longc || Cd.page == 1) {
                text(x0, y + 40, "Enter it on", 2, BG2);
                text(x0, y + 60, "another device:", 2, BG2);
                text(x0, y + 80, "Level sets,", 2, BG2);
                text(x0, y + 100, HINT("SELECT: Enter", "M: Enter", "...: Enter"), 2, BG2);
                text(x0, y + 120, "a code", 2, BG2);
            }
        } else {
            snprintf(b, sizeof b, "%d characters", (int)strlen(Cd.code));
            text(x0, SCREEN_H - 72, b, 2, BG2);
        }
#ifdef __EMSCRIPTEN__
        if (g_kbd == 2) {                         /* touch: copy buttons */
            tbtn(168, 442, 120, 24, "COPY", TAP_BTN, NULL, BTN_A);
            tbtn(304, 442, 168, 24, "COPY LINK", TAP_BTN, NULL, BTN_X);
        } else
        footer(HINT("A copy   X copy link   B back", "Enter copy  L link  Esc back", ""));
#else
        footer("B back");                         /* on the device there's nothing to copy to */
#endif
        return;
    }
    header("ENTER A CODE", "level or progress");
    /* what's typed so far: the last six rows, with a cursor */
    int nr = code_rows(Cd.code, rows, 32);
    if (Cd.n % 10 == 0) { rows[nr][0] = 0; nr++; }                /* a fresh row for the cursor */
    int first = nr > 6 ? nr - 6 : 0;
    int colw = text_w("XXXXX XXXXX", 2), x0 = (SCREEN_W - colw) / 2;
    for (int i = first; i < nr; i++) {
        char line[16];
        snprintf(line, sizeof line, "%s", rows[i]);
        if (i == nr - 1 && (now_ms() / 400) % 2) { int l = (int)strlen(line); if (l == 5) line[l++] = ' '; line[l++] = '_'; line[l] = 0; }
        text(x0, 48 + (i - first) * 28, line, 2, BG3);
    }
    if (now_ms() < Cd.msg_until) text_c(SCREEN_W / 2, 224, Cd.msg, 2, BG3);
    /* the keyboard */
    int kw = 48, kh = 36, kx = (SCREEN_W - KB_COLS * kw) / 2, ky = 256;
    for (int i = 0; i < KB_KEYS; i++) {
        int x, y, w;
        if (i < 32) { x = kx + (i % KB_COLS) * kw; y = ky + (i / KB_COLS) * kh; w = kw - 6; }
        else { x = kx + (i - 32) * 4 * kw; y = ky + 4 * kh; w = 4 * kw - 6; }
        int sel = i == Cd.sel && g_kbd != 2;
        if (sel) rect(x, y, w, kh - 6, BG3); else frame_rect(x, y, w, kh - 6, 2, BG2);
        char lab[8];
        if (i < 32) { lab[0] = B32[i]; lab[1] = 0; } else snprintf(lab, sizeof lab, "%s", i == 32 ? "Delete" : "Done");
        text_c(x + w / 2, y + 7, lab, 2, sel ? BG0 : BG3);
        tap_add(x - 2, y - 2, w + 4, kh, TAP_PICK, &Cd.sel, i);
    }
    footer(HINT("A type  B delete  START done", "Type or paste  Enter done", "Tap the keys"));
}

/* ---------------- My levels in the level list ---------------- */

static struct { int menu, sel, del_armed; } My;
/* the "+" offers New level and Enter a code; a level offers Edit, Play, Show its code (once it has
   one) and Delete */
enum { MI_NEW, MI_ENTER, MI_EDIT, MI_PLAY, MI_CODE, MI_DELETE };
static const char *my_items[5];
static int my_ids[5];
static int my_nitems(void)
{
    int n = 0, i = S.lv_sel;
    if (i >= g_sets[g_my].n) {
        my_ids[n] = MI_NEW; my_items[n++] = "New level";
        my_ids[n] = MI_ENTER; my_items[n++] = "Enter a code";
        return n;
    }
    my_ids[n] = MI_EDIT; my_items[n++] = "Edit";
    my_ids[n] = MI_PLAY; my_items[n++] = "Play";
    if (g_my_flags[i] & MY_OK) { my_ids[n] = MI_CODE; my_items[n++] = "Show its code"; }
    my_ids[n] = MI_DELETE; my_items[n++] = My.del_armed ? "Really delete it?" : "Delete";
    return n;
}

static void draw_my_levels(void)
{
    draw_bg_pattern();
    Set *s = &g_sets[g_my];
    int n = s->n + 1;                     /* and a "+" for adding */
    char b[96];
    snprintf(b, sizeof b, "%d/%d", set_solved(g_my), s->n);
    header(s->name, b);
    S.lv_sel = clampi(S.lv_sel, 0, n - 1);
    int row = S.lv_sel / LV_COLS;
    if (row < S.lv_top) S.lv_top = row;
    if (row >= S.lv_top + LV_ROWS) S.lv_top = row - LV_ROWS + 1;
    int cw = 64, ch = 32, gx = 64, gy = 48;
    for (int r = 0; r < LV_ROWS; r++)
        for (int c = 0; c < LV_COLS; c++) {
            int i = (S.lv_top + r) * LV_COLS + c;
            if (i >= n) continue;
            int x = gx + c * cw, y = gy + r * ch;
            tap_add(x - 4, y - 2, cw, ch, TAP_PICK, &S.lv_sel, i);
            int sel = i == S.lv_sel, done = i < s->n && s->best_moves[i] != 0;
            if (sel) rect(x, y, cw - 8, ch - 4, BG3);
            else if (done) rect(x, y, cw - 8, ch - 4, BG1);
            else frame_rect(x, y, cw - 8, ch - 4, 2, BG2);
            if (i < s->n) snprintf(b, sizeof b, "%d", i + 1); else snprintf(b, sizeof b, "+");
            text_c(x + (cw - 8) / 2, y + 6, b, 2, sel ? BG0 : BG3);
            if (i < s->n && fav_find(g_my, i) >= 0) heart(x + cw - 8 - 12, y + 2, 2, sel ? BG1 : BG2);
        }
    int py = 224, ix = 352;
    panel(16, py, SCREEN_W - 32, 240);
    if (S.lv_sel >= s->n) {
        text(48, py + 24, "Make a level of your own,", 2, BG3);
        text(48, py + 48, "or type in a code someone", 2, BG3);
        text(48, py + 72, "gave you.", 2, BG3);
        text(48, py + 112, "Levels you make can be up", 2, BG2);
        text(48, py + 136, "to 20 x 16. Solve one and", 2, BG2);
        text(48, py + 160, "it gets a code to share.", 2, BG2);
        text(48, py + 200, HINT("A add   B back", "Enter add   Esc back", "Tap: add"), 2, BG3);
    } else {
        int i = S.lv_sel;
        draw_thumbnail(s->maps[i], 32, py + 16, 304, 208);
        snprintf(b, sizeof b, "Level %d", i + 1);
        text(ix, py + 24, b, 2, BG3);
        int y = py + 48;
        if (s->best_moves[i] > 0) {
            text(ix, y, "Solved!", 2, BG3); y += 24;
            snprintf(b, sizeof b, "%d moves", s->best_moves[i]); text(ix, y, b, 2, BG2); y += 16;
            snprintf(b, sizeof b, "%d pushes", s->best_pushes[i]); text(ix, y, b, 2, BG2); y += 24;
        } else if (!(g_my_flags[i] & MY_OK)) {
            text(ix, y, "Draft", 2, BG3); y += 24;
            text(ix, y, "Solve it to", 2, BG2); y += 16;
            text(ix, y, "get its code", 2, BG2); y += 24;
        } else { text(ix, y, "Not yet", 2, BG2); y += 24; }
        if (g_my_par[i]) { snprintf(b, sizeof b, "Fewest: %d", g_my_par[i]); text(ix, y, b, 2, BG2); y += 24; }
        if (g_my_flags[i] & MY_CODE) text(ix, y, "From a code", 2, BG2);
        text(ix, py + 200, HINT("A choose  B back", "Enter choose", "Tap: choose"), 2, BG3);
    }
    if (My.menu) {
        int ni = my_nitems();
        int w = 384, h = 48 + ni * 32, x = (SCREEN_W - w) / 2, y = 120;
        panel(x, y, w, h);
        modal_taps(x, y, w, h, BTN_B);
        draw_menu_list(x + 32, y + 24, my_items, ni, My.sel);
        menu_taps(x + 32, y + 24, w - 48, ni, &My.sel);
    }
}

static void my_levels_key(SDLKey k)
{
    Set *s = &g_sets[g_my];
    int n = s->n + 1;
    if (My.menu) {
        int ni = my_nitems();
        if (k == BTN_UP) { menu_nav(&My.sel, ni, -1); My.del_armed = 0; }
        else if (k == BTN_DOWN) { menu_nav(&My.sel, ni, 1); My.del_armed = 0; }
        else if (k == BTN_B) { My.menu = 0; My.del_armed = 0; }
        else if (k == BTN_A) {
            int i = S.lv_sel;
            switch (my_ids[clampi(My.sel, 0, ni - 1)]) {
            case MI_NEW: My.menu = 0; if (s->n >= MY_MAX) toast("My levels is full"); else ed_open(-1); return;
            case MI_ENTER: My.menu = 0; code_enter(); return;
            case MI_PLAY: {
                const char *pr = map_problem(s->maps[i]);
                if (pr) { toast(pr); return; }
                My.menu = 0;
                go_to_level(g_my, i);
                break;
            }
            case MI_EDIT: My.menu = 0; ed_open(i); break;
            case MI_CODE: My.menu = 0; code_show(i); break;
            case MI_DELETE:
                if (!My.del_armed) { My.del_armed = 1; sfx_play(SFX_MENU); return; }
                My.menu = 0; My.del_armed = 0;
                my_delete(i);
                save_all();
                sfx_play(SFX_RESTART);
                toast("Deleted");
                break;
            }
        }
        S.dirty = 1;
        return;
    }
    if (k == BTN_LEFT) S.lv_sel = (S.lv_sel + n - 1) % n;
    else if (k == BTN_RIGHT) S.lv_sel = (S.lv_sel + 1) % n;
    else if (k == BTN_UP) S.lv_sel = S.lv_sel >= LV_COLS ? S.lv_sel - LV_COLS : S.lv_sel;
    else if (k == BTN_DOWN) S.lv_sel = S.lv_sel + LV_COLS < n ? S.lv_sel + LV_COLS : n - 1;
    else if (k == BTN_B) { S.screen = g_my_from_title ? SC_TITLE : SC_SETS; return; }
    else if (k == BTN_A || k == BTN_START) { My.menu = 1; My.sel = 0; My.del_armed = 0; sfx_play(SFX_SELECT); return; }
    else return;
    sfx_play(SFX_MENU);
}

/* ---------------- web: adding (and removing) level set files ---------------- */
#ifdef __EMSCRIPTEN__
/* Options -> Load a save file: the page wrote the chosen file to import.txt. Check it looks like a
   save, then ask before it replaces anything. */
EMSCRIPTEN_KEEPALIVE void web_save_import(void)
{
    FILE *f = fopen("import.txt", "r");
    int known = 0;
    if (f) {
        static char line[40000];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "palette ", 8) || !strncmp(line, "solved ", 7) || !strncmp(line, "current ", 8)) known++;
        fclose(f);
    }
    S.dirty = 1;
    if (!known) { remove("import.txt"); toast("That isn't a Stumped save file"); sfx_play(SFX_BUMP); return; }
    S.confirm = CF_IMPORT; S.confirm_sel = 0;
}

/* yes, load it: the current save becomes the backup, the file becomes the save, and everything reloads */
static void web_import_apply(void)
{
    FILE *f = fopen("import.txt", "a+");
    if (!f) return;
    char tail[8] = "";
    if (fseek(f, -4, SEEK_END) == 0) { size_t n = fread(tail, 1, 4, f); tail[n] = 0; }
    fseek(f, 0, SEEK_END);
    if (strcmp(tail, "end\n")) fprintf(f, "%send\n", tail[0] && tail[strlen(tail) - 1] != '\n' ? "\n" : "");
    fclose(f);
    save_all();                                    /* the current progress, safe as save.txt.bak next */
    rename(SAVE_FILE, SAVE_FILE ".bak");
    rename("import.txt", SAVE_FILE);
    free_sets();
    load_sets();
    cfg.cur_set = 0; cfg.cur_level = 0; cfg.cur_moves[0] = 0;
    load_save();
    apply_palette();
    music_set_volume(cfg.music_vol);
    sfx_set_volume(cfg.sfx_vol);
    save_all();
    S.sets_sel = 0;
    S.options_return = SC_TITLE;                   /* the level you were in may not exist now */
    sfx_play(SFX_SELECT);
    toast("Save loaded");
    S.dirty = 1;
}

/* the page wrote the files (or removed one); read the sets again and show the new one */
EMSCRIPTEN_KEEPALIVE void web_sets_reload(int added_levels)
{
    char names[MAX_SETS][48];
    int before = g_nsets;
    for (int s = 0; s < g_nsets; s++) snprintf(names[s], sizeof names[s], "%s", g_sets[s].name);
    char cur[48];
    snprintf(cur, sizeof cur, "%s", g_nsets ? g_sets[cfg.cur_set].name : "");
    save_all();
    free_sets();
    load_sets();
    cfg.cur_set = 0; cfg.cur_level = 0; cfg.cur_moves[0] = 0;
    load_save();
    int pick = -1;
    for (int s = 0; s < g_nsets && pick < 0; s++) {
        int seen = 0;
        for (int k = 0; k < before; k++) if (!strcmp(names[k], g_sets[s].name)) seen = 1;
        if (!seen) pick = s;
    }
    S.screen = SC_SETS;
    S.sets_sel = pick >= 0 ? pick : clampi(S.sets_sel, 0, g_nsets - 1);
    S.confirm = CF_NONE;
    if (added_levels < 0) toast("Added for now - too big to keep");
    else if (added_levels > 0 && pick >= 0) { char b[64]; snprintf(b, sizeof b, "Added %s", g_sets[pick].name); toast(b); }
    else if (added_levels > 0) toast("No levels found in that file");
    S.dirty = 1;
}

static void web_remove_set(int s)
{
    EM_ASM({ if (Module.stumpedRemoveSet) Module.stumpedRemoveSet(UTF8ToString($0)); }, g_sets[s].file);
    toast("Set removed");
}
#endif
