/* hwm - the X11 shell: window adoption, input, EWMH, RandR, gestures and the
 * event loop. The layout model and its commands are in layout.c. */
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

#include <stb_ds.h>

#include "config.h"

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MOUSEMASK (ButtonPressMask | ButtonReleaseMask | PointerMotionMask)
#define CLEANMASK(M)                                                           \
    ((M) & ~(numlockmask | LockMask) &                                         \
     (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask |    \
      Mod5Mask))

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
static size_t pubws;    /* the workspace _NET_CURRENT_DESKTOP last announced */
static int rrbase = -1; /* RandR event base; -1 without the extension */
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
            XGrabButton(dpy, buttons[i].button, buttons[i].mod | mods[j], w,
                        False, ButtonPressMask, GrabModeAsync, GrabModeSync,
                        None, None);
}

static void grabbuttons(Client *c, int isfocused) {
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
    if (!isfocused) /* click-to-focus: catch the first click, then replay it */
        XGrabButton(dpy, AnyButton, AnyModifier, c->win, False, ButtonPressMask,
                    GrabModeSync, GrabModeSync, None, None);
    grabwinbuttons(c->win);
}

static int getrootptr(int *x, int *y) {
    int di;
    unsigned int dui;
    Window dummy;

    return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

/* the active monitor is the one under the pointer */
static size_t activemon(void) {
    int x = -1, y = -1;

    getrootptr(&x, &y);
    return monat(x, y);
}

/* the layout's window of the world */

static void xapply(Client *c, int x, int y, int w, int h, int bw) {
    XSetWindowBorderWidth(dpy, c->win, (unsigned int)bw);
    XMoveResizeWindow(dpy, c->win, x, y, (unsigned int)w, (unsigned int)h);
}

static void xraise(Client *c) { XRaiseWindow(dpy, c->win); }

static void xfocus(Client *c) {
    Client *i;
    ptrdiff_t j;

    if (curws != pubws) {
        pubws = curws;
        setcardinal(root, atoms[NetCurDesktop], (long)curws);
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

static void xdesktop(Client *c) {
    setcardinal(c->win, atoms[NetWMDesktop], (long)c->ws);
}

/* keep the mouse on the monitor that has the focus */
static void xwarp(Monitor *m) {
    if (&mons[activemon()] != m)
        XWarpPointer(dpy, None, root, 0, 0, 0, 0, m->x + m->w / 2,
                     m->y + m->h / 2);
}

static long nowms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static const LayoutOps xops = {
    .apply = xapply,
    .raise = xraise,
    .focus = xfocus,
    .desktop = xdesktop,
    .warp = xwarp,
    .now = nowms,
};

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

    if (XGetWindowProperty(dpy, w, atoms[NetWMWindowType], 0L, 1L, False,
                           XA_ATOM, &real, &fmt, &n, &extra, &p) == Success &&
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
           size.min_width == size.max_width &&
           size.min_height == size.max_height;
}

/* WM_CLASS class, falling back to the instance name */
static char *getapp(Window w) {
    XClassHint ch = {NULL, NULL};
    char *app = NULL;

    if (!XGetClassHint(dpy, w, &ch))
        return NULL;
    if (ch.res_class && *ch.res_class)
        app = estrdup(ch.res_class);
    else if (ch.res_name && *ch.res_name)
        app = estrdup(ch.res_name);
    if (ch.res_class)
        XFree(ch.res_class);
    if (ch.res_name)
        XFree(ch.res_name);
    return app;
}

static void adopt(Window w, int follow) {
    XWindowAttributes wa;
    Window trans;
    Client t = {0}, *c;
    Atom type;
    int i, wantfocus = 1;

    if (findclient(w))
        return;
    if (!XGetWindowAttributes(dpy, w, &wa))
        return;
    t.win = w;
    t.app = getapp(w);
    t.x = wa.x;
    t.y = wa.y;
    t.w = wa.width;
    t.h = wa.height;
    type = getwintype(w);
    for (i = NetTypeDialog; i <= NetTypeNotification; i++)
        if (type == atoms[i])
            t.isfloating = 1;
    /* transient windows float, but only when the parent is a real
     * managed window: SDL and friends set WM_TRANSIENT_FOR to the
     * root window on ordinary top-levels */
    if ((XGetTransientForHint(dpy, w, &trans) && findclient(trans)) ||
        isfixedsize(w))
        t.isfloating = 1;
    if (type == atoms[NetTypeNotification])
        wantfocus = 0; /* notifications must not steal focus */
    c = manage(&t, follow);
    XSetWindowBorderWidth(dpy, w, borderpx);
    XSelectInput(dpy, w,
                 EnterWindowMask | FocusChangeMask | StructureNotifyMask);
    xdesktop(c);
    updateclientlist();
    XMapWindow(dpy, w);
    if (wantfocus && c->ws == curws) {
        focus(c);
    } else {
        XSetWindowBorder(dpy, w, unfocuspx);
        grabbuttons(c, 0);
    }
    savelayout(c);
}

static void drop(Client *c) {
    if (pressclient == c)
        pressclient = NULL;
    unmanage(c);
    updateclientlist();
}

/* refresh the monitor list from RandR, sorted left to right */
static void updatemons(void) {
    XRRMonitorInfo *info = NULL;
    Monitor *geoms = NULL, t;
    int i, j, n = 0, px = -1, py = -1;

    if (rrbase >= 0)
        info = XRRGetMonitors(dpy, root, True, &n);
    if (n < 1)
        n = 1; /* fallback: one monitor covering the whole screen */
    arrsetlen(geoms, n);
    for (i = 0; i < n; i++) {
        geoms[i].x = info ? info[i].x : 0;
        geoms[i].y = info ? info[i].y : 0;
        geoms[i].w = info ? info[i].width : sw;
        geoms[i].h = info ? info[i].height : sh;
    }
    if (info)
        XRRFreeMonitors(info);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && geoms[j - 1].x > geoms[j].x; j--) {
            t = geoms[j - 1];
            geoms[j - 1] = geoms[j];
            geoms[j] = t;
        }
    getrootptr(&px, &py);
    setmons(geoms, (size_t)n, px, py);
    arrfree(geoms);
}

/* commands */

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
        signal(SIGCHLD, SIG_DFL);    /* undo our SIG_IGN: it survives exec */
        unsetenv("WAYLAND_DISPLAY"); /* children must pick X11, not a host
                                        compositor */
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
                moveresize(c, x + dx, y + dy, w, h);
                break;
            case DragResize:
                moveresize(c, x, y, MAX(50, w + dx), MAX(50, h + dy));
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
    if (mode == DragWidth)
        savelayout(col->sel);
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
static void syncactivemon(void) { syncmon(activemon()); }

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
        drop(c);
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
            XGrabKey(dpy, code, keys[i].mod | mods[j], root, True,
                     GrabModeAsync, GrabModeAsync);
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
    adopt(ev->window, 1);
}

static void unmapnotify(XEvent *e) {
    Client *c = findclient(e->xunmap.window);

    if (c)
        drop(c);
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
    XChangeProperty(dpy, root, atoms[NetWMCheck], XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&checkwin, 1);
    XChangeProperty(dpy, root, atoms[NetSupported], XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&atoms[NetSupported],
                    NetWMWindowType - NetSupported + 1);
    setcardinal(root, atoms[NetNumDesktops], (long)nworkspaces);
    setcardinal(root, atoms[NetCurDesktop], (long)curws);
    pubws = curws;
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

    layoutinit(&xops);
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
            adopt(wins[i], 0);
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

/* restart once our binary has been replaced and has stopped changing:
 * the linker unlinks and rewrites it, so wait for two identical polls */
static void checkself(void) {
    static long last;
    static struct stat prev;
    static int prevok;
    long now = nowms();
    struct stat st;

    /* the settle check assumes ~1s polls; animation frames wake us faster */
    if (now - last < 1000)
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
        if (animstep())
            timeout = 1000 / 60;
        XFlush(dpy); /* the last animation frame must not wait for an event */
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
