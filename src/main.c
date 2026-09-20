// x11-datetime-overlay: Always-on-top top-right datetime overlay
// Pure C using XCB + cairo (xcb backend). Minimal CPU/GPU footprint.
// Builds with meson and gcc.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shape.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>

typedef struct {
  const char *font_family;
  double font_size_px;
  uint32_t margin_px;
  double fg_r, fg_g, fg_b;
  double bg_r, bg_g, bg_b;
  bool time_only;
  bool debug;
} options_t;

static void print_help(const char *prog) {
  fprintf(stdout,
    "x11-datetime-overlay - tiny always-on-top datetime overlay (XCB)\n"
    "\n"
    "Usage:\n"
    "  %s [--font FAMILY] [--size PX] [--fg #RRGGBB] [--bg #RRGGBB] [--margin PX] [--time-only] [--debug]\n"
    "  %s -h | --help\n"
    "\n"
    "Options:\n"
    "  -f, --font FAMILY     Font family name (default: \"DejaVu Sans Mono\").\n"
    "  -s, --size PX         Font size in pixels (default: 16).\n"
    "      --fg  #RRGGBB     Foreground/text color (default: #FFFFFF).\n"
    "      --bg  #RRGGBB     Background color (default: #000000).\n"
    "  -m, --margin PX       Outer margin from screen edges in pixels (default: 8).\n"
    "  -t, --time-only       Show only time (HH:MM:SS), omit the date.\n"
    "  -d, --debug           Verbose debug logs to stderr.\n"
    "  -h, --help            Show this help and exit.\n"
    "\n"
    "Example:\n"
    "  %s --time-only --font \"DejaVu Sans Mono\" --size 18 --fg #EAEAEA --bg #101010 --margin 10\n",
    prog, prog, prog
  );
}

static int parse_hex2(const char *p) {
  int v = 0;
  for (int i = 0; i < 2; ++i) {
    char c = p[i];
    if (!isxdigit((unsigned char)c)) return -1;
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (c - '0');
    else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
  }
  return v;
}

static bool parse_rgb_hex(const char *hex, double *r, double *g, double *b) {
  // Accept "#RRGGBB" or "RRGGBB"
  const char *p = hex;
  if (hex[0] == '#') p++;
  if (strlen(p) != 6) return false;
  int rr = parse_hex2(p);
  int gg = parse_hex2(p+2);
  int bb = parse_hex2(p+4);
  if (rr < 0 || gg < 0 || bb < 0) return false;
  *r = rr / 255.0;
  *g = gg / 255.0;
  *b = bb / 255.0;
  return true;
}

static xcb_visualtype_t *get_visualtype_for_screen(const xcb_setup_t *setup, xcb_screen_t *screen) {
  xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
  for (; di.rem; xcb_depth_next(&di)) {
    xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
    for (; vi.rem; xcb_visualtype_next(&vi)) {
      if (vi.data->visual_id == screen->root_visual) {
        return vi.data;
      }
    }
  }
  return NULL;
}

static void intern_atom(xcb_connection_t *c, const char *name, xcb_atom_t *out) {
  xcb_intern_atom_cookie_t ck = xcb_intern_atom(c, 0, strlen(name), name);
  xcb_intern_atom_reply_t *rp = xcb_intern_atom_reply(c, ck, NULL);
  if (rp) {
    *out = rp->atom;
    free(rp);
  } else {
    *out = XCB_ATOM_NONE;
  }
}

static const char *event_name(uint8_t rt) {
  switch (rt) {
    case XCB_EXPOSE: return "Expose";
    case XCB_VISIBILITY_NOTIFY: return "VisibilityNotify";
    case XCB_CONFIGURE_NOTIFY: return "ConfigureNotify";
    default: return "Other";
  }
}

static void set_evmh_hints(xcb_connection_t *c, xcb_window_t win) {
  // Best-effort EWMH hints: set type DOCK, stick to all desktops, keep ABOVE+STICKY
  xcb_atom_t _NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_DOCK;
  xcb_atom_t _NET_WM_STATE, _NET_WM_STATE_ABOVE, _NET_WM_STATE_STICKY;
  xcb_atom_t _NET_WM_DESKTOP;

  intern_atom(c, "_NET_WM_WINDOW_TYPE", & _NET_WM_WINDOW_TYPE);
  intern_atom(c, "_NET_WM_WINDOW_TYPE_DOCK", & _NET_WM_WINDOW_TYPE_DOCK);
  intern_atom(c, "_NET_WM_STATE", & _NET_WM_STATE);
  intern_atom(c, "_NET_WM_STATE_ABOVE", & _NET_WM_STATE_ABOVE);
  intern_atom(c, "_NET_WM_STATE_STICKY", & _NET_WM_STATE_STICKY);
  intern_atom(c, "_NET_WM_DESKTOP", & _NET_WM_DESKTOP);

  if (_NET_WM_WINDOW_TYPE != XCB_ATOM_NONE && _NET_WM_WINDOW_TYPE_DOCK != XCB_ATOM_NONE) {
    xcb_change_property(
      c, XCB_PROP_MODE_REPLACE, win,
      _NET_WM_WINDOW_TYPE, XCB_ATOM_ATOM, 32, 1, & _NET_WM_WINDOW_TYPE_DOCK
    );
  }

  if (_NET_WM_STATE != XCB_ATOM_NONE) {
    xcb_atom_t states[2];
    int n = 0;
    if (_NET_WM_STATE_STICKY != XCB_ATOM_NONE) states[n++] = _NET_WM_STATE_STICKY;
    if (_NET_WM_STATE_ABOVE  != XCB_ATOM_NONE) states[n++] = _NET_WM_STATE_ABOVE;
    if (n > 0) {
      xcb_change_property(
        c, XCB_PROP_MODE_REPLACE, win,
        _NET_WM_STATE, XCB_ATOM_ATOM, 32, n, states
      );
    }
  }

  if (_NET_WM_DESKTOP != XCB_ATOM_NONE) {
    // 0xFFFFFFFF means all desktops/workspaces per EWMH
    uint32_t all = 0xFFFFFFFFu;
    xcb_change_property(
      c, XCB_PROP_MODE_REPLACE, win,
      _NET_WM_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, &all
    );
  }
}

static void now_timestr(char *buf, size_t bufsz, bool time_only) {
  time_t t = time(NULL);
  struct tm lt;
  localtime_r(&t, &lt);
  const char *fmt = time_only ? "%H:%M:%S" : "%Y-%m-%d %H:%M:%S";
  strftime(buf, bufsz, fmt, &lt);
}

static long ms_to_next_second(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long ms = ts.tv_nsec / 1000000L;
  long rem = 1000L - (ms % 1000L);
  return (rem == 1000L) ? 0L : rem;
}

int main(int argc, char **argv) {
  options_t opt = {
    .font_family = "DejaVu Sans Mono",
    .font_size_px = 16.0,
    .margin_px = 8,
    .fg_r = 1.0, .fg_g = 1.0, .fg_b = 1.0,
    .bg_r = 0.0, .bg_g = 0.0, .bg_b = 0.0,
    .time_only = false,
    .debug = false
  };

  static struct option long_opts[] = {
    {"help",      no_argument,       0, 'h'},
    {"font",      required_argument, 0, 'f'},
    {"size",      required_argument, 0, 's'},
    {"fg",        required_argument, 0,  1  },
    {"bg",        required_argument, 0,  2  },
    {"margin",    required_argument, 0, 'm'},
    {"time-only", no_argument,       0, 't'},
    {"debug",     no_argument,       0, 'd'},
    {0,0,0,0}
  };

  int c, idx;
  while ((c = getopt_long(argc, argv, "hf:s:m:td", long_opts, &idx)) != -1) {
    switch (c) {
      case 'h': print_help(argv[0]); return 0;
      case 'f': opt.font_family = optarg; break;
      case 's': opt.font_size_px = strtod(optarg, NULL); if (opt.font_size_px <= 0) opt.font_size_px = 16.0; break;
      case 'm': opt.margin_px = (uint32_t)strtoul(optarg, NULL, 10); break;
      case 't': opt.time_only = true; break;
      case 'd': opt.debug = true; break;
      case 1:
        if (!parse_rgb_hex(optarg, &opt.fg_r, &opt.fg_g, &opt.fg_b)) {
          fprintf(stderr, "Invalid --fg color, use #RRGGBB\n"); return 2;
        }
        break;
      case 2:
        if (!parse_rgb_hex(optarg, &opt.bg_r, &opt.bg_g, &opt.bg_b)) {
          fprintf(stderr, "Invalid --bg color, use #RRGGBB\n"); return 2;
        }
        break;
      default:  print_help(argv[0]); return 2;
    }
  }

  if (opt.debug) {
    fprintf(stderr, "[debug] opts: font=\"%s\" size=%.1f margin=%u time_only=%d fg=%.3f,%.3f,%.3f bg=%.3f,%.3f,%.3f\n",
            opt.font_family, opt.font_size_px, opt.margin_px, opt.time_only,
            opt.fg_r, opt.fg_g, opt.fg_b, opt.bg_r, opt.bg_g, opt.bg_b);
  }

  int screen_num = 0;
  xcb_connection_t *cconn = xcb_connect(NULL, &screen_num);
  if (!cconn || xcb_connection_has_error(cconn)) {
    fprintf(stderr, "Failed to connect to X server\n");
    return 1;
  }

  const xcb_setup_t *setup = xcb_get_setup(cconn);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
  for (int s = 0; s < screen_num; ++s) xcb_screen_next(&iter);
  xcb_screen_t *screen = iter.data;
  if (!screen) {
    fprintf(stderr, "Could not get default screen\n");
    xcb_disconnect(cconn);
    return 1;
  }
  if (opt.debug) {
    fprintf(stderr, "[debug] screen: %ux%u, screen_num=%d\n",
            screen->width_in_pixels, screen->height_in_pixels, screen_num);
  }

  xcb_visualtype_t *visual = get_visualtype_for_screen(setup, screen);
  if (!visual) {
    fprintf(stderr, "Could not find visual for screen\n");
    xcb_disconnect(cconn);
    return 1;
  }

  // Create override-redirect window so the WM doesn't manage it and it stays above.
  xcb_window_t win = xcb_generate_id(cconn);
  uint32_t cw_values[2];
  uint32_t cw_mask = XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
  cw_values[0] = 1; // override-redirect = true
  cw_values[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_VISIBILITY_CHANGE;

  // Initial tiny size; will be resized after measuring text.
  uint16_t w = 64, h = 24;
  xcb_create_window(
    cconn,
    XCB_COPY_FROM_PARENT,              // depth
    win,
    screen->root,
    screen->width_in_pixels - w - opt.margin_px, // x
    opt.margin_px,                              // y
    w, h,
    0,                                // border
    XCB_WINDOW_CLASS_INPUT_OUTPUT,
    screen->root_visual,
    cw_mask, cw_values
  );
  if (opt.debug) {
    fprintf(stderr, "[debug] created window id=0x%08x\n", win);
  }

  // Make the window click-through (no input), so it never steals focus.
  xcb_shape_rectangles(
    cconn,
    XCB_SHAPE_SO_SET,
    XCB_SHAPE_SK_INPUT,
    XCB_CLIP_ORDERING_UNSORTED,
    win,
    0, 0,
    0,
    NULL
  );

  set_evmh_hints(cconn, win);

  // Map and raise
  xcb_map_window(cconn, win);
  uint32_t cfg_vals[1] = { XCB_STACK_MODE_ABOVE };
  xcb_configure_window(cconn, win, XCB_CONFIG_WINDOW_STACK_MODE, cfg_vals);
  xcb_flush(cconn);

  // Prepare a dummy cairo surface/context for text metrics (offscreen).
  cairo_surface_t *measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *measure_cr = cairo_create(measure_surface);
  cairo_select_font_face(measure_cr, opt.font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(measure_cr, opt.font_size_px);

  // Main loop: poll X for events, and redraw exactly each second.
  int xfd = xcb_get_file_descriptor(cconn);
  char last_str[64] = {0};

  for (;;) {
    long timeout_ms = ms_to_next_second();
    struct pollfd pfd = { .fd = xfd, .events = POLLIN };
    int pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr < 0 && errno == EINTR) continue;

    bool need_redraw = false;

    // Drain events (lightweight; we only care about expose/visibility)
    xcb_generic_event_t *ev;
    while ((ev = xcb_poll_for_event(cconn)) != NULL) {
      uint8_t rt = ev->response_type & ~0x80;
      if (opt.debug) {
        fprintf(stderr, "[debug] event: %s (%u)\n", event_name(rt), rt);
      }
      if (rt == XCB_EXPOSE || rt == XCB_VISIBILITY_NOTIFY || rt == XCB_CONFIGURE_NOTIFY) {
        need_redraw = true;
      }
      free(ev);
    }

    // Tick
    if (pr == 0) {
      need_redraw = true;
    }

    if (need_redraw) {
      char nowbuf[64];
      now_timestr(nowbuf, sizeof(nowbuf), opt.time_only);

      // Only re-measure/reposition if text width changed (we measure every tick; cheap)
      cairo_select_font_face(measure_cr, opt.font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(measure_cr, opt.font_size_px);

      cairo_text_extents_t te;
      cairo_text_extents(measure_cr, nowbuf, &te);

      cairo_font_extents_t fe;
      cairo_font_extents(measure_cr, &fe);

      int text_w = (int)(te.x_advance + 0.5);
      int text_h = (int)(fe.ascent + fe.descent + 0.5);

      uint16_t pad = (uint16_t)(opt.margin_px);
      uint16_t win_w = (uint16_t)(text_w + pad * 2);
      uint16_t win_h = (uint16_t)(text_h + pad * 2);

      int16_t new_x = (int16_t)((int)screen->width_in_pixels - (int)win_w - (int)opt.margin_px);
      int16_t new_y = (int16_t)opt.margin_px;

      uint32_t cfg[5];
      uint16_t mask = 0;
      int cidx = 0;
      mask |= XCB_CONFIG_WINDOW_X;      cfg[cidx++] = (uint32_t)new_x;
      mask |= XCB_CONFIG_WINDOW_Y;      cfg[cidx++] = (uint32_t)new_y;
      mask |= XCB_CONFIG_WINDOW_WIDTH;  cfg[cidx++] = (uint32_t)win_w;
      mask |= XCB_CONFIG_WINDOW_HEIGHT; cfg[cidx++] = (uint32_t)win_h;
      mask |= XCB_CONFIG_WINDOW_STACK_MODE; cfg[cidx++] = XCB_STACK_MODE_ABOVE;
      xcb_configure_window(cconn, win, mask, cfg);

      if (opt.debug) {
        fprintf(stderr, "[debug] tick str=\"%s\" text_w=%d text_h=%d win=%ux%u at (%d,%d)\n",
                nowbuf, text_w, text_h, win_w, win_h, new_x, new_y);
      }

      // Create drawing surface for this window size
      cairo_surface_t *surface = cairo_xcb_surface_create(cconn, win, visual, win_w, win_h);
      cairo_t *cr = cairo_create(surface);

      // Background
      cairo_set_source_rgb(cr, opt.bg_r, opt.bg_g, opt.bg_b);
      cairo_paint(cr);

      // Text
      cairo_select_font_face(cr, opt.font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, opt.font_size_px);
      cairo_set_source_rgb(cr, opt.fg_r, opt.fg_g, opt.fg_b);

      double text_x = pad - te.x_bearing;     // account for left bearing
      double text_y = pad + fe.ascent;        // baseline

      cairo_move_to(cr, text_x, text_y);
      cairo_show_text(cr, nowbuf);
      cairo_surface_flush(surface);
      cairo_destroy(cr);
      cairo_surface_destroy(surface);

      xcb_flush(cconn);
      strncpy(last_str, nowbuf, sizeof(last_str)-1);
      last_str[sizeof(last_str)-1] = '\0';
    }
  }

  // Unreachable in normal usage; kept for completeness
  cairo_destroy(measure_cr);
  cairo_surface_destroy(measure_surface);
  xcb_disconnect(cconn);
  return 0;
}
