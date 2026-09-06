/* test_layout.c - headless tests for the layout core. A fake shell records
 * what the layout asks of it; a fake clock drives the animations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "layout.h"

#include <stb_ds.h>

/* the layout's configuration, standing in for config.h */
const unsigned int borderpx = 2;
const unsigned int gappx = 6;
const unsigned int scrollanimms = 200;
const float defwidth = 0.5f;
const float floatsize = 0.6f;
const int preservelayout = 1;
const char layoutfile[] = "~/hwm.layout";
float *widths;
const size_t nworkspaces = 10;

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* the fake shell */

typedef struct {
    int x, y, w, h, bw;
} Geom;

static Geom applied[1024]; /* last geometry applied, by window */
static Client *lastfocus;
static int nfocus, nraise, ndesktop, nwarp;
static Monitor *lastwarp;
static long clk;

static void fapply(Client *c, int x, int y, int w, int h, int bw) {
    Geom g = {x, y, w, h, bw};

    applied[c->win] = g;
}

static void fraise(Client *c) {
    (void)c;
    nraise++;
}

static void ffocus(Client *c) {
    lastfocus = c;
    nfocus++;
}

static void fdesktop(Client *c) {
    (void)c;
    ndesktop++;
}

static void fwarp(Monitor *m) {
    lastwarp = m;
    nwarp++;
}

static long fnow(void) { return clk; }

static const LayoutOps ops = {fapply, fraise, ffocus, fdesktop, fwarp, fnow};

/* helpers */

static Geom G(Client *c) { return applied[c->win]; }

static Client *open_(const char *app, int floating, int x, int y, int w,
                     int h) {
    static unsigned long nextwin = 1;
    Client t = {0};

    t.win = nextwin++;
    t.app = app ? estrdup(app) : NULL;
    t.isfloating = floating;
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    return manage(&t, 1);
}

static Client *tile(const char *app) {
    Client *c = open_(app, 0, 0, 0, 100, 100);

    focus(c);
    savelayout(c);
    return c;
}

/* let scroll animations land */
static void settle(void) {
    clk += 1000;
    animstep();
}

static void cmd(void (*f)(const Arg *), int i) {
    Arg a = {.i = i};

    f(&a);
    settle();
}

static void cmdf(void (*f)(const Arg *), float v) {
    Arg a = {.f = v};

    f(&a);
    settle();
}

static ptrdiff_t ncols(void) { return arrlen(curwsp()->cols); }
static Column *col(int i) { return curwsp()->cols[i]; }

static void screen(int nmon) {
    Monitor m[2] = {{0, 0, 1000, 600, 0}, {1000, 0, 800, 600, 0}};

    setmons(m, (size_t)nmon, -1, -1);
}

/* start over with one 1000x600 monitor and nothing on it */
static void fresh(void) {
    while (arrlen(clients))
        unmanage(clients[0]);
    arrfree(clients);
    arrfree(mons);
    curws = 0;
    layoutinit(&ops);
    screen(1);
    unlink("/tmp/hwm-test-home/hwm.layout");
}

static char *slurp(const char *path) {
    static char buf[4096];
    FILE *fp = fopen(path, "r");
    size_t n = 0;

    if (fp) {
        n = fread(buf, 1, sizeof buf - 1, fp);
        fclose(fp);
    }
    buf[n] = '\0';
    return buf;
}

int main(void) {
    Client *a, *b, *c, *d;
    Geom g;

    setenv("HOME", "/tmp/hwm-test-home", 1);
    mkdir("/tmp/hwm-test-home", 0755);
    arrput(widths, 1.0f / 3.0f);
    arrput(widths, 0.5f);
    arrput(widths, 2.0f / 3.0f);
    arrput(widths, 1.0f);
    fresh();

    /* a window gets its own column of defwidth, inset by gap and border */
    a = tile(NULL);
    g = G(a);
    CHECK(ncols() == 1 && a->col == col(0) && !a->isfloating);
    CHECK(g.x == 6 && g.y == 6 && g.w == 484 && g.h == 584 && g.bw == 2);
    CHECK(focused() == a && lastfocus == a);

    /* new windows open right of the selection, never resizing others */
    b = tile(NULL);
    CHECK(ncols() == 2 && col(1) == b->col && G(b).x == 506);
    CHECK(G(a).w == 484);
    cmd(focushorz, -1);
    CHECK(focused() == a);
    c = tile(NULL);
    CHECK(ncols() == 3 && col(1) == c->col && col(2) == b->col);
    CHECK(curwsp()->scroll == 0);

    /* focus stops at the strip's ends */
    cmd(focushorz, -1);
    cmd(focushorz, -1);
    CHECK(focused() == a);

    /* stacking splits the column's height evenly */
    focus(c);
    cmd(stackto, -1);
    CHECK(ncols() == 2 && c->col == a->col && arrlen(a->col->clients) == 2);
    CHECK(a->col->clients[0] == a && a->col->clients[1] == c);
    CHECK(G(a).y == 6 && G(a).h == 284 && G(c).y == 306 && G(c).h == 284);
    CHECK(focused() == c);

    /* vertical focus and moves within a column */
    cmd(focusvert, -1);
    CHECK(focused() == a);
    cmd(focusvert, -1);
    CHECK(focused() == a);
    cmd(movevert, +1);
    CHECK(a->col->clients[0] == c && a->col->clients[1] == a);
    CHECK(G(c).y == 6 && G(a).y == 306);

    /* moving a stacked window splits it out; alone, it swaps columns */
    cmd(movehorz, +1);
    CHECK(ncols() == 3 && col(0) == c->col && col(1) == a->col &&
          col(2) == b->col);
    CHECK(arrlen(c->col->clients) == 1 && G(c).h == 584);
    cmd(movehorz, +1);
    CHECK(col(1) == b->col && col(2) == a->col);
    /* which scrolled the strip to keep the focused column on screen */
    CHECK(curwsp()->scroll == 500 && G(a).x == 506 && G(c).x == -494);

    /* scrolling clamps to the strip */
    cmdf(scrollby, -1.0f);
    CHECK(curwsp()->scroll == 0 && G(c).x == 6);
    cmdf(scrollby, +5.0f);
    CHECK(curwsp()->scroll == 500);

    /* snapping picks the nearest column edge aligned with a screen edge */
    trackscroll(curwsp(), 320);
    CHECK(curwsp()->scroll == 320 && G(c).x == -314);
    snapscroll(curwsp());
    settle();
    CHECK(curwsp()->scroll == 500);
    trackscroll(curwsp(), 120);
    snapscroll(curwsp());
    settle();
    CHECK(curwsp()->scroll == 0);

    /* animation eases the viewport toward the target, ease-out cubic */
    cmdf(scrollby, 0.0f);
    clk = 10000;
    cmdf(scrollby, +0.5f); /* settle() lands it */
    CHECK(curwsp()->scroll == 500 && !curwsp()->animating);
    cmdf(scrollby, -1.0f);
    {
        Arg half = {.f = +0.5f};

        scrollby(&half); /* begins at clk */
        CHECK(curwsp()->animating && curwsp()->scroll == 500);
        CHECK(G(c).x == 6); /* nothing moved yet */
        clk += 100;
        CHECK(animstep());
        CHECK(G(c).x == 6 - 437); /* 1 - (1-0.5)^3 = 0.875 of the way */
        clk += 100;
        CHECK(!animstep());
        CHECK(G(c).x == -494 && !curwsp()->animating);
    }
    /* drags and swipes cut an animation short */
    cmdf(scrollby, -1.0f);
    {
        Arg half = {.f = +0.5f};

        scrollby(&half);
        trackscroll(curwsp(), 100);
        CHECK(!curwsp()->animating && G(c).x == -94);
    }

    /* widths: presets, growth, clamping */
    fresh();
    a = tile(NULL);
    cmdf(setwidth, 1.0f / 3.0f);
    CHECK(G(a).w == 333 - 16);
    cmd(cyclewidth, 0);
    CHECK(a->col->width == 0.5f);
    cmd(cyclewidth, 0);
    CHECK(G(a).w == 666 - 16);
    cmdf(growwidth, +2.0f);
    CHECK(a->col->width == 1.0f && G(a).w == 984);
    cmdf(growwidth, -2.0f);
    CHECK(a->col->width == 0.1f && G(a).w == 100 - 16);

    /* a full column spans the monitor without gaps or borders */
    cmd(togglefull, 0);
    g = G(a);
    CHECK(a->col->full && g.x == 0 && g.y == 0 && g.w == 1000 && g.h == 600 &&
          g.bw == 0);
    cmd(togglefull, 0);
    CHECK(!a->col->full && G(a).bw == 2);

    /* floating: centered at floatsize, raised, out of the strip */
    b = tile(NULL);
    nraise = 0;
    cmd(togglefloat, 0);
    g = G(b);
    CHECK(b->isfloating && !b->col && ncols() == 1);
    CHECK(g.x == 198 && g.y == 118 && g.w == 600 && g.h == 360 && g.bw == 2);
    CHECK(nraise > 0 && focused() == b && curwsp()->floatsel == b);
    cmd(togglefloat, 0);
    CHECK(!b->isfloating && ncols() == 2 && col(1) == b->col);
    CHECK(curwsp()->floatsel == NULL);

    /* windows that float on arrival keep their geometry, centered when they
     * asked for no position, clamped onto the monitor otherwise */
    c = open_(NULL, 1, 0, 0, 300, 200);
    CHECK(c->x == 350 && c->y == 200 && G(c).x == 350 && G(c).w == 300);
    d = open_(NULL, 1, 900, 500, 300, 200);
    CHECK(d->x == 696 && d->y == 396);
    CHECK(arrlen(curwsp()->floats) == 2 && focused() == b);
    unmanage(c);
    unmanage(d);
    CHECK(arrlen(curwsp()->floats) == 0);

    /* closing a window: the column to its right inherits the focus, an
     * empty column disappears */
    c = tile(NULL); /* cols: a b c */
    focus(b);
    unmanage(b);
    CHECK(ncols() == 2 && col(0) == a->col && col(1) == c->col);
    CHECK(focused() == c && lastfocus == c);
    unmanage(c);
    unmanage(a);
    CHECK(ncols() == 0 && focused() == NULL && lastfocus == NULL);

    /* workspaces: sending parks the window offscreen, keeping its width;
     * viewing brings it back and warps the pointer */
    fresh();
    a = tile(NULL);
    cmdf(setwidth, 0.3f);
    ndesktop = nwarp = 0;
    cmd(sendto, 1);
    CHECK(a->ws == 1 && ndesktop == 1 && ncols() == 0 && curws == 0);
    CHECK(G(a).x == -3000 + 6 && G(a).w == 300 - 16);
    CHECK(wss[1].cols[0]->width == 0.3f);
    cmd(view, 1);
    CHECK(curws == 1 && nwarp == 1 && lastwarp == &mons[0]);
    CHECK(G(a).x == 6 && focused() == a);
    cmd(view, 1); /* no-op */
    CHECK(nwarp == 1);
    cmd(view, 0);
    CHECK(curws == 0 && G(a).x == -3000 + 6);

    /* two monitors: the second takes the first hidden workspace */
    fresh();
    a = tile(NULL);
    screen(2);
    CHECK(arrlen(mons) == 2 && mons[0].ws == 0 && mons[1].ws == 1 &&
          wss[1].mon == 1);
    CHECK(G(a).x == 6);
    cmd(view, 1);
    CHECK(lastwarp == &mons[1]);
    b = tile(NULL);
    CHECK(b->ws == 1 && G(b).x == 1006 && G(b).w == 400 - 16);
    /* focusing across monitors follows the window to its workspace */
    focus(a);
    CHECK(curws == 0 && focused() == a);
    syncmon(1);
    CHECK(curws == 1 && focused() == b);
    /* a workspace moves to the adjacent monitor; the old one shows the next
     * hidden workspace */
    cmd(movewsmon, -1);
    CHECK(wss[1].mon == 0 && mons[0].ws == 1 && mons[1].ws == 2 &&
          wss[2].mon == 1);
    CHECK(G(b).x == 6 && G(a).x < 0);
    /* a monitor going away hands its workspaces to the one under the pointer
     * and keeps the current workspace visible */
    cmd(view, 2);
    screen(1);
    CHECK(arrlen(mons) == 1 && wss[2].mon == 0 && mons[0].ws == 2);
    CHECK(curws == 2 && G(b).x < 0);

    /* placement memory: apps reopen where they were, hand edits apply at
     * once, `app:::` opts out, `app:::pct` pins a width */
    fresh();
    a = tile("edit");
    CHECK(!strcmp(slurp("/tmp/hwm-test-home/hwm.layout"), "edit:0:0:50\n"));
    b = tile("web");
    cmdf(setwidth, 0.7f);
    CHECK(!strcmp(slurp("/tmp/hwm-test-home/hwm.layout"),
                  "edit:0:0:50\nweb:0:1:70\n"));
    cmd(sendto, 3);
    CHECK(!strcmp(slurp("/tmp/hwm-test-home/hwm.layout"),
                  "edit:0:0:50\nweb:3:0:70\n"));
    cmd(view, 5);
    c = open_("web", 0, 0, 0, 1, 1); /* follows the placement */
    CHECK(c->ws == 3 && curws == 3 && c->col->width == 0.7f);
    CHECK(wss[3].cols[0] == c->col && wss[3].cols[1] == b->col);
    cmd(view, 5);
    {
        Client t = {0};

        t.win = 999;
        t.app = estrdup("edit");
        t.w = t.h = 1;
        d = manage(&t, 0); /* adopting at startup: no hopping around */
    }
    CHECK(d->ws == 0 && curws == 5);
    {
        FILE *fp = fopen("/tmp/hwm-test-home/hwm.layout", "w");

        fputs("edit:0:0:50\nfree:::\nwide:::35\nx:y:1:0:40\n", fp);
        fclose(fp);
    }
    c = tile("free");
    CHECK(c->ws == 5 && curws == 5 && c->col->width == 0.5f);
    c = tile("wide");
    CHECK(c->ws == 5 && c->col->width == 0.35f);
    cmdf(setwidth, 0.9f); /* opted-out apps are never recorded */
    CHECK(!strcmp(slurp("/tmp/hwm-test-home/hwm.layout"),
                  "edit:0:0:50\nfree:::\nwide:::35\nx:y:1:0:40\n"));
    c = tile("x:y"); /* app names may hold colons */
    CHECK(c->ws == 1 && curws == 1 && c->col->width == 0.4f);
    CHECK(!strcmp(slurp("/tmp/hwm-test-home/hwm.layout"),
                  "edit:0:0:50\nfree:::\nwide:::35\nx:y:1:0:40\n"));
    cmd(movehorz, +1); /* nothing to swap with: no change, no write */
    cmd(sendto, 2);
    CHECK(!strcmp(slurp("/tmp/hwm-test-home/hwm.layout"),
                  "edit:0:0:50\nfree:::\nwide:::35\nx:y:2:0:40\n"));

    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        puts("all layout tests passed");
    return failures ? 1 : 0;
}
