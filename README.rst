x11-datetime-overlay
====================

Tiny, always-on-top, top-right datetime overlay for X11, written in pure C
using XCB for windowing and Cairo (xcb backend) for text rendering. Designed
for ultra-low CPU/GPU usage: it only redraws once per second and uses a
click-through override-redirect window, so it never steals focus.

Features
--------
- Always-on-top, on all workspaces (EWMH hints + override-redirect).
- Stays visible even with fullscreen windows in i3 and many WMs.
- Top-right anchored; auto-sizes to the text and repositions each second.
- Click-through (no input), so it never interferes with your workflow.
- Configurable font family, font size (px), foreground and background colors.
- **New:** ``--time-only`` to show ``HH:MM:SS`` (no date).
- **New:** ``--debug`` adds verbose diagnostics to stderr.
- **New:** ``--flash MIN`` inverts colors when ``minute % MIN == 0`` and
  ``second == 0``, then smoothly fades back to normal over 30 seconds with
  50ms color updates (no startup timer; boundary-aligned). During a flash,
  the background fades and the foreground is always the inverse of the
  current background for maximum contrast.
- **New:** ``--show-flash-count`` appends ``(N)`` with the number of flashes
  since program start (only shown when ``N > 0``).

Build
-----
You need the development packages for XCB, XCB-Shape, and Cairo:

Debian/Ubuntu::

  sudo apt install build-essential meson ninja-build pkg-config \
       libxcb1-dev libxcb-shape0-dev libcairo2-dev

Fedora::

  sudo dnf install meson ninja-build pkgconf-pkg-config \
       libxcb-devel xcb-util-wm-devel cairo-devel

Then build and run::

  meson setup build
  meson compile -C build
  ./build/x11-datetime-overlay --time-only --font "DejaVu Sans Mono" \
    --size 18 --fg #EAEAEA --bg #101010 --margin 10 --flash 1 \
    --show-flash-count

Usage
-----
::

  x11-datetime-overlay [--font FAMILY] [--size PX] [--fg #RRGGBB] [--bg #RRGGBB] [--margin PX] [--time-only] [--flash MIN] [--show-flash-count] [--debug]
  x11-datetime-overlay -h | --help

Options:
  -f, --font FAMILY     Font family name (default: "DejaVu Sans Mono").
  -s, --size PX         Font size in pixels (default: 16).
      --fg  #RRGGBB     Foreground/text color (default: #FFFFFF).
      --bg  #RRGGBB     Background color (default: #000000).
  -m, --margin PX       Outer margin (default: 8).
  -t, --time-only       Show only time (HH:MM:SS), omit the date.
  -F, --flash MIN       Boundary-aligned flash: triggers at each minute where
                        (minute % MIN == 0) at second 00. Disabled if 0.
  -c, --show-flash-count
                        Append "(N)" with total flashes since start (N>0).
  -d, --debug           Verbose debug logs to stderr (window geometry, events).
  -h, --help            Show help.

Notes on Window Behavior
------------------------
- The program creates an override-redirect window. This bypasses the window
  manager so the overlay is not reparented, is placed above managed windows,
  and appears on all workspaces. EWMH hints (``_NET_WM_STATE_ABOVE``,
  ``_NET_WM_STATE_STICKY``, ``_NET_WM_WINDOW_TYPE_DOCK``, and
  ``_NET_WM_DESKTOP=0xFFFFFFFF``) are set as best-effort hints to maximize
  compatibility across WMs.

- On i3 (and many tiling WMs), override-redirect windows typically remain
  visible even when another window is fullscreen. Some compositors or WM
  configurations may still choose to cover such windows; if so, ensure no
  rules explicitly block override-redirect/dock windows or force them below.

Performance
-----------
The program wakes up only once per second, measures and repaints the single
line of text, and uses Cairo's xcb backend for efficient text rendering.
During a flash fade, colors update every 50ms for smoothness. Memory
footprint is minimal; CPU/GPU usage stays low.
