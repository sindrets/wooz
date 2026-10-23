#ifndef _WOOZ_H
#define _WOOZ_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-client.h>

#include "box.h"

// Number of buffers used to render the magnified region client-side (when
// anti-aliasing is disabled). Using two buffers avoids writing to memory the
// compositor is currently reading.
#define WOOZ_SCALED_BUFFER_COUNT 2

struct wooz_config {
  uint32_t close_key; // Linux input event code for close action (0 = default Esc)
  bool mouse_track;   // Enable mouse tracking
  double initial_zoom; // Initial zoom percentage (0.0 = no zoom, 0.1 = 10%)
  char *output_filter; // Filter to specific output name (NULL = all outputs)
  bool invert_scroll; // Invert scroll direction (scroll up zooms in)
  bool antialias;     // Enable anti-aliasing of the magnified region
};

struct wooz_state {
  struct wl_compositor *compositor;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct zxdg_output_manager_v1 *xdg_output_manager;
  struct zwlr_screencopy_manager_v1 *screencopy_manager;
  struct wp_viewporter *viewporter;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
  struct wp_cursor_shape_device_v1 *cursor_shape_device;
  struct wl_keyboard *keyboard;
  struct wl_list outputs;
  struct wl_list windows;

  struct wooz_window *focused;
  struct wooz_config config;

  // Key repeat state
  uint32_t pressed_key;
  int repeat_timer_fd;

  size_t n_done;
};

struct wooz_buffer;

struct wooz_output {
  struct wooz_state *state;
  struct wl_output *wl_output;
  struct zxdg_output_v1 *xdg_output;
  struct wl_list link;

  struct wooz_box geometry;
  enum wl_output_transform transform;
  int32_t scale;
  double ratio;

  struct wooz_box logical_geometry;
  double logical_scale; // guessed from the logical size
  char *name;

  struct wooz_buffer *buffer;
  struct zwlr_screencopy_frame_v1 *screencopy_frame;
  uint32_t screencopy_frame_flags; // enum zwlr_screencopy_frame_v1_flags
};

struct wooz_window {
  struct wooz_state *state;
  struct wooz_output *output;
  struct wl_list link;

  struct zwlr_layer_surface_v1 *layer_surface;
  struct wp_viewport *viewport;
  struct wl_surface *surface;

  // Viewport source rectangle.
  struct wooz_boxf view_source;
  struct wooz_boxf initial_view_source; // For restore/unzoom

  // Mouse pointer position if window is focused.
  double pointer_x;
  double pointer_y;
  bool pointer_pressed;

  // Double-click detection
  uint32_t last_click_time;
  uint32_t last_click_button;

  bool is_focused;
  bool initial_zoom_applied;
  // Client-side nearest-neighbor rendering of the magnified region, used when
  // anti-aliasing is disabled.
  struct wooz_buffer *scaled_buffers[WOOZ_SCALED_BUFFER_COUNT];
  int scaled_buffer_index;
  bool antialias;

  // Cached destination-x -> source-x mapping for the nearest-neighbor sampler
  // (only depends on the sampled region and buffer widths).
  int32_t *column_map;
  int32_t column_map_src_width, column_map_dest_width;

  // Source rectangle of the sampled buffer that is currently attached, used to
  // skip resampling when a pan doesn't change what is sampled.
  int32_t sampled_x, sampled_y, sampled_width, sampled_height;
  int32_t sampled_buffer_width, sampled_buffer_height;
  bool sampled_valid;
};

#endif
