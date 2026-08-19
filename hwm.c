#include <X11/XF86keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libudev.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hwm.h"


static void die(const char *msg);
static void *erealloc(void *ptr, size_t size);
#define STBDS_REALLOC(ctx, ptr, size) erealloc(ptr, size)
#define STBDS_FREE(ctx, ptr) free(ptr)
#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#include "config.h"

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MOUSEMASK (ButtonPressMask | ButtonReleaseMask | PointerMotionMask)
#define CLEANMASK(M)                                                           \
  ((M) & ~(numlockmask | LockMask) &                                           \
   (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask |      \
    Mod5Mask))

typedef struct Client Client;
typedef struct Column Column;

struct Client {
  Window win;
  Column *col;    /* NULL while floating */
  int x, y, w, h; /* last applied geometry */
  int isfloating;
  size_t ws;
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

  int scroll; /* viewport offset in px (the animation target) */
  int animfrom;              /* displayed scroll when the animation began */
  struct timespec animstart;
  int animating;
  size_t mon; /* monitor this workspace lives on */
} Workspace;

typedef struct {
  int x, y, w, h; /* geometry from RandR */
  size_t ws;      /* workspace shown here (nworkspaces = none) */
} Monitor;

static void buttonpress(XEvent *e);
static void clientmessage(XEvent *e);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static void destroynotify(XEvent *e);
static void enternotify(XEvent *e);
static void focusin(XEvent *e);
static void keypress(XEvent *e);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static void unmapnotify(XEvent *e);

static Display *dpy;
static Window root;
static int screen, sw, sh;
static unsigned long focuspx, unfocuspx;
static unsigned int numlockmask;
static Workspace *wss;
static size_t curws;
static Client **clients; /* stb_ds array, every managed window in map order */
static Monitor *mons;    /* stb_ds array, left to right */
static int rrbase = -1;  /* RandR event base; -1 without the extension */
static int running = 1;
static int dorestart;
static char selfpath[PATH_MAX];
static struct stat selfstat;
static int selfok;
static struct libinput *li; /* NULL: touchpad gestures unavailable */
static int swiping;         /* a three-finger swipe is in flight */
static float swipex;        /* the scroll position it is tracking */
/* interned in setup() from atomnames[]; keep both lists in step.
 * Two ranges matter: NetSupported..NetWMWindowType is advertised as
 * _NET_SUPPORTED, and windows of type NetTypeDialog..NetTypeNotification
 * are managed floating */
enum {
  WMProtocols,
  WMDelete,
  NetSupported,
  NetClientList,
  NetActiveWindow,
  NetCurDesktop,
  NetNumDesktops,
  NetWMDesktop,
  NetWMCheck,
  NetWMWindowType,
  NetWMName,
  NetTypeDialog,
  NetTypeUtility,
  NetTypeToolbar,
  NetTypeSplash,
  NetTypeMenu,
  NetTypeNotification,
  AtomLast
};
static const char *atomnames[AtomLast] = {
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "_NET_SUPPORTED",
    "_NET_CLIENT_LIST",
    "_NET_ACTIVE_WINDOW",
    "_NET_CURRENT_DESKTOP",
    "_NET_NUMBER_OF_DESKTOPS",
    "_NET_WM_DESKTOP",
    "_NET_SUPPORTING_WM_CHECK",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_NAME",
    "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_TYPE_UTILITY",
    "_NET_WM_WINDOW_TYPE_TOOLBAR",
    "_NET_WM_WINDOW_TYPE_SPLASH",
    "_NET_WM_WINDOW_TYPE_MENU",
    "_NET_WM_WINDOW_TYPE_NOTIFICATION",
};
static Atom atoms[AtomLast];
static Client *pressclient; /* client under the most recent button press */
static Window checkwin;
static int (*xerrorxlib)(Display *, XErrorEvent *);

static void (*handler[LASTEvent])(XEvent *) = {
    [ButtonPress] = buttonpress,
    [ClientMessage] = clientmessage,
    [ConfigureNotify] = configurenotify,
    [ConfigureRequest] = configurerequest,
    [DestroyNotify] = destroynotify,
    [EnterNotify] = enternotify,
    [FocusIn] = focusin,
    [KeyPress] = keypress,
    [MappingNotify] = mappingnotify,
    [MapRequest] = maprequest,
    [UnmapNotify] = unmapnotify,
};

static void die(const char *msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

static void *ecalloc(size_t nmemb, size_t size) {
  void *p = calloc(nmemb, size);
  if (!p)
    die("hwm: out of memory");
  return p;
}

static void *erealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if (!p)
    die("hwm: out of memory");
  return p;
}

static Workspace *curwsp(void) { return &wss[curws]; }

static Client *focused(void) {
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

static Monitor *wsmon(size_t wi) {
  size_t m = wss[wi].mon;

  return &mons[m < (size_t)arrlen(mons) ? m : 0];
}

static int wsvisible(size_t wi) { return wsmon(wi)->ws == wi; }

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
  struct timespec now;
  float t;

  if (!ws->animating)
    return ws->scroll;
  clock_gettime(CLOCK_MONOTONIC, &now);
  t = (float)((now.tv_sec - ws->animstart.tv_sec) * 1000 +
              (now.tv_nsec - ws->animstart.tv_nsec) / 1000000) /
      (float)scrollanimms;
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
  clock_gettime(CLOCK_MONOTONIC, &ws->animstart);
  ws->animating = 1;
}

static void ensurevisible(Column *col) {
  Workspace *ws = &wss[col->ws];
  int vx = colvx(ws, col), cw = colpx(col), uw = wsmon(col->ws)->w;

  beginscroll(ws);
  if (cw >= uw || vx < ws->scroll)
    ws->scroll = vx;
  else if (vx + cw > ws->scroll + uw)
    ws->scroll = vx + cw - uw;
  clampscroll(ws);
}

/* glide to the nearest column edge aligned with a screen edge */
static void snapscroll(Workspace *ws) {
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

static Client *findclient(Window w) {
  ptrdiff_t i;

  for (i = 0; i < arrlen(clients); i++)
    if (clients[i]->win == w)
      return clients[i];
  return NULL;
}

static void resizeclient(Client *c, int x, int y, int w, int h) {
  c->x = x;
  c->y = y;
  c->w = MAX(1, w);
  c->h = MAX(1, h);
  XMoveResizeWindow(dpy, c->win, c->x, c->y, (unsigned int)c->w,
                    (unsigned int)c->h);
}

static void arrangews(size_t wi) {
  Workspace *ws = &wss[wi];
  Monitor *m = wsmon(wi);
  Column *col;
  Client *c;
  ptrdiff_t ci, i, n;
  int xoff = wsvisible(wi) ? 0 : -3 * sw; /* park hidden workspaces offscreen */
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
      XSetWindowBorderWidth(dpy, c->win, (unsigned int)bw);
      resizeclient(c, xoff + m->x + x + gap, m->y + y + gap,
                   cw - 2 * (gap + bw), ch - 2 * (gap + bw));
    }
    x += cw;
  }
  /* floats keep their own (absolute) geometry above the strip */
  for (i = 0; i < arrlen(ws->floats); i++) {
    c = ws->floats[i];
    XSetWindowBorderWidth(dpy, c->win, borderpx);
    XMoveResizeWindow(dpy, c->win, xoff + c->x, c->y, (unsigned int)c->w,
                      (unsigned int)c->h);
    XRaiseWindow(dpy, c->win);
  }
}

/* 1:1 scroll tracking, shared by pointer drags and touchpad swipes */
static void trackscroll(Workspace *ws, int target) {
  ws->animating = 0;
  ws->scroll = target;
  clampscroll(ws);
  arrangews((size_t)(ws - wss));
}

static void setcardinal(Window w, Atom prop, long value) {
  XChangeProperty(dpy, w, prop, XA_CARDINAL, 32, PropModeReplace,
                  (unsigned char *)&value, 1);
}

/* rofi and friends read _NET_CLIENT_LIST to enumerate windows */
static void updateclientlist(void) {
  ptrdiff_t i;

  XDeleteProperty(dpy, root, atoms[NetClientList]);
  for (i = 0; i < arrlen(clients); i++)
    XChangeProperty(dpy, root, atoms[NetClientList], XA_WINDOW, 32,
                    PropModeAppend, (unsigned char *)&clients[i]->win, 1);
}

static void grabwinbuttons(Window w) {
  unsigned int mods[] = {0, LockMask, numlockmask, numlockmask | LockMask};
  ptrdiff_t i;
  size_t j;

  for (i = 0; i < arrlen(buttons); i++)
    for (j = 0; j < LENGTH(mods); j++)
      XGrabButton(dpy, buttons[i].button, buttons[i].mod | mods[j], w, False,
                  ButtonPressMask, GrabModeAsync, GrabModeSync, None, None);
}

static void grabbuttons(Client *c, int isfocused) {
  XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
  if (!isfocused) /* click-to-focus: catch the first click, then replay it */
    XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, ButtonPressMask,
                GrabModeSync, GrabModeSync, None, None);
  grabwinbuttons(c->win);
}

static void focus(Client *c) {
  Workspace *ws;
  Client *i;
  ptrdiff_t j;

  /* focusing a window on another (visible) workspace follows it there */
  if (c && c->ws != curws && wsvisible(c->ws)) {
    curws = c->ws;
    setcardinal(root, atoms[NetCurDesktop], (long)curws);
  }
  ws = curwsp();
  if (c) {
    if (c->isfloating) {
      ws->floatsel = c;
      XRaiseWindow(dpy, c->win);
    } else {
      ws->floatsel = NULL;
      ws->selcol = c->col;
      c->col->sel = c;
    }
  }
  for (j = 0; j < arrlen(clients); j++) {
    i = clients[j];
    if (i->ws != curws)
      continue;
    XSetWindowBorder(dpy, i->win, i == c ? focuspx : unfocuspx);
    grabbuttons(i, i == c);
  }
  XSetInputFocus(dpy, c ? c->win : root, RevertToPointerRoot, CurrentTime);
  if (c)
    XChangeProperty(dpy, root, atoms[NetActiveWindow], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&c->win, 1);
  else
    XDeleteProperty(dpy, root, atoms[NetActiveWindow]);
}

/* insert c after col's selection and make it the selection */
static void attach(Column *col, Client *c) {
  ptrdiff_t at = col->sel ? clientidx(col, col->sel) + 1 : arrlen(col->clients);

  arrins(col->clients, at, c);
  col->sel = c;
  c->col = col;
  c->ws = col->ws;
  wss[col->ws].selcol = col;
}

/* put c alone into a new column inserted after `after` (NULL = leftmost) */
static Column *attachnew(size_t wi, Column *after, Client *c) {
  Workspace *ws = &wss[wi];
  Column *col = ecalloc(1, sizeof(Column));
  ptrdiff_t at = after ? colidx(ws, after) + 1 : 0;

  col->width = defwidth;
  col->ws = wi;
  arrins(ws->cols, at, col);
  attach(col, c);
  return col;
}

/* remove c from its column; empty columns are freed */
static void detach(Client *c) {
  Column *col = c->col;
  Workspace *ws = &wss[col->ws];
  ptrdiff_t i = clientidx(col, c), ci;

  arrdel(col->clients, i);
  if (col->sel == c)
    col->sel =
        arrlen(col->clients)
            ? col->clients[i < arrlen(col->clients) ? i
                                                    : arrlen(col->clients) - 1]
            : NULL;
  if (!arrlen(col->clients)) {
    ci = colidx(ws, col);
    arrdel(ws->cols, ci);
    if (ws->selcol == col)
      ws->selcol =
          arrlen(ws->cols)
              ? ws->cols[ci < arrlen(ws->cols) ? ci : arrlen(ws->cols) - 1]
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

static void sendconfigure(Client *c) {
  XConfigureEvent ce = {0};

  ce.type = ConfigureNotify;
  ce.display = dpy;
  ce.event = c->win;
  ce.window = c->win;
  ce.x = c->x;
  ce.y = c->y;
  ce.width = c->w;
  ce.height = c->h;
  ce.border_width = c->col && c->col->full ? 0 : (int)borderpx;
  XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static int sendproto(Client *c, Atom proto) {
  Atom *protos;
  int n, exists = 0;
  XEvent ev;

  if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
    while (!exists && n--)
      exists = protos[n] == proto;
    XFree(protos);
  }
  if (exists) {
    memset(&ev, 0, sizeof(ev));
    ev.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = atoms[WMProtocols];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)proto;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
  }
  return exists;
}

static Atom getwintype(Window w) {
  Atom type = None, real;
  int fmt;
  unsigned long n, extra;
  unsigned char *p = NULL;

  if (XGetWindowProperty(dpy, w, atoms[NetWMWindowType], 0L, 1L, False, XA_ATOM,
                         &real, &fmt, &n, &extra, &p) == Success &&
      p) {
    if (n)
      type = *(Atom *)p;
    XFree(p);
  }
  return type;
}

static int isfixedsize(Window w) {
  XSizeHints size;
  long msize;

  if (!XGetWMNormalHints(dpy, w, &size, &msize))
    return 0;
  return (size.flags & PMinSize) && (size.flags & PMaxSize) &&
         size.min_width > 0 && size.min_height > 0 &&
         size.min_width == size.max_width && size.min_height == size.max_height;
}

/* keep the requested geometry; center windows that didn't ask for a
 * position, clamp the rest onto their monitor */
static void placefloat(Client *c, XWindowAttributes *wa) {
  Monitor *m = wsmon(c->ws);

  c->w = MAX(1, wa->width);
  c->h = MAX(1, wa->height);
  c->x = wa->x;
  c->y = wa->y;
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

static void manage(Window w) {
  Workspace *ws = curwsp();
  XWindowAttributes wa;
  Window trans;
  Client *c;
  Atom type;
  int i, wantfocus = 1;

  if (findclient(w))
    return;
  if (!XGetWindowAttributes(dpy, w, &wa))
    return;
  c = ecalloc(1, sizeof(Client));
  c->win = w;
  c->ws = curws;
  type = getwintype(w);
  for (i = NetTypeDialog; i <= NetTypeNotification; i++)
    if (type == atoms[i])
      c->isfloating = 1;
  /* transient windows float, but only when the parent is a real
   * managed window: SDL and friends set WM_TRANSIENT_FOR to the
   * root window on ordinary top-levels */
  if ((XGetTransientForHint(dpy, w, &trans) && findclient(trans)) ||
      isfixedsize(w))
    c->isfloating = 1;
  if (c->isfloating) {
    placefloat(c, &wa);
    arrput(ws->floats, c);
    if (type == atoms[NetTypeNotification])
      wantfocus = 0; /* notifications must not steal focus */
  } else {
    ensurevisible(attachnew(curws, ws->selcol, c));
  }
  arrput(clients, c);
  XSetWindowBorderWidth(dpy, w, borderpx);
  XSelectInput(dpy, w, EnterWindowMask | FocusChangeMask | StructureNotifyMask);
  setcardinal(w, atoms[NetWMDesktop], (long)curws);
  updateclientlist();
  arrangews(curws);
  XMapWindow(dpy, w);
  if (wantfocus) {
    focus(c);
  } else {
    XSetWindowBorder(dpy, w, unfocuspx);
    grabbuttons(c, 0);
  }
}

static void unmanage(Client *c) {
  size_t wi = c->ws;
  ptrdiff_t i;

  if (pressclient == c)
    pressclient = NULL;
  if (c->isfloating)
    detachfloat(c);
  else
    detach(c);
  for (i = 0; i < arrlen(clients); i++)
    if (clients[i] == c) {
      arrdel(clients, i);
      break;
    }
  free(c);
  updateclientlist();
  arrangews(wi);
  if (wi == curws)
    focus(focused());
}

static int getrootptr(int *x, int *y) {
  int di;
  unsigned int dui;
  Window dummy;

  return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

/* the active monitor is the one under the pointer */
static size_t activemon(void) {
  int x, y;
  ptrdiff_t i;

  if (getrootptr(&x, &y))
    for (i = 0; i < arrlen(mons); i++)
      if (x >= mons[i].x && x < mons[i].x + mons[i].w && y >= mons[i].y &&
          y < mons[i].y + mons[i].h)
        return (size_t)i;
  return wss[curws].mon;
}

/* keep the mouse on the monitor that has the focus */
static void warptomon(size_t m) {
  if (activemon() != m)
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, mons[m].x + mons[m].w / 2,
                 mons[m].y + mons[m].h / 2);
}

/* refresh the monitor list from RandR. A new monitor takes over the first
 * hidden workspace; workspaces of detached monitors move to the active one. */
static void updatemons(void) {
  XRRMonitorInfo *info = NULL, t;
  int i, j, n = 0;
  ptrdiff_t oldn = arrlen(mons), m;
  size_t k, am;

  if (rrbase >= 0)
    info = XRRGetMonitors(dpy, root, True, &n);
  if (n < 1)
    n = 1; /* fallback: one monitor covering the whole screen */
  if (info)
    for (i = 1; i < n; i++) /* sort left to right */
      for (j = i; j > 0 && info[j - 1].x > info[j].x; j--) {
        t = info[j - 1];
        info[j - 1] = info[j];
        info[j] = t;
      }
  arrsetlen(mons, n);
  for (i = 0; i < n; i++) {
    mons[i].x = info ? info[i].x : 0;
    mons[i].y = info ? info[i].y : 0;
    mons[i].w = info ? info[i].width : sw;
    mons[i].h = info ? info[i].height : sh;
    if (i >= oldn)
      mons[i].ws = nworkspaces;
  }
  if (info)
    XRRFreeMonitors(info);
  if (!oldn)
    mons[0].ws = curws;
  if ((ptrdiff_t)n < oldn) {
    am = activemon();
    if (am >= (size_t)n) /* pointer on a dead monitor */
      am = 0;
    for (k = 0; k < nworkspaces; k++)
      if (wss[k].mon >= (size_t)n)
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
}

/* clamp, not layout: callers rearrange (and scroll) themselves */
static void setcolwidth(Column *col, float w) {
  col->width = w < 0.1f ? 0.1f : w > 1.0f ? 1.0f : w;
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
}

void growwidth(const Arg *arg) {
  Column *col = curwsp()->selcol;

  if (!col)
    return;
  setcolwidth(col, col->width + arg->f);
  ensurevisible(col);
  arrangews(curws);
}

void setwidth(const Arg *arg) {
  Column *col = curwsp()->selcol;

  if (!col)
    return;
  setcolwidth(col, arg->f);
  ensurevisible(col);
  arrangews(curws);
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
}

void view(const Arg *arg) {
  size_t old, m;

  if (arg->i < 0 || (size_t)arg->i >= nworkspaces || (size_t)arg->i == curws)
    return;
  curws = (size_t)arg->i;
  m = wss[curws].mon;
  old = mons[m].ws; /* the workspace this monitor showed before */
  mons[m].ws = curws;
  setcardinal(root, atoms[NetCurDesktop], (long)curws);
  if (old < nworkspaces && old != curws)
    arrangews(old);
  arrangews(curws);
  warptomon(m);
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
  warptomon((size_t)nm);
  focus(focused());
}

void sendto(const Arg *arg) {
  Workspace *target;
  Client *c = focused();

  if (!c || arg->i < 0 || (size_t)arg->i >= nworkspaces ||
      (size_t)arg->i == curws)
    return;
  target = &wss[arg->i];
  if (c->isfloating) {
    detachfloat(c);
    arrput(target->floats, c);
  } else {
    detach(c);
    attachnew((size_t)arg->i, target->selcol, c);
  }
  c->ws = (size_t)arg->i;
  setcardinal(c->win, atoms[NetWMDesktop], arg->i);
  arrangews(curws);
  arrangews((size_t)arg->i);
  focus(focused());
}

void killclient(const Arg *arg) {
  Client *c = focused();

  (void)arg;
  if (!c)
    return;
  if (!sendproto(c, atoms[WMDelete]))
    XKillClient(dpy, c->win);
}

void spawn(const Arg *arg) {
  if (fork() == 0) {
    if (dpy)
      close(ConnectionNumber(dpy));
    setsid();
    signal(SIGCHLD, SIG_DFL); /* undo our SIG_IGN: it survives exec */
    unsetenv(
        "WAYLAND_DISPLAY"); /* children must pick X11, not a host compositor */
    execvp(((char **)arg->v)[0], (char **)arg->v);
    fprintf(stderr, "hwm: execvp %s failed\n", ((char **)arg->v)[0]);
    exit(1);
  }
}

void quit(const Arg *arg) {
  (void)arg;
  running = 0;
}

void restart(const Arg *arg) {
  (void)arg;
  dorestart = 1;
  running = 0;
}

/* Mod+drag: one pointer grab, throttled to 60 Hz, drives all drag kinds.
 * DragMove/DragResize act on pressclient (a float), DragScroll pans the
 * strip, DragWidth resizes the selected column. */
enum { DragMove, DragResize, DragScroll, DragWidth };

static void drag(int mode) {
  Workspace *ws = curwsp();
  Client *c = pressclient;
  Column *col = ws->selcol;
  int rx, ry, dx, dy, x = 0, y = 0, w = 0, h = 0, scroll = ws->scroll;
  float width = col ? col->width : 0.0f;
  Time last = 0;
  XEvent ev;

  ws->animating = 0; /* drags track the pointer 1:1 */
  if (mode == DragMove || mode == DragResize) {
    x = c->x;
    y = c->y;
    w = c->w;
    h = c->h;
  }
  if (!getrootptr(&rx, &ry))
    return;
  if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
                   None, None, CurrentTime) != GrabSuccess)
    return;
  do {
    XMaskEvent(dpy, MOUSEMASK | SubstructureRedirectMask, &ev);
    switch (ev.type) {
    case ConfigureRequest:
    case MapRequest:
      handler[ev.type](&ev);
      break;
    case MotionNotify:
      if (ev.xmotion.time - last < 1000 / 60)
        break;
      last = ev.xmotion.time;
      dx = ev.xmotion.x_root - rx;
      dy = ev.xmotion.y_root - ry;
      switch (mode) {
      case DragMove:
        resizeclient(c, x + dx, y + dy, w, h);
        break;
      case DragResize:
        resizeclient(c, x, y, MAX(50, w + dx), MAX(50, h + dy));
        break;
      case DragScroll:
        trackscroll(ws, scroll - dx);
        break;
      case DragWidth:
        setcolwidth(col, width + (float)dx / (float)wsmon(col->ws)->w);
        arrangews(curws);
        break;
      }
      break;
    }
  } while (ev.type != ButtonRelease);
  XUngrabPointer(dpy, CurrentTime);
}

void dragscroll(const Arg *arg) {
  (void)arg;
  drag(pressclient && pressclient->isfloating ? DragMove : DragScroll);
}

void dragwidth(const Arg *arg) {
  (void)arg;
  if (pressclient && pressclient->isfloating)
    drag(DragResize);
  else if (curwsp()->selcol)
    drag(DragWidth);
}

/* event handlers */

/* the mouse picks the active monitor: input acts on its visible workspace */
static void syncactivemon(void) {
  size_t k = mons[activemon()].ws;

  if (k < nworkspaces && k != curws) {
    curws = k;
    setcardinal(root, atoms[NetCurDesktop], (long)curws);
    focus(focused());
  }
}

static void buttonpress(XEvent *e) {
  XButtonPressedEvent *ev = &e->xbutton;
  Client *c = findclient(ev->window);
  ptrdiff_t i;

  syncactivemon();
  pressclient = c;
  if (c) {
    if (c != focused())
      focus(c);
    XAllowEvents(dpy, ReplayPointer, CurrentTime);
  }
  for (i = 0; i < arrlen(buttons); i++)
    if (buttons[i].button == ev->button && buttons[i].func &&
        CLEANMASK(buttons[i].mod) == CLEANMASK(ev->state))
      buttons[i].func(&buttons[i].arg);
}

static void clientmessage(XEvent *e) {
  XClientMessageEvent *ev = &e->xclient;
  Client *c;
  Arg a;

  if (ev->message_type == atoms[NetActiveWindow]) {
    c = findclient(ev->window);
    if (!c)
      return;
    if (c->ws != curws) {
      a.i = (int)c->ws;
      view(&a);
    }
    focus(c);
    if (c->col)
      ensurevisible(c->col);
    arrangews(curws);
  } else if (ev->message_type == atoms[NetCurDesktop]) {
    a.i = (int)ev->data.l[0];
    view(&a);
  }
}

static void configurenotify(XEvent *e) {
  XConfigureEvent *ev = &e->xconfigure;

  if (ev->window == root && (ev->width != sw || ev->height != sh)) {
    sw = ev->width;
    sh = ev->height;
    updatemons();
  }
}

static void configurerequest(XEvent *e) {
  XConfigureRequestEvent *ev = &e->xconfigurerequest;
  Client *c = findclient(ev->window);
  XWindowChanges wc;

  if (c && c->isfloating) {
    /* floats may place themselves (notifications, dialogs) */
    if (ev->value_mask & CWX)
      c->x = ev->x;
    if (ev->value_mask & CWY)
      c->y = ev->y;
    if (ev->value_mask & CWWidth)
      c->w = MAX(1, ev->width);
    if (ev->value_mask & CWHeight)
      c->h = MAX(1, ev->height);
    if (c->ws == curws)
      XMoveResizeWindow(dpy, c->win, c->x, c->y, (unsigned int)c->w,
                        (unsigned int)c->h);
    else
      sendconfigure(c);
  } else if (c) {
    sendconfigure(c); /* we tile; tell it where it really is */
  } else {
    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.border_width = ev->border_width;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    XConfigureWindow(dpy, ev->window, (unsigned int)ev->value_mask, &wc);
  }
  XSync(dpy, False);
}

static void destroynotify(XEvent *e) {
  Client *c = findclient(e->xdestroywindow.window);

  if (c)
    unmanage(c);
}

static void enternotify(XEvent *e) {
  XCrossingEvent *ev = &e->xcrossing;
  Client *c;

  if (!focusfollowsmouse)
    return;
  if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) &&
      ev->window != root)
    return;
  c = findclient(ev->window);
  if (c && c != focused())
    focus(c);
}

static void focusin(XEvent *e) {
  Client *c = focused();

  if (c && e->xfocus.window != c->win)
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
}

static void keypress(XEvent *e) {
  XKeyEvent *ev = &e->xkey;
  KeySym keysym = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0, 0);
  ptrdiff_t i;

  syncactivemon();
  for (i = 0; i < arrlen(keys); i++)
    if (keysym == keys[i].keysym && keys[i].func &&
        CLEANMASK(keys[i].mod) == CLEANMASK(ev->state))
      keys[i].func(&keys[i].arg);
}

static void updatenumlockmask(void) {
  XModifierKeymap *modmap = XGetModifierMapping(dpy);
  int i, j;

  numlockmask = 0;
  for (i = 0; i < 8; i++)
    for (j = 0; j < modmap->max_keypermod; j++)
      if (modmap->modifiermap[i * modmap->max_keypermod + j] ==
          XKeysymToKeycode(dpy, XK_Num_Lock))
        numlockmask = (1u << i);
  XFreeModifiermap(modmap);
}

static void grabkeys(void) {
  unsigned int mods[] = {0, LockMask, numlockmask, numlockmask | LockMask};
  KeyCode code;
  ptrdiff_t i;
  size_t j;

  updatenumlockmask();
  XUngrabKey(dpy, AnyKey, AnyModifier, root);
  for (i = 0; i < arrlen(keys); i++) {
    code = XKeysymToKeycode(dpy, keys[i].keysym);
    if (!code)
      continue;
    for (j = 0; j < LENGTH(mods); j++)
      XGrabKey(dpy, code, keys[i].mod | mods[j], root, True, GrabModeAsync,
               GrabModeAsync);
  }
}

static void mappingnotify(XEvent *e) {
  XMappingEvent *ev = &e->xmapping;

  XRefreshKeyboardMapping(ev);
  if (ev->request == MappingKeyboard)
    grabkeys();
}

static void maprequest(XEvent *e) {
  XMapRequestEvent *ev = &e->xmaprequest;
  XWindowAttributes wa;

  if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect)
    return;
  manage(ev->window);
}

static void unmapnotify(XEvent *e) {
  Client *c = findclient(e->xunmap.window);

  if (c)
    unmanage(c);
}

/* startup */

static int xerror(Display *d, XErrorEvent *ee) {
  /* ignore errors from windows that vanished under us, like dwm */
  if (ee->error_code == BadWindow ||
      (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch) ||
      (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch) ||
      (ee->request_code == X_GrabButton && ee->error_code == BadAccess) ||
      (ee->request_code == X_GrabKey && ee->error_code == BadAccess))
    return 0;
  fprintf(stderr, "hwm: X error: request %d, error %d\n", ee->request_code,
          ee->error_code);
  return xerrorxlib(d, ee);
}

static int xerrorstart(Display *d, XErrorEvent *ee) {
  (void)d;
  (void)ee;
  die("hwm: another window manager is already running");
  return -1;
}

static unsigned long getcolor(const char *name) {
  XColor c, exact;

  if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &c, &exact))
    die("hwm: cannot allocate color");
  return c.pixel;
}

/* advertise just enough EWMH for pagers/switchers like rofi */
static void initewmh(void) {
  Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);

  checkwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
  XChangeProperty(dpy, checkwin, atoms[NetWMCheck], XA_WINDOW, 32,
                  PropModeReplace, (unsigned char *)&checkwin, 1);
  XChangeProperty(dpy, checkwin, atoms[NetWMName], utf8, 8, PropModeReplace,
                  (unsigned char *)"hwm", 3);
  XChangeProperty(dpy, root, atoms[NetWMCheck], XA_WINDOW, 32, PropModeReplace,
                  (unsigned char *)&checkwin, 1);
  XChangeProperty(dpy, root, atoms[NetSupported], XA_ATOM, 32, PropModeReplace,
                  (unsigned char *)&atoms[NetSupported],
                  NetWMWindowType - NetSupported + 1);
  setcardinal(root, atoms[NetNumDesktops], (long)nworkspaces);
  setcardinal(root, atoms[NetCurDesktop], (long)curws);
}

static void setup(void) {
  int i, di;

  signal(SIGCHLD, SIG_IGN); /* auto-reap spawned children */
  if (!(dpy = XOpenDisplay(NULL)))
    die("hwm: cannot open display");
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);
  sw = DisplayWidth(dpy, screen);
  sh = DisplayHeight(dpy, screen);

  /* becoming the WM fails if another one already redirects the root */
  xerrorxlib = XSetErrorHandler(xerrorstart);
  XSelectInput(dpy, root, SubstructureRedirectMask);
  XSync(dpy, False);
  XSetErrorHandler(xerror);
  XSync(dpy, False);

  wss = ecalloc(nworkspaces, sizeof(Workspace));
  if (XRRQueryExtension(dpy, &rrbase, &di))
    XRRSelectInput(dpy, root,
                   RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                       RROutputChangeNotifyMask);
  else
    rrbase = -1;
  updatemons();
  focuspx = getcolor(col_focus);
  unfocuspx = getcolor(col_unfocus);
  for (i = 0; i < AtomLast; i++)
    atoms[i] = XInternAtom(dpy, atomnames[i], False);
  initewmh();
  XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
  XSelectInput(dpy, root,
               SubstructureRedirectMask | SubstructureNotifyMask |
                   StructureNotifyMask | ButtonPressMask);
  grabkeys();
  grabwinbuttons(root);
  XSync(dpy, False);
}

static void scan(void) {
  Window d1, d2, *wins = NULL;
  XWindowAttributes wa;
  unsigned int i, num;

  if (!XQueryTree(dpy, root, &d1, &d2, &wins, &num))
    return;
  for (i = 0; i < num; i++)
    if (XGetWindowAttributes(dpy, wins[i], &wa) && !wa.override_redirect &&
        wa.map_state == IsViewable)
      manage(wins[i]);
  if (wins)
    XFree(wins);
}

static void autostartrun(void) {
  ptrdiff_t i;

  for (i = 0; i < arrlen(autostart); i++) {
    const char *cmd[] = {"/bin/zsh", "-c", autostart[i], NULL};
    Arg a = {.v = cmd};

    spawn(&a);
  }
}

static void initselfwatch(const char *argv0) {
  ssize_t n = readlink("/proc/self/exe", selfpath, sizeof selfpath - 1);

  if (n > 0)
    selfpath[n] = '\0';
  else if (strchr(argv0, '/') && strlen(argv0) < sizeof selfpath)
    strcpy(selfpath, argv0);
  else
    return; /* bare name in PATH; no watch, Mod+Shift+r still works */
  selfok = stat(selfpath, &selfstat) == 0;
}

static int samefile(const struct stat *a, const struct stat *b) {
  return a->st_ino == b->st_ino && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
         a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

/* restart once our binary has been replaced and has stopped changing:
 * the linker unlinks and rewrites it, so wait for two identical polls */
/* three-finger swipes scroll the strip like a pointer drag. The X server
 * forwards no touchpad gestures, so they are read from libinput itself;
 * that needs read access to /dev/input (the input group) */
static int openrestricted(const char *path, int flags, void *data) {
  (void)data;
  return open(path, flags | O_CLOEXEC); /* don't leak fds across restart */
}

static void closerestricted(int fd, void *data) {
  (void)data;
  close(fd);
}

static const struct libinput_interface gestureiface = {
  .open_restricted = openrestricted,
  .close_restricted = closerestricted,
};

static void initgestures(void) {
  struct udev *udev = udev_new();

  if (!udev)
    return;
  li = libinput_udev_create_context(&gestureiface, NULL, udev);
  udev_unref(udev); /* the context keeps its own reference */
  if (li && libinput_udev_assign_seat(li, "seat0") < 0) {
    libinput_unref(li);
    li = NULL;
  }
  if (!li)
    fprintf(stderr, "hwm: no touchpad gestures (libinput unavailable)\n");
}

static void gestureevents(void) {
  struct libinput_event *ev;
  struct libinput_event_gesture *ge;
  Workspace *ws;

  libinput_dispatch(li);
  while ((ev = libinput_get_event(li)) != NULL) {
    ge = libinput_event_get_gesture_event(ev);
    switch (libinput_event_get_type(ev)) {
    case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
      if (libinput_event_gesture_get_finger_count(ge) != 3)
        break;
      syncactivemon(); /* the gesture acts where the pointer is */
      swipex = (float)curwsp()->scroll;
      swiping = 1;
      break;
    case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
      if (!swiping)
        break;
      ws = curwsp();
      swipex -= (float)libinput_event_gesture_get_dx(ge) * gesturescale;
      trackscroll(ws, (int)swipex);
      swipex = (float)ws->scroll; /* don't wind up past the ends */
      break;
    case LIBINPUT_EVENT_GESTURE_SWIPE_END:
      if (!swiping)
        break;
      swiping = 0;
      snapscroll(curwsp());
      arrangews(curws);
      break;
    default:
      break;
    }
    libinput_event_destroy(ev);
  }
}

static void checkself(void) {
  static struct timespec last;
  static struct stat prev;
  static int prevok;
  struct timespec now;
  struct stat st;

  /* the settle check assumes ~1s polls; animation frames wake us faster */
  clock_gettime(CLOCK_MONOTONIC, &now);
  if ((now.tv_sec - last.tv_sec) * 1000 +
          (now.tv_nsec - last.tv_nsec) / 1000000 <
      1000)
    return;
  last = now;
  if (!selfok || stat(selfpath, &st) != 0) {
    prevok = 0;
    return;
  }
  if (samefile(&st, &selfstat)) {
    prevok = 0;
    return;
  }
  if (prevok && samefile(&st, &prev)) {
    dorestart = 1;
    running = 0;
    return;
  }
  prev = st;
  prevok = 1;
}

/* advance scroll animations one frame; nonzero while any are in flight */
static int animstep(void) {
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

static void run(void) {
  XEvent ev;
  fd_set fds;
  struct timeval tv;
  long timeout;
  int xfd = ConnectionNumber(dpy);
  int gfd = li ? libinput_get_fd(li) : -1;

  while (running) {
    while (XPending(dpy)) {
      XNextEvent(dpy, &ev);
      if (rrbase >= 0 && (ev.type == rrbase + RRScreenChangeNotify ||
                          ev.type == rrbase + RRNotify)) {
        if (ev.type == rrbase + RRScreenChangeNotify)
          XRRUpdateConfiguration(&ev);
        sw = DisplayWidth(dpy, screen);
        sh = DisplayHeight(dpy, screen);
        updatemons();
        focus(focused());
      } else if (ev.type < LASTEvent && handler[ev.type])
        handler[ev.type](&ev);
      if (!running)
        return;
    }
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    if (gfd >= 0)
      FD_SET(gfd, &fds);
    timeout = 1000;
    if (animstep()) {
      XFlush(dpy);
      timeout = 1000 / 60;
    }
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    if (select(MAX(xfd, gfd) + 1, &fds, NULL, NULL, &tv) < 0) {
      if (errno == EINTR)
        continue;
      die("hwm: select failed");
    }
    if (gfd >= 0 && FD_ISSET(gfd, &fds)) {
      gestureevents();
      XFlush(dpy);
    }
    if (!FD_ISSET(xfd, &fds))
      checkself();
  }
}

int main(int argc, char *argv[]) {
  if (argc == 2 && !strcmp(argv[1], "-v")) {
    printf("hwm %s\n", HWM_VERSION);
    return 0;
  }
  if (argc > 1)
    die("usage: hwm [-v]");
  initconfig();
  setup();
  initgestures();
  initselfwatch(argv[0]);
  scan();
  autostartrun();
  run();
  if (li)
    libinput_unref(li);
  XCloseDisplay(dpy);
  if (dorestart) {
    if (selfpath[0])
      execv(selfpath, argv);
    execvp(argv[0], argv);
    die("hwm: restart failed");
  }
  return 0;
}
