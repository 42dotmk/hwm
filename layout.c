/* hwm layout core. See layout.h. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "layout.h"

#define STBDS_REALLOC(ctx, ptr, size) erealloc(ptr, size)
#define STBDS_FREE(ctx, ptr) free(ptr)
#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))

/* a remembered placement, one `app:ws:idx:pct` line of layoutfile. An
 * `app:::pct` line (no workspace, no index) is a pinned width only: the app
 * opens where you are and hwm never rewrites its line */
typedef struct {
    char *app;
    int ws;  /* workspace, -1 = wherever you are */
    int idx; /* column index, -1 with ws */
    int pct; /* column width, percent of the screen, 0 = default */
} Rule;

Workspace *wss;
size_t curws;
Client **clients;
Monitor *mons;
static LayoutOps op;

void layoutinit(const LayoutOps *ops) {
    op = *ops;
    wss = ecalloc(nworkspaces, sizeof(Workspace));
}

void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

void *ecalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (!p)
        die("hwm: out of memory");
    return p;
}

void *erealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (!p)
        die("hwm: out of memory");
    return p;
}

char *estrdup(const char *s) {
    char *p = strdup(s);
    if (!p)
        die("hwm: out of memory");
    return p;
}

Workspace *curwsp(void) { return &wss[curws]; }

Client *focused(void) {
    Workspace *ws = curwsp();

    if (ws->floatsel)
        return ws->floatsel;
    return ws->selcol ? ws->selcol->sel : NULL;
}

static ptrdiff_t colidx(Workspace *ws, Column *col) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(ws->cols); i++)
        if (ws->cols[i] == col)
            return i;
    return -1;
}

static ptrdiff_t clientidx(Column *col, Client *c) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(col->clients); i++)
        if (col->clients[i] == c)
            return i;
    return -1;
}

static ptrdiff_t floatidx(Workspace *ws, Client *c) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(ws->floats); i++)
        if (ws->floats[i] == c)
            return i;
    return -1;
}

Monitor *wsmon(size_t wi) {
    size_t m = wss[wi].mon;

    return &mons[m < (size_t)arrlen(mons) ? m : 0];
}

static int wsvisible(size_t wi) { return wsmon(wi)->ws == wi; }

/* where hidden workspaces are parked: well left of every monitor */
static int parkoff(void) {
    ptrdiff_t i;
    int right = 0;

    for (i = 0; i < arrlen(mons); i++)
        right = MAX(right, mons[i].x + mons[i].w);
    return -3 * right;
}

static int colpx(Column *col) {
    Monitor *m = wsmon(col->ws);

    if (col->full)
        return m->w;
    return MAX(50, (int)(col->width * (float)m->w));
}

static int colvx(Workspace *ws, Column *col) {
    ptrdiff_t i;
    int x = 0;

    for (i = 0; i < arrlen(ws->cols) && ws->cols[i] != col; i++)
        x += colpx(ws->cols[i]);
    return x;
}

static void clampscroll(Workspace *ws) {
    ptrdiff_t i;
    int tw = 0, max;

    for (i = 0; i < arrlen(ws->cols); i++)
        tw += colpx(ws->cols[i]);
    max = MAX(0, tw - wsmon((size_t)(ws - wss))->w);
    if (ws->scroll > max)
        ws->scroll = max;
    if (ws->scroll < 0)
        ws->scroll = 0;
}

/* viewport offset to lay out at: eased from animfrom toward scroll */
static int dispscroll(Workspace *ws) {
    float t;

    if (!ws->animating)
        return ws->scroll;
    t = (float)(op.now() - ws->animstart) / (float)scrollanimms;
    if (t >= 1.0f) {
        ws->animating = 0;
        return ws->scroll;
    }
    t = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); /* ease-out cubic */
    return ws->animfrom + (int)(t * (float)(ws->scroll - ws->animfrom));
}

/* call before changing ws->scroll: the viewport glides there from wherever
 * it is now, including from the middle of an animation in flight */
static void beginscroll(Workspace *ws) {
    if (!scrollanimms || !wsvisible((size_t)(ws - wss)))
        return;
    ws->animfrom = dispscroll(ws);
    ws->animstart = op.now();
    ws->animating = 1;
}

void ensurevisible(Column *col) {
    Workspace *ws = &wss[col->ws];
    int vx = colvx(ws, col), cw = colpx(col), uw = wsmon(col->ws)->w;

    beginscroll(ws);
    if (cw >= uw || vx < ws->scroll)
        ws->scroll = vx;
    else if (vx + cw > ws->scroll + uw)
        ws->scroll = vx + cw - uw;
    clampscroll(ws);
}

void snapscroll(Workspace *ws) {
    ptrdiff_t i;
    int mw = wsmon((size_t)(ws - wss))->w;
    int vx = 0, cw, cand, d, best = ws->scroll, bd = INT_MAX;

    if (!arrlen(ws->cols))
        return;
    beginscroll(ws);
    for (i = 0; i < arrlen(ws->cols); i++) {
        cw = colpx(ws->cols[i]);
        cand = vx; /* column's left edge at the left of the screen */
        d = abs(cand - ws->scroll);
        if (d < bd) {
            bd = d;
            best = cand;
        }
        cand = vx + cw - mw; /* right edge at the right of the screen */
        d = abs(cand - ws->scroll);
        if (d < bd) {
            bd = d;
            best = cand;
        }
        vx += cw;
    }
    ws->scroll = best;
    clampscroll(ws);
}

Client *findclient(unsigned long win) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(clients); i++)
        if (clients[i]->win == win)
            return clients[i];
    return NULL;
}

void moveresize(Client *c, int x, int y, int w, int h) {
    c->x = x;
    c->y = y;
    c->w = MAX(1, w);
    c->h = MAX(1, h);
    op.apply(c, c->x, c->y, c->w, c->h, (int)borderpx);
}

void arrangews(size_t wi) {
    Workspace *ws = &wss[wi];
    Monitor *m = wsmon(wi);
    Column *col;
    Client *c;
    ptrdiff_t ci, i, n;
    int xoff = wsvisible(wi) ? 0 : parkoff(); /* park hidden workspaces */
    int x, y, cw, ch, gap, bw;

    clampscroll(ws);
    if (ws->animating && ws->scroll == ws->animfrom)
        ws->animating = 0; /* retarget landed where we already are */
    x = -dispscroll(ws);
    for (ci = 0; ci < arrlen(ws->cols); ci++) {
        col = ws->cols[ci];
        cw = colpx(col);
        n = arrlen(col->clients);
        /* cells tile edge to edge; gaps are mere decoration, an inset
         * of every window inside its cell (none for a full column) */
        gap = col->full ? 0 : (int)gappx;
        bw = col->full ? 0 : (int)borderpx;
        for (i = 0; i < n; i++) {
            c = col->clients[i];
            y = (int)(i * m->h / n);
            ch = (int)((i + 1) * m->h / n) - y;
            c->x = xoff + m->x + x + gap;
            c->y = m->y + y + gap;
            c->w = MAX(1, cw - 2 * (gap + bw));
            c->h = MAX(1, ch - 2 * (gap + bw));
            op.apply(c, c->x, c->y, c->w, c->h, bw);
        }
        x += cw;
    }
    /* floats keep their own (absolute) geometry above the strip */
    for (i = 0; i < arrlen(ws->floats); i++) {
        c = ws->floats[i];
        op.apply(c, xoff + c->x, c->y, c->w, c->h, (int)borderpx);
        op.raise(c);
    }
}

void trackscroll(Workspace *ws, int target) {
    ws->animating = 0;
    ws->scroll = target;
    clampscroll(ws);
    arrangews((size_t)(ws - wss));
}

void focus(Client *c) {
    Workspace *ws;

    /* focusing a window on another (visible) workspace follows it there */
    if (c && c->ws != curws && wsvisible(c->ws))
        curws = c->ws;
    ws = curwsp();
    if (c) {
        if (c->isfloating) {
            ws->floatsel = c;
            op.raise(c);
        } else {
            ws->floatsel = NULL;
            ws->selcol = c->col;
            c->col->sel = c;
        }
    }
    op.focus(c);
}

/* insert c after col's selection and make it the selection */
static void attach(Column *col, Client *c) {
    ptrdiff_t at =
        col->sel ? clientidx(col, col->sel) + 1 : arrlen(col->clients);

    arrins(col->clients, at, c);
    col->sel = c;
    c->col = col;
    c->ws = col->ws;
    wss[col->ws].selcol = col;
}

/* put c alone into a new column inserted at index `at` */
static Column *attachat(size_t wi, ptrdiff_t at, Client *c) {
    Workspace *ws = &wss[wi];
    Column *col = ecalloc(1, sizeof(Column));

    col->width = defwidth;
    col->ws = wi;
    arrins(ws->cols, at, col);
    attach(col, c);
    return col;
}

/* put c alone into a new column inserted after `after` (NULL = leftmost) */
static Column *attachnew(size_t wi, Column *after, Client *c) {
    return attachat(wi, after ? colidx(&wss[wi], after) + 1 : 0, c);
}

void setcolwidth(Column *col, float w) {
    col->width = w < 0.1f ? 0.1f : w > 1.0f ? 1.0f : w;
}

/* remove c from its column; empty columns are freed */
static void detach(Client *c) {
    Column *col = c->col;
    Workspace *ws = &wss[col->ws];
    ptrdiff_t i = clientidx(col, c), ci;

    arrdel(col->clients, i);
    if (col->sel == c)
        col->sel = arrlen(col->clients)
                       ? col->clients[i < arrlen(col->clients)
                                          ? i
                                          : arrlen(col->clients) - 1]
                       : NULL;
    if (!arrlen(col->clients)) {
        ci = colidx(ws, col);
        arrdel(ws->cols, ci);
        if (ws->selcol == col)
            ws->selcol =
                arrlen(ws->cols)
                    ? ws->cols[ci < arrlen(ws->cols) ? ci
                                                     : arrlen(ws->cols) - 1]
                    : NULL;
        arrfree(col->clients);
        free(col);
    }
    c->col = NULL;
}

/* remove c from its workspace's float list */
static void detachfloat(Client *c) {
    Workspace *ws = &wss[c->ws];

    arrdel(ws->floats, floatidx(ws, c));
    if (ws->floatsel == c)
        ws->floatsel = NULL;
}

/* keep the requested geometry; center windows that didn't ask for a
 * position, clamp the rest onto their monitor */
static void placefloat(Client *c) {
    Monitor *m = wsmon(c->ws);

    c->w = MAX(1, c->w);
    c->h = MAX(1, c->h);
    if (c->x <= 0 && c->y <= 0) {
        c->x = m->x + (m->w - c->w) / 2;
        c->y = m->y + (m->h - c->h) / 2;
    }
    if (c->x + c->w + 2 * (int)borderpx > m->x + m->w)
        c->x = m->x + m->w - c->w - 2 * (int)borderpx;
    if (c->y + c->h + 2 * (int)borderpx > m->y + m->h)
        c->y = m->y + m->h - c->h - 2 * (int)borderpx;
    c->x = MAX(m->x, c->x);
    c->y = MAX(m->y, c->y);
}

/* placement memory: layoutfile holds one `app:workspace:column:percent`
 * line per app. It is read afresh whenever a window is placed, so edits by
 * hand take effect at once, and rewritten whenever a placement changes:
 * a few lines through a temp file and rename, microseconds and atomic */

static const char *layoutpath(void) {
    static char path[PATH_MAX];
    const char *home = getenv("HOME");

    if (layoutfile[0] == '~' && home)
        snprintf(path, sizeof path, "%s%s", home, layoutfile + 1);
    else
        snprintf(path, sizeof path, "%s", layoutfile);
    return path;
}

/* split from the right, so the app name itself may contain colons */
static int parserule(char *line, Rule *r) {
    char *f[3], *p;
    int i;

    for (i = 2; i >= 0; i--) {
        if (!(p = strrchr(line, ':')))
            return 0;
        *p = '\0';
        f[i] = p + 1;
    }
    if (!*line)
        return 0;
    r->app = estrdup(line);
    r->ws = *f[0] ? atoi(f[0]) : -1;
    r->idx = *f[1] ? atoi(f[1]) : -1;
    r->pct = atoi(f[2]);
    return 1;
}

static Rule *loadlayout(void) {
    Rule *rules = NULL, r;
    char line[512];
    FILE *fp;

    if (!preservelayout || !(fp = fopen(layoutpath(), "r")))
        return NULL;
    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (parserule(line, &r))
            arrput(rules, r);
    }
    fclose(fp);
    return rules;
}

static void freelayout(Rule *rules) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(rules); i++)
        free(rules[i].app);
    arrfree(rules);
}

static Rule *findrule(Rule *rules, const char *app) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(rules); i++)
        if (!strcmp(rules[i].app, app))
            return &rules[i];
    return NULL;
}

static void writelayout(Rule *rules) {
    const char *path = layoutpath();
    char tmp[PATH_MAX + 8], *slash;
    FILE *fp;
    ptrdiff_t i;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((slash = strrchr(tmp, '/'))) { /* the directory may not exist yet */
        *slash = '\0';
        mkdir(tmp, 0755);
        *slash = '/';
    }
    if (!(fp = fopen(tmp, "w"))) {
        fprintf(stderr, "hwm: cannot write %s\n", tmp);
        return;
    }
    for (i = 0; i < arrlen(rules); i++)
        if (rules[i].ws < 0) {
            fprintf(fp, "%s:::", rules[i].app);
            if (rules[i].pct > 0)
                fprintf(fp, "%d", rules[i].pct);
            fputc('\n', fp);
        } else {
            fprintf(fp, "%s:%d:%d:%d\n", rules[i].app, rules[i].ws,
                    rules[i].idx, rules[i].pct);
        }
    if (fclose(fp) != 0 || rename(tmp, path) != 0)
        fprintf(stderr, "hwm: cannot write %s\n", path);
}

/* remember where c sits, so the next window of its app opens there */
void savelayout(Client *c) {
    Rule *rules, *r, n;

    if (!preservelayout || !c || !c->app || !c->col)
        return;
    n.app = c->app;
    n.ws = (int)c->ws;
    n.idx = (int)colidx(&wss[c->ws], c->col);
    n.pct = (int)(c->col->width * 100.0f + 0.5f);
    rules = loadlayout();
    r = findrule(rules, c->app);
    if (r && (r->ws < 0 || /* opens wherever you are: never recorded */
              (r->ws == n.ws && r->idx == n.idx && r->pct == n.pct))) {
        freelayout(rules);
        return;
    }
    if (r) {
        r->ws = n.ws;
        r->idx = n.idx;
        r->pct = n.pct;
    } else {
        n.app = estrdup(c->app);
        arrput(rules, n);
    }
    writelayout(rules);
    freelayout(rules);
}

/* tile a new window where its app was last placed, else next to the
 * selection. follow: switch to that workspace (not while adopting windows
 * at startup, which would hop around) */
static Column *place(Client *c, int follow) {
    Rule *rules = NULL, *r = NULL;
    Column *col;
    Arg a;

    if (c->app) {
        rules = loadlayout();
        r = findrule(rules, c->app);
    }
    if (r && r->ws >= 0 && (size_t)r->ws < nworkspaces) {
        if (follow) {
            a.i = r->ws;
            view(&a);
        }
        col = attachat((size_t)r->ws,
                       MIN(MAX(r->idx, 0), arrlen(wss[r->ws].cols)), c);
    } else {
        col = attachnew(curws, curwsp()->selcol, c);
    }
    if (r && r->pct > 0)
        setcolwidth(col, (float)r->pct / 100.0f);
    freelayout(rules);
    return col;
}

Client *manage(const Client *t, int follow) {
    Client *c = ecalloc(1, sizeof(Client));

    c->win = t->win;
    c->app = t->app;
    c->isfloating = t->isfloating;
    c->x = t->x;
    c->y = t->y;
    c->w = t->w;
    c->h = t->h;
    c->ws = curws;
    if (c->isfloating) {
        placefloat(c);
        arrput(curwsp()->floats, c);
    } else {
        ensurevisible(place(c, follow));
    }
    arrput(clients, c);
    arrangews(c->ws);
    return c;
}

void unmanage(Client *c) {
    size_t wi = c->ws;
    ptrdiff_t i;

    if (c->isfloating)
        detachfloat(c);
    else
        detach(c);
    for (i = 0; i < arrlen(clients); i++)
        if (clients[i] == c) {
            arrdel(clients, i);
            break;
        }
    free(c->app);
    free(c);
    arrangews(wi);
    if (wi == curws)
        focus(focused());
}

size_t monat(int x, int y) {
    ptrdiff_t i;

    for (i = 0; i < arrlen(mons); i++)
        if (x >= mons[i].x && x < mons[i].x + mons[i].w && y >= mons[i].y &&
            y < mons[i].y + mons[i].h)
            return (size_t)i;
    return wss[curws].mon;
}

/* a new monitor takes over the first hidden workspace; workspaces of
 * detached monitors move to the one under the pointer */
void setmons(const Monitor *geoms, size_t n, int px, int py) {
    ptrdiff_t oldn = arrlen(mons), m;
    size_t i, k, am;

    if (n < 1)
        n = 1;
    arrsetlen(mons, n);
    for (i = 0; i < n; i++) {
        mons[i].x = geoms[i].x;
        mons[i].y = geoms[i].y;
        mons[i].w = geoms[i].w;
        mons[i].h = geoms[i].h;
        if ((ptrdiff_t)i >= oldn)
            mons[i].ws = nworkspaces;
    }
    if (!oldn)
        mons[0].ws = curws;
    if ((ptrdiff_t)n < oldn) {
        am = monat(px, py);
        if (am >= n) /* pointer on a dead monitor */
            am = 0;
        for (k = 0; k < nworkspaces; k++)
            if (wss[k].mon >= n)
                wss[k].mon = am;
        mons[wss[curws].mon].ws = curws; /* keep the focus visible */
    }
    for (m = 0; m < (ptrdiff_t)n; m++) {
        if (mons[m].ws < nworkspaces && wss[mons[m].ws].mon == (size_t)m)
            continue;
        mons[m].ws = nworkspaces;
        for (k = 0; k < nworkspaces; k++)
            if (!wsvisible(k)) { /* first hidden workspace */
                wss[k].mon = (size_t)m;
                mons[m].ws = k;
                break;
            }
    }
    for (k = 0; k < nworkspaces; k++)
        arrangews(k);
}

void syncmon(size_t m) {
    size_t k = mons[m].ws;

    if (k < nworkspaces && k != curws) {
        curws = k;
        focus(focused());
    }
}

int animstep(void) {
    ptrdiff_t i;
    int busy = 0;

    for (i = 0; i < arrlen(mons); i++) {
        if (mons[i].ws >= nworkspaces || !wss[mons[i].ws].animating)
            continue;
        arrangews(mons[i].ws);
        busy |= wss[mons[i].ws].animating;
    }
    return busy;
}

/* commands */

void focushorz(const Arg *arg) {
    Workspace *ws = curwsp();
    Column *col;
    ptrdiff_t i;

    if (!ws->selcol)
        return;
    i = colidx(ws, ws->selcol) + (arg->i > 0 ? 1 : -1);
    if (i < 0 || i >= arrlen(ws->cols))
        return;
    col = ws->cols[i];
    focus(col->sel ? col->sel : col->clients[0]);
    ensurevisible(col);
    arrangews(curws);
}

void focusvert(const Arg *arg) {
    Client *c = focused();
    ptrdiff_t i;

    if (!c || c->isfloating)
        return;
    i = clientidx(c->col, c) + (arg->i > 0 ? 1 : -1);
    if (i < 0 || i >= arrlen(c->col->clients))
        return;
    focus(c->col->clients[i]);
}

void movehorz(const Arg *arg) {
    Workspace *ws = curwsp();
    Client *c = focused();
    Column *col, *after;
    ptrdiff_t i, j;

    if (!c || c->isfloating)
        return;
    col = c->col;
    i = colidx(ws, col);
    if (arrlen(col->clients) == 1) {
        /* window is alone in its column: swap columns */
        j = i + (arg->i > 0 ? 1 : -1);
        if (j < 0 || j >= arrlen(ws->cols))
            return;
        ws->cols[i] = ws->cols[j];
        ws->cols[j] = col;
    } else {
        /* split it out into its own new column */
        after = arg->i > 0 ? col : (i > 0 ? ws->cols[i - 1] : NULL);
        detach(c);
        attachnew(curws, after, c);
    }
    ensurevisible(c->col);
    arrangews(curws);
    focus(c);
    savelayout(c);
}

/* consume: stack the focused window into the adjacent column */
void stackto(const Arg *arg) {
    Workspace *ws = curwsp();
    Client *c = focused();
    Column *col;
    ptrdiff_t i;

    if (!c || c->isfloating)
        return;
    i = colidx(ws, c->col) + (arg->i > 0 ? 1 : -1);
    if (i < 0 || i >= arrlen(ws->cols))
        return;
    col = ws->cols[i]; /* before detach: it may free c's column */
    detach(c);
    attach(col, c);
    ensurevisible(col);
    arrangews(curws);
    focus(c);
    savelayout(c);
}

void movevert(const Arg *arg) {
    Client *c = focused();
    Column *col;
    ptrdiff_t i, j;

    if (!c || c->isfloating)
        return;
    col = c->col;
    i = clientidx(col, c);
    j = i + (arg->i > 0 ? 1 : -1);
    if (j < 0 || j >= arrlen(col->clients))
        return;
    col->clients[i] = col->clients[j];
    col->clients[j] = c;
    arrangews(curws);
    savelayout(c);
}

void cyclewidth(const Arg *arg) {
    Column *col = curwsp()->selcol;
    ptrdiff_t i, best = 0;
    float d, bd = 2.0f;

    (void)arg;
    if (!col || !arrlen(widths))
        return;
    for (i = 0; i < arrlen(widths); i++) {
        d = col->width - widths[i];
        if (d < 0)
            d = -d;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    col->width = widths[(best + 1) % arrlen(widths)];
    ensurevisible(col);
    arrangews(curws);
    savelayout(col->sel);
}

void growwidth(const Arg *arg) {
    Column *col = curwsp()->selcol;

    if (!col)
        return;
    setcolwidth(col, col->width + arg->f);
    ensurevisible(col);
    arrangews(curws);
    savelayout(col->sel);
}

void setwidth(const Arg *arg) {
    Column *col = curwsp()->selcol;

    if (!col)
        return;
    setcolwidth(col, arg->f);
    ensurevisible(col);
    arrangews(curws);
    savelayout(col->sel);
}

void scrollby(const Arg *arg) {
    beginscroll(curwsp());
    curwsp()->scroll += (int)(arg->f * (float)wsmon(curws)->w);
    arrangews(curws);
}

void togglefull(const Arg *arg) {
    Client *c = focused();

    (void)arg;
    if (!c)
        return;
    if (c->isfloating)
        togglefloat(NULL); /* tile it; a full column is just a column */
    c->col->full = !c->col->full;
    ensurevisible(c->col);
    arrangews(curws);
}

void togglefloat(const Arg *arg) {
    Workspace *ws = curwsp();
    Monitor *m = wsmon(curws);
    Client *c = focused();

    (void)arg;
    if (!c)
        return;
    if (c->isfloating) {
        detachfloat(c);
        c->isfloating = 0;
        ensurevisible(attachnew(curws, ws->selcol, c));
    } else {
        detach(c);
        c->isfloating = 1;
        c->w = (int)((float)m->w * floatsize);
        c->h = (int)((float)m->h * floatsize);
        c->x = m->x + (m->w - c->w) / 2 - (int)borderpx;
        c->y = m->y + (m->h - c->h) / 2 - (int)borderpx;
        arrput(ws->floats, c);
    }
    arrangews(curws);
    focus(c);
    savelayout(c);
}

void view(const Arg *arg) {
    size_t old, m;

    if (arg->i < 0 || (size_t)arg->i >= nworkspaces || (size_t)arg->i == curws)
        return;
    curws = (size_t)arg->i;
    m = wss[curws].mon;
    old = mons[m].ws; /* the workspace this monitor showed before */
    mons[m].ws = curws;
    if (old < nworkspaces && old != curws)
        arrangews(old);
    arrangews(curws);
    op.warp(&mons[m]);
    focus(focused());
}

/* move the focused workspace to the adjacent monitor and follow it */
void movewsmon(const Arg *arg) {
    size_t om = wss[curws].mon, k, r = nworkspaces, prev;
    ptrdiff_t nm;

    if (arrlen(mons) < 2)
        return;
    nm = (ptrdiff_t)om + (arg->i > 0 ? 1 : -1);
    if (nm < 0)
        nm = arrlen(mons) - 1;
    else if (nm >= arrlen(mons))
        nm = 0;
    /* the old monitor needs another workspace to show */
    for (k = 0; k < nworkspaces && r == nworkspaces; k++)
        if (k != curws && wss[k].mon == om)
            r = k;
    for (k = 0; k < nworkspaces && r == nworkspaces; k++)
        if (k != curws && !wsvisible(k))
            r = k;
    if (r == nworkspaces)
        return;
    wss[r].mon = om;
    mons[om].ws = r;
    prev = mons[nm].ws;
    wss[curws].mon = (size_t)nm;
    mons[nm].ws = curws;
    if (prev < nworkspaces && prev != curws)
        arrangews(prev);
    arrangews(r);
    arrangews(curws);
    op.warp(&mons[nm]);
    focus(focused());
}

void sendto(const Arg *arg) {
    Workspace *target;
    Client *c = focused();
    float width;

    if (!c || arg->i < 0 || (size_t)arg->i >= nworkspaces ||
        (size_t)arg->i == curws)
        return;
    target = &wss[arg->i];
    if (c->isfloating) {
        detachfloat(c);
        arrput(target->floats, c);
    } else {
        width = c->col->width;
        detach(c);
        setcolwidth(attachnew((size_t)arg->i, target->selcol, c), width);
    }
    c->ws = (size_t)arg->i;
    op.desktop(c);
    arrangews(curws);
    arrangews((size_t)arg->i);
    focus(focused());
    savelayout(c);
}
