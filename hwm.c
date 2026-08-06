/* hwm - a scrollable-column tiling window manager for X11.
 *
 * Windows live in columns on an infinite horizontal strip; the screen is a
 * viewport that scrolls over it (like niri). Opening a window never resizes
 * the others. One window per column by default; Mod+Shift+direction moves
 * windows between columns. No decorations except borders, configured in
 * config.c, in the spirit of dwm.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>

#include "hwm.h"

static void die(const char *msg);
static void *erealloc(void *ptr, size_t size);
#define STBDS_REALLOC(ctx, ptr, size) erealloc(ptr, size)
#define STBDS_FREE(ctx, ptr) free(ptr)
#define STB_DS_IMPLEMENTATION
#include <stb_ds.h>

#define LENGTH(X)    (sizeof(X) / sizeof((X)[0]))
#define MAX(A, B)    ((A) > (B) ? (A) : (B))
#define MOUSEMASK    (ButtonPressMask|ButtonReleaseMask|PointerMotionMask)
#define CLEANMASK(M) ((M) & ~(numlockmask|LockMask) \
	& (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))

typedef struct Client Client;
typedef struct Column Column;

struct Client {
	Window win;
	Column *col;      /* NULL while floating */
	int x, y, w, h;   /* last applied geometry */
	int isfull;
	int isfloating;
	size_t ws;
};

struct Column {
	Client **clients; /* stb_ds array, top to bottom */
	Client *sel;      /* focused window of this column */
	float width;      /* fraction of the usable screen width */
	size_t ws;
};

typedef struct {
	Column **cols;   /* stb_ds array, left to right */
	Column *selcol;
	Client **floats; /* stb_ds array, floating windows, bottom to top */
	Client *floatsel; /* focused floating window, or NULL */
	int scroll;      /* viewport offset in px */
} Workspace;

static void buttonpress(XEvent *e);
static void clientmessage(XEvent *e);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static void destroynotify(XEvent *e);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
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
static int running = 1;
static int dorestart;
static char selfpath[PATH_MAX];
static struct stat selfstat;
static int selfok;
static Atom wm_protocols, wm_delete;
static Atom net_supported, net_clientlist, net_activewindow, net_curdesktop,
            net_numdesktops, net_wmdesktop, net_wmcheck, net_wmname;
static Atom net_wmwintype, net_wmtype_notification;
static Atom floattypes[6]; /* window types managed as floating */
static Client *pressclient; /* client under the most recent button press */
static Window checkwin;
static Window indwin;       /* workspace indicator popup, bottom right */
static GC indgc;
static XFontStruct *indfont;
static int indw, indh;
static int indshown;
static struct timespec indhide; /* when to unmap the indicator */
static int (*xerrorxlib)(Display *, XErrorEvent *);

static void (*handler[LASTEvent])(XEvent *) = {
	[ButtonPress] = buttonpress,
	[ClientMessage] = clientmessage,
	[ConfigureNotify] = configurenotify,
	[ConfigureRequest] = configurerequest,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[Expose] = expose,
	[FocusIn] = focusin,
	[KeyPress] = keypress,
	[MappingNotify] = mappingnotify,
	[MapRequest] = maprequest,
	[UnmapNotify] = unmapnotify,
};

static void
die(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

static void *
ecalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb, size);
	if (!p)
		die("hwm: out of memory");
	return p;
}

static void *
erealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size);
	if (!p)
		die("hwm: out of memory");
	return p;
}

static Workspace *
curwsp(void)
{
	return &wss[curws];
}

static Client *
focused(void)
{
	Workspace *ws = curwsp();

	if (ws->floatsel)
		return ws->floatsel;
	return ws->selcol ? ws->selcol->sel : NULL;
}

static ptrdiff_t
colidx(Workspace *ws, Column *col)
{
	ptrdiff_t i;

	for (i = 0; i < arrlen(ws->cols); i++)
		if (ws->cols[i] == col)
			return i;
	return -1;
}

static ptrdiff_t
clientidx(Column *col, Client *c)
{
	ptrdiff_t i;

	for (i = 0; i < arrlen(col->clients); i++)
		if (col->clients[i] == c)
			return i;
	return -1;
}

static ptrdiff_t
floatidx(Workspace *ws, Client *c)
{
	ptrdiff_t i;

	for (i = 0; i < arrlen(ws->floats); i++)
		if (ws->floats[i] == c)
			return i;
	return -1;
}

static int
usew(void)
{
	return sw - 2 * (int)padding;
}

static int
useh(void)
{
	return sh - 2 * (int)padding;
}

static int
colpx(Column *col)
{
	return MAX(50, (int)(col->width * (float)usew()));
}

static int
colvx(Workspace *ws, Column *col)
{
	ptrdiff_t i;
	int x = 0;

	for (i = 0; i < arrlen(ws->cols) && ws->cols[i] != col; i++)
		x += colpx(ws->cols[i]) + (int)margin;
	return x;
}

static void
clampscroll(Workspace *ws)
{
	ptrdiff_t i;
	int tw = 0, max;

	for (i = 0; i < arrlen(ws->cols); i++)
		tw += colpx(ws->cols[i])
		      + (i + 1 < arrlen(ws->cols) ? (int)margin : 0);
	max = MAX(0, tw - usew());
	if (ws->scroll > max)
		ws->scroll = max;
	if (ws->scroll < 0)
		ws->scroll = 0;
}

static void
ensurevisible(Column *col)
{
	Workspace *ws = &wss[col->ws];
	int vx = colvx(ws, col), cw = colpx(col), uw = usew();

	if (cw >= uw || vx < ws->scroll)
		ws->scroll = vx;
	else if (vx + cw > ws->scroll + uw)
		ws->scroll = vx + cw - uw;
	clampscroll(ws);
}

static Client *
findclient(Window w)
{
	Column *col;
	ptrdiff_t j, k;
	size_t i;

	for (i = 0; i < nworkspaces; i++) {
		for (j = 0; j < arrlen(wss[i].cols); j++) {
			col = wss[i].cols[j];
			for (k = 0; k < arrlen(col->clients); k++)
				if (col->clients[k]->win == w)
					return col->clients[k];
		}
		for (j = 0; j < arrlen(wss[i].floats); j++)
			if (wss[i].floats[j]->win == w)
				return wss[i].floats[j];
	}
	return NULL;
}

static void
resizeclient(Client *c, int x, int y, int w, int h)
{
	c->x = x;
	c->y = y;
	c->w = MAX(1, w);
	c->h = MAX(1, h);
	XMoveResizeWindow(dpy, c->win, c->x, c->y,
	                  (unsigned int)c->w, (unsigned int)c->h);
}

static void
arrangews(size_t wi)
{
	Workspace *ws = &wss[wi];
	Column *col;
	Client *c;
	ptrdiff_t ci, i, n;
	int uh = useh();
	int xoff = (wi == curws) ? 0 : -3 * sw; /* park hidden workspaces offscreen */
	int x, y, cw, h, each;

	clampscroll(ws);
	x = (int)padding - ws->scroll;
	for (ci = 0; ci < arrlen(ws->cols); ci++) {
		col = ws->cols[ci];
		cw = colpx(col);
		n = arrlen(col->clients);
		if (n) {
			each = MAX(1, (uh - ((int)n - 1) * (int)margin) / (int)n);
			y = (int)padding;
			for (i = 0; i < n; i++) {
				c = col->clients[i];
				h = i + 1 < n ? each : (int)padding + uh - y;
				resizeclient(c, xoff + x, y,
				             cw - 2 * (int)borderpx,
				             h - 2 * (int)borderpx);
				y += h + (int)margin;
			}
		}
		x += cw + (int)margin;
	}
	for (ci = 0; ci < arrlen(ws->cols); ci++)
		for (i = 0; i < arrlen(ws->cols[ci]->clients); i++) {
			c = ws->cols[ci]->clients[i];
			if (c->isfull) {
				resizeclient(c, xoff, 0, sw, sh);
				XRaiseWindow(dpy, c->win);
			}
		}
	/* floats keep their own geometry and stay above the strip */
	for (i = 0; i < arrlen(ws->floats); i++) {
		c = ws->floats[i];
		if (c->isfull)
			XMoveResizeWindow(dpy, c->win, xoff, 0,
			                  (unsigned int)sw, (unsigned int)sh);
		else
			XMoveResizeWindow(dpy, c->win, xoff + c->x, c->y,
			                  (unsigned int)c->w, (unsigned int)c->h);
		XRaiseWindow(dpy, c->win);
	}
}

static void
setcardinal(Window w, Atom prop, long value)
{
	XChangeProperty(dpy, w, prop, XA_CARDINAL, 32, PropModeReplace,
	                (unsigned char *)&value, 1);
}

/* rofi and friends read _NET_CLIENT_LIST to enumerate windows */
static void
updateclientlist(void)
{
	Column *col;
	ptrdiff_t j, k;
	size_t i;

	XDeleteProperty(dpy, root, net_clientlist);
	for (i = 0; i < nworkspaces; i++) {
		for (j = 0; j < arrlen(wss[i].cols); j++) {
			col = wss[i].cols[j];
			for (k = 0; k < arrlen(col->clients); k++)
				XChangeProperty(dpy, root, net_clientlist,
				                XA_WINDOW, 32, PropModeAppend,
				                (unsigned char *)&col->clients[k]->win,
				                1);
		}
		for (j = 0; j < arrlen(wss[i].floats); j++)
			XChangeProperty(dpy, root, net_clientlist,
			                XA_WINDOW, 32, PropModeAppend,
			                (unsigned char *)&wss[i].floats[j]->win,
			                1);
	}
}

static void
grabbuttons(Client *c, int isfocused)
{
	unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
	size_t i, j;

	XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
	if (!isfocused) /* click-to-focus: catch the first click, then replay it */
		XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
		            ButtonPressMask, GrabModeSync, GrabModeSync,
		            None, None);
	for (i = 0; i < nbuttons; i++)
		for (j = 0; j < LENGTH(mods); j++)
			XGrabButton(dpy, buttons[i].button,
			            buttons[i].mod | mods[j], c->win, False,
			            ButtonPressMask, GrabModeAsync, GrabModeSync,
			            None, None);
}

static void
focus(Client *c)
{
	Workspace *ws = curwsp();
	Client *i;
	ptrdiff_t ci, j;

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
	for (ci = 0; ci < arrlen(ws->cols); ci++)
		for (j = 0; j < arrlen(ws->cols[ci]->clients); j++) {
			i = ws->cols[ci]->clients[j];
			XSetWindowBorder(dpy, i->win,
			                 i == c ? focuspx : unfocuspx);
			grabbuttons(i, i == c);
		}
	for (j = 0; j < arrlen(ws->floats); j++) {
		i = ws->floats[j];
		XSetWindowBorder(dpy, i->win, i == c ? focuspx : unfocuspx);
		grabbuttons(i, i == c);
	}
	XSetInputFocus(dpy, c ? c->win : root, RevertToPointerRoot,
	               CurrentTime);
	if (c)
		XChangeProperty(dpy, root, net_activewindow, XA_WINDOW, 32,
		                PropModeReplace, (unsigned char *)&c->win, 1);
	else
		XDeleteProperty(dpy, root, net_activewindow);
}

/* insert col into workspace wi after `after` (NULL = leftmost) */
static Column *
newcol(size_t wi, Column *after, float width)
{
	Workspace *ws = &wss[wi];
	Column *col = ecalloc(1, sizeof(Column));
	ptrdiff_t at = after ? colidx(ws, after) + 1 : 0;

	col->width = width;
	col->ws = wi;
	arrins(ws->cols, at, col);
	return col;
}

/* remove c from its column; empty columns are freed */
static void
detach(Client *c)
{
	Column *col = c->col;
	Workspace *ws = &wss[col->ws];
	ptrdiff_t i = clientidx(col, c), ci;

	arrdel(col->clients, i);
	if (col->sel == c)
		col->sel = arrlen(col->clients)
		           ? col->clients[i < arrlen(col->clients)
		                          ? i : arrlen(col->clients) - 1]
		           : NULL;
	if (!arrlen(col->clients)) {
		ci = colidx(ws, col);
		arrdel(ws->cols, ci);
		if (ws->selcol == col)
			ws->selcol = arrlen(ws->cols)
			             ? ws->cols[ci < arrlen(ws->cols)
			                        ? ci : arrlen(ws->cols) - 1]
			             : NULL;
		arrfree(col->clients);
		free(col);
	}
	c->col = NULL;
}

static void
sendconfigure(Client *c)
{
	XConfigureEvent ce = {0};

	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->isfull ? 0 : (int)borderpx;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static int
sendproto(Client *c, Atom proto)
{
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
		ev.xclient.message_type = wm_protocols;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = (long)proto;
		ev.xclient.data.l[1] = CurrentTime;
		XSendEvent(dpy, c->win, False, NoEventMask, &ev);
	}
	return exists;
}

static Atom
getwintype(Window w)
{
	Atom type = None, real;
	int fmt;
	unsigned long n, extra;
	unsigned char *p = NULL;

	if (XGetWindowProperty(dpy, w, net_wmwintype, 0L, 1L, False, XA_ATOM,
	                       &real, &fmt, &n, &extra, &p) == Success && p) {
		if (n)
			type = *(Atom *)p;
		XFree(p);
	}
	return type;
}

static int
isfixedsize(Window w)
{
	XSizeHints size;
	long msize;

	if (!XGetWMNormalHints(dpy, w, &size, &msize))
		return 0;
	return (size.flags & PMinSize) && (size.flags & PMaxSize)
	       && size.min_width > 0 && size.min_height > 0
	       && size.min_width == size.max_width
	       && size.min_height == size.max_height;
}

static void
manage(Window w)
{
	Workspace *ws = curwsp();
	XWindowAttributes wa;
	Window trans;
	Client *c;
	Column *col;
	Atom type;
	int wantfocus = 1;
	size_t i;

	if (findclient(w))
		return;
	if (!XGetWindowAttributes(dpy, w, &wa))
		return;
	c = ecalloc(1, sizeof(Client));
	c->win = w;
	c->ws = curws;
	type = getwintype(w);
	for (i = 0; i < LENGTH(floattypes); i++)
		if (type == floattypes[i])
			c->isfloating = 1;
	if (XGetTransientForHint(dpy, w, &trans) || isfixedsize(w))
		c->isfloating = 1;
	if (c->isfloating) {
		/* keep the requested geometry; center windows that didn't ask
		 * for a position, clamp the rest onto the screen */
		c->w = MAX(1, wa.width);
		c->h = MAX(1, wa.height);
		c->x = wa.x;
		c->y = wa.y;
		if (c->x <= 0 && c->y <= 0) {
			c->x = (sw - c->w) / 2;
			c->y = (sh - c->h) / 2;
		}
		if (c->x + c->w + 2 * (int)borderpx > sw)
			c->x = sw - c->w - 2 * (int)borderpx;
		if (c->y + c->h + 2 * (int)borderpx > sh)
			c->y = sh - c->h - 2 * (int)borderpx;
		c->x = MAX(0, c->x);
		c->y = MAX(0, c->y);
		arrput(ws->floats, c);
		if (type == net_wmtype_notification)
			wantfocus = 0; /* notifications must not steal focus */
	} else {
		col = newcol(curws, ws->selcol, defwidth);
		arrput(col->clients, c);
		col->sel = c;
		c->col = col;
		ws->selcol = col;
		ensurevisible(col);
	}
	XSetWindowBorderWidth(dpy, w, borderpx);
	XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|StructureNotifyMask);
	setcardinal(w, net_wmdesktop, (long)curws);
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

static void
unmanage(Client *c)
{
	size_t wi = c->ws;
	Workspace *ws = &wss[wi];

	if (pressclient == c)
		pressclient = NULL;
	if (c->isfloating) {
		arrdel(ws->floats, floatidx(ws, c));
		if (ws->floatsel == c)
			ws->floatsel = NULL;
	} else {
		detach(c);
	}
	free(c);
	updateclientlist();
	arrangews(wi);
	if (wi == curws)
		focus(focused());
}

static int
getrootptr(int *x, int *y)
{
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

/* workspace indicator: a small popup in the bottom-right corner showing the
 * workspace number, unmapped again wsindms after the last switch */

static void
drawindicator(void)
{
	char text[16];
	int len, tw;

	len = snprintf(text, sizeof text, "%zu", curws + 1);
	tw = XTextWidth(indfont, text, len);
	XClearWindow(dpy, indwin);
	XDrawString(dpy, indwin, indgc, (indw - tw) / 2,
	            (indh - indfont->ascent - indfont->descent) / 2
	            + indfont->ascent, text, len);
}

static void
hideindicator(void)
{
	XUnmapWindow(dpy, indwin);
	indshown = 0;
}

static long
indicatorms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (indhide.tv_sec - now.tv_sec) * 1000
	       + (indhide.tv_nsec - now.tv_nsec) / 1000000;
}

static void
showindicator(void)
{
	XSetWindowAttributes swa;
	char text[16];
	int len, tw, pad;

	if (!wsindms)
		return;
	if (!indfont) {
		indfont = XLoadQueryFont(dpy, wsindfont);
		if (!indfont)
			indfont = XLoadQueryFont(dpy, "fixed");
		if (!indfont)
			return;
	}
	if (!indwin) {
		swa.override_redirect = True;
		swa.background_pixel = unfocuspx;
		swa.border_pixel = focuspx;
		swa.event_mask = ExposureMask;
		indwin = XCreateWindow(dpy, root, 0, 0, 1, 1, borderpx,
		                       CopyFromParent, CopyFromParent,
		                       CopyFromParent,
		                       CWOverrideRedirect|CWBackPixel
		                       |CWBorderPixel|CWEventMask, &swa);
		indgc = XCreateGC(dpy, indwin, 0, NULL);
		XSetFont(dpy, indgc, indfont->fid);
		XSetForeground(dpy, indgc, focuspx);
	}
	len = snprintf(text, sizeof text, "%zu", curws + 1);
	tw = XTextWidth(indfont, text, len);
	pad = indfont->ascent + indfont->descent;
	indh = 2 * pad;
	indw = MAX(indh, tw + pad);
	XMoveResizeWindow(dpy, indwin,
	                  sw - indw - 2 * (int)borderpx - (int)padding,
	                  sh - indh - 2 * (int)borderpx - (int)padding,
	                  (unsigned int)indw, (unsigned int)indh);
	XMapRaised(dpy, indwin);
	drawindicator();
	clock_gettime(CLOCK_MONOTONIC, &indhide);
	indhide.tv_sec += wsindms / 1000;
	indhide.tv_nsec += (long)(wsindms % 1000) * 1000000L;
	if (indhide.tv_nsec >= 1000000000L) {
		indhide.tv_sec++;
		indhide.tv_nsec -= 1000000000L;
	}
	indshown = 1;
}

/* commands */

/* drop fullscreen when focus moves away, so the new selection is visible */
static void
exitfull(Client *c)
{
	if (!c || !c->isfull)
		return;
	c->isfull = 0;
	XSetWindowBorderWidth(dpy, c->win, borderpx);
	arrangews(curws);
}

void
focushorz(const Arg *arg)
{
	Workspace *ws = curwsp();
	Column *col;
	ptrdiff_t i;

	if (!ws->selcol)
		return;
	i = colidx(ws, ws->selcol) + (arg->i > 0 ? 1 : -1);
	if (i < 0 || i >= arrlen(ws->cols))
		return;
	exitfull(focused());
	col = ws->cols[i];
	focus(col->sel ? col->sel : col->clients[0]);
	ensurevisible(col);
	arrangews(curws);
}

void
focusvert(const Arg *arg)
{
	Client *c = focused();
	ptrdiff_t i;

	if (!c || c->isfloating)
		return;
	i = clientidx(c->col, c) + (arg->i > 0 ? 1 : -1);
	if (i < 0 || i >= arrlen(c->col->clients))
		return;
	exitfull(c);
	focus(c->col->clients[i]);
}

void
movehorz(const Arg *arg)
{
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
		col = newcol(curws, after, defwidth);
		arrput(col->clients, c);
		col->sel = c;
		c->col = col;
		ws->selcol = col;
	}
	ensurevisible(c->col);
	arrangews(curws);
	focus(c);
}

void
movevert(const Arg *arg)
{
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

void
cyclewidth(const Arg *arg)
{
	Column *col = curwsp()->selcol;
	size_t i, best = 0;
	float d, bd = 2.0f;

	(void)arg;
	if (!col || !nwidths)
		return;
	for (i = 0; i < nwidths; i++) {
		d = col->width - widths[i];
		if (d < 0)
			d = -d;
		if (d < bd) {
			bd = d;
			best = i;
		}
	}
	col->width = widths[(best + 1) % nwidths];
	ensurevisible(col);
	arrangews(curws);
}

void
growwidth(const Arg *arg)
{
	Column *col = curwsp()->selcol;

	if (!col)
		return;
	col->width += arg->f;
	if (col->width < 0.1f)
		col->width = 0.1f;
	if (col->width > 1.0f)
		col->width = 1.0f;
	ensurevisible(col);
	arrangews(curws);
}

void
scrollby(const Arg *arg)
{
	curwsp()->scroll += (int)(arg->f * (float)usew());
	arrangews(curws);
}

void
togglefull(const Arg *arg)
{
	Client *c = focused();

	(void)arg;
	if (!c)
		return;
	c->isfull = !c->isfull;
	XSetWindowBorderWidth(dpy, c->win, c->isfull ? 0 : borderpx);
	arrangews(curws);
}

void
togglefloat(const Arg *arg)
{
	Workspace *ws = curwsp();
	Client *c = focused();
	Column *col;

	(void)arg;
	if (!c)
		return;
	if (c->isfloating) {
		arrdel(ws->floats, floatidx(ws, c));
		if (ws->floatsel == c)
			ws->floatsel = NULL;
		c->isfloating = 0;
		col = newcol(curws, ws->selcol, defwidth);
		arrput(col->clients, c);
		col->sel = c;
		c->col = col;
		ws->selcol = col;
		ensurevisible(col);
	} else {
		detach(c);
		c->isfloating = 1;
		/* floats in place, keeping the geometry it had in the strip */
		arrput(ws->floats, c);
	}
	arrangews(curws);
	focus(c);
}

void
view(const Arg *arg)
{
	size_t old = curws;

	if (arg->i < 0 || (size_t)arg->i >= nworkspaces
	    || (size_t)arg->i == curws)
		return;
	curws = (size_t)arg->i;
	setcardinal(root, net_curdesktop, (long)curws);
	arrangews(old);
	arrangews(curws);
	focus(focused());
	showindicator();
}

void
sendto(const Arg *arg)
{
	Workspace *target;
	Client *c = focused();
	Column *col;

	if (!c || arg->i < 0 || (size_t)arg->i >= nworkspaces
	    || (size_t)arg->i == curws)
		return;
	target = &wss[arg->i];
	if (c->isfloating) {
		Workspace *ws = curwsp();

		arrdel(ws->floats, floatidx(ws, c));
		if (ws->floatsel == c)
			ws->floatsel = NULL;
		arrput(target->floats, c);
	} else {
		detach(c);
		col = newcol((size_t)arg->i, target->selcol, defwidth);
		arrput(col->clients, c);
		col->sel = c;
		c->col = col;
		target->selcol = col;
	}
	c->ws = (size_t)arg->i;
	setcardinal(c->win, net_wmdesktop, arg->i);
	arrangews(curws);
	arrangews((size_t)arg->i);
	focus(focused());
}

void
killclient(const Arg *arg)
{
	Client *c = focused();

	(void)arg;
	if (!c)
		return;
	if (!sendproto(c, wm_delete))
		XKillClient(dpy, c->win);
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		unsetenv("WAYLAND_DISPLAY"); /* children must pick X11, not a host compositor */
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "hwm: execvp %s failed\n",
		        ((char **)arg->v)[0]);
		exit(1);
	}
}

void
quit(const Arg *arg)
{
	(void)arg;
	running = 0;
}

void
restart(const Arg *arg)
{
	(void)arg;
	dorestart = 1;
	running = 0;
}

/* Mod+drag on a floating window: move it (or resize with doresize) */
static void
dragfloat(Client *c, int doresize)
{
	int rx, ry, x = c->x, y = c->y, w = c->w, h = c->h;
	Time last = 0;
	XEvent ev;

	if (!getrootptr(&rx, &ry))
		return;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync,
	                 GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
		return;
	do {
		XMaskEvent(dpy, MOUSEMASK|SubstructureRedirectMask, &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if (ev.xmotion.time - last < 1000 / 60)
				break;
			last = ev.xmotion.time;
			if (doresize)
				resizeclient(c, x, y,
				             MAX(50, w + ev.xmotion.x_root - rx),
				             MAX(50, h + ev.xmotion.y_root - ry));
			else
				resizeclient(c, x + ev.xmotion.x_root - rx,
				             y + ev.xmotion.y_root - ry, w, h);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
}

void
dragscroll(const Arg *arg)
{
	Workspace *ws = curwsp();
	int rx, ry, start = ws->scroll;
	Time last = 0;
	XEvent ev;

	(void)arg;
	if (pressclient && pressclient->isfloating && !pressclient->isfull) {
		dragfloat(pressclient, 0);
		return;
	}
	if (!getrootptr(&rx, &ry))
		return;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync,
	                 GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
		return;
	do {
		XMaskEvent(dpy, MOUSEMASK|SubstructureRedirectMask, &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if (ev.xmotion.time - last < 1000 / 60)
				break;
			last = ev.xmotion.time;
			ws->scroll = start - (ev.xmotion.x_root - rx);
			arrangews(curws);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
}

void
dragwidth(const Arg *arg)
{
	Column *col = curwsp()->selcol;
	int rx, ry;
	float start, w;
	Time last = 0;
	XEvent ev;

	(void)arg;
	if (pressclient && pressclient->isfloating && !pressclient->isfull) {
		dragfloat(pressclient, 1);
		return;
	}
	if (!col)
		return;
	start = col->width;
	if (!getrootptr(&rx, &ry))
		return;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync,
	                 GrabModeAsync, None, None, CurrentTime) != GrabSuccess)
		return;
	do {
		XMaskEvent(dpy, MOUSEMASK|SubstructureRedirectMask, &ev);
		switch (ev.type) {
		case ConfigureRequest:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if (ev.xmotion.time - last < 1000 / 60)
				break;
			last = ev.xmotion.time;
			w = start + (float)(ev.xmotion.x_root - rx)
			    / (float)usew();
			if (w < 0.1f)
				w = 0.1f;
			if (w > 1.0f)
				w = 1.0f;
			col->width = w;
			arrangews(curws);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
}

/* event handlers */

static void
buttonpress(XEvent *e)
{
	XButtonPressedEvent *ev = &e->xbutton;
	Client *c = findclient(ev->window);
	size_t i;

	pressclient = c;
	if (c) {
		if (c != focused())
			focus(c);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
	}
	for (i = 0; i < nbuttons; i++)
		if (buttons[i].button == ev->button && buttons[i].func
		    && CLEANMASK(buttons[i].mod) == CLEANMASK(ev->state))
			buttons[i].func(&buttons[i].arg);
}

static void
clientmessage(XEvent *e)
{
	XClientMessageEvent *ev = &e->xclient;
	Client *c;
	Arg a;

	if (ev->message_type == net_activewindow) {
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
	} else if (ev->message_type == net_curdesktop) {
		a.i = (int)ev->data.l[0];
		view(&a);
	}
}

static void
configurenotify(XEvent *e)
{
	XConfigureEvent *ev = &e->xconfigure;
	size_t i;

	if (ev->window == root && (ev->width != sw || ev->height != sh)) {
		sw = ev->width;
		sh = ev->height;
		for (i = 0; i < nworkspaces; i++)
			arrangews(i);
	}
}

static void
configurerequest(XEvent *e)
{
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	Client *c = findclient(ev->window);
	XWindowChanges wc;

	if (c && c->isfloating && !c->isfull) {
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
			XMoveResizeWindow(dpy, c->win, c->x, c->y,
			                  (unsigned int)c->w, (unsigned int)c->h);
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
		XConfigureWindow(dpy, ev->window, (unsigned int)ev->value_mask,
		                 &wc);
	}
	XSync(dpy, False);
}

static void
destroynotify(XEvent *e)
{
	Client *c = findclient(e->xdestroywindow.window);

	if (c)
		unmanage(c);
}

static void
enternotify(XEvent *e)
{
	XCrossingEvent *ev = &e->xcrossing;
	Client *c;

	if (!focusfollowsmouse)
		return;
	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior)
	    && ev->window != root)
		return;
	c = findclient(ev->window);
	if (c && c != focused())
		focus(c);
}

static void
expose(XEvent *e)
{
	XExposeEvent *ev = &e->xexpose;

	if (indshown && ev->window == indwin && !ev->count)
		drawindicator();
}

static void
focusin(XEvent *e)
{
	Client *c = focused();

	if (c && e->xfocus.window != c->win)
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
}

static void
keypress(XEvent *e)
{
	XKeyEvent *ev = &e->xkey;
	KeySym keysym = XkbKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0, 0);
	size_t i;

	for (i = 0; i < nkeys; i++)
		if (keysym == keys[i].keysym && keys[i].func
		    && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state))
			keys[i].func(&keys[i].arg);
}

static void
updatenumlockmask(void)
{
	XModifierKeymap *modmap = XGetModifierMapping(dpy);
	int i, j;

	numlockmask = 0;
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j]
			    == XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = (1u << i);
	XFreeModifiermap(modmap);
}

static void
grabkeys(void)
{
	unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
	KeyCode code;
	size_t i, j;

	updatenumlockmask();
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < nkeys; i++) {
		code = XKeysymToKeycode(dpy, keys[i].keysym);
		if (!code)
			continue;
		for (j = 0; j < LENGTH(mods); j++)
			XGrabKey(dpy, code, keys[i].mod | mods[j], root, True,
			         GrabModeAsync, GrabModeAsync);
	}
}

static void
grabrootbuttons(void)
{
	unsigned int mods[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
	size_t i, j;

	for (i = 0; i < nbuttons; i++)
		for (j = 0; j < LENGTH(mods); j++)
			XGrabButton(dpy, buttons[i].button,
			            buttons[i].mod | mods[j], root, False,
			            ButtonPressMask, GrabModeAsync, GrabModeSync,
			            None, None);
}

static void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabkeys();
}

static void
maprequest(XEvent *e)
{
	XMapRequestEvent *ev = &e->xmaprequest;
	XWindowAttributes wa;

	if (!XGetWindowAttributes(dpy, ev->window, &wa)
	    || wa.override_redirect)
		return;
	manage(ev->window);
}

static void
unmapnotify(XEvent *e)
{
	Client *c = findclient(e->xunmap.window);

	if (c)
		unmanage(c);
}

/* startup */

static int
xerror(Display *d, XErrorEvent *ee)
{
	/* ignore errors from windows that vanished under us, like dwm */
	if (ee->error_code == BadWindow
	    || (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	    || (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	    || (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	    || (ee->request_code == X_GrabKey && ee->error_code == BadAccess))
		return 0;
	fprintf(stderr, "hwm: X error: request %d, error %d\n",
	        ee->request_code, ee->error_code);
	return xerrorxlib(d, ee);
}

static int
xerrorstart(Display *d, XErrorEvent *ee)
{
	(void)d;
	(void)ee;
	die("hwm: another window manager is already running");
	return -1;
}

static unsigned long
getcolor(const char *name)
{
	XColor c, exact;

	if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &c,
	                      &exact))
		die("hwm: cannot allocate color");
	return c.pixel;
}

/* advertise just enough EWMH for pagers/switchers like rofi */
static void
initewmh(void)
{
	Atom supported[] = { net_supported, net_clientlist, net_activewindow,
	                     net_curdesktop, net_numdesktops, net_wmdesktop,
	                     net_wmcheck, net_wmwintype };
	Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	checkwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, checkwin, net_wmcheck, XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&checkwin, 1);
	XChangeProperty(dpy, checkwin, net_wmname, utf8, 8,
	                PropModeReplace, (unsigned char *)"hwm", 3);
	XChangeProperty(dpy, root, net_wmcheck, XA_WINDOW, 32,
	                PropModeReplace, (unsigned char *)&checkwin, 1);
	XChangeProperty(dpy, root, net_supported, XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)supported,
	                LENGTH(supported));
	setcardinal(root, net_numdesktops, (long)nworkspaces);
	setcardinal(root, net_curdesktop, (long)curws);
}

static void
setup(void)
{
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
	focuspx = getcolor(col_focus);
	unfocuspx = getcolor(col_unfocus);
	wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	net_supported = XInternAtom(dpy, "_NET_SUPPORTED", False);
	net_clientlist = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	net_activewindow = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	net_curdesktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
	net_numdesktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
	net_wmdesktop = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
	net_wmcheck = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	net_wmname = XInternAtom(dpy, "_NET_WM_NAME", False);
	net_wmwintype = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	net_wmtype_notification =
		XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
	floattypes[0] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	floattypes[1] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
	floattypes[2] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
	floattypes[3] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
	floattypes[4] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
	floattypes[5] = net_wmtype_notification;
	initewmh();
	XDefineCursor(dpy, root, XCreateFontCursor(dpy, XC_left_ptr));
	XSelectInput(dpy, root, SubstructureRedirectMask|SubstructureNotifyMask
	             |StructureNotifyMask|ButtonPressMask);
	grabkeys();
	grabrootbuttons();
	XSync(dpy, False);
}

static void
scan(void)
{
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;
	unsigned int i, num;

	if (!XQueryTree(dpy, root, &d1, &d2, &wins, &num))
		return;
	for (i = 0; i < num; i++)
		if (XGetWindowAttributes(dpy, wins[i], &wa)
		    && !wa.override_redirect && wa.map_state == IsViewable)
			manage(wins[i]);
	if (wins)
		XFree(wins);
}

static void
autostartrun(void)
{
	size_t i;

	for (i = 0; i < nautostart; i++) {
		const char *cmd[] = { "/bin/sh", "-c", autostart[i], NULL };
		Arg a = { .v = cmd };

		spawn(&a);
	}
}

static void
initselfwatch(const char *argv0)
{
	ssize_t n = readlink("/proc/self/exe", selfpath, sizeof selfpath - 1);

	if (n > 0)
		selfpath[n] = '\0';
	else if (strchr(argv0, '/') && strlen(argv0) < sizeof selfpath)
		strcpy(selfpath, argv0);
	else
		return; /* bare name in PATH; no watch, Mod+Shift+r still works */
	selfok = stat(selfpath, &selfstat) == 0;
}

static int
samefile(const struct stat *a, const struct stat *b)
{
	return a->st_ino == b->st_ino
	       && a->st_mtim.tv_sec == b->st_mtim.tv_sec
	       && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

/* restart once our binary has been replaced and has stopped changing:
 * the linker unlinks and rewrites it, so wait for two identical polls */
static void
checkself(void)
{
	static struct stat prev;
	static int prevok;
	struct stat st;

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

static void
run(void)
{
	XEvent ev;
	fd_set fds;
	struct timeval tv;
	long ms;
	int xfd = ConnectionNumber(dpy);

	while (running) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			if (ev.type < LASTEvent && handler[ev.type])
				handler[ev.type](&ev);
			if (!running)
				return;
		}
		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		if (indshown) {
			ms = indicatorms();
			if (ms <= 0) {
				hideindicator();
				XFlush(dpy);
			} else if (ms < 1000) {
				tv.tv_sec = ms / 1000;
				tv.tv_usec = (ms % 1000) * 1000;
			}
		}
		if (select(xfd + 1, &fds, NULL, NULL, &tv) < 0) {
			if (errno == EINTR)
				continue;
			die("hwm: select failed");
		}
		if (!FD_ISSET(xfd, &fds))
			checkself();
	}
}

int
main(int argc, char *argv[])
{
	(void)argc;
	initconfig();
	setup();
	initselfwatch(argv[0]);
	scan();
	autostartrun();
	run();
	XCloseDisplay(dpy);
	if (dorestart) {
		if (selfpath[0])
			execv(selfpath, argv);
		execvp(argv[0], argv);
		die("hwm: restart failed");
	}
	return 0;
}
