# hwm

A scrollable-column tiling window manager for X11, in the suckless style.
Niri's layout model, dwm's construction: ~900 lines of C11, Xlib as the only
dependency, configured by editing `config.h` and recompiling. No decorations
except borders, no menus, no bars.

## Layout model

Windows live in columns on an infinite horizontal strip. The screen is a
viewport that scrolls over the strip, so opening a window never resizes the
ones you already have. Each new window opens in its own column to the right
of the focused one. A column can hold several windows stacked vertically
(move a window into position with Mod+Shift+direction). There are 9
independent workspaces, each with its own strip.

Gaps are decoration, not layout: columns and cells tile edge to edge, and
every window is simply inset by `gappx` inside its cell (set in `config.h`).

Not everything tiles: dialogs, notifications, splash screens, utility and
menu windows, transient windows, and fixed-size windows float above the
strip at the position and size they ask for (centered if they don't ask
for one). Notifications never steal focus. Mod+Shift+Space toggles the
focused window between floating and tiled; a float can be moved with
Mod+left-drag and resized with Mod+right-drag.

## Build

    make            # needs libX11 headers (libX11-devel)
    sudo make install

Run it from `~/.xinitrc`:

    exec hwm

To try it without leaving your current session, install Xephyr and run a
nested server:

    Xephyr -screen 1280x720 :1 &
    DISPLAY=:1 ./hwm &
    DISPLAY=:1 xterm &

## Default bindings (Mod = Super)

| Binding                    | Action                                     |
|----------------------------|--------------------------------------------|
| Mod+Return                 | spawn terminal                             |
| Mod+Space                  | app launcher (rofi)                        |
| Mod+Tab                    | workspace/window overview (hws)            |
| Mod+q                      | close window                               |
| Mod+Shift+e                | quit hwm                                   |
| Mod+Shift+r                | restart hwm in place                       |
| Mod+h/l (or arrows)        | focus column left/right, auto-scrolls      |
| Mod+j/k                    | focus window down/up within the column     |
| Mod+Shift+h/l              | move window: alone → swap columns; stacked → split into its own column |
| Mod+Shift+j/k              | move window down/up within the column      |
| Mod+Ctrl+h/l               | stack window into the adjacent column      |
| Mod+r                      | cycle column width (1/3, 1/2, 2/3, 1)      |
| Mod+Ctrl+1..4              | set column width to the Nth preset         |
| Mod+minus / Mod+equal      | shrink / grow column width                 |
| Mod+f                      | toggle fullscreen                          |
| Mod+Shift+Space            | toggle floating for the focused window     |
| Mod+[ / Mod+]              | scroll the strip left/right                |
| Mod+1..9                   | switch workspace                           |
| Mod+Shift+1..9             | send window to workspace                   |
| Mod+left-drag              | scroll the strip; on a float: move it      |
| Mod+right-drag             | resize the focused column or float         |
| Mod+scroll wheel           | scroll the strip                           |
| click                      | focus window (focus also follows mouse)    |

## Configuration

Everything lives in `config.h`: colors, border width, gap size,
width presets, workspace count, keys, mouse buttons, and autostart
commands. Edit and `make`. The key, button, width, and autostart tables
are stb_ds arrays assembled in `initconfig()`, so bindings can also be
generated with plain loops — the per-workspace keys are.

hwm watches its own binary and restarts in place when it changes, so a
plain `make` (or `sudo make install`, if you run the installed copy)
applies the new configuration within a couple of seconds. Windows survive
a restart — hwm re-adopts them on startup — but the arrangement resets:
every window comes back in its own column on workspace 1. Mod+Shift+r
forces a restart without a rebuild.

`autostart` in `config.h` is a list of shell commands run when hwm starts,
including after every restart — keep them idempotent or guard them
(e.g. `pgrep -x app || app`).
