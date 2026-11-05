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
  char *output_filter; // Filter to specific output name (NULL = current output)
  bool output_is_name;
  bool invert_scroll;  // Invert scroll direction (scroll up zooms out)
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
  struct wp_viewporter *viewporter;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct zwp_pointer_gestures_v1 *pointer_gestures;
  struct zwp_pointer_gesture_pinch_v1 *pinch;
  // Axis source of the current frame (enum wl_pointer_axis_source):
  // WL_POINTER_AXIS_SOURCE_FINGER pans, anything else zooms.
  uint32_t axis_source;
  // Depressed keyboard modifiers (enum wl_keyboard_modifier).
  uint32_t modifiers;
  // Optional protocol (cursor-shape-v1) used to hide the pointer while the
  // screen is copied and to restore it afterwards.
  struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
  struct wp_cursor_shape_device_v1 *cursor_shape_device;
  struct wl_keyboard *keyboard;
  struct wl_list outputs;
  struct wl_list windows;

  struct wooz_window *focused;
  struct wooz_config config;
  struct wooz_pool
      *sampler_pool; // Worker pool for the nearest-neighbor sampler

  // Key repeat state
  uint32_t pressed_key;
  int repeat_timer_fd;

  // Latest wl_pointer.enter serial: required to set the cursor image.
  bool enter_serial_valid;
  uint32_t enter_serial;

  // The windows are mapped with a transparent placeholder while the first
  // frames are being copied: the compositor only gives pointer focus, and with
  // it the serial needed to hide the pointer, once the surface has a buffer.
  bool windows_pending;
  struct wl_buffer *placeholder_buffer;
  // Set when the pointer is found on one of the probe surfaces while wooz is
  // looking for the current output.
  struct wooz_window *probe_pick;
  bool probe_resolved;
  // The probes could not tell, so the windows show on every output after all.
  bool show_all_outputs;
  // Copies requested, and how many of them have completed.
  size_t n_capture, n_done;
  bool running;
  bool capture_cursor_hidden;
  // The pointer moved since the copies were requested, so they may show it: it
  // then stays hidden instead of being restored (see capture_restore_cursor()).
  bool input_during_capture;
};

struct wooz_buffer;
struct wooz_pool;

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
  double logical_scale;  // guessed from the logical size
  char *xdg_output_name; // from zxdg_output_v1.name, matched by --output

  struct wooz_buffer *buffer;
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

// A run of destination pixels that all sample the same source column.
struct wooz_run {
  int32_t x;      // First destination pixel of the run
  int32_t column; // Source column the run samples
};
struct wooz_window {
  struct wooz_state *state;
  struct wooz_output *output;
  struct wl_list link;

  struct zwlr_layer_surface_v1 *layer_surface;
  struct wp_viewport *viewport;
  struct wl_surface *surface;
  struct zwlr_screencopy_frame_v1 *screencopy_frame;

  // Viewport source rectangle.
  struct wooz_boxf view_source;
  struct wooz_zoom zoom;
  struct wooz_boxf initial_view_source; // For restore/unzoom

  // Mouse pointer position if window is focused.
  double pointer_x;
  double pointer_y;
  bool pointer_pressed;

  // Pinch gesture (touchpad) state. The view is derived from pinch_base and the
  // fingers' translation, both measured from the start of the gesture.
  bool pinch_active;
  struct wooz_boxf pinch_base;
  // Source pixel under the fingers when the gesture began, and the logical
  // position it began at.
  double pinch_anchor_x, pinch_anchor_y;
  double pinch_center_x, pinch_center_y;
  double pinch_dx, pinch_dy;

  // Double-click detection
  uint32_t last_click_time;
  uint32_t last_click_button;

  // The pointer is on this window: it has pointer focus, and it was the
  // current output when wooz started.
  bool pointer_is_over;
  bool configured; // A layer surface configure has been received
  // The surface shows the placeholder buffer instead of a copied frame.
  bool placeholder;
  // The frame has been copied and can be shown, see capture_start().
  bool frame_ready;
  // true while running output / cursor probes
  bool probing;
  bool initial_zoom_applied;

  // Where `wl_pointer.enter` reported the pointer while the placeholder was
  // mapped: on the screens the pointer is on, those coordinates are its
  // position.
  bool pointer_entered;
  double pointer_enter_x, pointer_enter_y;

  // Client-side nearest-neighbor rendering of the magnified region, used when
  // anti-aliasing is disabled.
  struct wooz_buffer *scaled_buffers[WOOZ_SCALED_BUFFER_COUNT];
  int scaled_buffer_index;
  bool antialias;

  // Destination-x -> source-x mapping for the nearest-neighbor sampler.
  int32_t *column_map;
  int32_t column_map_dest_width;

  // Runs of destination pixels sampling the same source column, used instead of
  // the column map at high magnification (see RUN_FILL_MIN_STEP).
  struct wooz_run *runs;
  int32_t runs_capacity, run_count;

  // Source rectangle and sampling grid of the attached buffer, used to skip
  // resampling when a view change doesn't move the grid by a destination pixel.
  int32_t sampled_x, sampled_y;
  double sampled_phase_x, sampled_phase_y;
  double sampled_step_x, sampled_step_y;
  int32_t sampled_buffer_width, sampled_buffer_height;
  enum wl_shm_format sampled_format;
  bool sampled_valid;
};

#endif
