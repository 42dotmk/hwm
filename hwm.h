/* hwm - shared types and config declarations. See config.c for configuration. */
#ifndef HWM_H
#define HWM_H

#include <stddef.h>
#include <X11/Xlib.h>

typedef union {
	int i;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	Arg arg;
} Key;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	Arg arg;
} Button;

/* commands, bindable in config.c */
void focushorz(const Arg *arg);   /* .i = -1 left / +1 right (columns) */
void focusvert(const Arg *arg);   /* .i = -1 up / +1 down (within column) */
void movehorz(const Arg *arg);    /* move window/column left or right */
void movevert(const Arg *arg);    /* move window up or down in its column */
void cyclewidth(const Arg *arg);  /* cycle column width through widths[] */
void growwidth(const Arg *arg);   /* .f = width delta, fraction of screen */
void scrollby(const Arg *arg);    /* .f = scroll delta, fraction of screen */
void togglefull(const Arg *arg);  /* fullscreen the focused window */
void togglefloat(const Arg *arg); /* float/tile the focused window */
void view(const Arg *arg);        /* .i = workspace to show */
void sendto(const Arg *arg);      /* .i = workspace to send window to */
void killclient(const Arg *arg);
void spawn(const Arg *arg);       /* .v = char *argv[] */
void quit(const Arg *arg);
void restart(const Arg *arg);     /* exec ourselves; picks up a rebuilt binary */
void dragscroll(const Arg *arg);  /* mouse: drag the strip */
void dragwidth(const Arg *arg);   /* mouse: resize the focused column */

/* configuration, defined in config.c; the arrays are stb_ds arrays built
 * by initconfig(), which must run before setup() */
void initconfig(void);
extern const unsigned int borderpx;
extern const unsigned int padding;
extern const unsigned int margin;
extern const char col_focus[];
extern const char col_unfocus[];
extern const int focusfollowsmouse;
extern const float defwidth;
extern float *widths;
extern size_t nwidths;
extern const size_t nworkspaces;
extern Key *keys;
extern size_t nkeys;
extern Button *buttons;
extern size_t nbuttons;
extern const char **autostart;   /* sh -c commands run on start and reload */
extern size_t nautostart;

#endif
