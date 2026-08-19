# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

hwm is a scrollable-column tiling window manager for X11 in the suckless style: niri's layout model, dwm's construction. C11, Xlib is the only dependency, configured by editing `config.h` and recompiling. No decorations except borders, no bars.

## Build and run

- `make` — builds `hwm`. Strict flags (`-std=c11 -pedantic -Wall -Wextra`); keep the build warning-free.
- `make install` — symlinks the binary into `~/.local/bin` (no sudo; the symlink means a rebuild is enough).
- There is no test suite or linter. Verify changes by running under a nested X server:

      Xephyr -screen 1280x720 :1 &
      DISPLAY=:1 ./hwm &
      DISPLAY=:1 xterm &

  Headless-ish driving: hwm answers EWMH client messages (e.g. `_NET_CURRENT_DESKTOP` to switch workspaces), and `xwininfo`/`xprop`/`xwd` work against the Xephyr display. `xdotool`/`wmctrl`/Xvfb are not installed on this machine.

**Live restart:** a running hwm watches its own binary (`checkself()`) and re-execs itself a couple of seconds after `make` replaces it. If you rebuild while the user's session runs this WM, the new code goes live immediately — windows are re-adopted but the arrangement resets. Mod+Shift+r forces a restart.

## Architecture

Three files: `hwm.c` (the entire WM), `config.h` (user configuration, included by `hwm.c` — one translation unit), `hwm.h` (shared types, bindable command declarations, config declarations). `vendor/stb_ds.h` supplies dynamic arrays (`arrput`/`arrdel`/`arrlen`); its implementation is compiled into `hwm.c` with `erealloc` as allocator.

Data model (all stb_ds arrays): `Workspace` → `cols` (Columns, left→right) + `floats`; `Column` → `clients` (top→bottom). Windows live in columns on an infinite horizontal strip; the screen is a viewport with per-workspace `scroll` offset. Opening a window never resizes others — each new window gets its own column. `arrangews()` is the single layout function: it recomputes all geometry for one workspace from this model.

Monitors (`mons`, from XRandR via `updatemons()`, sorted left→right): each `Workspace` has a `mon`, each `Monitor` shows one workspace (`ws`). The active monitor is the one under the pointer — `syncactivemon()` re-targets `curws` on every key/button press, and cross-monitor `view`/`movewsmon` (Mod+comma/period) warp the pointer to keep that model consistent. Attach: a new monitor takes over the first hidden workspace. Detach: its workspaces move to the active monitor. hwm only *reads* the arrangement; enabling an output is done with the `xrandr` CLI.

Non-obvious mechanics:

- **Hidden workspaces are not unmapped** — `arrangews()` parks them offscreen at `x - 3*sw`. Unmapping would generate UnmapNotify events indistinguishable from windows closing themselves (`unmapnotify()` → `unmanage()`).
- **Event loop** (`run()`): drains X events, then `select()`s on the X fd with a timeout. The timeout drives the binary self-watch and the workspace-indicator auto-hide; no other timers exist.
- **Floating**: dialogs, notifications, utility/menu/splash/toolbar types, transients, and fixed-size windows float above the strip (`Client.col == NULL`). Notifications never steal focus.
- **Click-to-focus** works by sync-grabbing the first button press on unfocused windows and replaying it (`grabbuttons()`/`buttonpress()`).
- **X errors** from vanished windows are deliberately swallowed in `xerror()`, dwm-style.
- Just enough EWMH is advertised for rofi/pagers: `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`, `_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP`.

Adding a user-facing command: implement `void name(const Arg *arg)` in `hwm.c`, declare it in `hwm.h` with a one-line comment, bind it in `config.h`. The key/button/width/autostart tables are stb_ds arrays assembled in `initconfig()` (which must run before `setup()`), so bindings can be generated in loops — the per-workspace keys are.

## Style

dwm-style C11: tabs, K&R braces, short lowercase function names, forward declarations and the `handler[]` event-function table at the top of `hwm.c`, `/* comments */` only where the code can't say it. Config values are `const` globals defined in `config.h` and declared in `hwm.h`.
