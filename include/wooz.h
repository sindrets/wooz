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
  uint32_t
      close_key;    // Linux input event code for close action (0 = default Esc)
  bool mouse_track; // Enable mouse tracking
  double initial_zoom; // Initial zoom percentage (0.0 = no zoom, 0.1 = 10%)
  char *output_filter; // Filter to specific output name (NULL = all outputs)
  bool invert_scroll;  // Invert scroll direction (scroll up zooms in)
  bool antialias;      // Enable anti-aliasing of the magnified region
  // Initial zoom position in output logical coordinates, instead of the cursor.
  bool zoom_at;
  double zoom_at_x, zoom_at_y;
};

struct wooz_state {
  struct wl_compositor *compositor;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_shm *shm;
  struct zxdg_output_manager_v1 *xdg_output_manager;
  struct zwlr_screencopy_manager_v1 *screencopy_manager;
  // Optional protocols (ext-image-copy-capture) used to locate the pointer.
  struct ext_image_copy_capture_manager_v1 *image_copy_capture_manager;
  struct ext_output_image_capture_source_manager_v1
      *output_image_capture_source_manager;
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

  // Cursor probe, used to read the pointer position on this output without
  // relying on pointer focus. Only set up when the compositor supports
  // ext-image-copy-capture.
  struct ext_image_capture_source_v1 *cursor_source;
  struct ext_image_copy_capture_cursor_session_v1 *cursor_session;
  bool cursor_known;
  int32_t cursor_x, cursor_y; // hotspot, in output buffer (physical) pixels
};

// Animated zoom state: the view glides from `from` to `target` over
// `duration_ms`, following an easing curve.
struct wooz_zoom {
  struct wooz_boxf from;
  struct wooz_boxf target;
  double start_ms;
  double duration_ms;
  bool active;
  struct wl_callback *frame_callback; // Paces the animation with repaints
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
  struct wooz_zoom zoom;
  struct wooz_boxf initial_view_source; // For restore/unzoom

  // Mouse pointer position if window is focused.
  double pointer_x;
  double pointer_y;
  bool pointer_pressed;

  // Double-click detection
  uint32_t last_click_time;
  uint32_t last_click_button;

  bool is_focused;
  bool configured; // A layer surface configure has been received
  bool initial_zoom_applied;
  // Client-side nearest-neighbor rendering of the magnified region, used when
  // anti-aliasing is disabled.
  struct wooz_buffer *scaled_buffers[WOOZ_SCALED_BUFFER_COUNT];
  int scaled_buffer_index;
  bool antialias;

  // Destination-x -> source-x mapping for the nearest-neighbor sampler.
  int32_t *column_map;
  int32_t column_map_dest_width;

  // Source rectangle and sampling grid of the attached buffer, used to skip
  // resampling when a view change doesn't move the grid by a destination pixel.
  int32_t sampled_x, sampled_y;
  double sampled_phase_x, sampled_phase_y;
  double sampled_step_x, sampled_step_y;
  int32_t sampled_buffer_width, sampled_buffer_height;
  bool sampled_valid;
};

#endif
