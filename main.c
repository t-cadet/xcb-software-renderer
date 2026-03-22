// gcc -std=c23 -O2 -g -Wall -Wextra main.c -lxcb -lxcb-dri3 -lxcb-present -lxcb-cursor -lxcb-render -lxcb-sync -lxcb-keysyms -o main

// To improve:
//  - Use XKB extension to handle keyboard events (better layout reload, dead keys & modes)
//  - Optimize rendering when the window is hidden
//  - Implement adaptive frame-pacing
//  - Handle windows larger than screen, multiple monitors, monitor change etc
//  - Close all resources in DIE?

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <linux/udmabuf.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>
#include <xcb/xcb_cursor.h>
#include <xcb/xcb_keysyms.h>

// #include <X11/keysymdef.h>

#define BUFFER_COUNT 2
#define PRESENT_COMPLETE_NOTIFY_TIMEOUT 50

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0xFF000000

#define COLOR_SCAN_LINE    0xFFFF3A5C
#define COLOR_STRIPE_A     0xFF071C2C
#define COLOR_STRIPE_B     0xFFFF6B35
#define COLOR_DT_GRAPH     0xFF00E5CC
#define COLOR_BUTTON       0xFF7B2FBE
#define COLOR_BUTTON_HOVER 0xFFA855F7

const char* DRI3_EXTENSION_NAME = "DRI3";
const char* PRESENT_EXTENSION_NAME = "Present";
const char* RENDER_EXTENSION_NAME = "RENDER";
const char* SYNC_EXTENSION_NAME = "SYNC";

uint8_t DRI3_MAJOR_OPCODE = 0;
uint8_t PRESENT_MAJOR_OPCODE = 0;
uint8_t RENDER_MAJOR_OPCODE = 0;
uint8_t SYNC_MAJOR_OPCODE = 0;

#define SYNC_DESIRED_MAJOR_VERSION 3
#define SYNC_DESIRED_MINOR_VERSION 1

#define ARRAY_SIZE(xs) (sizeof(xs)/sizeof((xs)[0]))

#define DIE(connection, message) die(connection, message, __FILE__, __LINE__)
void die(xcb_connection_t *connection, const char *message, const char* file, int line) {
  fprintf(stderr, "%s:%d: [ERROR] %s\n", file, line, message);
  if (connection) xcb_disconnect(connection);
  exit(1);
}

#define FRAME_DT_GRAPH_SIZE 120
float FRAME_DT_GRAPH[FRAME_DT_GRAPH_SIZE] = {0};

typedef enum Cursor {
  Cursor_Default = 0,
  Cursor_Pointer,
  Cursor_Text,

  Cursor_Size,
} Cursor;

const char* get_cursor_name(Cursor cursor) {
  switch (cursor) {
    case Cursor_Default: return "default";
    case Cursor_Pointer: return "pointer";
    case Cursor_Text: return "text";
    default: {
      DIE(NULL, "get_cursor_name");
      return NULL;
    }
  }
}

typedef struct App {
  int width, height, stride;

  bool paused;
  int stripes_frame;

  int mouse_x, mouse_y;
  Cursor cursor;
} App;

typedef struct Events {
  bool clicked;
  char key;
} Events;

void draw_rectangle(App *app, uint32_t *buf, int start_x, int start_y, int width, int height, int stride, int color) {
  int end_x = start_x + width;
  int end_y = start_y + height;

  if (end_x > app->width) end_x = app->width;
  if (end_y > app->height) end_y = app->height;

  if (start_x < 0) start_x = 0;
  if (start_y < 0) start_y = 0;

  for (int y = start_y; y < end_y; ++y) {
    for (int x = start_x; x < end_x; ++x) {
      buf[y*stride + x] = color;
    }
  }
}

void render(App *app, uint32_t *buf, Events* events, int frame, float processing_dt) {
  // Clear
  // draw_rectangle(app, buf, 0, 0, app->width, app->height, app->stride, BLACK);
  app->cursor = Cursor_Default;

  // Tear test
  int stripes_speed = 3;
  int stripes_width = 64;
  for (int y = 0; y < app->height; y++) {
    for (int x = 0; x < app->width; x++) {
      int stripe_pos = (x + app->stripes_frame*stripes_speed) % (stripes_width*2);
      uint32_t color = (stripe_pos < stripes_width) ? COLOR_STRIPE_A : COLOR_STRIPE_B;
      buf[y*app->stride + x] = color;
    }
  }
  int scan_line_y = (app->stripes_frame % 120)*app->height/120;
  draw_rectangle(app, buf, 0, scan_line_y, app->width, 1, app->stride, COLOR_SCAN_LINE);
  if (!app->paused) app->stripes_frame += 1;

  // Frame dt graph
  FRAME_DT_GRAPH[frame % FRAME_DT_GRAPH_SIZE] = processing_dt;

  int frame_base_width = app->width / FRAME_DT_GRAPH_SIZE;
  int rem_width = app->width % FRAME_DT_GRAPH_SIZE;

  int FPS_AT_MAX_HEIGHT = 60;
  int half_height = app->height / 2;

  for (int y = 0; y < half_height; ++y) {
    for (int frame_index=0, frame_start_x=0; frame_index < FRAME_DT_GRAPH_SIZE; ++frame_index) {

      int frame_dt_index = (frame + 1 + frame_index) % FRAME_DT_GRAPH_SIZE;
      int frame_height = FRAME_DT_GRAPH[frame_dt_index]*half_height*FPS_AT_MAX_HEIGHT;
      if (frame_height > half_height) frame_height = half_height;

      int frame_width_with_rem = frame_base_width + ((frame_index < rem_width) ? 1 : 0);

      if (y >= (half_height - frame_height)) {
        for (int x = frame_start_x; x < frame_start_x + frame_width_with_rem - 1; ++x) {
          buf[y*app->stride + x] = COLOR_DT_GRAPH;
        }
      }

      frame_start_x += frame_width_with_rem;
    }
  }

  // Pause Button
  int button_w = 80;
  int button_h = 50;
  int button_x = (app->width - button_w)/2;
  int button_y = (app->height*3 - button_h*2)/4;
  int button_color = COLOR_BUTTON;

  if ((app->mouse_x >= button_x && app->mouse_x < (button_x + button_w)) &&
      (app->mouse_y >= button_y && app->mouse_y < (button_y + button_h))) {
    button_color = COLOR_BUTTON_HOVER;
    app->cursor = Cursor_Pointer;
    if (events->clicked) {
      app->paused = !app->paused;
    }
  }

  draw_rectangle(app, buf, button_x, button_y, button_w, button_h, app->stride, button_color);

  // Pause Key
  if (events->key == ' ') {
    app->paused = !app->paused;
  }
}

const char* xcb_connect_error_to_string(int error_code) {
  switch (error_code) {
    case 0: return "XCB_CONN_OK: not an error";
    case XCB_CONN_ERROR: return "XCB_CONN_ERROR: xcb connection errors because of socket, pipe and other stream errors";
    case XCB_CONN_CLOSED_EXT_NOTSUPPORTED: return "XCB_CONN_CLOSED_EXT_NOTSUPPORTED: xcb connection shutdown because of extension not supported";
    case XCB_CONN_CLOSED_MEM_INSUFFICIENT: return "XCB_CONN_CLOSED_MEM_INSUFFICIENT: malloc(), calloc() and realloc() error upon failure, for eg ENOMEM";
    case XCB_CONN_CLOSED_REQ_LEN_EXCEED: return "XCB_CONN_CLOSED_REQ_LEN_EXCEED: Connection closed, exceeding request length that server accepts";
    case XCB_CONN_CLOSED_PARSE_ERR: return "XCB_CONN_CLOSED_PARSE_ERR: Connection closed, error during parsing display string";
    case XCB_CONN_CLOSED_INVALID_SCREEN: return "XCB_CONN_CLOSED_INVALID_SCREEN: Connection closed because the server does not have a screen matching the display";
    case XCB_CONN_CLOSED_FDPASSING_FAILED: return "XCB_CONN_CLOSED_FDPASSING_FAILED: Connection closed because some FD passing operation failed";
    default: return "XCB_CONN_UNKNOWN: unknown error, this is probably a bug";
  }
}

const char* xcb_error_code_to_string(uint8_t error_code) {
  // xproto.h
  switch (error_code) {
    case XCB_REQUEST:        return "BadRequest";
    case XCB_VALUE:          return "BadValue";
    case XCB_WINDOW:         return "BadWindow";
    case XCB_PIXMAP:         return "BadPixmap";
    case XCB_ATOM:           return "BadAtom";
    case XCB_CURSOR:         return "BadCursor";
    case XCB_FONT:           return "BadFont";
    case XCB_MATCH:          return "BadMatch";
    case XCB_DRAWABLE:       return "BadDrawable";
    case XCB_ACCESS:         return "BadAccess";
    case XCB_ALLOC:          return "BadAlloc";
    case XCB_COLORMAP:       return "BadColormap";
    case XCB_G_CONTEXT:      return "BadGContext";
    case XCB_ID_CHOICE:      return "BadIDChoice";
    case XCB_NAME:           return "BadName";
    case XCB_LENGTH:         return "BadLength";
    case XCB_IMPLEMENTATION: return "BadImplementation";
    default:                 return "Unknown";
  }
}

const char* xcb_major_code_to_string(uint8_t major_code) {
  // xproto.h
  switch (major_code) {
    case XCB_CREATE_WINDOW:   return "CreateWindow";
    case XCB_MAP_WINDOW:      return "MapWindow";
    case XCB_CHANGE_PROPERTY: return "ChangeProperty";
    default: {
      if (major_code == DRI3_MAJOR_OPCODE) return DRI3_EXTENSION_NAME;
      else if (major_code == PRESENT_MAJOR_OPCODE) return PRESENT_EXTENSION_NAME;
      else if (major_code == RENDER_MAJOR_OPCODE) return RENDER_EXTENSION_NAME;
      else if (major_code == SYNC_MAJOR_OPCODE) return SYNC_EXTENSION_NAME;
      return "Unknown";
    }
  }
}

const char* xcb_dri3_minor_code_to_string(uint8_t minor_code) {
  // dri3.h
  switch (minor_code) {
    case XCB_DRI3_QUERY_VERSION:      return "QueryVersion";
    case XCB_DRI3_PIXMAP_FROM_BUFFER: return "PixmapFromBuffer";
    default: return "Unknown";
  }
}

const char* xcb_present_minor_code_to_string(uint8_t minor_code) {
  // present.h
  switch (minor_code) {
    case XCB_PRESENT_QUERY_VERSION: return "QueryVersion";
    case XCB_PRESENT_PIXMAP:        return "Pixmap";
    case XCB_PRESENT_NOTIFY_MSC:    return "NotifyMSC";
    case XCB_PRESENT_SELECT_INPUT:  return "SelectInput";
    default: return "Unknown";
  }
}

const char* xcb_render_minor_code_to_string(uint8_t minor_code) {
  // render.h
  switch (minor_code) {
    case XCB_RENDER_QUERY_VERSION:      return "QueryVersion";
    case XCB_RENDER_QUERY_PICT_FORMATS: return "QueryPictFormats";
    case XCB_RENDER_CREATE_PICTURE:     return "CreatePicture";
    case XCB_RENDER_FREE_PICTURE:       return "FreePicture";
    case XCB_RENDER_CREATE_CURSOR:      return "CreateCursor";
    case XCB_RENDER_CREATE_ANIM_CURSOR: return "CreateAnimCursor";
    default: return "Unknown";
  }
}

const char* xcb_sync_minor_code_to_string(uint8_t minor_code) {
  // sync.h
  switch (minor_code) {
    case XCB_SYNC_INITIALIZE:      return "Initialize";
    case XCB_SYNC_CREATE_COUNTER:  return "CreateCounter";
    case XCB_SYNC_SET_COUNTER:     return "SetCounter";
    case XCB_SYNC_DESTROY_COUNTER: return "DestroyCounter";
    default: return "Unknown";
  }
}

const char* xcb_minor_code_to_string(uint8_t major_code, uint8_t minor_code) {
  if (major_code == DRI3_MAJOR_OPCODE) return xcb_dri3_minor_code_to_string(minor_code);
  if (major_code == PRESENT_MAJOR_OPCODE) return xcb_present_minor_code_to_string(minor_code);
  if (major_code == RENDER_MAJOR_OPCODE) return xcb_render_minor_code_to_string(minor_code);
  if (major_code == SYNC_MAJOR_OPCODE) return xcb_sync_minor_code_to_string(minor_code);
  return "";
}

void xcb_print_error(xcb_generic_error_t *error) {
  const char* error_string = xcb_error_code_to_string(error->error_code);
  const char* major_error_string = xcb_major_code_to_string(error->major_code);
  const char* minor_error_string = xcb_minor_code_to_string(error->major_code, error->minor_code);
  fprintf(stderr, "[ERROR]: sequence=%d: %s:%s:%s (code=%d:%d:%d)\n",
          error->sequence, major_error_string, minor_error_string, error_string,
          error->major_code, error->minor_code, error->error_code);
}

void xcb_print_event(const xcb_generic_event_t *event) {
  if (!event) {
    fprintf(stderr, "[XCB_EVENT] NULL event\n");
    return;
  }

  uint8_t type      = event->response_type & 0x7F;
  int     synthetic = (event->response_type & 0x80) != 0;

  switch (type) {
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE: {
      const xcb_key_press_event_t *e = (const xcb_key_press_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] %s synthetic=%d time=%u keycode=%u state=0x%04X"
                      " win=0x%08X root_xy=(%d,%d) event_xy=(%d,%d)\n",
              type == XCB_KEY_PRESS ? "KeyPress" : "KeyRelease",
              synthetic, e->time, e->detail, e->state,
              e->event, e->root_x, e->root_y, e->event_x, e->event_y);
    } break;
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
      const xcb_button_press_event_t *e = (const xcb_button_press_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] %s synthetic=%d time=%u button=%u state=0x%04X"
                      " win=0x%08X root_xy=(%d,%d) event_xy=(%d,%d)\n",
              type == XCB_BUTTON_PRESS ? "ButtonPress" : "ButtonRelease",
              synthetic, e->time, e->detail, e->state,
              e->event, e->root_x, e->root_y, e->event_x, e->event_y);
    } break;
    case XCB_MOTION_NOTIFY: {
      const xcb_motion_notify_event_t *e = (const xcb_motion_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] MotionNotify synthetic=%d time=%u hint=%d"
                      " state=0x%04X win=0x%08X event_xy=(%d,%d)\n",
              synthetic, e->time, e->detail, e->state,
              e->event, e->event_x, e->event_y);
    } break;
    case XCB_ENTER_NOTIFY:
    case XCB_LEAVE_NOTIFY: {
      const xcb_enter_notify_event_t *e = (const xcb_enter_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] %s synthetic=%d time=%u mode=%d detail=%d"
                      " win=0x%08X child=0x%08X event_xy=(%d,%d)\n",
              type == XCB_ENTER_NOTIFY ? "EnterNotify" : "LeaveNotify",
              synthetic, e->time, e->mode, e->detail,
              e->event, e->child, e->event_x, e->event_y);
    } break;
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: {
      const xcb_focus_in_event_t *e = (const xcb_focus_in_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] %s synthetic=%d mode=%d detail=%d win=0x%08X\n",
              type == XCB_FOCUS_IN ? "FocusIn" : "FocusOut",
              synthetic, e->mode, e->detail, e->event);
    } break;
    case XCB_KEYMAP_NOTIFY: {
      const xcb_keymap_notify_event_t *e = (const xcb_keymap_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] KeymapNotify synthetic=%d keys[0]=0x%02X ...\n",
              synthetic, e->keys[0]);
    } break;
    case XCB_EXPOSE: {
      const xcb_expose_event_t *e = (const xcb_expose_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] Expose synthetic=%d win=0x%08X"
                      " xy=(%d,%d) wh=(%d,%d) count=%d\n",
              synthetic, e->window, e->x, e->y, e->width, e->height, e->count);
    } break;
    case XCB_GRAPHICS_EXPOSURE: {
      const xcb_graphics_exposure_event_t *e = (const xcb_graphics_exposure_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] GraphicsExpose synthetic=%d drawable=0x%08X"
                      " xy=(%d,%d) wh=(%d,%d) count=%d\n",
              synthetic, e->drawable, e->x, e->y, e->width, e->height, e->count);
    } break;
    case XCB_NO_EXPOSURE: {
      const xcb_no_exposure_event_t *e = (const xcb_no_exposure_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] NoExpose synthetic=%d drawable=0x%08X minor=%u major=%u\n",
              synthetic, e->drawable, e->minor_opcode, e->major_opcode);
    } break;
    case XCB_VISIBILITY_NOTIFY: {
      const xcb_visibility_notify_event_t *e = (const xcb_visibility_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] VisibilityNotify synthetic=%d win=0x%08X state=%d\n",
              synthetic, e->window, e->state);
    } break;
    case XCB_CREATE_NOTIFY: {
      const xcb_create_notify_event_t *e = (const xcb_create_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] CreateNotify synthetic=%d parent=0x%08X win=0x%08X"
                      " xy=(%d,%d) wh=(%d,%d) border=%d\n",
              synthetic, e->parent, e->window,
              e->x, e->y, e->width, e->height, e->border_width);
    } break;
    case XCB_DESTROY_NOTIFY: {
      const xcb_destroy_notify_event_t *e = (const xcb_destroy_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] DestroyNotify synthetic=%d event=0x%08X win=0x%08X\n",
              synthetic, e->event, e->window);
    } break;
    case XCB_UNMAP_NOTIFY: {
      const xcb_unmap_notify_event_t *e = (const xcb_unmap_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] UnmapNotify synthetic=%d event=0x%08X win=0x%08X"
                      " from_configure=%d\n",
              synthetic, e->event, e->window, e->from_configure);
    } break;
    case XCB_MAP_NOTIFY: {
      const xcb_map_notify_event_t *e = (const xcb_map_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] MapNotify synthetic=%d event=0x%08X win=0x%08X"
                      " override_redirect=%d\n",
              synthetic, e->event, e->window, e->override_redirect);
    } break;
    case XCB_MAP_REQUEST: {
      const xcb_map_request_event_t *e = (const xcb_map_request_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] MapRequest synthetic=%d parent=0x%08X win=0x%08X\n",
              synthetic, e->parent, e->window);
    } break;
    case XCB_REPARENT_NOTIFY: {
      const xcb_reparent_notify_event_t *e = (const xcb_reparent_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] ReparentNotify synthetic=%d event=0x%08X win=0x%08X"
                      " parent=0x%08X xy=(%d,%d)\n",
              synthetic, e->event, e->window, e->parent, e->x, e->y);
    } break;
    case XCB_CONFIGURE_NOTIFY: {
      const xcb_configure_notify_event_t *e = (const xcb_configure_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] ConfigureNotify synthetic=%d win=0x%08X"
                      " xy=(%d,%d) wh=(%d,%d) border=%d above=0x%08X\n",
              synthetic, e->window,
              e->x, e->y, e->width, e->height, e->border_width, e->above_sibling);
    } break;
    case XCB_CONFIGURE_REQUEST: {
      const xcb_configure_request_event_t *e = (const xcb_configure_request_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] ConfigureRequest synthetic=%d win=0x%08X"
                      " xy=(%d,%d) wh=(%d,%d) border=%d mask=0x%04X\n",
              synthetic, e->window,
              e->x, e->y, e->width, e->height, e->border_width, e->value_mask);
    } break;
    case XCB_GRAVITY_NOTIFY: {
      const xcb_gravity_notify_event_t *e = (const xcb_gravity_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] GravityNotify synthetic=%d event=0x%08X win=0x%08X"
                      " xy=(%d,%d)\n",
              synthetic, e->event, e->window, e->x, e->y);
    } break;
    case XCB_RESIZE_REQUEST: {
      const xcb_resize_request_event_t *e = (const xcb_resize_request_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] ResizeRequest synthetic=%d win=0x%08X wh=(%d,%d)\n",
              synthetic, e->window, e->width, e->height);
    } break;
    case XCB_CIRCULATE_NOTIFY: {
      const xcb_circulate_notify_event_t *e = (const xcb_circulate_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] CirculateNotify synthetic=%d event=0x%08X win=0x%08X"
                      " place=%d\n",
              synthetic, e->event, e->window, e->place);
    } break;
    case XCB_CIRCULATE_REQUEST: {
      const xcb_circulate_request_event_t *e = (const xcb_circulate_request_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] CirculateRequest synthetic=%d event=0x%08X win=0x%08X"
                      " place=%d\n",
              synthetic, e->event, e->window, e->place);
    } break;
    case XCB_PROPERTY_NOTIFY: {
      const xcb_property_notify_event_t *e = (const xcb_property_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] PropertyNotify synthetic=%d win=0x%08X"
                      " atom=%u state=%d time=%u\n",
              synthetic, e->window, e->atom, e->state, e->time);
    } break;
    case XCB_SELECTION_CLEAR: {
      const xcb_selection_clear_event_t *e = (const xcb_selection_clear_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] SelectionClear synthetic=%d owner=0x%08X"
                      " selection=%u time=%u\n",
              synthetic, e->owner, e->selection, e->time);
    } break;
    case XCB_SELECTION_REQUEST: {
      const xcb_selection_request_event_t *e = (const xcb_selection_request_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] SelectionRequest synthetic=%d owner=0x%08X"
                      " requestor=0x%08X selection=%u target=%u property=%u time=%u\n",
              synthetic, e->owner, e->requestor,
              e->selection, e->target, e->property, e->time);
    } break;
    case XCB_SELECTION_NOTIFY: {
      const xcb_selection_notify_event_t *e = (const xcb_selection_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] SelectionNotify synthetic=%d requestor=0x%08X"
                      " selection=%u target=%u property=%u time=%u\n",
              synthetic, e->requestor,
              e->selection, e->target, e->property, e->time);
    } break;
    case XCB_COLORMAP_NOTIFY: {
      const xcb_colormap_notify_event_t *e = (const xcb_colormap_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] ColormapNotify synthetic=%d win=0x%08X"
                      " colormap=%u new=%d state=%d\n",
              synthetic, e->window, e->colormap, e->_new, e->state);
    } break;
    case XCB_CLIENT_MESSAGE: {
      const xcb_client_message_event_t *e = (const xcb_client_message_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] ClientMessage synthetic=%d win=0x%08X"
                      " type=%u format=%d data[0]=0x%08X\n",
              synthetic, e->window, e->type, e->format, e->data.data32[0]);
    } break;
    case XCB_MAPPING_NOTIFY: {
      const xcb_mapping_notify_event_t *e = (const xcb_mapping_notify_event_t *)event;
      fprintf(stderr, "[XCB_EVENT] MappingNotify synthetic=%d request=%d"
                      " first_keycode=%u count=%u\n",
              synthetic, e->request, e->first_keycode, e->count);
    } break;
    default: {
      fprintf(stderr, "[XCB_EVENT] Unknown type=%d synthetic=%d\n", type, synthetic);
    } break;
  }
}

#define XCB_INTERN_ATOM_REPLY_OR_DIE(connection, cookie) xcb_intern_atom_reply_or_die(connection, cookie, #cookie, __FILE__, __LINE__)
xcb_atom_t xcb_intern_atom_reply_or_die(xcb_connection_t *connection, xcb_intern_atom_cookie_t cookie, const char* die_message, const char* file, int line) {
  xcb_generic_error_t *error = NULL;
  xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, &error);
  if (error) {
    xcb_print_error(error);
    free(error);
    die(connection, die_message, file, line);
  }
  if (!reply) die(connection, die_message, file, line);
  xcb_atom_t atom = reply->atom;
  free(reply);
  return atom;
}

#define XCB_QUERY_EXTENSION_MAJOR_OPCODE_OR_DIE(connection, cookie) xcb_query_extension_reply_or_die(connection, cookie, #cookie, __FILE__, __LINE__)
uint8_t xcb_query_extension_reply_or_die(xcb_connection_t *connection, xcb_query_extension_cookie_t cookie, const char* die_message, const char* file, int line) {
  xcb_generic_error_t *error = NULL;
  xcb_query_extension_reply_t *reply = xcb_query_extension_reply(connection, cookie, &error);
  if (error) {
    xcb_print_error(error);
    free(error);
    die(connection, die_message, file, line);
  }
  if (!reply) die(connection, die_message, file, line);
  if (!reply->present) die(connection, die_message, file, line);
  uint8_t major_opcode = reply->major_opcode;
  free(reply);
  return major_opcode;
}

int main() {
  App app = {0};
  
  // Connect to X11 server
  int preferred_screen = 0;
  xcb_connection_t *connection = xcb_connect(NULL, &preferred_screen);
  int xcb_connect_error_code = xcb_connection_has_error(connection);
  if (xcb_connect_error_code > 0) DIE(connection, xcb_connect_error_to_string(xcb_connect_error_code));

  // Prefetch extension data
  xcb_query_extension_cookie_t query_extension_dri3_cookie = xcb_query_extension(connection, strlen(DRI3_EXTENSION_NAME), DRI3_EXTENSION_NAME);
  xcb_query_extension_cookie_t query_extension_present_cookie = xcb_query_extension(connection, strlen(PRESENT_EXTENSION_NAME), PRESENT_EXTENSION_NAME);
  xcb_query_extension_cookie_t query_extension_sync_cookie = xcb_query_extension(connection, strlen(SYNC_EXTENSION_NAME), SYNC_EXTENSION_NAME);
  xcb_sync_initialize_cookie_t xcb_sync_initialize_cookie = xcb_sync_initialize(connection, SYNC_DESIRED_MAJOR_VERSION, SYNC_DESIRED_MINOR_VERSION);

  // Prefetch atoms
  bool only_if_exists = false;
  xcb_intern_atom_cookie_t wm_protocols_cookie = xcb_intern_atom(connection, only_if_exists, strlen("WM_PROTOCOLS"), "WM_PROTOCOLS");
  xcb_intern_atom_cookie_t wm_delete_window_cookie = xcb_intern_atom(connection, only_if_exists, strlen("WM_DELETE_WINDOW"), "WM_DELETE_WINDOW");
  xcb_intern_atom_cookie_t wm_sync_request_cookie = xcb_intern_atom(connection, only_if_exists, strlen("_NET_WM_SYNC_REQUEST"), "_NET_WM_SYNC_REQUEST");
  xcb_intern_atom_cookie_t wm_sync_request_counter_cookie = xcb_intern_atom(connection, only_if_exists, strlen("_NET_WM_SYNC_REQUEST_COUNTER"), "_NET_WM_SYNC_REQUEST_COUNTER");

  // Prefetch keyboard mapping
  xcb_key_symbols_t *key_symbols = xcb_key_symbols_alloc(connection);
  if (!key_symbols) DIE(connection, "xcb_key_symbols_alloc");

  xcb_flush(connection);

  // Find preferred screen
  int screen_i = 0;
  const xcb_setup_t *setup = xcb_get_setup(connection);
  xcb_screen_iterator_t screen_it = xcb_setup_roots_iterator(setup);
  while (screen_it.rem && screen_i < preferred_screen) {
    ++screen_i;
    xcb_screen_next(&screen_it);
  }
  if (screen_i != preferred_screen) DIE(connection, "cannot find preferred screen");
  xcb_screen_t *screen = screen_it.data;

  // Find visual
  xcb_visualid_t visual = 0;
  int depth = 24;
  int visual_class = XCB_VISUAL_CLASS_TRUE_COLOR;
  for (xcb_depth_iterator_t depth_it = xcb_screen_allowed_depths_iterator(screen); depth_it.rem; xcb_depth_next(&depth_it)) {
    if (depth_it.data->depth == depth) {
      for (xcb_visualtype_iterator_t visual_it = xcb_depth_visuals_iterator(depth_it.data); visual_it.rem; xcb_visualtype_next(&visual_it)) {
        if (visual_it.data->_class == visual_class) {
          visual = visual_it.data->visual_id;
          break;
        }
      }
    }
  }
  if (!visual) DIE(connection, "cannot find required visual");

  // Create cursors
  xcb_cursor_t cursors[Cursor_Size] = {0};
  xcb_cursor_context_t *cursor_context = NULL;
  int xcb_cursor_context_new_error = xcb_cursor_context_new(connection, screen, &cursor_context);
  if (xcb_cursor_context_new_error < 0) {
    DIE(connection, strerror(-xcb_cursor_context_new_error));
  }
  for (int cursor_index = 0; cursor_index < Cursor_Size; ++cursor_index) {
    cursors[cursor_index] = xcb_cursor_load_cursor(cursor_context, get_cursor_name(cursor_index));
    if (!cursors[cursor_index]) DIE(connection, "xcb_cursor_load_cursor");
  }
  const xcb_query_extension_reply_t *query_extension_render_reply = xcb_get_extension_data(connection, &xcb_render_id);
  if (!query_extension_render_reply) DIE(connection, "xcb_get_extension_data: render");
  RENDER_MAJOR_OPCODE = query_extension_render_reply->major_opcode;
  xcb_cursor_context_free(cursor_context);

  // Create window
  app.width = screen->width_in_pixels/2;
  app.height = screen->height_in_pixels/2;
  int x = (screen->width_in_pixels - app.width)/2;
  int y = (screen->height_in_pixels - app.height)/2;
  int border_width = 0;
  int class = XCB_WINDOW_CLASS_INPUT_OUTPUT;
  uint32_t value_mask = XCB_CW_BACK_PIXMAP | XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
  xcb_create_window_value_list_t value_list = {0};
  value_list.background_pixmap = XCB_BACK_PIXMAP_NONE;
  value_list.bit_gravity = XCB_GRAVITY_STATIC;
  value_list.event_mask = XCB_EVENT_MASK_KEY_PRESS
                        | XCB_EVENT_MASK_KEY_RELEASE
                        | XCB_EVENT_MASK_BUTTON_PRESS
                        | XCB_EVENT_MASK_BUTTON_RELEASE
                        // | XCB_EVENT_MASK_ENTER_WINDOW
                        // | XCB_EVENT_MASK_LEAVE_WINDOW
                        | XCB_EVENT_MASK_POINTER_MOTION
                        // | XCB_EVENT_MASK_KEYMAP_STATE
                        | XCB_EVENT_MASK_VISIBILITY_CHANGE
                        | XCB_EVENT_MASK_STRUCTURE_NOTIFY
                        // | XCB_EVENT_MASK_FOCUS_CHANGE
                        ;
  value_list.cursor = cursors[app.cursor];
  if (visual != screen->root_visual) {
    value_mask |= XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP;
    value_list.border_pixel = screen->black_pixel;
    value_list.colormap = xcb_generate_id(connection);
    xcb_create_colormap(connection, XCB_COLORMAP_ALLOC_NONE, value_list.colormap, screen->root, visual);
  }
  xcb_window_t window = xcb_generate_id(connection);
  xcb_create_window_aux(connection, depth, window, screen->root, x, y, app.width, app.height, border_width, class, visual, value_mask, &value_list);

  // Set window title
  const char* title = "XCB Software Renderer";
  uint8_t format = 8;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, format, strlen(title), title);
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_ICON_NAME, XCB_ATOM_STRING, format, strlen(title), title);

  // Retrieve atoms & register for window manager protocols
  xcb_atom_t wm_protocols = XCB_INTERN_ATOM_REPLY_OR_DIE(connection, wm_protocols_cookie);
  xcb_atom_t wm_delete_window = XCB_INTERN_ATOM_REPLY_OR_DIE(connection, wm_delete_window_cookie);
  xcb_atom_t wm_sync_request = XCB_INTERN_ATOM_REPLY_OR_DIE(connection, wm_sync_request_cookie);
  xcb_atom_t wm_sync_request_counter = XCB_INTERN_ATOM_REPLY_OR_DIE(connection, wm_sync_request_counter_cookie);

  xcb_atom_t wm_protocol_atoms[] = { wm_delete_window, wm_sync_request };
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, wm_protocols, XCB_ATOM_ATOM, sizeof(xcb_atom_t)*8, ARRAY_SIZE(wm_protocol_atoms), wm_protocol_atoms);

  // Retrieve extension major opcode
  DRI3_MAJOR_OPCODE = XCB_QUERY_EXTENSION_MAJOR_OPCODE_OR_DIE(connection, query_extension_dri3_cookie);
  PRESENT_MAJOR_OPCODE = XCB_QUERY_EXTENSION_MAJOR_OPCODE_OR_DIE(connection, query_extension_present_cookie);
  SYNC_MAJOR_OPCODE = XCB_QUERY_EXTENSION_MAJOR_OPCODE_OR_DIE(connection, query_extension_sync_cookie);

  // Create buffers
  int stride = (screen->width_in_pixels * 4 + 63) & ~63;
  app.stride = stride / 4;

  int page_size = getpagesize();
  int size = (stride * screen->height_in_pixels + (page_size - 1)) & ~(page_size - 1);

  int udmabuf_device = open("/dev/udmabuf", O_RDWR);
  if (udmabuf_device < 0) DIE(connection, strerror(errno));

  uint32_t *buffers[BUFFER_COUNT] = {0};
  int udmabufs[BUFFER_COUNT] = {0};
  xcb_pixmap_t pixmaps[BUFFER_COUNT] = {0};

  for (int i = 0; i < BUFFER_COUNT; ++i) {
    int memfd = memfd_create("buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (memfd < 0) DIE(connection, strerror(errno));

    if (ftruncate(memfd, size) < 0)
      DIE(connection, strerror(errno));

    if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) == -1)
      DIE(connection, strerror(errno));

    buffers[i] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (buffers[i] == MAP_FAILED) DIE(connection, strerror(errno));
    memset(buffers[i], 0, size);

    struct udmabuf_create create = {0};
    create.memfd = memfd;
    create.size = size;
    create.flags = UDMABUF_FLAGS_CLOEXEC;
    udmabufs[i] = ioctl(udmabuf_device, UDMABUF_CREATE, &create);
    if (udmabufs[i] == -1) DIE(connection, strerror(errno));

    uint8_t bpp = 32;
    pixmaps[i] = xcb_generate_id(connection);
    xcb_dri3_pixmap_from_buffer(connection, pixmaps[i], window, size, screen->width_in_pixels, screen->height_in_pixels, stride, depth, bpp, udmabufs[i]);

    close(memfd);
  }

  close(udmabuf_device);

  // Special event queue for Present events
  xcb_present_event_t present_event = xcb_generate_id(connection);
  xcb_present_select_input(connection, present_event, window, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
  xcb_special_event_t *special_event = xcb_register_for_special_xge(connection, &xcb_present_id, present_event, NULL);
  if (!special_event) DIE(connection, "xcb_register_for_special_xge");

  int epollfd = epoll_create1(EPOLL_CLOEXEC);
  if (epollfd == -1) DIE(connection, strerror(errno));

  struct epoll_event epoll_event = {0};
  struct epoll_event epoll_event_out = {0};
  epoll_event.events = EPOLLIN;
  epoll_event.data.fd = xcb_get_file_descriptor(connection);

  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, epoll_event.data.fd, &epoll_event) == -1)
    DIE(connection, strerror(errno));

  // Setup Sync extension
  xcb_generic_error_t *xcb_sync_initialize_reply_error = NULL;
  xcb_sync_initialize_reply_t *sync_initialize_reply = xcb_sync_initialize_reply(connection, xcb_sync_initialize_cookie, &xcb_sync_initialize_reply_error);
  if (xcb_sync_initialize_reply_error) {
    xcb_print_error(xcb_sync_initialize_reply_error);
    free(xcb_sync_initialize_reply_error);
    DIE(connection, "xcb_sync_initialize_reply_error");
  }
  if (!sync_initialize_reply) DIE(connection, "sync_initialize_reply");
  if (sync_initialize_reply->major_version  < SYNC_DESIRED_MAJOR_VERSION ||
     (sync_initialize_reply->major_version == SYNC_DESIRED_MAJOR_VERSION && 
      sync_initialize_reply->minor_version  < SYNC_DESIRED_MINOR_VERSION)) {
    DIE(connection, "sync_initialize_reply: version mismatch");
  }
  free(sync_initialize_reply);

  xcb_sync_counter_t sync_counter = xcb_generate_id(connection);
  xcb_sync_int64_t zero_sync_value = {0, 0};
  xcb_sync_create_counter(connection, sync_counter, zero_sync_value);
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, wm_sync_request_counter, XCB_ATOM_CARDINAL, sizeof(sync_counter)*8, 1, &sync_counter);

  // Show window
  xcb_map_window(connection, window);

  // App outputs
  Cursor cursor = app.cursor;

  // Event loop state
  bool quit = false;
  int current_buffer = 0;
  int frame_count = 0;
  float processing_dt = 0.0;

  xcb_sync_int64_t pending_sync_value = zero_sync_value;
  xcb_sync_int64_t acknowledged_sync_value = zero_sync_value;

  while (!quit) {
    struct timespec start_time = {0};
    struct timespec end_time = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &start_time) == -1)
      DIE(connection, strerror(errno));

    int xcb_connection_error_code = xcb_connection_has_error(connection);
    if (xcb_connection_error_code > 0) DIE(connection, xcb_connect_error_to_string(xcb_connection_error_code));

    // Handle events
    Events app_events = {0};
    xcb_generic_event_t *event = NULL;
    while ((event = xcb_poll_for_event(connection))) {
      if (event->response_type == 0) {
        xcb_generic_error_t *error = (xcb_generic_error_t *)event;
        xcb_print_error(error);
        DIE(connection, "xcb_print_error");
      } else {
        xcb_print_event(event);
        switch (event->response_type & 0x7F) {
          case XCB_CONFIGURE_NOTIFY: {
            const xcb_configure_notify_event_t *e = (const xcb_configure_notify_event_t *)event;
            app.width = e->width;
            app.height = e->height;
            if (app.width > screen->width_in_pixels) {
              fprintf(stderr, "[WARNING] cannot resize window to be larger than screen, clamping to screen width\n");
              app.width = screen->width_in_pixels;
            }
            if (app.height > screen->height_in_pixels) {
              fprintf(stderr, "[WARNING] cannot resize window to be taller than screen, clamping to screen height\n");
              app.height = screen->height_in_pixels;
            }
            acknowledged_sync_value = pending_sync_value;
            pending_sync_value = zero_sync_value;
          } break;
          case XCB_CLIENT_MESSAGE: {
            const xcb_client_message_event_t *e = (const xcb_client_message_event_t *)event;
            if (e->type == wm_protocols) {
              if(e->data.data32[0] == wm_delete_window) {
                fprintf(stderr, "Received WM_DELETE_WINDOW, quitting...\n");
                quit = true;
              } else if (e->data.data32[0] == wm_sync_request) {
                pending_sync_value.lo = e->data.data32[2];
                pending_sync_value.hi = e->data.data32[3];
              }
            }
          } break;
          case XCB_MAPPING_NOTIFY: {
            xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *)event;
            xcb_refresh_keyboard_mapping(key_symbols, e);
          } break;
          case XCB_KEY_PRESS: {
            xcb_key_press_event_t *e = (xcb_key_press_event_t *)event;
            // Note: blocks if the reply from a mapping refresh has yet to arrive
            xcb_keysym_t keysym = xcb_key_press_lookup_keysym(key_symbols, e, 0);
            if (keysym >= ' ' && keysym <= 255) {
              app_events.key = keysym;
            }
          } break;
          case XCB_BUTTON_PRESS: {
            const xcb_button_press_event_t *e = (const xcb_button_press_event_t *)event;
            app_events.clicked = (e->detail == 1);
          } break;
          case XCB_MOTION_NOTIFY: {
            const xcb_motion_notify_event_t *e = (const xcb_motion_notify_event_t *)event;
            app.mouse_x = e->event_x;
            app.mouse_y = e->event_y;
          } break;
        }
      }
      free(event);
    }

    // Render
    render(&app, buffers[current_buffer], &app_events, frame_count, processing_dt);

    uint32_t serial = current_buffer;
    xcb_xfixes_region_t valid = 0;
    xcb_xfixes_region_t update = 0;
    int16_t x_off = 0;
    int16_t y_off = 0;
    xcb_randr_crtc_t target_crtc = 0;
    xcb_sync_fence_t wait_fence = 0;
    xcb_sync_fence_t idle_fence = 0;
    uint32_t options = XCB_PRESENT_OPTION_NONE;
    uint64_t target_msc = 0;
    uint64_t divisor = 0;
    uint64_t remainder = 0;
    uint32_t notifies_len = 0;
    const xcb_present_notify_t *notifies = NULL;
    xcb_present_pixmap(connection, window, pixmaps[current_buffer], serial,
                      valid, update, x_off, y_off, target_crtc, wait_fence, idle_fence, options,
                      target_msc, divisor, remainder, notifies_len, notifies);

    if (cursor != app.cursor) {
      xcb_change_window_attributes_value_list_t attributes = {.cursor = cursors[app.cursor]};
      xcb_change_window_attributes_aux(connection, window, XCB_CW_CURSOR, &attributes);
      cursor = app.cursor;
      fprintf(stderr, "Changed cursor.\n");
    }

    xcb_flush(connection);

    if (clock_gettime(CLOCK_MONOTONIC, &end_time) == -1)
      DIE(connection, strerror(errno));
    processing_dt = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec)*1e-9;

    // V-Sync
    for (;;) {
      int ready_fds_count = epoll_wait(epollfd, &epoll_event_out, 1, PRESENT_COMPLETE_NOTIFY_TIMEOUT);
      if (ready_fds_count > 0) {
        xcb_generic_event_t *extension_event = NULL;
        while ((extension_event = xcb_poll_for_special_event(connection, special_event))) {
          if ((extension_event->response_type & ~0x80) == XCB_GE_GENERIC) {
            // xcb_print_event(extension_event);
            xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)extension_event;
            if (ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
              free(ge);
              goto sync_end;
            }
          } else if (extension_event->response_type == 0) {
            xcb_generic_error_t *error = (xcb_generic_error_t *)extension_event;
            xcb_print_error(error);
            DIE(connection, "xcb_print_error");
          }
          free(extension_event);
        }
      } else if (ready_fds_count == 0) {
        // Reached timeout. Errors are handled
        // at the beginning of the next event loop.
        break;
      } else {
        if (errno == EINTR) continue;
        DIE(connection, strerror(errno));
      }
    }
  sync_end:

    if (acknowledged_sync_value.lo != 0 || acknowledged_sync_value.hi != 0) {
      xcb_sync_set_counter(connection, sync_counter, acknowledged_sync_value);
      acknowledged_sync_value = zero_sync_value;
    }

  #if 0
    struct timespec sync_end_time = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &sync_end_time) == -1)
      DIE(connection, strerror(errno));
    float sync_dt = (sync_end_time.tv_sec - start_time.tv_sec) + (sync_end_time.tv_nsec - start_time.tv_nsec)*1e-9;
    fprintf(stderr, "--- FRAME %d DONE: %0.2f ms, sync: %0.2f ms ---\n", frame_count, processing_dt*1e3, sync_dt*1e3);
  #endif

    ++frame_count;
    current_buffer = (current_buffer + 1) % BUFFER_COUNT;
  }
  
  for (int i = 0; i < BUFFER_COUNT; ++i) {
    if (buffers[i]) munmap(buffers[i], size);
    if (udmabufs[i] >= 0) close(udmabufs[i]);
  }
  xcb_sync_destroy_counter(connection, sync_counter);
  xcb_unregister_for_special_event(connection, special_event);
  xcb_key_symbols_free(key_symbols);
  xcb_disconnect(connection);
  return 0;
}
