/* hwm layout core: the scrollable-column model and every command that
 * changes it. No X11 in here — the shell (hwm.c) supplies the window system
 * through LayoutOps, which is what lets test_layout.c drive this headless. */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <stddef.h>

typedef union {
    int i;
    float f;
    const void *v;
} Arg;

typedef struct Client Client;
typedef struct Column Column;

struct Client {
    unsigned long win; /* the shell's handle (an X11 Window) */
    Column *col;       /* NULL while floating */
    int x, y, w, h;    /* last applied geometry */
    int isfloating;
    size_t ws;
    char *app; /* WM_CLASS class, the identity placements are kept by */
};

struct Column {
    Client **clients; /* stb_ds array, top to bottom */
    Client *sel;      /* focused window of this column */
    float width;      /* fraction of the usable screen width */
    int full;         /* spans the whole screen, no gaps or borders */
    size_t ws;
};

typedef struct {
    Column **cols; /* stb_ds array, left to right */
    Column *selcol;

    Client **floats;  /* stb_ds array, floating windows, bottom to top */
    Client *floatsel; /* focused floating window, or NULL */

    int scroll;   /* viewport offset in px (the animation target) */
    int animfrom; /* displayed scroll when the animation began */
    long animstart;
    int animating;
    size_t mon; /* monitor this workspace lives on */
} Workspace;

typedef struct {
    int x, y, w, h;
    size_t ws; /* workspace shown here (nworkspaces = none) */
} Monitor;

/* what the layout asks of its shell */
typedef struct {
    /* put c's frame at x,y with size w,h and border width bw */
    void (*apply)(Client *c, int x, int y, int w, int h, int bw);
    void (*raise)(Client *c);
    /* c is now the focused window of curws (NULL: nothing is); curws itself
     * may have changed */
    void (*focus)(Client *c);
    void (*desktop)(Client *c); /* c->ws changed */
    void (*warp)(Monitor *m);   /* bring the pointer onto m */
    long (*now)(void);          /* monotonic milliseconds */
} LayoutOps;

/* model, shared with the shell. mons and clients are stb_ds arrays */
extern Workspace *wss;
extern size_t curws;
extern Client **clients; /* every managed window in map order */
extern Monitor *mons;    /* left to right */

void layoutinit(const LayoutOps *ops);
void die(const char *msg);
void *ecalloc(size_t nmemb, size_t size);
void *erealloc(void *ptr, size_t size);
char *estrdup(const char *s);

Workspace *curwsp(void);
Client *focused(void);
Monitor *wsmon(size_t wi);
Client *findclient(unsigned long win);
size_t monat(int x, int y); /* monitor under a point; curws's if none */

/* adopt a window: t carries win, app (owned from here on), isfloating and
 * the requested geometry (floats keep it). follow: switch to the workspace
 * a remembered placement names */
Client *manage(const Client *t, int follow);
void unmanage(Client *c);
void focus(Client *c);
void arrangews(size_t wi);
void ensurevisible(Column *col);
void moveresize(Client *c, int x, int y, int w, int h);
void setcolwidth(Column *col, float w);      /* clamp only: caller rearranges */
void trackscroll(Workspace *ws, int target); /* 1:1: drags and swipes */
void snapscroll(Workspace *ws); /* glide to the nearest column edge */
void savelayout(Client *c);     /* placement memory */
int animstep(void);             /* advance animations; nonzero while busy */
/* replace the monitor list (sorted left to right) with n geometries; px,py
 * is the pointer, which picks where a detached monitor's workspaces go */
void setmons(const Monitor *geoms, size_t n, int px, int py);
void syncmon(size_t m); /* make monitor m's workspace current */

/* commands, bindable in config.h */
void focushorz(const Arg *arg);   /* .i = -1 left / +1 right (columns) */
void focusvert(const Arg *arg);   /* .i = -1 up / +1 down (within column) */
void movehorz(const Arg *arg);    /* move window/column left or right */
void stackto(const Arg *arg);     /* stack window into the adjacent column */
void movevert(const Arg *arg);    /* move window up or down in its column */
void cyclewidth(const Arg *arg);  /* cycle column width through widths[] */
void growwidth(const Arg *arg);   /* .f = width delta, fraction of screen */
void setwidth(const Arg *arg);    /* .f = column width, fraction of screen */
void scrollby(const Arg *arg);    /* .f = scroll delta, fraction of screen */
void togglefull(const Arg *arg);  /* fullscreen the focused column */
void togglefloat(const Arg *arg); /* float/tile the focused window */
void view(const Arg *arg);        /* .i = workspace to show */
void sendto(const Arg *arg);      /* .i = workspace to send window to */
void movewsmon(
    const Arg *arg); /* .i = -1/+1: move workspace to adjacent monitor */

/* configuration the layout reads, defined in config.h (or by the tests) */
extern const unsigned int borderpx;
extern const unsigned int gappx;
extern const unsigned int
    scrollanimms; /* scroll animation duration in ms; 0 disables */
extern const float defwidth;
extern const float
    floatsize; /* size of newly floated windows, fraction of monitor */
extern const int preservelayout; /* reopen apps where they were last placed */
extern const char layoutfile[];  /* where placements are kept; ~ is $HOME */
extern float *widths;            /* stb_ds array */
extern const size_t nworkspaces;

#endif
