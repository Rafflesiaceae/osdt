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
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <sys/inotify.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xcb.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define FLASH_DURATION_SEC 30
#define FLASH_STEP_MS 50

typedef enum {
  IMAGE_SIZE_NATIVE,
  IMAGE_SIZE_EXACT,
  IMAGE_SIZE_PERCENT
} image_size_mode_t;

typedef struct {
  image_size_mode_t mode;
  int width;
  int height;
  double percent;
} image_size_t;

typedef struct {
  const char *font_family;
  double font_size_px;
  uint32_t margin_px;
  double fg_r, fg_g, fg_b;
  double bg_r, bg_g, bg_b;
  const char *text_path;
  bool text_from_stdin;
  const char *image_path;
  const char *image_size_arg;
  const char *anchor;
  bool time_only;
  bool no_datetime;
  bool debug;
  int flash_minutes; // 0 disables
  bool show_flash_count;
} options_t;

typedef struct {
  bool active;
  time_t start;
  long last_boundary_min_epoch; // epoch minutes of last trigger
} flash_state_t;

typedef struct {
  char **lines;
  size_t count;
} text_lines_t;

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
  bool open;
} stdin_text_t;

typedef enum {
  ANCHOR_TOP,
  ANCHOR_TOP_LEFT,
  ANCHOR_TOP_RIGHT,
  ANCHOR_RIGHT,
  ANCHOR_BOTTOM_RIGHT,
  ANCHOR_BOTTOM,
  ANCHOR_BOTTOM_LEFT,
  ANCHOR_LEFT,
  ANCHOR_CENTER,
  ANCHOR_INVALID
} anchor_t;

typedef struct {
  int x;
  int y;
  int w;
  int h;
  bool valid;
} monitor_rect_t;

static void print_help(const char *prog) {
  fprintf(stdout,
    "x11-datetime-overlay - tiny always-on-top datetime overlay (XCB)\n"
    "\n"
    "Usage:\n"
    "  %s [--font FAMILY] [--size PX] [--fg #RRGGBB] [--bg #RRGGBB] [--margin PX] [--anchor POS] [--text FILE] [-img|--image FILE [SIZE]] [--time-only] [--no-datetime] [--flash MIN] [--show-flash-count] [--debug]\n"
    "  %s -h | --help\n"
    "\n"
    "Options:\n"
    "  -f, --font FAMILY     Font family name (default: \"DejaVu Sans Mono\").\n"
    "  -s, --size PX         Font size in pixels (default: 14).\n"
    "      --fg  #RRGGBB     Foreground/text color (default: #FFFFFF).\n"
    "      --bg  #RRGGBB     Background color (default: #000000).\n"
    "  -m, --margin PX       Outer margin from screen edges in pixels (default: 0).\n"
    "  -a, --anchor POS      Anchor position: top, top-left, top-right, right,\n"
    "                       bottom-right, bottom, bottom-left, left, center (default: top-right).\n"
    "  -t, --text FILE       Show normalized file contents below the time; watches for changes.\n"
    "                       Lines starting with '#' are ignored as comments.\n"
    "                       Trailing whitespace and surrounding blank lines are removed.\n"
    "                       Use '-' to read text from stdin; piped stdin is auto-read.\n"
    "                       A zero byte refreshes stdin text with the frame so far.\n"
    "  -img, --image FILE    Draw an image below the text at its native size.\n"
    "      --image-size SIZE Resize it to WIDTHxHEIGHT or a percentage such as 50%%.\n"
    "                       SIZE may instead follow FILE as a positional argument.\n"
    "  -T, --time-only       Show only time (HH:MM:SS), omit the date.\n"
    "      --no-datetime     Do not show the date or time; display only text input.\n"
    "  -F, --flash MIN       Boundary flash at minute %% MIN == 0 (sec==00), fade 30s.\n"
    "  -c, --show-flash-count Append \"(N)\" with total flashes since start (N>0).\n"
    "  -d, --debug           Verbose debug logs to stderr.\n"
    "  -h, --help            Show this help and exit.\n"
    "\n"
    "Example:\n"
    "  %s --no-datetime --image picture.png 50%% --anchor center\n",
    prog, prog, prog
  );
}

static bool parse_positive_int(const char *text, size_t len, int *out) {
  if (len == 0 || len >= 32) return false;

  char buf[32];
  memcpy(buf, text, len);
  buf[len] = '\0';

  char *end = NULL;
  errno = 0;
  unsigned long value = strtoul(buf, &end, 10);
  if (errno != 0 || end == buf || *end != '\0' || value == 0 || value > UINT16_MAX) {
    return false;
  }
  *out = (int)value;
  return true;
}

static bool parse_image_size(const char *arg, image_size_t *size) {
  size_t len = strlen(arg);
  if (len > 1 && arg[len - 1] == '%') {
    char *end = NULL;
    errno = 0;
    double percent = strtod(arg, &end);
    if (errno != 0 || end == arg || end != arg + len - 1 ||
        !isfinite(percent) || percent <= 0.0) {
      return false;
    }
    *size = (image_size_t){ .mode = IMAGE_SIZE_PERCENT, .percent = percent };
    return true;
  }

  // Exact dimensions deliberately allow non-proportional resizing.
  const char *separator = strchr(arg, 'x');
  if (!separator) separator = strchr(arg, 'X');
  int width = 0;
  int height = 0;
  if (!separator || strchr(separator + 1, 'x') || strchr(separator + 1, 'X') ||
      !parse_positive_int(arg, (size_t)(separator - arg), &width) ||
      !parse_positive_int(separator + 1, strlen(separator + 1), &height)) {
    return false;
  }
  *size = (image_size_t){
    .mode = IMAGE_SIZE_EXACT,
    .width = width,
    .height = height
  };
  return true;
}

static cairo_surface_t *load_image(const char *path, int *width, int *height) {
  // Use Cairo's in-process PNG decoder first. Besides being faster for the
  // most common overlay format, it also works when sandboxing prevents an
  // installed GdkPixbuf loader from starting its helper process.
  cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
  if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
    *width = cairo_image_surface_get_width(surface);
    *height = cairo_image_surface_get_height(surface);
    return surface;
  }
  cairo_surface_destroy(surface);

  // GdkPixbuf supplies JPEG and any additional formats for which the system
  // has installed loaders.
  GError *error = NULL;
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &error);
  if (!pixbuf) {
    fprintf(stderr, "Failed to load image \"%s\": %s\n", path,
            error ? error->message : "unknown image error");
    g_clear_error(&error);
    return NULL;
  }

  *width = gdk_pixbuf_get_width(pixbuf);
  *height = gdk_pixbuf_get_height(pixbuf);
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, *width, *height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    fprintf(stderr, "Failed to allocate image surface for \"%s\": %s\n", path,
            cairo_status_to_string(cairo_surface_status(surface)));
    cairo_surface_destroy(surface);
    g_object_unref(pixbuf);
    return NULL;
  }

  const int source_stride = gdk_pixbuf_get_rowstride(pixbuf);
  const int channels = gdk_pixbuf_get_n_channels(pixbuf);
  const bool has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
  const guchar *source = gdk_pixbuf_read_pixels(pixbuf);
  const int target_stride = cairo_image_surface_get_stride(surface);
  unsigned char *target = cairo_image_surface_get_data(surface);

  // Cairo ARGB32 pixels are native-endian, premultiplied 32-bit values.
  for (int y = 0; y < *height; ++y) {
    const guchar *source_row = source + y * source_stride;
    uint32_t *target_row = (uint32_t *)(target + y * target_stride);
    for (int x = 0; x < *width; ++x) {
      const guchar *pixel = source_row + x * channels;
      uint32_t alpha = has_alpha ? pixel[3] : 255;
      uint32_t red = ((uint32_t)pixel[0] * alpha + 127) / 255;
      uint32_t green = ((uint32_t)pixel[1] * alpha + 127) / 255;
      uint32_t blue = ((uint32_t)pixel[2] * alpha + 127) / 255;
      target_row[x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
  }
  cairo_surface_mark_dirty(surface);
  g_object_unref(pixbuf);
  return surface;
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
  (void)setup; // silence unused parameter warning
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

static anchor_t parse_anchor(const char *s) {
  if (!s) return ANCHOR_INVALID;
  if (strcmp(s, "top") == 0) return ANCHOR_TOP;
  if (strcmp(s, "top-left") == 0) return ANCHOR_TOP_LEFT;
  if (strcmp(s, "top-right") == 0) return ANCHOR_TOP_RIGHT;
  if (strcmp(s, "right") == 0) return ANCHOR_RIGHT;
  if (strcmp(s, "bottom-right") == 0) return ANCHOR_BOTTOM_RIGHT;
  if (strcmp(s, "bottom") == 0) return ANCHOR_BOTTOM;
  if (strcmp(s, "bottom-left") == 0) return ANCHOR_BOTTOM_LEFT;
  if (strcmp(s, "left") == 0) return ANCHOR_LEFT;
  if (strcmp(s, "center") == 0) return ANCHOR_CENTER;
  return ANCHOR_INVALID;
}

static void anchor_position(anchor_t anchor, int origin_x, int origin_y,
                            int screen_w, int screen_h,
                            uint16_t win_w, uint16_t win_h, uint32_t margin_px,
                            int16_t *out_x, int16_t *out_y) {
  int x = 0;
  int y = 0;
  switch (anchor) {
    case ANCHOR_TOP:
      x = (screen_w - (int)win_w) / 2;
      y = (int)margin_px;
      break;
    case ANCHOR_TOP_LEFT:
      x = (int)margin_px;
      y = (int)margin_px;
      break;
    case ANCHOR_RIGHT:
      x = screen_w - (int)win_w - (int)margin_px;
      y = (screen_h - (int)win_h) / 2;
      break;
    case ANCHOR_BOTTOM_RIGHT:
      x = screen_w - (int)win_w - (int)margin_px;
      y = screen_h - (int)win_h - (int)margin_px;
      break;
    case ANCHOR_BOTTOM:
      x = (screen_w - (int)win_w) / 2;
      y = screen_h - (int)win_h - (int)margin_px;
      break;
    case ANCHOR_BOTTOM_LEFT:
      x = (int)margin_px;
      y = screen_h - (int)win_h - (int)margin_px;
      break;
    case ANCHOR_LEFT:
      x = (int)margin_px;
      y = (screen_h - (int)win_h) / 2;
      break;
    case ANCHOR_CENTER:
      // Center ignores the edge margin because it is not anchored to a screen edge.
      x = (screen_w - (int)win_w) / 2;
      y = (screen_h - (int)win_h) / 2;
      break;
    case ANCHOR_TOP_RIGHT:
    default:
      x = screen_w - (int)win_w - (int)margin_px;
      y = (int)margin_px;
      break;
  }
  *out_x = (int16_t)(origin_x + x);
  *out_y = (int16_t)(origin_y + y);
}

static void free_text_lines(text_lines_t *t) {
  if (!t) return;
  for (size_t i = 0; i < t->count; ++i) {
    free(t->lines[i]);
  }
  free(t->lines);
  t->lines = NULL;
  t->count = 0;
}

static bool append_text_line(text_lines_t *t, const char *line, size_t len) {
  char *copy = (char *)malloc(len + 1);
  if (!copy) {
    return false;
  }
  memcpy(copy, line, len);
  copy[len] = '\0';

  char **new_lines = (char **)realloc(t->lines, sizeof(char *) * (t->count + 1));
  if (!new_lines) {
    free(copy);
    return false;
  }

  t->lines = new_lines;
  t->lines[t->count++] = copy;
  return true;
}

static bool load_text_lines_buffer(const char *buf, size_t len, text_lines_t *out) {
  text_lines_t tmp = {0};
  size_t start = 0;

  // Convert an in-memory stdin frame to the same line model used by file
  // input, trimming CRLF/LF terminators before rendering.
  while (start < len) {
    size_t end = start;
    while (end < len && buf[end] != '\n') {
      end++;
    }

    size_t line_len = end - start;
    while (line_len > 0 && buf[start + line_len - 1] == '\r') {
      line_len--;
    }

    if (!append_text_line(&tmp, buf + start, line_len)) {
      free_text_lines(&tmp);
      free_text_lines(out);
      return false;
    }

    start = (end < len) ? end + 1 : end;
  }

  free_text_lines(out);
  *out = tmp;
  return true;
}

static bool load_text_lines_stream(FILE *f, text_lines_t *out) {
  text_lines_t tmp = {0};
  char *line = NULL;
  size_t cap = 0;
  ssize_t nread;

  // Normalize file input while storing it as independent overlay rows.
  while ((nread = getline(&line, &cap, f)) != -1) {
    size_t len = (size_t)nread;
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
      len--;
    }

    // A leading hash marks a whole-line comment in text files.
    if (len > 0 && line[0] == '#') {
      continue;
    }

    // Do not retain blank rows before the first visible line.
    if (len == 0 && tmp.count == 0) {
      continue;
    }

    if (!append_text_line(&tmp, line, len)) {
      free(line);
      free_text_lines(&tmp);
      free_text_lines(out);
      return false;
    }
  }
  free(line);

  // Blank rows within content are meaningful, but trailing ones are not.
  while (tmp.count > 0 && tmp.lines[tmp.count - 1][0] == '\0') {
    free(tmp.lines[tmp.count - 1]);
    tmp.count--;
  }

  free_text_lines(out);
  *out = tmp;
  return true;
}

static bool load_text_lines(const char *path, text_lines_t *out) {
  FILE *f = fopen(path, "r");
  if (!f) {
    free_text_lines(out);
    return false;
  }

  bool ok = load_text_lines_stream(f, out);
  fclose(f);
  return ok;
}

static void free_stdin_text(stdin_text_t *s) {
  if (!s) return;
  free(s->buf);
  s->buf = NULL;
  s->len = 0;
  s->cap = 0;
  s->open = false;
}

static bool append_stdin_text(stdin_text_t *s, const char *buf, size_t len) {
  if (len == 0) {
    return true;
  }
  if (s->len > SIZE_MAX - len) {
    errno = ENOMEM;
    return false;
  }

  size_t need = s->len + len;
  if (need > s->cap) {
    size_t new_cap = s->cap ? s->cap : 4096;
    while (new_cap < need) {
      if (new_cap > SIZE_MAX / 2) {
        new_cap = need;
        break;
      }
      new_cap *= 2;
    }

    char *new_buf = (char *)realloc(s->buf, new_cap);
    if (!new_buf) {
      return false;
    }
    s->buf = new_buf;
    s->cap = new_cap;
  }

  memcpy(s->buf + s->len, buf, len);
  s->len += len;
  return true;
}

static bool publish_stdin_text(stdin_text_t *s, text_lines_t *out) {
  bool ok = load_text_lines_buffer(s->buf, s->len, out);
  s->len = 0;
  return ok;
}

static bool process_stdin_text(stdin_text_t *s, const char *buf, size_t len,
                               text_lines_t *out, bool *updated) {
  size_t start = 0;

  // A zero byte is a frame boundary: render everything received since the
  // previous boundary, then collect the next update from a fresh buffer.
  for (size_t i = 0; i < len; ++i) {
    if (buf[i] != '\0') {
      continue;
    }
    if (!append_stdin_text(s, buf + start, i - start)) {
      return false;
    }
    if (!publish_stdin_text(s, out)) {
      return false;
    }
    *updated = true;
    start = i + 1;
  }

  return append_stdin_text(s, buf + start, len - start);
}

static bool drain_stdin_text(stdin_text_t *s, text_lines_t *out, bool *updated) {
  char buf[4096];

  while (s->open) {
    ssize_t nread = read(STDIN_FILENO, buf, sizeof(buf));
    if (nread > 0) {
      if (!process_stdin_text(s, buf, (size_t)nread, out, updated)) {
        return false;
      }
      continue;
    }
    if (nread == 0) {
      s->open = false;
      if (s->len > 0) {
        if (!publish_stdin_text(s, out)) {
          return false;
        }
        *updated = true;
      }
      return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }

  return true;
}

static bool get_pointer_xy(xcb_connection_t *c, xcb_window_t root, int *x, int *y) {
  xcb_query_pointer_cookie_t ck = xcb_query_pointer(c, root);
  xcb_query_pointer_reply_t *rp = xcb_query_pointer_reply(c, ck, NULL);
  if (!rp) return false;
  *x = rp->root_x;
  *y = rp->root_y;
  free(rp);
  return true;
}

static monitor_rect_t get_current_monitor(xcb_connection_t *c, xcb_window_t root,
                                          xcb_screen_t *screen, bool debug) {
  monitor_rect_t fallback = {
    .x = 0, .y = 0,
    .w = (int)screen->width_in_pixels,
    .h = (int)screen->height_in_pixels,
    .valid = true
  };
  int px = 0, py = 0;
  if (!get_pointer_xy(c, root, &px, &py)) {
    if (debug) fprintf(stderr, "[debug] pointer query failed; using full screen\n");
    return fallback;
  }

  xcb_randr_get_monitors_cookie_t ck = xcb_randr_get_monitors(c, root, 1);
  xcb_randr_get_monitors_reply_t *rp = xcb_randr_get_monitors_reply(c, ck, NULL);
  if (!rp) {
    if (debug) fprintf(stderr, "[debug] RandR get_monitors failed; using full screen\n");
    return fallback;
  }

  monitor_rect_t picked = {0};
  xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(rp);
  int primary_idx = -1;
  int idx = 0;
  for (; it.rem; xcb_randr_monitor_info_next(&it), ++idx) {
    xcb_randr_monitor_info_t *m = it.data;
    int mx = (int)m->x;
    int my = (int)m->y;
    int mw = (int)m->width;
    int mh = (int)m->height;
    if (m->primary) primary_idx = idx;
    if (px >= mx && px < mx + mw && py >= my && py < my + mh) {
      picked = (monitor_rect_t){ .x = mx, .y = my, .w = mw, .h = mh, .valid = true };
      break;
    }
  }

  if (!picked.valid) {
    int target_idx = (primary_idx >= 0) ? primary_idx : 0;
    it = xcb_randr_get_monitors_monitors_iterator(rp);
    idx = 0;
    for (; it.rem; xcb_randr_monitor_info_next(&it), ++idx) {
      if (idx == target_idx) {
        xcb_randr_monitor_info_t *m = it.data;
        picked = (monitor_rect_t){
          .x = (int)m->x,
          .y = (int)m->y,
          .w = (int)m->width,
          .h = (int)m->height,
          .valid = true
        };
        break;
      }
    }
  }

  if (debug && picked.valid) {
    fprintf(stderr, "[debug] monitor: %dx%d at (%d,%d) pointer=(%d,%d)\n",
            picked.w, picked.h, picked.x, picked.y, px, py);
  }

  free(rp);
  return picked.valid ? picked : fallback;
}

static long ms_to_next_second(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  long ms = ts.tv_nsec / 1000000L;

  // At an exact boundary, wait for the next second instead of returning zero
  // and repainting repeatedly until the clock advances by one millisecond.
  return 1000L - ms;
}

int main(int argc, char **argv) {
  options_t opt = {
    .font_family = "DejaVu Sans Mono",
    .font_size_px = 14.0,
    .margin_px = 0,
    .fg_r = 1.0, .fg_g = 1.0, .fg_b = 1.0,
    .bg_r = 0.0, .bg_g = 0.0, .bg_b = 0.0,
    .text_path = NULL,
    .text_from_stdin = false,
    .image_path = NULL,
    .image_size_arg = NULL,
    .anchor = "top-right",
    .time_only = false,
    .no_datetime = false,
    .debug = false,
    .flash_minutes = 0,
    .show_flash_count = false
  };

  static struct option long_opts[] = {
    {"help",      no_argument,       0, 'h'},
    {"font",      required_argument, 0, 'f'},
    {"size",      required_argument, 0, 's'},
    {"fg",        required_argument, 0,  1  },
    {"bg",        required_argument, 0,  2  },
    {"margin",    required_argument, 0, 'm'},
    {"anchor",    required_argument, 0, 'a'},
    {"text",      required_argument, 0, 't'},
    {"img",       required_argument, 0,  4  },
    {"image",     required_argument, 0,  4  },
    {"image-size", required_argument, 0, 5  },
    {"time-only", no_argument,       0, 'T'},
    {"no-datetime", no_argument,     0,  3  },
    {"flash",     required_argument, 0, 'F'},
    {"show-flash-count", no_argument, 0, 'c'},
    {"debug",     no_argument,       0, 'd'},
    {0,0,0,0}
  };

  int c, idx;
  // GNU's long-only parser supports both the requested -img spelling and
  // conventional double-dash long options while retaining short options.
  while ((c = getopt_long_only(argc, argv, "hf:s:m:a:t:F:cdT", long_opts, &idx)) != -1) {
    switch (c) {
      case 'h': print_help(argv[0]); return 0;
      case 'f': opt.font_family = optarg; break;
      case 's': opt.font_size_px = strtod(optarg, NULL); if (opt.font_size_px <= 0) opt.font_size_px = 16.0; break;
      case 'm': opt.margin_px = (uint32_t)strtoul(optarg, NULL, 10); break;
      case 'a': opt.anchor = optarg; break;
      case 't':
        if (strcmp(optarg, "-") == 0) {
          opt.text_from_stdin = true;
          opt.text_path = NULL;
        } else {
          opt.text_path = optarg;
          opt.text_from_stdin = false;
        }
        break;
      case 'T': opt.time_only = true; break;
      case 3: opt.no_datetime = true; break;
      case 4: opt.image_path = optarg; break;
      case 5: opt.image_size_arg = optarg; break;
      case 'F': {
          long v = strtol(optarg, NULL, 10);
          if (v < 0) v = 0;
          opt.flash_minutes = (int)v;
        } break;
      case 'c': opt.show_flash_count = true; break;
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

  // A single positional argument after --image is its convenient resize
  // form, for example: --image photo.jpg 50%.
  if (optind < argc) {
    if (opt.image_path && !opt.image_size_arg && optind + 1 == argc) {
      opt.image_size_arg = argv[optind];
    } else {
      fprintf(stderr, "Unexpected argument \"%s\"\n", argv[optind]);
      return 2;
    }
  }
  if (opt.image_size_arg && !opt.image_path) {
    fprintf(stderr, "--image-size requires --image\n");
    return 2;
  }

  image_size_t image_size = { .mode = IMAGE_SIZE_NATIVE };
  if (opt.image_size_arg && !parse_image_size(opt.image_size_arg, &image_size)) {
    fprintf(stderr, "Invalid image size \"%s\"; use WIDTHxHEIGHT or PERCENT%%\n",
            opt.image_size_arg);
    return 2;
  }

  anchor_t anchor = parse_anchor(opt.anchor);
  if (anchor == ANCHOR_INVALID) {
    fprintf(stderr, "Invalid --anchor value \"%s\"\n", opt.anchor ? opt.anchor : "");
    return 2;
  }

  // Treat piped input as overlay text when no explicit text file was chosen.
  // Interactive stdin remains ignored so a normal launch never blocks.
  if (!opt.text_path && !opt.text_from_stdin && !isatty(STDIN_FILENO)) {
    opt.text_from_stdin = true;
  }

  if (opt.debug) {
    fprintf(stderr, "[debug] opts: font=\"%s\" size=%.1f margin=%u time_only=%d no_datetime=%d flash_minutes=%d show_flash_count=%d text=\"%s\" text_stdin=%d image=\"%s\" image_size=\"%s\" anchor=\"%s\" fg=%.3f,%.3f,%.3f bg=%.3f,%.3f,%.3f\n",
            opt.font_family, opt.font_size_px, opt.margin_px, opt.time_only, opt.no_datetime, opt.flash_minutes,
            opt.show_flash_count, opt.text_path ? opt.text_path : "", opt.text_from_stdin,
            opt.image_path ? opt.image_path : "", opt.image_size_arg ? opt.image_size_arg : "native",
            opt.anchor ? opt.anchor : "",
            opt.fg_r, opt.fg_g, opt.fg_b, opt.bg_r, opt.bg_g, opt.bg_b);
  }

  cairo_surface_t *image_surface = NULL;
  int image_source_w = 0;
  int image_source_h = 0;
  int image_w = 0;
  int image_h = 0;
  if (opt.image_path) {
    image_surface = load_image(opt.image_path, &image_source_w, &image_source_h);
    if (!image_surface) return 1;

    image_w = image_source_w;
    image_h = image_source_h;
    if (image_size.mode == IMAGE_SIZE_EXACT) {
      image_w = image_size.width;
      image_h = image_size.height;
    } else if (image_size.mode == IMAGE_SIZE_PERCENT) {
      double scaled_width = image_source_w * image_size.percent / 100.0;
      double scaled_height = image_source_h * image_size.percent / 100.0;
      if (scaled_width > UINT16_MAX || scaled_height > UINT16_MAX) {
        fprintf(stderr, "Image resize \"%s\" exceeds X11's maximum dimensions\n",
                opt.image_size_arg);
        cairo_surface_destroy(image_surface);
        return 2;
      }
      image_w = (int)(scaled_width + 0.5);
      image_h = (int)(scaled_height + 0.5);
      if (image_w < 1) image_w = 1;
      if (image_h < 1) image_h = 1;
    }
    uint64_t padded_width = (uint64_t)image_w + 2u * opt.margin_px;
    uint64_t padded_height = (uint64_t)image_h + 2u * opt.margin_px;
    if (padded_width > UINT16_MAX || padded_height > UINT16_MAX) {
      fprintf(stderr, "Displayed image plus margin exceeds X11's maximum dimensions\n");
      cairo_surface_destroy(image_surface);
      return 2;
    }
    if (opt.debug) {
      fprintf(stderr, "[debug] image: source=%dx%d display=%dx%d\n",
              image_source_w, image_source_h, image_w, image_h);
    }
  }

  int screen_num = 0;
  xcb_connection_t *cconn = xcb_connect(NULL, &screen_num);
  if (!cconn || xcb_connection_has_error(cconn)) {
    fprintf(stderr, "Failed to connect to X server\n");
    if (image_surface) cairo_surface_destroy(image_surface);
    return 1;
  }

  const xcb_setup_t *setup = xcb_get_setup(cconn);
  xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
  for (int s = 0; s < screen_num; ++s) xcb_screen_next(&iter);
  xcb_screen_t *screen = iter.data;
  if (!screen) {
    fprintf(stderr, "Could not get default screen\n");
    if (image_surface) cairo_surface_destroy(image_surface);
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
    if (image_surface) cairo_surface_destroy(image_surface);
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
    screen->width_in_pixels - w - opt.margin_px, // x (placeholder; updated after measure)
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

  // Completed frames are copied from an off-screen pixmap in one X request,
  // preventing partially painted frames from becoming visible.
  xcb_gcontext_t copy_gc = xcb_generate_id(cconn);
  uint32_t copy_gc_values[1] = { 0 };
  xcb_create_gc(cconn, copy_gc, win, XCB_GC_GRAPHICS_EXPOSURES,
                copy_gc_values);
  xcb_pixmap_t back_pixmap = XCB_NONE;
  cairo_surface_t *back_surface = NULL;
  uint16_t back_w = 0;
  uint16_t back_h = 0;

  // Track applied geometry so the once-per-second clock update does not ask
  // X11 to reconfigure an unchanged window and generate another expose cycle.
  // The placeholder geometry is not cached, so the first rendered frame must
  // explicitly apply every calculated field.
  bool geometry_initialized = false;
  int16_t current_x = 0;
  int16_t current_y = 0;
  uint16_t current_w = 0;
  uint16_t current_h = 0;
  xcb_flush(cconn);

  // Prepare a dummy cairo surface/context for text metrics (offscreen).
  cairo_surface_t *measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *measure_cr = cairo_create(measure_surface);
  cairo_select_font_face(measure_cr, opt.font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(measure_cr, opt.font_size_px);

  // Flash state (boundary-aligned)
  flash_state_t flash = { .active = false, .start = 0, .last_boundary_min_epoch = -1 };
  uint64_t flash_count = 0;

  // Main loop: poll X for events. During flash, update every 50ms; otherwise,
  // sleep to next second to keep CPU minimal.
  int xfd = xcb_get_file_descriptor(cconn);
  char last_str[128] = {0};
  text_lines_t text = {0};
  stdin_text_t stdin_text = {0};
  int text_fd = -1;
  int text_wd = -1;
  const uint32_t text_watch_mask = IN_CLOSE_WRITE | IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF | IN_ATTRIB | IN_MOVED_TO | IN_CREATE;

  if (opt.text_from_stdin) {
    stdin_text.open = true;
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags < 0) {
      if (opt.debug) fprintf(stderr, "[debug] fcntl(F_GETFL) failed for stdin: %s\n", strerror(errno));
      stdin_text.open = false;
    } else if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK) < 0) {
      if (opt.debug) fprintf(stderr, "[debug] fcntl(F_SETFL) failed for stdin: %s\n", strerror(errno));
      stdin_text.open = false;
    }
  } else if (opt.text_path) {
    if (!load_text_lines(opt.text_path, &text) && opt.debug) {
      fprintf(stderr, "[debug] failed to load text file \"%s\": %s\n",
              opt.text_path, strerror(errno));
    }
    text_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (text_fd < 0) {
      if (opt.debug) fprintf(stderr, "[debug] inotify_init1 failed: %s\n", strerror(errno));
    } else {
      text_wd = inotify_add_watch(text_fd, opt.text_path, text_watch_mask);
      if (text_wd < 0 && opt.debug) {
        fprintf(stderr, "[debug] inotify_add_watch failed for \"%s\": %s\n",
                opt.text_path, strerror(errno));
      }
    }
  }

  for (;;) {
    if (opt.text_path && text_fd >= 0 && text_wd < 0) {
      text_wd = inotify_add_watch(text_fd, opt.text_path, text_watch_mask);
      if (text_wd >= 0 && opt.debug) {
        fprintf(stderr, "[debug] inotify watch added for \"%s\"\n", opt.text_path);
      }
    }

    long timeout_ms = flash.active ? FLASH_STEP_MS : ms_to_next_second();

    struct pollfd pfds[3];
    int nfds = 0;
    int text_poll_idx = -1;
    int stdin_poll_idx = -1;

    pfds[0].fd = xfd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    nfds++;
    if (text_fd >= 0) {
      text_poll_idx = nfds;
      pfds[nfds].fd = text_fd;
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      nfds++;
    }
    if (stdin_text.open) {
      stdin_poll_idx = nfds;
      pfds[nfds].fd = STDIN_FILENO;
      pfds[nfds].events = POLLIN | POLLHUP | POLLERR;
      pfds[nfds].revents = 0;
      nfds++;
    }

    int pr = poll(pfds, nfds, (int)timeout_ms);
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

    if (stdin_poll_idx >= 0 && (pfds[stdin_poll_idx].revents & (POLLIN | POLLHUP | POLLERR))) {
      bool stdin_updated = false;
      if (!drain_stdin_text(&stdin_text, &text, &stdin_updated)) {
        if (opt.debug) fprintf(stderr, "[debug] failed to read text from stdin: %s\n", strerror(errno));
        stdin_text.open = false;
      }
      if (stdin_updated) {
        need_redraw = true;
      }
    }

    if (text_poll_idx >= 0 && pfds[text_poll_idx].revents & POLLIN) {
      char ibuf[4096];
      ssize_t nread;
      bool reload_text = false;
      while ((nread = read(text_fd, ibuf, sizeof(ibuf))) > 0) {
        size_t off = 0;
        while (off < (size_t)nread) {
          struct inotify_event *iev = (struct inotify_event *)(ibuf + off);
          if (iev->mask & (IN_CLOSE_WRITE | IN_MODIFY | IN_MOVED_TO | IN_ATTRIB | IN_CREATE)) {
            reload_text = true;
          }
          if (iev->mask & (IN_MOVE_SELF | IN_DELETE_SELF | IN_IGNORED)) {
            text_wd = -1;
            reload_text = true;
          }
          off += sizeof(struct inotify_event) + iev->len;
        }
      }
      if (reload_text) {
        if (!load_text_lines(opt.text_path, &text) && opt.debug) {
          fprintf(stderr, "[debug] failed to reload text file \"%s\": %s\n",
                  opt.text_path, strerror(errno));
        }
        need_redraw = true;
      }
    }

    // Tick
    if (pr == 0) {
      need_redraw = true;
    }

    if (need_redraw) {
      time_t now = time(NULL);
      struct tm lt;
      localtime_r(&now, &lt);

      // Boundary-aligned flash trigger: minute % flash_minutes == 0 at sec 00
      if (opt.flash_minutes > 0) {
        long epoch_min = (long)(now / 60);
        if (!flash.active && lt.tm_sec == 0 && (lt.tm_min % opt.flash_minutes) == 0) {
          if (flash.last_boundary_min_epoch != epoch_min) {
            flash.active = true;
            flash.start = now;
            flash.last_boundary_min_epoch = epoch_min;
            flash_count++;
            if (opt.debug) {
              fprintf(stderr, "[debug] flash start: epoch_min=%ld (time %02d:%02d) count=%llu\n",
                      epoch_min, lt.tm_hour, lt.tm_min,
                      (unsigned long long)flash_count);
            }
          }
        }
        if (flash.active) {
          double elapsed = difftime(now, flash.start);
          if (elapsed >= FLASH_DURATION_SEC) {
            flash.active = false;
            if (opt.debug) fprintf(stderr, "[debug] flash end (count=%llu)\n",
                                   (unsigned long long)flash_count);
          }
        }
      }

      char dispbuf[128] = {0};
      if (!opt.no_datetime) {
        char nowbuf[64];
        now_timestr(nowbuf, sizeof(nowbuf), opt.time_only);

        // Compose the clock line with the optional flash count. The count is
        // tied to the datetime line, so --no-datetime suppresses it too.
        if (opt.show_flash_count && flash_count > 0) {
          snprintf(dispbuf, sizeof(dispbuf), "%s (%llu)", nowbuf,
                   (unsigned long long)flash_count);
        } else {
          snprintf(dispbuf, sizeof(dispbuf), "%s", nowbuf);
        }
      }

      // Re-measure text
      cairo_select_font_face(measure_cr, opt.font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(measure_cr, opt.font_size_px);

      cairo_text_extents_t te;
      cairo_font_extents_t fe;
      cairo_font_extents(measure_cr, &fe);

      double line_h = fe.ascent + fe.descent;
      size_t datetime_lines = opt.no_datetime ? 0 : 1;
      size_t total_lines = datetime_lines + text.count;
      int text_w = 0;
      for (size_t i = 0; i < total_lines; ++i) {
        const char *line = (i < datetime_lines) ? dispbuf : text.lines[i - datetime_lines];
        cairo_text_extents(measure_cr, line, &te);
        double line_wd = te.x_advance;
        if (te.width > line_wd) line_wd = te.width;
        int line_w = (int)(line_wd + 0.5);
        if (line_w > text_w) text_w = line_w;
      }
      int text_h = (int)(line_h * (double)total_lines + 0.5);

      uint16_t pad = (uint16_t)(opt.margin_px);
      int content_w = text_w > image_w ? text_w : image_w;
      int content_h = text_h + image_h;
      uint16_t win_w = (uint16_t)(content_w + pad * 2);
      uint16_t win_h = (uint16_t)(content_h + pad * 2);
      // X11 windows cannot be zero-sized; keep textless overlays valid while
      // still drawing no visible datetime content.
      if (win_w == 0) win_w = 1;
      if (win_h == 0) win_h = 1;

      int16_t new_x = 0;
      int16_t new_y = 0;
      monitor_rect_t mon = get_current_monitor(cconn, screen->root, screen, opt.debug);
      anchor_position(anchor, mon.x, mon.y, mon.w, mon.h,
                      win_w, win_h, opt.margin_px, &new_x, &new_y);

      uint32_t cfg[4];
      uint16_t mask = 0;
      int cidx = 0;
      if (!geometry_initialized || new_x != current_x) {
        mask |= XCB_CONFIG_WINDOW_X;
        cfg[cidx++] = (uint32_t)new_x;
      }
      if (!geometry_initialized || new_y != current_y) {
        mask |= XCB_CONFIG_WINDOW_Y;
        cfg[cidx++] = (uint32_t)new_y;
      }
      if (!geometry_initialized || win_w != current_w) {
        mask |= XCB_CONFIG_WINDOW_WIDTH;
        cfg[cidx++] = (uint32_t)win_w;
      }
      if (!geometry_initialized || win_h != current_h) {
        mask |= XCB_CONFIG_WINDOW_HEIGHT;
        cfg[cidx++] = (uint32_t)win_h;
      }

      // Compute colors:
      // - Normal: use configured fg/bg.
      // - Flash: background fades from inverted(orig_bg) -> orig_bg over 30s;
      //          foreground is always inverse of CURRENT background.
      double fg_r = opt.fg_r, fg_g = opt.fg_g, fg_b = opt.fg_b;
      double bg_r = opt.bg_r, bg_g = opt.bg_g, bg_b = opt.bg_b;

      if (flash.active) {
        double elapsed = difftime(now, flash.start);
        double p = elapsed / (double)FLASH_DURATION_SEC; // 0 -> 1
        if (p < 0.0) {
          if (opt.debug) fprintf(stderr, "[debug] clamp p<0: %.6f\n", p);
          p = 0.0;
        } else if (p > 1.0) {
          if (opt.debug) fprintf(stderr, "[debug] clamp p>1: %.6f\n", p);
          p = 1.0;
        }

        double inv_bg_r = 1.0 - opt.bg_r, inv_bg_g = 1.0 - opt.bg_g, inv_bg_b = 1.0 - opt.bg_b;

        bg_r = inv_bg_r * (1.0 - p) + opt.bg_r * p;
        bg_g = inv_bg_g * (1.0 - p) + opt.bg_g * p;
        bg_b = inv_bg_b * (1.0 - p) + opt.bg_b * p;

        fg_r = 1.0 - bg_r;
        fg_g = 1.0 - bg_g;
        fg_b = 1.0 - bg_b;

        if (opt.debug) {
          fprintf(stderr, "[debug] flash tick: p=%.3f bg=%.3f,%.3f,%.3f fg(inv)=%.3f,%.3f,%.3f disp=\"%s\"\n",
                  p, bg_r, bg_g, bg_b, fg_r, fg_g, fg_b, dispbuf);
        }
      }

      if (opt.debug && !flash.active) {
        fprintf(stderr, "[debug] tick disp=\"%s\" text_w=%d text_h=%d win=%ux%u at (%d,%d) flash=%d count=%llu\n",
                dispbuf, text_w, text_h, win_w, win_h, new_x, new_y, flash.active,
                (unsigned long long)flash_count);
      }

      if (!back_surface || back_w != win_w || back_h != win_h) {
        // Reallocate only when content dimensions change; the pixmap remains
        // reusable for ordinary datetime ticks.
        if (back_surface) cairo_surface_destroy(back_surface);
        if (back_pixmap != XCB_NONE) xcb_free_pixmap(cconn, back_pixmap);
        back_pixmap = xcb_generate_id(cconn);
        xcb_create_pixmap(cconn, screen->root_depth, back_pixmap, win,
                          win_w, win_h);
        back_surface = cairo_xcb_surface_create(cconn, back_pixmap, visual,
                                                win_w, win_h);
        back_w = win_w;
        back_h = win_h;
      }

      // Render the whole frame off-screen before changing anything visible.
      cairo_t *cr = cairo_create(back_surface);

      // Background
      cairo_set_source_rgb(cr, bg_r, bg_g, bg_b);
      cairo_paint(cr);

      // Text
      cairo_select_font_face(cr, opt.font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
      cairo_set_font_size(cr, opt.font_size_px);
      cairo_set_source_rgb(cr, fg_r, fg_g, fg_b);

      for (size_t i = 0; i < total_lines; ++i) {
        const char *line = (i < datetime_lines) ? dispbuf : text.lines[i - datetime_lines];
        cairo_text_extents(cr, line, &te);
        double text_x = pad - te.x_bearing;     // account for left bearing
        double text_y = pad + fe.ascent + (double)i * line_h; // baseline
        cairo_move_to(cr, text_x, text_y);
        cairo_show_text(cr, line);
      }

      if (image_surface) {
        // Clip before scaling so interpolation cannot paint outside the
        // image's allocated part of the overlay.
        double image_x = pad;
        double image_y = pad + text_h;
        cairo_save(cr);
        cairo_rectangle(cr, image_x, image_y, image_w, image_h);
        cairo_clip(cr);
        cairo_translate(cr, image_x, image_y);
        cairo_scale(cr, (double)image_w / image_source_w,
                    (double)image_h / image_source_h);
        cairo_set_source_surface(cr, image_surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
      }
      cairo_destroy(cr);

      // Flush Cairo's pixmap operations first, then place the complete frame
      // in the window atomically from the X server's perspective. Geometry
      // changes immediately precede the copy to minimize resize exposure.
      cairo_surface_flush(back_surface);
      if (mask != 0) {
        xcb_configure_window(cconn, win, mask, cfg);
        current_x = new_x;
        current_y = new_y;
        current_w = win_w;
        current_h = win_h;
        geometry_initialized = true;
      }
      xcb_copy_area(cconn, back_pixmap, win, copy_gc,
                    0, 0, 0, 0, win_w, win_h);

      xcb_flush(cconn);
      strncpy(last_str, dispbuf, sizeof(last_str)-1);
      last_str[sizeof(last_str)-1] = '\0';
    }
  }

  // Unreachable in normal usage; kept for completeness
  free_stdin_text(&stdin_text);
  free_text_lines(&text);
  cairo_destroy(measure_cr);
  cairo_surface_destroy(measure_surface);
  if (back_surface) cairo_surface_destroy(back_surface);
  if (back_pixmap != XCB_NONE) xcb_free_pixmap(cconn, back_pixmap);
  xcb_free_gc(cconn, copy_gc);
  if (image_surface) cairo_surface_destroy(image_surface);
  xcb_disconnect(cconn);
  return 0;
}
