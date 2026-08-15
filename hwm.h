/* hwm - shared types and config declarations. See config.c for configuration. */
#ifndef HWM_H
#define HWM_H

#include <stddef.h>
#include <X11/Xlib.h>

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

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
void movewsmon(const Arg *arg);   /* .i = -1/+1: move workspace to adjacent monitor */
void killclient(const Arg *arg);
void spawn(const Arg *arg);       /* .v = char *argv[] */
void quit(const Arg *arg);
void restart(const Arg *arg);     /* exec ourselves; picks up a rebuilt binary */
void dragscroll(const Arg *arg);  /* mouse: drag the strip */
void dragwidth(const Arg *arg);   /* mouse: resize the focused column */

/* configuration, defined in config.c; the pointers are stb_ds arrays built
 * by initconfig(), which must run before setup(); length via arrlen() */
void initconfig(void);
extern const unsigned int borderpx;
extern const unsigned int gappx;
extern const char col_focus[];
extern const char col_unfocus[];
extern const int focusfollowsmouse;
extern const char wsindfont[];      /* core X font for the workspace indicator */
extern const unsigned int wsindms;  /* indicator display time in ms; 0 disables */
extern const unsigned int scrollanimms; /* scroll animation duration in ms; 0 disables */
extern const float defwidth;
extern const float floatsize; /* size of newly floated windows, fraction of monitor */
extern const float gesturescale; /* three-finger swipe: scroll px per touchpad px */
extern float *widths;
extern const size_t nworkspaces;
extern Key *keys;
extern Button *buttons;
extern const char **autostart;   /* sh -c commands run on start and reload */

#endif
