#include <getopt.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "buffer.h"
#include "output-layout.h"
#include "wooz.h"

#include "cursor-shape-v1-protocol.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"
#include "viewporter-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "wlr-screencopy-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"

// Key codes from linux/input-event-codes.h
#define KEY_ESC 1
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_Q 16
#define KEY_A 30
#define KEY_X 45
#define KEY_KPMINUS 74
#define KEY_KPPLUS 78
#define KEY_KP0 82
#define KEY_UP 103
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108

#define min(x, y) (x < y ? x : y)
#define max(x, y) (x > y ? x : y)

#define MAX_SCROLL 16
#define DOUBLE_CLICK_TIME_MS 400
#define KEYBOARD_PAN_STEP 50.0
#define KEYBOARD_ZOOM_STEP 10.0
#define KEY_REPEAT_DELAY_MS 500
#define KEY_REPEAT_RATE_MS 50

// Animated zoom: a zoom step glides to its target over a duration that grows
// with the distance to travel, so a single notch stays snappy while a big jump
// (e.g. zooming back out) gets more time.
#define ZOOM_ANIMATION_MS 120.0
#define ZOOM_ANIMATION_DISTANCE_GAIN 2.0
#define ZOOM_ANIMATION_MAX_MS 320.0

// From 4x magnification on, the runs of destination pixels that sample the same
// source column are long enough that filling them beats an indexed load per
// pixel; below that the runs are too short and the loop overhead wins.
#define RUN_FILL_MIN_STEP 0.25

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

// Ease-out curve: maximum velocity at the start, settling at the end. Steps
// that arrive mid-animation stay responsive, because every leg of the animation
// starts out fast instead of easing in.
static double ease_out_cubic(double t) {
  double f = 1.0 - t;

  return 1.0 - f * f * f;
}

static bool same_view(const struct wooz_boxf *a, const struct wooz_boxf *b) {
  return a->x == b->x && a->y == b->y && a->width == b->width &&
         a->height == b->height;
}

// Keeps a view inside the captured buffer.
static void clamp_view(struct wooz_window *win, struct wooz_boxf *view) {
  int32_t width = win->output->buffer->width;
  int32_t height = win->output->buffer->height;

  view->width = max(min(view->width, width), MAX_SCROLL * win->output->ratio);
  view->height = max(min(view->height, height), MAX_SCROLL);
  view->x = max(min(view->x, width - view->width), 0);
  view->y = max(min(view->y, height - view->height), 0);
}

// Applies a zoom step to `view`, centered on the given logical position.
static void view_zoom(struct wooz_window *win, struct wooz_boxf *view,
                      double zoom_change, double center_x, double center_y) {
  double ratio = win->output->ratio;

  // Calculate the zoom change in pixels
  double scroll = zoom_change;

  if (view->width - scroll * ratio < MAX_SCROLL ||
      view->height - scroll < MAX_SCROLL)
    return;

  // Calculate center point as ratio of viewport
  double dx = center_x / (double)win->output->logical_geometry.width;
  double dy = center_y / (double)win->output->logical_geometry.height;

  view->x += round(scroll * ratio * dx);
  view->width -= round(scroll * ratio);
  view->y += round(scroll * dy);
  view->height -= round(scroll);
}

static bool is_32bit_shm_format(enum wl_shm_format format) {
  return format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
         format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888;
}

// Builds the destination-x -> source-x mapping for `step` source pixels per
// destination pixel, offset by `phase` (how far the view origin sits inside the
// destination grid). It is rebuilt every frame because the phase changes as the
// view moves, and a few thousand divisions are nothing next to the resampling.
static const int32_t *get_column_map(struct wooz_window *win,
                                     int32_t dest_width, double step,
                                     double phase, int32_t max_column) {
  if (win->column_map == NULL || win->column_map_dest_width != dest_width) {
    int32_t *map =
        realloc(win->column_map, (size_t)dest_width * sizeof(int32_t));
    if (map == NULL) {
      return NULL;
    }

    win->column_map = map;
    win->column_map_dest_width = dest_width;
  }

  for (int32_t x = 0; x < dest_width; ++x) {
    win->column_map[x] = min((int32_t)floor(phase + x * step), max_column);
  }

  return win->column_map;
}

// Builds the runs of destination pixels that sample the same source column,
// from the same `step`/`phase` mapping as get_column_map().
static bool build_runs(struct wooz_window *win, int32_t dest_width, double step,
                       double phase, int32_t max_column) {
  if (win->runs == NULL || win->runs_capacity < dest_width) {
    struct wooz_run *runs =
        realloc(win->runs, (size_t)dest_width * sizeof(struct wooz_run));
    if (runs == NULL) {
      return false;
    }

    win->runs = runs;
    win->runs_capacity = dest_width;
  }

  int32_t count = 0;
  int32_t x = 0;
  while (x < dest_width) {
    int32_t column = min((int32_t)floor(phase + x * step), max_column);

    // The run ends where the mapping reaches the next source column (or the
    // clamped edge, where the last column extends to the end).
    int32_t next_x;
    if (column >= max_column) {
      next_x = dest_width;
    } else {
      next_x = (int32_t)ceil((column + 1 - phase) / step);
      next_x = max(min(next_x, dest_width), x + 1);
    }

    win->runs[count].x = x;
    win->runs[count].column = column;
    ++count;
    x = next_x;
  }

  win->run_count = count;
  return true;
}

// Magnify the view source rectangle to the whole buffer using nearest-neighbor
// sampling, bypassing the bilinear filtering the compositor applies when
// scaling. Returns the buffer to attach, or NULL when client-side sampling
// isn't possible (in which case the compositor is left to scale the viewport).
// `*changed` is set to false when the sampled buffer is already attached and up
// to date, in which case the caller has nothing to commit.
static struct wooz_buffer *render_nearest_neighbor(struct wooz_window *win,
                                                   bool *changed) {
  struct wooz_buffer *src = win->output->buffer;

  *changed = true;

  // On rotated or flipped outputs the viewport is expressed in the surface
  // local coordinate space, which doesn't match the pixel layout of the
  // screencopy buffer. Leave the scaling to the compositor there.
  if (win->output->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
    return NULL;
  }

  if (!is_32bit_shm_format(src->format) || src->width < 1 || src->height < 1) {
    return NULL;
  }

  int32_t width = src->width;
  int32_t height = src->height;

  double step_x = win->view_source.width / (double)width;
  double step_y = win->view_source.height / (double)height;
  // The sampling grid is anchored at the fractional view origin: the fraction
  // is carried as a destination-pixel phase so the magnified image slides one
  // screen pixel at a time instead of jumping a whole source pixel.
  if (step_x <= 0.0 || step_y <= 0.0) {
    return NULL;
  }

  int32_t x0 = (int32_t)floor(win->view_source.x);
  int32_t y0 = (int32_t)floor(win->view_source.y);
  double phase_x = win->view_source.x - x0;
  double phase_y = win->view_source.y - y0;

  // Keep the sampled rectangle inside the buffer.
  if (x0 < 0) {
    x0 = 0;
    phase_x = 0.0;
  }
  if (y0 < 0) {
    y0 = 0;
    phase_y = 0.0;
  }

  int32_t src_width =
      min((int32_t)floor(phase_x + (width - 1) * step_x) + 1, width - x0);
  int32_t src_height =
      min((int32_t)floor(phase_y + (height - 1) * step_y) + 1, height - y0);
  if (src_width < 1 || src_height < 1) {
    return NULL;
  }

  // A view change that doesn't move the sampling grid by a whole destination
  // pixel samples the exact same pixels: keep the buffer that is already
  // attached instead of resampling and re-uploading the whole screen.
  if (win->sampled_valid && x0 == win->sampled_x && y0 == win->sampled_y &&
      phase_x == win->sampled_phase_x && phase_y == win->sampled_phase_y &&
      step_x == win->sampled_step_x && step_y == win->sampled_step_y &&
      width == win->sampled_buffer_width &&
      height == win->sampled_buffer_height) {
    *changed = false;
    return win->scaled_buffers[win->scaled_buffer_index];
  }

  // Alternate between two buffers so that the compositor doesn't read memory
  // that is currently being written to.
  int index = (win->scaled_buffer_index + 1) % WOOZ_SCALED_BUFFER_COUNT;
  struct wooz_buffer *dst = win->scaled_buffers[index];
  if (dst == NULL || dst->width != width || dst->height != height ||
      dst->format != src->format) {
    destroy_buffer(dst);
    dst = create_buffer(win->state->shm, src->format, width, height, width * 4);
    win->scaled_buffers[index] = dst;
    if (dst == NULL) {
      return NULL;
    }
  }
  win->scaled_buffer_index = index;

  // From 4x magnification on, fill runs of destination pixels instead of
  // loading the source column for every pixel.
  bool use_runs = step_x <= RUN_FILL_MIN_STEP;
  const int32_t *columns = NULL;
  const struct wooz_run *runs = NULL;
  int32_t run_count = 0;

  if (use_runs) {
    if (!build_runs(win, width, step_x, phase_x, src_width - 1)) {
      return NULL;
    }

    runs = win->runs;
    run_count = win->run_count;
  } else {
    columns = get_column_map(win, width, step_x, phase_x, src_width - 1);
    if (columns == NULL) {
      return NULL;
    }
  }

  const uint32_t *src_data = src->data;
  uint32_t *dst_data = dst->data;
  size_t src_stride = (size_t)src->stride / sizeof(uint32_t);
  size_t dst_stride = (size_t)dst->stride / sizeof(uint32_t);
  const uint32_t *prev_src_row = NULL;
  uint32_t *prev_dst_row = NULL;

  for (int32_t y = 0; y < height; ++y) {
    int32_t sy = min((int32_t)floor(phase_y + y * step_y), src_height - 1);
    const uint32_t *src_row =
        src_data + (size_t)(y0 + sy) * src_stride + (size_t)x0;
    uint32_t *dst_row = dst_data + (size_t)y * dst_stride;

    if (src_row == prev_src_row) {
      // Magnification repeats source rows: duplicate the previous output row.
      memcpy(dst_row, prev_dst_row, (size_t)width * sizeof(uint32_t));
      continue;
    }

    prev_src_row = src_row;
    prev_dst_row = dst_row;

    if (step_x == 1.0 && phase_x == 0.0) {
      memcpy(dst_row, src_row, (size_t)width * sizeof(uint32_t));
      continue;
    }

    if (use_runs) {
      for (int32_t r = 0; r < run_count; ++r) {
        int32_t start = runs[r].x;
        int32_t end = r + 1 < run_count ? runs[r + 1].x : width;
        uint32_t value = src_row[runs[r].column];

        for (int32_t x = start; x < end; ++x) {
          dst_row[x] = value;
        }
      }
    } else {
      for (int32_t x = 0; x < width; ++x) {
        dst_row[x] = src_row[columns[x]];
      }
    }
  }

  win->sampled_valid = true;
  win->sampled_x = x0;
  win->sampled_y = y0;
  win->sampled_phase_x = phase_x;
  win->sampled_phase_y = phase_y;
  win->sampled_step_x = step_x;
  win->sampled_step_y = step_y;
  win->sampled_buffer_width = width;
  win->sampled_buffer_height = height;

  return dst;
}

static void render_window(struct wooz_window *win) {
  clamp_view(win, &win->view_source);

  struct wooz_buffer *buffer = NULL;
  bool changed = true;

  if (!win->antialias) {
    buffer = render_nearest_neighbor(win, &changed);
  }

  if (!changed) {
    // The sampled buffer is attached and up to date: nothing to commit.
    return;
  }

  if (buffer != NULL) {
    // Attach the magnified buffer as a whole so that the compositor doesn't
    // filter the pixels sampled above.
    wl_surface_attach(win->surface, buffer->wl_buffer, 0, 0);
    wp_viewport_set_source(
        win->viewport, wl_fixed_from_int(0), wl_fixed_from_int(0),
        wl_fixed_from_int(buffer->width), wl_fixed_from_int(buffer->height));
    wl_surface_damage_buffer(win->surface, 0, 0, buffer->width, buffer->height);
  } else {
    // Let the compositor crop and bilinearly scale the view source rectangle.
    if (win->sampled_valid) {
      // Switching away from the sampled buffer: repaint the whole surface.
      wl_surface_damage_buffer(win->surface, 0, 0, win->output->buffer->width,
                               win->output->buffer->height);
      win->sampled_valid = false;
    }
    wl_surface_attach(win->surface, win->output->buffer->wl_buffer, 0, 0);
    wp_viewport_set_source(win->viewport,
                           wl_fixed_from_double(win->view_source.x),
                           wl_fixed_from_double(win->view_source.y),
                           wl_fixed_from_double(win->view_source.width),
                           wl_fixed_from_double(win->view_source.height));
  }

  wl_surface_commit(win->surface);
}

static void zoom_animation_step(struct wooz_window *win);

static void zoom_frame_done(void *data, struct wl_callback *callback,
                            uint32_t callback_data) {
  struct wooz_window *win = data;

  wl_callback_destroy(callback);
  win->zoom.frame_callback = NULL;
  zoom_animation_step(win);
}

static const struct wl_callback_listener zoom_frame_listener = {
    .done = zoom_frame_done,
};

// Requests a frame callback, so that animation steps are paced by the
// compositor's repaints instead of a timer.
static void zoom_request_frame(struct wooz_window *win) {
  if (win->zoom.frame_callback != NULL) {
    return;
  }

  win->zoom.frame_callback = wl_surface_frame(win->surface);
  wl_callback_add_listener(win->zoom.frame_callback, &zoom_frame_listener, win);
}

// The view that zoom input applies to: while a zoom is in flight that is the
// animation target, so that rapid steps accumulate instead of fighting each
// other.
static struct wooz_boxf *zoom_reference(struct wooz_window *win) {
  return win->zoom.active ? &win->zoom.target : &win->view_source;
}

// (Re)starts the animation towards win->zoom.target from the view that is
// currently displayed: a step arriving mid-animation retargets the curve from
// where the view is now, which keeps rapid scrolling continuous and lets the
// displayed speed follow the input speed.
//
// Returns true when an animation was started, in which case the window has been
// rendered.
static bool zoom_start(struct wooz_window *win) {
  struct wooz_zoom *zoom = &win->zoom;

  if (same_view(&win->view_source, &zoom->target)) {
    zoom->active = false;
    return false;
  }

  zoom->from = win->view_source;

  // Perceptual distance: the log of the zoom ratio. It is 0 for a view change
  // without a zoom (e.g. restoring the view at 100%) and grows with the factor.
  double distance = fabs(log2(zoom->target.width / zoom->from.width));
  zoom->duration_ms =
      ZOOM_ANIMATION_MS * (1.0 + ZOOM_ANIMATION_DISTANCE_GAIN * distance);
  if (zoom->duration_ms > ZOOM_ANIMATION_MAX_MS) {
    zoom->duration_ms = ZOOM_ANIMATION_MAX_MS;
  }

  zoom->start_ms = now_ms();
  zoom->active = true;

  if (zoom->frame_callback == NULL) {
    zoom_request_frame(win);
    // Commit so that the compositor delivers the callback on its next repaint;
    // the first animation step then runs from that callback.
    win->sampled_valid = false;
    render_window(win);
  }

  return true;
}

// Applies a zoom step, animating towards the result. Consecutive steps add up
// on the animation target, so scrolling rapidly accelerates the zoom instead of
// restarting it.
static void zoom_by(struct wooz_window *win, double zoom_change,
                    double center_x, double center_y) {
  struct wooz_zoom *zoom = &win->zoom;

  if (!zoom->active) {
    zoom->target = win->view_source;
  }

  struct wooz_boxf previous = zoom->target;
  view_zoom(win, &zoom->target, zoom_change, center_x, center_y);
  clamp_view(win, &zoom->target);

  if (same_view(&previous, &zoom->target)) {
    // Zoom limit reached, the target didn't move.
    return;
  }

  zoom_start(win);
}

static void zoom_to_view(struct wooz_window *win,
                         const struct wooz_boxf *view) {
  win->zoom.target = *view;
  zoom_start(win);
}

static void restore_view(struct wooz_window *win) {
  zoom_to_view(win, &win->initial_view_source);
}

// Shifts the view, keeping a running zoom animation in sync.
static void pan_view(struct wooz_window *win, double dx, double dy) {
  win->view_source.x += dx;
  win->view_source.y += dy;

  if (win->zoom.active) {
    win->zoom.from.x += dx;
    win->zoom.from.y += dy;
    win->zoom.target.x += dx;
    win->zoom.target.y += dy;
  }
}

static void zoom_animation_step(struct wooz_window *win) {
  struct wooz_zoom *zoom = &win->zoom;

  if (!zoom->active) {
    return;
  }

  double t = (now_ms() - zoom->start_ms) / zoom->duration_ms;

  if (t >= 1.0) {
    win->view_source = zoom->target;
    zoom->active = false;
  } else {
    double eased = ease_out_cubic(t);

    win->view_source.x = zoom->from.x + (zoom->target.x - zoom->from.x) * eased;
    win->view_source.y = zoom->from.y + (zoom->target.y - zoom->from.y) * eased;
    win->view_source.width =
        zoom->from.width + (zoom->target.width - zoom->from.width) * eased;
    win->view_source.height =
        zoom->from.height + (zoom->target.height - zoom->from.height) * eased;
    zoom_request_frame(win);
  }

  // The nearest-neighbor path skips the commit when the sampled region didn't
  // change; animation frames have to commit for the next callback to arrive.
  win->sampled_valid = false;
  render_window(win);
}

static void handle_key_action(struct wooz_state *state, uint32_t key) {
  struct wooz_window *win = state->focused;
  if (win == NULL) {
    return;
  }

  switch (key) {
  case KEY_EQUAL: // For keyboards where + is shift+=
  case KEY_KPPLUS:
    // Zoom in at center
    zoom_by(win, KEYBOARD_ZOOM_STEP, win->output->logical_geometry.width / 2.0,
            win->output->logical_geometry.height / 2.0);
    break;

  case KEY_MINUS:
  case KEY_KPMINUS:
    // Zoom out at center
    zoom_by(win, -KEYBOARD_ZOOM_STEP, win->output->logical_geometry.width / 2.0,
            win->output->logical_geometry.height / 2.0);
    break;

  case KEY_LEFT:
    pan_view(win, -KEYBOARD_PAN_STEP, 0);
    render_window(win);
    break;

  case KEY_RIGHT:
    pan_view(win, KEYBOARD_PAN_STEP, 0);
    render_window(win);
    break;

  case KEY_UP:
    pan_view(win, 0, -KEYBOARD_PAN_STEP);
    render_window(win);
    break;

  case KEY_DOWN:
    pan_view(win, 0, KEYBOARD_PAN_STEP);
    render_window(win);
    break;
  }
}

static void stop_key_repeat(struct wooz_state *state) {
  if (state->repeat_timer_fd >= 0 && state->pressed_key != 0) {
    struct itimerspec its = {0};
    timerfd_settime(state->repeat_timer_fd, 0, &its, NULL);
    state->pressed_key = 0;
  }
}

static void start_key_repeat(struct wooz_state *state, uint32_t key) {
  state->pressed_key = key;

  if (state->repeat_timer_fd < 0) {
    state->repeat_timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (state->repeat_timer_fd < 0) {
      return;
    }
  }

  struct itimerspec its;
  its.it_value.tv_sec = KEY_REPEAT_DELAY_MS / 1000;
  its.it_value.tv_nsec = (KEY_REPEAT_DELAY_MS % 1000) * 1000000;
  its.it_interval.tv_sec = KEY_REPEAT_RATE_MS / 1000;
  its.it_interval.tv_nsec = (KEY_REPEAT_RATE_MS % 1000) * 1000000;

  timerfd_settime(state->repeat_timer_fd, 0, &its, NULL);
}

static bool is_repeatable_key(uint32_t key) {
  return key == KEY_EQUAL || key == KEY_KPPLUS || key == KEY_MINUS ||
         key == KEY_KPMINUS || key == KEY_LEFT || key == KEY_RIGHT ||
         key == KEY_UP || key == KEY_DOWN;
}

static void screencopy_frame_handle_buffer(
    void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
    uint32_t width, uint32_t height, uint32_t stride) {
  struct wooz_output *output = data;

  output->buffer =
      create_buffer(output->state->shm, format, width, height, stride);
  if (output->buffer == NULL) {
    fprintf(stderr, "failed to create buffer\n");
    exit(EXIT_FAILURE);
  }

  // Handle rotated screens.
  if (output->transform & WL_OUTPUT_TRANSFORM_90) {
    int32_t tmp = output->buffer->width;
    output->buffer->width = output->buffer->height;
    output->buffer->height = tmp;
  }

  zwlr_screencopy_frame_v1_copy(frame, output->buffer->wl_buffer);
}

static void screencopy_frame_handle_flags(
    void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
  struct wooz_output *output = data;
  output->screencopy_frame_flags = flags;
}

static void screencopy_frame_handle_ready(
    void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t tv_sec_hi,
    uint32_t tv_sec_lo, uint32_t tv_nsec) {
  struct wooz_output *output = data;
  ++output->state->n_done;
}

static void
screencopy_frame_handle_failed(void *data,
                               struct zwlr_screencopy_frame_v1 *frame) {
  struct wooz_output *output = data;
  fprintf(stderr, "failed to copy output %s\n", output->name);
  exit(EXIT_FAILURE);
}

static const struct zwlr_screencopy_frame_v1_listener
    screencopy_frame_listener = {
        .buffer = screencopy_frame_handle_buffer,
        .flags = screencopy_frame_handle_flags,
        .ready = screencopy_frame_handle_ready,
        .failed = screencopy_frame_handle_failed,
};

static void xdg_output_handle_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
  struct wooz_output *output = data;

  output->logical_geometry.x = x;
  output->logical_geometry.y = y;
}

static void xdg_output_handle_logical_size(void *data,
                                           struct zxdg_output_v1 *xdg_output,
                                           int32_t width, int32_t height) {
  struct wooz_output *output = data;

  output->logical_geometry.width = width;
  output->logical_geometry.height = height;
}

static void xdg_output_handle_done(void *data,
                                   struct zxdg_output_v1 *xdg_output) {
  struct wooz_output *output = data;

  // Guess the output scale from the logical size
  int32_t width = output->geometry.width;
  output->logical_scale = (double)width / output->logical_geometry.width;
  output->ratio = (double)output->logical_geometry.width /
                  (double)output->logical_geometry.height;
}

static void xdg_output_handle_name(void *data,
                                   struct zxdg_output_v1 *xdg_output,
                                   const char *name) {
  struct wooz_output *output = data;
  output->name = strdup(name);
}

static void xdg_output_handle_description(void *data,
                                          struct zxdg_output_v1 *xdg_output,
                                          const char *name) {
  // No-op
}

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t physical_width,
                                   int32_t physical_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform) {
  struct wooz_output *output = data;

  output->geometry.x = x;
  output->geometry.y = y;
  output->transform = transform;
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh) {
  struct wooz_output *output = data;

  if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
    output->geometry.width = output->transform ? height : width;
    output->geometry.height = output->transform ? width : height;
  }
}

static void output_handle_done(void *data, struct wl_output *wl_output) {
  // No-op
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t factor) {
  struct wooz_output *output = data;
  output->scale = factor;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
};

// Applies the configured initial zoom, centered on the position given with
// --zoom-at, on the pointer when its position is known, or on the screen
// center.
//
// The wl_pointer position is preferred when the compositor reports it before
// the first configure. Compositors that only report it once the pointer moves
// are covered by the ext-image-copy-capture cursor probe, and the screen center
// is the last resort.
//
// The zoom itself is animated, like any other zoom gesture.
//
// Returns true when the zoom was applied, in which case the window has been
// rendered.
static bool apply_initial_zoom(struct wooz_window *win) {
  if (win->initial_zoom_applied || !win->configured ||
      win->state->config.initial_zoom <= 0.0) {
    return false;
  }

  const struct wooz_config *config = &win->state->config;
  struct wooz_output *output = win->output;
  double zoom_x;
  double zoom_y;

  if (config->zoom_at) {
    zoom_x = config->zoom_at_x;
    zoom_y = config->zoom_at_y;
  } else if (win->pointer_x >= 0.0 && win->pointer_y >= 0.0) {
    zoom_x = win->pointer_x;
    zoom_y = win->pointer_y;
  } else if (output->cursor_known &&
             output->transform == WL_OUTPUT_TRANSFORM_NORMAL) {
    // The cursor probe reports the hotspot in the output's buffer pixels.
    double scale = output->logical_scale > 0.0 ? output->logical_scale : 1.0;
    zoom_x = output->cursor_x / scale;
    zoom_y = output->cursor_y / scale;
  } else {
    zoom_x = output->logical_geometry.width / 2.0;
    zoom_y = output->logical_geometry.height / 2.0;
  }

  double zoom_pixels = output->geometry.height * config->initial_zoom;
  win->zoom.target = win->view_source;
  view_zoom(win, &win->zoom.target, zoom_pixels, zoom_x, zoom_y);
  clamp_view(win, &win->zoom.target);
  win->initial_zoom_applied = true;

  // zoom_start() renders when it starts the animation; render once when the
  // zoom is a no-op (e.g. already at the maximum zoom level).
  if (!zoom_start(win)) {
    render_window(win);
  }

  return true;
}

static void layer_surface_handle_configure(
    void *data, struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
    uint32_t width, uint32_t height) {
  struct wooz_window *win = data;
  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  // Force a render: the destination size below has to be committed.
  win->sampled_valid = false;
  if (width != 0 && height != 0 && win->viewport != NULL) {
    wp_viewport_set_destination(win->viewport, width, height);
  }

  win->configured = true;

  // Zoom onto the pointer if its position is known, and onto the screen center
  // otherwise.
  if (!apply_initial_zoom(win)) {
    render_window(win);
  }
}

static void
layer_surface_handle_closed(void *data,
                            struct zwlr_layer_surface_v1 *layer_surface) {
  struct wooz_window *win = data;
  win->state->n_done = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy) {
  struct wooz_state *state = data;

  struct wooz_window *window;
  wl_list_for_each(window, &state->windows, link) {
    if (window->surface == surface) {
      window->is_focused = true;
      state->focused = window;
      window->pointer_x = wl_fixed_to_double(sx);
      window->pointer_y = wl_fixed_to_double(sy);
    } else {
      window->is_focused = false;
    }
  }

  if (state->cursor_shape_device != NULL) {
    wp_cursor_shape_device_v1_set_shape(
        state->cursor_shape_device, serial,
        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
  }
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface) {
  struct wooz_state *state = data;

  struct wooz_window *window;
  wl_list_for_each(window, &state->windows, link) {
    if (window->surface == surface) {
      window->is_focused = false;
      break;
    }
  }
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
  struct wooz_state *state = data;
  struct wooz_window *win = state->focused;

  if (win == NULL) {
    return;
  }

  double x = wl_fixed_to_double(sx);
  double y = wl_fixed_to_double(sy);

  if (win->pointer_pressed) {
    double scale = win->view_source.width / win->output->logical_geometry.width;

    double dx = (x - win->pointer_x) * scale;
    double dy = (y - win->pointer_y) * scale;

    pan_view(win, -dx, -dy);
    render_window(win);
  } else if (state->config.mouse_track) {
    // Mouse tracking: center viewport on mouse position
    double viewport_center_x = win->pointer_x;
    double viewport_center_y = win->pointer_y;

    double new_center_x = x;
    double new_center_y = y;

    double dx = (new_center_x - viewport_center_x);
    double dy = (new_center_y - viewport_center_y);

    pan_view(win, dx, dy);
    render_window(win);
  }

  win->pointer_x = x;
  win->pointer_y = y;
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer,
                                  uint32_t serial, uint32_t time,
                                  uint32_t button, uint32_t button_state) {
  struct wooz_state *state = data;
  struct wooz_window *win = state->focused;

  if (button == BTN_LEFT) {
    if (button_state == WL_POINTER_BUTTON_STATE_PRESSED) {
      // Check for double-click
      if (win->last_click_button == BTN_LEFT &&
          (time - win->last_click_time) < DOUBLE_CLICK_TIME_MS) {
        // Double-click detected - restore view
        restore_view(win);
        win->last_click_time = 0;
      } else {
        win->last_click_time = time;
        win->last_click_button = button;
      }
      win->pointer_pressed = true;
    } else {
      win->pointer_pressed = false;
    }
  } else if (button == BTN_RIGHT &&
             button_state == WL_POINTER_BUTTON_STATE_RELEASED) {
    win->state->n_done = 0;
  }
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
                                uint32_t time, uint32_t axis,
                                wl_fixed_t value) {

  struct wooz_state *state = data;
  struct wooz_window *win = state->focused;

  struct wooz_boxf *reference = zoom_reference(win);
  double scale = reference->width / win->output->geometry.width;
  // x10 for faster zoom.
  double scroll = wl_fixed_to_double(value) * 2 * scale * 10;

  // Invert scroll if configured
  if (state->config.invert_scroll) {
    scroll = -scroll;
  }

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    zoom_by(win, scroll, win->pointer_x, win->pointer_y);
  }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
};

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                   uint32_t format, int32_t fd, uint32_t size) {
  close(fd);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys) {
  // No-op
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface) {
  // No-op
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t key_state) {
  struct wooz_state *state = data;
  struct wooz_window *win = state->focused;

  if (win == NULL) {
    return;
  }
  if (key_state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    // Stop key repeat when any key is released
    if (state->pressed_key == key) {
      stop_key_repeat(state);
    }
    return;
  }

  // Check for custom close key
  if (state->config.close_key != 0 && key == state->config.close_key) {
    state->n_done = 0;
    return;
  }

  switch (key) {
  case KEY_ESC:
    // Default close behavior (if no custom close key)
    if (state->config.close_key == 0) {
      state->n_done = 0;
    }
    break;

  case KEY_0:
  case KEY_KP0:
    // Restore/unzoom
    restore_view(win);
    break;

  case KEY_A:
    // Toggle anti-aliasing of the magnified region
    win->antialias = !win->antialias;
    render_window(win);
    break;

  case KEY_EQUAL: // For keyboards where + is shift+=
  case KEY_KPPLUS:
  case KEY_MINUS:
  case KEY_KPMINUS:
  case KEY_LEFT:
  case KEY_RIGHT:
  case KEY_UP:
  case KEY_DOWN:
    // Handle the key action immediately
    handle_key_action(state, key);
    // Start key repeat for these keys
    if (is_repeatable_key(key)) {
      start_key_repeat(state, key);
    }
    break;
  }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched,
                                      uint32_t mods_locked, uint32_t group) {
  // No-op
}

static void keyboard_handle_repeat_info(void *data,
                                        struct wl_keyboard *keyboard,
                                        int32_t rate, int32_t delay) {
  // No-op
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
                                     uint32_t capabilities) {
  struct wooz_state *state = data;

  if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0) {
    state->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(state->pointer, &pointer_listener, state);
    if (state->cursor_shape_manager != NULL) {
      state->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
          state->cursor_shape_manager, state->pointer);
    }
  } else {
    if (state->pointer != NULL) {
      wl_pointer_destroy(state->pointer);
      state->pointer = NULL;
    }
    if (state->cursor_shape_device != NULL) {
      wp_cursor_shape_device_v1_destroy(state->cursor_shape_device);
      state->cursor_shape_device = NULL;
    }
  }

  if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0) {
    state->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
  } else {
    if (state->keyboard != NULL) {
      wl_keyboard_destroy(state->keyboard);
      state->keyboard = NULL;
    }
  }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
};

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version) {
  struct wooz_state *state = data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    state->compositor =
        wl_registry_bind(registry, name, &wl_compositor_interface, 5);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
    uint32_t bind_version = (version > 2) ? 2 : version;
    state->xdg_output_manager = wl_registry_bind(
        registry, name, &zxdg_output_manager_v1_interface, bind_version);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    struct wooz_output *output = calloc(1, sizeof(struct wooz_output));
    output->state = state;
    output->scale = 1;
    output->wl_output =
        wl_registry_bind(registry, name, &wl_output_interface, 3);
    wl_output_add_listener(output->wl_output, &output_listener, output);
    wl_list_insert(&state->outputs, &output->link);
  } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) ==
             0) {
    state->screencopy_manager = wl_registry_bind(
        registry, name, &zwlr_screencopy_manager_v1_interface, 1);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    state->layer_shell =
        wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
  } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
    state->viewporter =
        wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    state->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
    wl_seat_add_listener(state->seat, &seat_listener, state);
  } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) ==
             0) {
    state->cursor_shape_manager = wl_registry_bind(
        registry, name, &wp_cursor_shape_manager_v1_interface, 1);
  } else if (strcmp(interface,
                    ext_image_copy_capture_manager_v1_interface.name) == 0) {
    state->image_copy_capture_manager = wl_registry_bind(
        registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
  } else if (strcmp(
                 interface,
                 ext_output_image_capture_source_manager_v1_interface.name) ==
             0) {
    state->output_image_capture_source_manager = wl_registry_bind(
        registry, name, &ext_output_image_capture_source_manager_v1_interface,
        1);
  }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name) {
  // who cares
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static const char usage[] =
    "Usage: wooz [options...]\n"
    "\n"
    "Options:\n"
    "  -h, --help              Show help message and quit\n"
    "  --map-close KEY         Set key to close (e.g., 'Esc', 'q')\n"
    "  --mouse-track           Enable mouse tracking (follow mouse without "
    "clicking)\n"
    "  --output NAME           Run on specific output (e.g., 'DP-1')\n"
    "  --zoom-in PERCENT       Set initial zoom percentage (e.g., '10%', "
    "'50%')\n"
    "  --zoom-at X,Y           Zoom at a position instead of the cursor "
    "(output\n"
    "  --invert-scroll         Invert scroll direction (scroll up zooms in)\n"
    "  --no-aa                 Start with anti-aliasing disabled "
    "(nearest-neighbor)\n"
    "                          logical coordinates)\n"
    "\n"
    "Controls:\n"
    "  Mouse scroll            Zoom in/out at mouse position\n"
    "  Left click + drag       Pan the view\n"
    "  Right click             Exit\n"
    "  Double click            Restore/unzoom\n"
    "  +/-                     Zoom in/out at center\n"
    "  Arrow keys              Pan the view\n"
    "  0                       Restore/unzoom\n"
    "  a                       Toggle anti-aliasing (nearest-neighbor when "
    "off)\n"
    "  Esc                     Exit (default)\n";

static bool should_include_output(struct wooz_output *output,
                                  const char *filter) {
  // If no filter is set, include all outputs
  if (filter == NULL) {
    return true;
  }

  // If output doesn't have a name yet, we can't filter it
  if (output->name == NULL) {
    return false;
  }

  // Check if the output name matches the filter
  return strcmp(output->name, filter) == 0;
}

static void cursor_session_handle_enter(
    void *data, struct ext_image_copy_capture_cursor_session_v1 *session) {
  // A position event follows, nothing to do here.
}

static void cursor_session_handle_leave(
    void *data, struct ext_image_copy_capture_cursor_session_v1 *session) {
  struct wooz_output *output = data;

  output->cursor_known = false;
}

static void cursor_session_handle_position(
    void *data, struct ext_image_copy_capture_cursor_session_v1 *session,
    int32_t x, int32_t y) {
  struct wooz_output *output = data;

  output->cursor_x = x;
  output->cursor_y = y;
  output->cursor_known = true;
}

static void cursor_session_handle_hotspot(
    void *data, struct ext_image_copy_capture_cursor_session_v1 *session,
    int32_t x, int32_t y) {
  // The hotspot of the cursor image is irrelevant for placing the view.
}

static const struct ext_image_copy_capture_cursor_session_v1_listener
    cursor_session_listener = {
        .enter = cursor_session_handle_enter,
        .leave = cursor_session_handle_leave,
        .position = cursor_session_handle_position,
        .hotspot = cursor_session_handle_hotspot,
};

// Sets up a cursor probe per output, so that the initial zoom can be centered
// on the pointer without wl_pointer (which only reports its position to the
// focused surface, and some compositors only do so once the pointer moves).
// Best effort: compositors without ext-image-copy-capture keep using the
// wl_pointer position and, failing that, the screen center.
static void setup_cursor_probes(struct wooz_state *state) {
  if (state->config.zoom_at || state->image_copy_capture_manager == NULL ||
      state->output_image_capture_source_manager == NULL ||
      state->pointer == NULL) {
    return;
  }

  struct wooz_output *output;
  wl_list_for_each(output, &state->outputs, link) {
    if (!should_include_output(output, state->config.output_filter)) {
      continue;
    }

    output->cursor_source =
        ext_output_image_capture_source_manager_v1_create_source(
            state->output_image_capture_source_manager, output->wl_output);
    if (output->cursor_source == NULL) {
      continue;
    }

    output->cursor_session =
        ext_image_copy_capture_manager_v1_create_pointer_cursor_session(
            state->image_copy_capture_manager, output->cursor_source,
            state->pointer);
    if (output->cursor_session == NULL) {
      continue;
    }

    ext_image_copy_capture_cursor_session_v1_add_listener(
        output->cursor_session, &cursor_session_listener, output);
  }

  // Receive the initial cursor positions before the windows are created.
  wl_display_roundtrip(state->display);

  // Only the startup position is needed, so drop the probes again: capture
  // sessions shouldn't stay open for the lifetime of the magnifier.
  wl_list_for_each(output, &state->outputs, link) {
    if (output->cursor_session != NULL) {
      ext_image_copy_capture_cursor_session_v1_destroy(output->cursor_session);
      output->cursor_session = NULL;
    }
    if (output->cursor_source != NULL) {
      ext_image_capture_source_v1_destroy(output->cursor_source);
      output->cursor_source = NULL;
    }
  }
}

static uint32_t parse_key_name(const char *name) {
  if (strcmp(name, "Esc") == 0 || strcmp(name, "Escape") == 0) {
    return KEY_ESC;
  } else if (strcmp(name, "q") == 0 || strcmp(name, "Q") == 0) {
    return KEY_Q;
  } else if (strcmp(name, "x") == 0 || strcmp(name, "X") == 0) {
    return KEY_X;
  }
  return 0; // Invalid key
}

int main(int argc, char *argv[]) {
  struct wooz_config config = {0};
  // Compositor scaling is bilinear: anti-aliasing is on unless disabled.
  config.antialias = true;

  static struct option long_options[] = {
      {"help", no_argument, 0, 'h'},
      {"map-close", required_argument, 0, 'c'},
      {"mouse-track", no_argument, 0, 'm'},
      {"output", required_argument, 0, 'o'},
      {"zoom-in", required_argument, 0, 'z'},
      {"invert-scroll", no_argument, 0, 'i'},
      {"no-aa", no_argument, 0, 'A'},
      {"zoom-at", required_argument, 0, 'Z'},
      {0, 0, 0, 0}};

  int opt;
  int option_index = 0;
  while ((opt = getopt_long(argc, argv, "h", long_options, &option_index)) !=
         -1) {
    switch (opt) {
    case 'h':
      printf("%s", usage);
      return EXIT_SUCCESS;
    case 'c': {
      config.close_key = parse_key_name(optarg);
      if (config.close_key == 0) {
        fprintf(stderr, "Invalid key name: %s (supported: Esc, q, x)\n",
                optarg);
        return EXIT_FAILURE;
      }
      break;
    }
    case 'm':
      config.mouse_track = true;
      break;
    case 'o':
      config.output_filter = strdup(optarg);
      break;
    case 'z': {
      char *endptr;
      double zoom = strtod(optarg, &endptr);
      if (*endptr == '%') {
        config.initial_zoom = zoom / 100.0;
      } else {
        config.initial_zoom = zoom;
      }
      if (config.initial_zoom < 0.0 || config.initial_zoom >= 1.0) {
        fprintf(stderr, "Invalid zoom percentage: %s (must be 0-99%%)\n",
                optarg);
        return EXIT_FAILURE;
      }
      break;
    }
    case 'i':
      config.invert_scroll = true;
      break;
    case 'A':
      config.antialias = false;
      break;
    case 'Z': {
      double x;
      double y;
      int consumed = 0;

      if (sscanf(optarg, " %lf , %lf %n", &x, &y, &consumed) != 2 ||
          optarg[consumed] != '\0') {
        fprintf(stderr, "Invalid position: %s (expected 'x,y')\n", optarg);
        return EXIT_FAILURE;
      }

      config.zoom_at = true;
      config.zoom_at_x = x;
      config.zoom_at_y = y;
      break;
    }
    default:
      fprintf(stderr, "%s", usage);
      return EXIT_FAILURE;
    }
  }

  struct wooz_state state = {0};
  state.config = config;
  state.repeat_timer_fd = -1;
  wl_list_init(&state.outputs);
  wl_list_init(&state.windows);

  state.display = wl_display_connect(NULL);
  if (state.display == NULL) {
    fprintf(stderr, "failed to create display\n");
    return EXIT_FAILURE;
  }

  state.registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(state.registry, &registry_listener, &state);
  if (wl_display_roundtrip(state.display) < 0) {
    fprintf(stderr, "wl_display_roundtrip() failed\n");
    return EXIT_FAILURE;
  }

  if (state.compositor == NULL) {
    fprintf(stderr, "wl_compositor is missing\n");
    return EXIT_FAILURE;
  }
  if (state.layer_shell == NULL) {
    fprintf(stderr, "compositor doesn't support wlr-layer-shell\n");
    return EXIT_FAILURE;
  }
  if (state.shm == NULL) {
    fprintf(stderr, "compositor doesn't support wl_shm\n");
    return EXIT_FAILURE;
  }
  if (state.screencopy_manager == NULL) {
    fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
    return EXIT_FAILURE;
  }
  if (state.viewporter == NULL) {
    fprintf(stderr, "compositor doesn't support viewporter\n");
    return EXIT_FAILURE;
  }
  if (state.seat == NULL) {
    fprintf(stderr, "compositor doesn't support seat\n");
    return EXIT_FAILURE;
  }
  if (wl_list_empty(&state.outputs)) {
    fprintf(stderr, "no wl_output\n");
    return EXIT_FAILURE;
  }

  if (state.xdg_output_manager != NULL) {
    struct wooz_output *output;
    wl_list_for_each(output, &state.outputs, link) {
      output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
          state.xdg_output_manager, output->wl_output);
      zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener,
                                  output);
    }

    if (wl_display_roundtrip(state.display) < 0) {
      fprintf(stderr, "wl_display_roundtrip() failed\n");
      return EXIT_FAILURE;
    }
  } else {
    fprintf(stderr, "warning: zxdg_output_manager_v1 isn't available, "
                    "guessing the output layout\n");

    struct wooz_output *output;
    wl_list_for_each(output, &state.outputs, link) {
      guess_output_logical_geometry(output);
    }
  }

  setup_cursor_probes(&state);

  size_t n_pending = 0;
  struct wooz_output *output;
  wl_list_for_each(output, &state.outputs, link) {
    // Skip outputs that don't match the filter
    if (!should_include_output(output, state.config.output_filter)) {
      continue;
    }

    output->screencopy_frame = zwlr_screencopy_manager_v1_capture_output(
        state.screencopy_manager, false, output->wl_output);
    zwlr_screencopy_frame_v1_add_listener(output->screencopy_frame,
                                          &screencopy_frame_listener, output);

    ++n_pending;
  }

  if (n_pending == 0) {
    if (state.config.output_filter != NULL) {
      fprintf(stderr, "no output found matching '%s'\n",
              state.config.output_filter);
    } else {
      fprintf(stderr, "no outputs found\n");
    }
    return EXIT_FAILURE;
  }

  bool done = false;
  while (!done && wl_display_dispatch(state.display) != -1) {
    done = (state.n_done == n_pending);
  }
  if (!done) {
    fprintf(stderr, "failed to screenshoot all outputs\n");
    return EXIT_FAILURE;
  }

  wl_list_for_each(output, &state.outputs, link) {
    // Skip outputs that don't match the filter
    if (!should_include_output(output, state.config.output_filter)) {
      continue;
    }

    struct wooz_window *win = calloc(1, sizeof(struct wooz_window));
    wl_list_insert(&state.windows, &win->link);
    win->state = &state;
    // On by default; --no-aa starts with nearest-neighbor sampling instead.
    win->antialias = state.config.antialias;
    win->output = output;
    // The pointer position is unknown until the compositor reports it.
    win->pointer_x = -1.0;
    win->pointer_y = -1.0;
    win->surface = wl_compositor_create_surface(state.compositor);
    win->viewport = wp_viewporter_get_viewport(state.viewporter, win->surface);
    win->view_source = (struct wooz_boxf){
        .x = (double)output->geometry.x,
        .y = (double)output->geometry.y,
        .width = (double)output->geometry.width,
        .height = (double)output->geometry.height,
    };
    // Store initial view for restore/unzoom
    win->initial_view_source = win->view_source;

    if (win->surface == NULL) {
      fprintf(stderr, "failed to create wayland surface\n");
      return EXIT_FAILURE;
    }

    wl_surface_set_buffer_transform(win->surface, output->transform);

    win->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.layer_shell, win->surface, output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wooz");
    zwlr_layer_surface_v1_add_listener(win->layer_surface,
                                       &layer_surface_listener, win);
    zwlr_layer_surface_v1_set_anchor(win->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    // No exclusive zone: cover the entire output, including areas
    // occupied by other layers (e.g. panels)
    zwlr_layer_surface_v1_set_exclusive_zone(win->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        win->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    wl_surface_commit(win->surface);
  }

  state.n_done = 1;

  // Main event loop with timer support
  int wl_fd = wl_display_get_fd(state.display);
  struct pollfd fds[2];

  while (state.n_done) {
    // Prepare events before dispatching
    while (wl_display_prepare_read(state.display) != 0) {
      wl_display_dispatch_pending(state.display);
    }
    wl_display_flush(state.display);

    // Set up polling
    fds[0].fd = wl_fd;
    fds[0].events = POLLIN;
    fds[1].fd = state.repeat_timer_fd;
    fds[1].events = POLLIN;

    int nfds = (state.repeat_timer_fd >= 0) ? 2 : 1;

    if (poll(fds, nfds, -1) < 0) {
      wl_display_cancel_read(state.display);
      break;
    }

    // Handle timer events
    if (nfds > 1 && (fds[1].revents & POLLIN)) {
      uint64_t expirations;
      read(state.repeat_timer_fd, &expirations, sizeof(expirations));
      if (state.pressed_key != 0) {
        handle_key_action(&state, state.pressed_key);
      }
    }

    // Handle Wayland events
    if (fds[0].revents & POLLIN) {
      wl_display_read_events(state.display);
      wl_display_dispatch_pending(state.display);
    } else {
      wl_display_cancel_read(state.display);
    }
  }

  // Clean up key repeat timer
  if (state.repeat_timer_fd >= 0) {
    stop_key_repeat(&state);
    close(state.repeat_timer_fd);
  }

  struct wooz_window *win;
  struct wooz_window *window_tmp;
  wl_list_for_each_safe(win, window_tmp, &state.windows, link) {
    output = win->output;

    // Free window.
    wl_list_remove(&win->link);
    if (win->layer_surface != NULL)
      zwlr_layer_surface_v1_destroy(win->layer_surface);
    if (win->viewport != NULL)
      wp_viewport_destroy(win->viewport);
    if (win->zoom.frame_callback != NULL)
      wl_callback_destroy(win->zoom.frame_callback);
    if (win->surface != NULL)
      wl_surface_destroy(win->surface);
    for (int i = 0; i < WOOZ_SCALED_BUFFER_COUNT; ++i) {
      destroy_buffer(win->scaled_buffers[i]);
    }
    free(win->column_map);
    free(win->runs);
    free(win);

    // Free output.
    wl_list_remove(&output->link);
    if (output->name != NULL)
      free(output->name);
    if (output->screencopy_frame != NULL) {
      zwlr_screencopy_frame_v1_destroy(output->screencopy_frame);
    }
    destroy_buffer(output->buffer);
    if (output->xdg_output != NULL) {
      zxdg_output_v1_destroy(output->xdg_output);
    }
    wl_output_release(output->wl_output);
    free(output);
  }
  zwlr_screencopy_manager_v1_destroy(state.screencopy_manager);
  if (state.image_copy_capture_manager != NULL) {
    ext_image_copy_capture_manager_v1_destroy(state.image_copy_capture_manager);
  }
  if (state.output_image_capture_source_manager != NULL) {
    ext_output_image_capture_source_manager_v1_destroy(
        state.output_image_capture_source_manager);
  }
  if (state.xdg_output_manager != NULL) {
    zxdg_output_manager_v1_destroy(state.xdg_output_manager);
  }
  if (state.cursor_shape_device) {
    wp_cursor_shape_device_v1_destroy(state.cursor_shape_device);
  }
  if (state.cursor_shape_manager) {
    wp_cursor_shape_manager_v1_destroy(state.cursor_shape_manager);
  }
  if (state.pointer) {
    wl_pointer_release(state.pointer);
  }
  if (state.keyboard) {
    wl_keyboard_release(state.keyboard);
  }
  wl_seat_release(state.seat);
  zwlr_layer_shell_v1_destroy(state.layer_shell);
  wp_viewporter_destroy(state.viewporter);
  wl_shm_destroy(state.shm);
  wl_registry_destroy(state.registry);
  wl_compositor_destroy(state.compositor);
  wl_display_disconnect(state.display);

  // Free config resources
  if (state.config.output_filter != NULL) {
    free(state.config.output_filter);
  }

  return EXIT_SUCCESS;
}
