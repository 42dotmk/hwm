# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

hwm is a scrollable-column tiling window manager for X11 in the suckless style: niri's layout model, dwm's construction. C11, Xlib is the only dependency, configured by editing `config.h` and recompiling. No decorations except borders, no bars.

## Build and run

- `make` — builds `hwm`. Strict flags (`-std=c11 -pedantic -Wall -Wextra`); keep the build warning-free.
- `make check` — builds and runs `test_layout`, the headless tests of the layout core (no X needed). Add cases there whenever the model or a command changes; the expected pixel values are easy to derive from the 1000x600 test monitor, gap 6, border 2.
- `make install` — symlinks the binary into `~/.local/bin` (no sudo; the symlink means a rebuild is enough).
- There is no linter. Verify shell-side changes (anything in `hwm.c`) by running under a nested X server:

      Xephyr -screen 1280x720 :1 &
      DISPLAY=:1 ./hwm &
      DISPLAY=:1 xterm &

  Headless-ish driving: hwm answers EWMH client messages (e.g. `_NET_CURRENT_DESKTOP` to switch workspaces), and `xwininfo`/`xprop`/`xwd` work against the Xephyr display. `xdotool`/`wmctrl`/Xvfb are not installed on this machine.

**Live restart:** a running hwm watches its own binary (`checkself()`) and re-execs itself a couple of seconds after `make` replaces it. If you rebuild while the user's session runs this WM, the new code goes live immediately — windows are re-adopted but the arrangement resets. Mod+Shift+r forces a restart.

## Architecture

Two layers, strictly separated (the same split as hterm's `term.c` / `main.c`):

- **`layout.c` / `layout.h`** — the layout core: the Workspace/Column/Client model, monitors, scrolling and its animation, placement memory, and every layout command (`focushorz` … `movewsmon`). It has **no X11 dependency** (only libc) — that is what makes `test_layout.c` possible. It talks to the window system only through the six callbacks in `LayoutOps` (`apply` geometry+border, `raise`, `focus`, `desktop`, `warp`, `now`), installed by `layoutinit()`. `Client.win` is an opaque `unsigned long` handle. The model globals (`wss`, `curws`, `clients`, `mons`) are shared with the shell, dwm-style. Keep it that way: anything Xlib-flavored belongs in `hwm.c`.
- **`hwm.c`** — the X11 shell: window adoption (`adopt()` reads WM_CLASS, window type, transient/fixed-size hints, then calls the core's `manage()`), the `LayoutOps` implementations, EWMH properties, RandR → `setmons()`, key/button grabs and the drag loop, libinput gestures, the self-watch, and the event loop. Shell-only commands (`killclient`, `spawn`, `quit`, `restart`, `dragscroll`, `dragwidth`) live here.
- `config.h` (user configuration, included by `hwm.c`) and `hwm.h` (Key/Button types, shell commands, shell config declarations; includes `layout.h`, which declares the config the core reads). `vendor/stb_ds.h` supplies dynamic arrays (`arrput`/`arrdel`/`arrlen`); its implementation is compiled into `layout.c` with `erealloc` as allocator.
- `test_layout.c` — a fake shell (records applied geometry per window, counts focus/raise/warp calls, fake clock) plus `CHECK()` assertions, hterm-style. It defines the core's config globals itself, so it never includes `config.h`.

Data model (all stb_ds arrays): `Workspace` → `cols` (Columns, left→right) + `floats`; `Column` → `clients` (top→bottom). Windows live in columns on an infinite horizontal strip; the screen is a viewport with per-workspace `scroll` offset. Opening a window never resizes others — each new window gets its own column. `arrangews()` is the single layout function: it recomputes all geometry for one workspace from this model.

Monitors (`mons`, from XRandR via the shell's `updatemons()` → core `setmons()`, sorted left→right): each `Workspace` has a `mon`, each `Monitor` shows one workspace (`ws`). The active monitor is the one under the pointer — `syncactivemon()` re-targets `curws` on every key/button press, and cross-monitor `view`/`movewsmon` (Mod+comma/period) warp the pointer to keep that model consistent. Attach: a new monitor takes over the first hidden workspace. Detach: its workspaces move to the active monitor. hwm only *reads* the arrangement; enabling an output is done with the `xrandr` CLI.

Non-obvious mechanics:

- **Placement memory** (`preservelayout`): `manage()` → `place()` looks the window's WM_CLASS class up in `layoutfile` (`app:workspace:column:percent` lines) and tiles it there, switching to that workspace unless adopting at startup; every command that moves/reorders/resizes a tiled window ends with `savelayout()`, which rewrites the file atomically (temp + rename). The file is re-read on every lookup and save, so there is no in-memory copy and hand edits apply immediately. An `app:::[percent]` line (ws = -1 after parsing) opts the app out: it opens at the current selection and `savelayout()` leaves the line alone.
- **Hidden workspaces are not unmapped** — `arrangews()` parks them offscreen, three screen widths to the left (`parkoff()`). Unmapping would generate UnmapNotify events indistinguishable from windows closing themselves (`unmapnotify()` → `unmanage()`).
- **Event loop** (`run()`): drains X events, then `select()`s on the X fd (and libinput's) with a timeout. The timeout drives the binary self-watch and scroll animation frames (`animstep()`); no other timers exist. It flushes Xlib every iteration so the last animation frame never waits for an event.
- **Floating**: dialogs, notifications, utility/menu/splash/toolbar types, transients, and fixed-size windows float above the strip (`Client.col == NULL`). Notifications never steal focus.
- **Click-to-focus** works by sync-grabbing the first button press on unfocused windows and replaying it (`grabbuttons()`/`buttonpress()`).
- **X errors** from vanished windows are deliberately swallowed in `xerror()`, dwm-style.
- Just enough EWMH is advertised for rofi/pagers: `_NET_CLIENT_LIST`, `_NET_ACTIVE_WINDOW`, `_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP`.

Adding a user-facing command: implement `void name(const Arg *arg)` in `layout.c` if it only touches the model (declare it in `layout.h`, add a test), or in `hwm.c` if it needs X (declare it in `hwm.h`); either way a one-line comment at the declaration, then bind it in `config.h`. The key/button/width/autostart tables are stb_ds arrays assembled in `initconfig()` (which must run before `setup()`), so bindings can be generated in loops — the per-workspace keys are.

## Style

dwm-style C11 formatted by clang-format via the repo's `.clang-format` (shared across the siblings: 4-space indent, attached braces, 80 columns — run `clang-format -i` on files you touch): short lowercase function names, forward declarations and the `handler[]` event-function table at the top of `hwm.c`, `/* comments */` only where the code can't say it. Config values are `const` globals defined in `config.h` and declared in `hwm.h` (shell) or `layout.h` (core).
