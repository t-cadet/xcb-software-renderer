// gcc -std=c23 -O0 -g -Wall main.c -lxcb -lxcb-dri3 -lxcb-present -lxcb-cursor -lxcb-render -o main

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/udmabuf.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb_cursor.h>

#define BUFFER_COUNT 2
#define RED 0xFFFF0000
#define WHITE 0xFFFFFFFF
#define BLACK 0xFF000000

const char* DRI3_EXTENSION_NAME = "DRI3";
const char* PRESENT_EXTENSION_NAME = "Present";
const char* RENDER_EXTENSION_NAME = "RENDER";

uint8_t DRI3_MAJOR_OPCODE = 0;
uint8_t PRESENT_MAJOR_OPCODE = 0;
uint8_t RENDER_MAJOR_OPCODE = 0;

#define DIE(connection, message) die(connection, message, __FILE__, __LINE__)
void die(xcb_connection_t *connection, const char *message, const char* file, int line) {
  fprintf(stderr, "%s:%d: [ERROR] %s\n", file, line, message);
  if (connection) xcb_disconnect(connection);
  exit(1);
}

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
  int width, height;
  Cursor cursor;
} App;

void render(App *app, uint32_t *buf, int frame) {
  if (frame == 300) app->cursor = Cursor_Text;

  for (int y = 0; y < app->height; y++) {
    for (int x = 0; x < app->width; x++) {
      int idx = y * app->width + x;
      int stripe_pos = (x + frame * 5) % 100;
      uint32_t color = (stripe_pos < 50) ? WHITE : BLACK;
      if (y == (frame * 10) % app->height) {
        color = RED;
      }
      buf[idx] = color;
    }
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

const char* xcb_minor_code_to_string(uint8_t major_code, uint8_t minor_code) {
  if (major_code == DRI3_MAJOR_OPCODE) return xcb_dri3_minor_code_to_string(minor_code);
  if (major_code == PRESENT_MAJOR_OPCODE) return xcb_present_minor_code_to_string(minor_code);
  if (major_code == RENDER_MAJOR_OPCODE) return xcb_render_minor_code_to_string(minor_code);
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

int main() {
  App app = {0};
  
  // Connect to X11 server
  int preferred_screen = 0;
  xcb_connection_t *connection = xcb_connect(NULL, &preferred_screen);
  int xcb_connect_error_code = xcb_connection_has_error(connection);
  if (xcb_connect_error_code > 0) DIE(connection, xcb_connect_error_to_string(xcb_connect_error_code));

  // Prefetch extension data (no need to check extension versions as we only use v1.0 requests)
  xcb_query_extension_cookie_t query_extension_dri3_cookie = xcb_query_extension(connection, strlen(DRI3_EXTENSION_NAME), DRI3_EXTENSION_NAME);
  xcb_query_extension_cookie_t query_extension_present_cookie = xcb_query_extension(connection, strlen(PRESENT_EXTENSION_NAME), PRESENT_EXTENSION_NAME);
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
  int x = (screen->width_in_pixels - app.width)/2;
  int y = (screen->height_in_pixels - app.height)/2;
  app.width = screen->width_in_pixels/2;
  app.height = screen->height_in_pixels/2;
  int border_width = 0;
  int class = XCB_WINDOW_CLASS_INPUT_OUTPUT;
  uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
  xcb_create_window_value_list_t value_list = {0};
  value_list.background_pixel = screen->black_pixel;
  // TODO: more events
  value_list.event_mask = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS;
  value_list.cursor = cursors[app.cursor];
  if (visual != screen->root_visual) {
    value_mask |= XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP;
    value_list.border_pixel = screen->black_pixel;
    value_list.colormap = xcb_generate_id(connection);
    // TODO: check these args
    xcb_create_colormap(connection, XCB_COLORMAP_ALLOC_NONE, value_list.colormap, screen->root, visual);
  }
  xcb_window_t window = xcb_generate_id(connection);
  xcb_create_window_aux(connection, depth, window, screen->root, x, y, app.width, app.height, border_width, class, visual, value_mask, &value_list);

  // Set window title
  const char* title = "XCB Software Renderer";
  uint8_t format = 8;
  xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, format, strlen(title), title);

  // TODO: register atom to close window

  // Retrieve extension major opcode
  // TODO: factor code
  xcb_generic_error_t *query_extension_dri3_error = NULL;
  xcb_generic_error_t *query_extension_present_error = NULL;

  xcb_query_extension_reply_t *query_extension_dri3_reply = xcb_query_extension_reply(connection, query_extension_dri3_cookie, &query_extension_dri3_error);
  xcb_query_extension_reply_t *query_extension_present_reply = xcb_query_extension_reply(connection, query_extension_present_cookie, &query_extension_present_error);

  if (query_extension_dri3_error) {
    xcb_print_error(query_extension_dri3_error);
    free(query_extension_dri3_error);
    DIE(connection, "query_extension_dri3_error");
  }
  if (query_extension_present_error) {
    xcb_print_error(query_extension_present_error);
    free(query_extension_present_error);
    DIE(connection, "query_extension_present_error");
  }

  if (!query_extension_dri3_reply) DIE(connection, "query_extension_dri3_reply: null reply");
  if (!query_extension_present_reply) DIE(connection, "query_extension_present_reply: null reply");

  if (!query_extension_dri3_reply->present) DIE(connection, "DRI3 extension is not available");
  if (!query_extension_present_reply->present) DIE(connection, "Present extension is not available");

  DRI3_MAJOR_OPCODE = query_extension_dri3_reply->major_opcode;
  PRESENT_MAJOR_OPCODE = query_extension_present_reply->major_opcode;

  // Create buffers
  int stride = (screen->width_in_pixels * 4 + 63) & ~63;
  int size = stride * screen->height_in_pixels;

  int udmabuf_device = open("/dev/udmabuf", O_RDWR);
  if (udmabuf_device < 0) DIE(connection, "cannot open udmabuf device");

  uint32_t *buffers[BUFFER_COUNT] = {0};
  xcb_pixmap_t pixmaps[BUFFER_COUNT] = {0};

  for (int i = 0; i < BUFFER_COUNT; ++i) {
    int memfd = memfd_create("buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ftruncate(memfd, size);
    fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);

    buffers[i] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    memset(buffers[i], 0, size);

    struct udmabuf_create create = {0};
    create.memfd = memfd;
    create.size = size;
    create.flags = UDMABUF_FLAGS_CLOEXEC;
    int udmabuf = ioctl(udmabuf_device, UDMABUF_CREATE, &create);

    uint8_t bpp = 32;
    pixmaps[i] = xcb_generate_id(connection);
    xcb_dri3_pixmap_from_buffer(connection, pixmaps[i], window, size, screen->width_in_pixels, screen->height_in_pixels, stride, depth, bpp, udmabuf);

    // close(udmabuf);
    // close(memfd);
  }

  close(udmabuf_device);

  xcb_present_event_t present_event = xcb_generate_id(connection);
  xcb_present_select_input(connection, present_event, window, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
  xcb_special_event_t *special_event = xcb_register_for_special_xge(connection, &xcb_present_id, present_event, NULL);

  xcb_map_window(connection, window);

  // TODO: window CreateNotify event
  // TODO: MapNotify event
  // TODO: correctly position image based on x, y, w, h
  // TODO: handle resize
  // TODO?: avoid flicker first frame by rendering once

  bool quit = false;
  int current_buffer = 0;
  uint32_t frame_count = 0;

  Cursor cursor = app.cursor;

#if 1
  while (!quit) {
    // TODO: poll xcb_connection_has_error

    render(&app, buffers[current_buffer], frame_count);

    if (cursor != app.cursor) {
      xcb_change_window_attributes_value_list_t attributes = {.cursor = cursors[app.cursor]};
      xcb_change_window_attributes_aux(connection, window, XCB_CW_CURSOR, &attributes);
      cursor = app.cursor;
      printf("changing cursor\n");
    }

    xcb_present_pixmap(connection, window, pixmaps[current_buffer], frame_count,
                      0, 0, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE, XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, NULL);
    xcb_flush(connection);

    {
      xcb_generic_event_t *ev;
      while ((ev = xcb_poll_for_event(connection))) {
        if (ev->response_type == 0) {
          xcb_generic_error_t *error = (xcb_generic_error_t *)ev;
          xcb_print_error(error);
        } else {
          fprintf(stderr, "XCB event: type=%d\n", ev->response_type & ~0x80);
        }
        free(ev);
      }
    }

    {
      #if 0
      xcb_generic_event_t *ev;
      while ((ev = xcb_wait_for_special_event(connection, special_event)) != NULL) {
          if ((ev->response_type & ~0x80) == XCB_GE_GENERIC) {
              xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)ev;
              if (ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
                  free(ev);
                  break;
              }
          } else if (ev->response_type == 0) {
            xcb_generic_error_t *error = (xcb_generic_error_t *)ev;
            xcb_print_error(error);
          }
          free(ev);
      }
      #endif
    }
    usleep(1000);

    ++frame_count;
    current_buffer = (current_buffer + 1) % BUFFER_COUNT;
  }
#else
  xcb_flush(connection);
  pause();
#endif
  
  for (int i = 0; i < BUFFER_COUNT; ++i) {
    if (buffers[i]) munmap(buffers[i], size);
  }
  xcb_disconnect(connection);
  return 0;
}
