/* hwm configuration. Edit, then rebuild with `make`. */
#include <X11/keysym.h>
#include <X11/XF86keysym.h>
#include <stb_ds.h>

#include "hwm.h"

#define MODKEY Mod4Mask /* Super */

/* appearance */
const unsigned int borderpx = 2;          /* window border width in px */
const unsigned int gappx    = 6;          /* decoration: empty space around every window */
const char col_focus[]      = "#7aa2f7";  /* focused border color */
const char col_unfocus[]    = "#3b4261";  /* unfocused border color */
const int focusfollowsmouse = 0;
const char wsindfont[]      = "10x20";    /* workspace indicator font ("fixed" as fallback) */
const unsigned int wsindms  = 750;        /* workspace indicator display time in ms; 0 disables */
const unsigned int scrollanimms = 200;    /* scroll animation duration in ms; 0 disables */

/* layout */
const float defwidth  = 0.5f;             /* width of new columns, fraction of screen */
const float floatsize = 0.6f;             /* size of newly floated windows, fraction of monitor */
const float gesturescale = 3.0f;          /* three-finger swipe: scroll px per touchpad px */
const size_t nworkspaces = 10;

/* stb_ds arrays, built by initconfig() */
float *widths;
Key *keys;
Button *buttons;
const char **autostart;

static const char *termcmd[]    = { "hterm", NULL };
static const char *menucmd[]    = { "rofi", "-combi-modes", "window,drun,run,ssh", "-modes", "combi", "-show", "combi", NULL };
static const char *wincmd[]     = { "rofi", "-show", "window", NULL };
static const char *switchercmd[] = { "hws", NULL };
static const char *browsercmd[] = { "firefox", NULL };
static const char *filescmd[]   = { "thunar", NULL };
static const char *tmuxcmd[]    = { "hterm", "-e", "tmux", "new-session", "-A", "-s", "main", NULL };
static const char *mailcmd[]    = { "hterm", "-e", "hed", "-c", "mail", NULL };
static const char *todocmd[]    = { "hterm", "-e", "hed", "/home/halicea/org/todo.md", NULL };
static const char *calcmd[]     = { "firefox", "https://calendar.google.com", NULL };
static const char *traycmd[]    = { "pkill", "-USR1", "-x", "htray", NULL };
static const char *trayinputcmd[] = { "pkill", "-USR2", "-x", "htray", NULL };
static const char *volupcmd[]   = { "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+", NULL };
static const char *voldowncmd[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", NULL };
static const char *mutecmd[]    = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
static const char *micmutecmd[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle", NULL };
static const char *briupcmd[]   = { "brightnessctl", "set", "10%+", NULL };
static const char *bridowncmd[] = { "brightnessctl", "set", "10%-", NULL };
static const char *playcmd[]    = { "playerctl", "play-pause", NULL };
static const char *nextcmd[]    = { "playerctl", "next", NULL };
static const char *prevcmd[]    = { "playerctl", "previous", NULL };

static const Key basekeys[] = {
	/* modifier            key             function     argument */
	{ MODKEY,              XK_Return,      spawn,       { .v = termcmd } },
	{ MODKEY,              XK_space,       spawn,       { .v = menucmd } },
	{ MODKEY,              XK_o,           spawn,       { .v = wincmd } },
	{ MODKEY,              XK_Tab,         spawn,       { .v = switchercmd } },
	{ MODKEY,              XK_b,           spawn,       { .v = browsercmd } },
	{ MODKEY,              XK_e,           spawn,       { .v = filescmd } },
	{ MODKEY|ShiftMask,    XK_Return,      spawn,       { .v = tmuxcmd } },
	{ MODKEY,              XK_m,           spawn,       { .v = mailcmd } },
	{ MODKEY,              XK_c,           spawn,       { .v = calcmd } },
	{ MODKEY,              XK_t,           spawn,       { .v = todocmd } },
	{ MODKEY,              XK_z,           spawn,       { .v = traycmd } },
	{ MODKEY|ShiftMask,    XK_z,           spawn,       { .v = trayinputcmd } },
	{ MODKEY,              XK_q,           killclient,  {0} },
	{ MODKEY|ShiftMask,    XK_e,           quit,        {0} },
	{ MODKEY|ShiftMask,    XK_r,           restart,     {0} },

	{ MODKEY,              XK_h,           focushorz,   { .i = -1 } },
	{ MODKEY,              XK_l,           focushorz,   { .i = +1 } },
	{ MODKEY,              XK_k,           focusvert,   { .i = -1 } },
	{ MODKEY,              XK_j,           focusvert,   { .i = +1 } },


	{ MODKEY|ShiftMask,    XK_h,           movehorz,    { .i = -1 } },
	{ MODKEY|ShiftMask,    XK_l,           movehorz,    { .i = +1 } },
	{ MODKEY|ShiftMask,    XK_k,           movevert,    { .i = -1 } },
	{ MODKEY|ShiftMask,    XK_j,           movevert,    { .i = +1 } },


	{ MODKEY|ControlMask,  XK_h,           stackto,     { .i = -1 } },
	{ MODKEY|ControlMask,  XK_l,           stackto,     { .i = +1 } },


	{ MODKEY,              XK_r,           cyclewidth,  {0} },
	{ MODKEY,              XK_minus,       growwidth,   { .f = -0.05f } },
	{ MODKEY,              XK_equal,       growwidth,   { .f = +0.05f } },
	{ MODKEY,              XK_f,           togglefull,  {0} },
	{ MODKEY|ShiftMask,    XK_space,       togglefloat, {0} },
	{ MODKEY,              XK_bracketleft, scrollby,    { .f = -0.25f } },
	{ MODKEY,              XK_bracketright,scrollby,    { .f = +0.25f } },
	{ MODKEY,              XK_comma,       movewsmon,   { .i = -1 } },
	{ MODKEY,              XK_period,      movewsmon,   { .i = +1 } },

	{ 0, XF86XK_AudioRaiseVolume,  spawn, { .v = volupcmd } },
	{ 0, XF86XK_AudioLowerVolume,  spawn, { .v = voldowncmd } },
	{ 0, XF86XK_AudioMute,         spawn, { .v = mutecmd } },
	{ 0, XF86XK_AudioMicMute,      spawn, { .v = micmutecmd } },
	{ 0, XF86XK_MonBrightnessUp,   spawn, { .v = briupcmd } },
	{ 0, XF86XK_MonBrightnessDown, spawn, { .v = bridowncmd } },
	{ 0, XF86XK_AudioPlay,         spawn, { .v = playcmd } },
	{ 0, XF86XK_AudioNext,         spawn, { .v = nextcmd } },
	{ 0, XF86XK_AudioPrev,         spawn, { .v = prevcmd } },
};

static const Button basebuttons[] = {
	/* modifier   button   function    argument */
	{ MODKEY,     Button1, dragscroll, {0} }, /* on a float: move it */
	{ MODKEY,     Button3, dragwidth,  {0} }, /* on a float: resize it */
	{ MODKEY,     Button4, scrollby,   { .f = -0.1f } },
	{ MODKEY,     Button5, scrollby,   { .f = +0.1f } },
};

/* run with `sh -c` when hwm starts, including after a reload —
 * keep these idempotent or guard them (e.g. `pgrep x || x`) */
static const char *autostartcmds[] = {
	"xsetroot -solid '#1a1b26'", /* instant fallback while feh loads */
	"feh --no-fehbg --bg-fill --randomize ~/pictures/backgrounds/preffered",
	"setxkbmap -layout us,mk -option '' -option caps:escape -option shift:both_capslock -option grp:lalt_lshift_toggle",
	"pipewire",
	"htray",
	"nm-applet",
};

static const float widthpresets[] = { 1.0f/3.0f, 0.5f, 2.0f/3.0f, 1.0f }; /* cyclewidth */
#define SETARR(vals, arr) do { size_t i; for (i = 0; i < LENGTH(vals); i++) arrput(arr, vals[i]); } while (0)
/* one binding per element of vals: mod + (key + i) -> fn(arg), with i usable in arg */
#define SETKEYS(vals, mod, key, fn, ...) do { size_t i; for (i = 0; i < LENGTH(vals); i++) \
	arrput(keys, ((Key){ (mod), (key) + i, fn, __VA_ARGS__ })); } while (0)

void
initconfig(void)
{
	size_t i;

	SETARR(widthpresets, widths);
	SETARR(basekeys, keys);
	SETARR(basebuttons, buttons);
	SETARR(autostartcmds, autostart);
	/* Mod+Ctrl+N sets the column width to the Nth preset
	 * (not Mod+Shift+N: that already sends the window to workspace N) */
	SETKEYS(widthpresets, MODKEY|ControlMask, XK_1, setwidth,
	        { .f = widthpresets[i] });
	/* Mod+N views workspace N, Mod+Shift+N sends the focused window there */
	for (i = 0; i < nworkspaces; i++) {
		arrput(keys, ((Key){ MODKEY, XK_0 + i, view, { .i = (int)i } }));
		arrput(keys, ((Key){ MODKEY|ShiftMask, XK_0 + i, sendto, { .i = (int)i } }));
	}
}

