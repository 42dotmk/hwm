/* hwm - the X11 shell's types and config declarations. The layout model and
 * its commands live in layout.h; see config.h for configuration. */
#ifndef HWM_H
#define HWM_H

#ifndef HWM_VERSION
#define HWM_VERSION "dev"
#endif

#include <X11/Xlib.h>
#include <stddef.h>

#include "layout.h"

#define LENGTH(X) (sizeof(X) / sizeof((X)[0]))

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

/* shell commands, bindable in config.h alongside the layout's */
void killclient(const Arg *arg);
void spawn(const Arg *arg); /* .v = char *argv[] */
void quit(const Arg *arg);
void restart(const Arg *arg);    /* exec ourselves; picks up a rebuilt binary */
void dragscroll(const Arg *arg); /* mouse: drag the strip */
void dragwidth(const Arg *arg);  /* mouse: resize the focused column */

/* configuration, defined in config.h; the pointers are stb_ds arrays built
 * by initconfig(), which must run before setup(); length via arrlen() */
void initconfig(void);
extern const char col_focus[];
extern const char col_unfocus[];
extern const int focusfollowsmouse;
extern const float
    gesturescale; /* three-finger swipe: scroll px per touchpad px */
extern Key *keys;
extern Button *buttons;
extern const char **autostart; /* sh -c commands run on start and reload */

#endif
