/*
 * Paint (C port) - an X11 client over the wire protocol, for the Linux host
 *
 * No Xlib or xcb: x11_wire_linux.c opens the socket named by $DISPLAY,
 * authenticates with the MIT-MAGIC-COOKIE-1 from ~/.Xauthority, reads the
 * setup and keeps one connection (the program has one display). Requests are
 * built in place: xw_req() hands back the zeroed request and the caller fills
 * its fields at their protocol offsets in the host's byte order (the
 * connection's). Replies are waited for in order; events met meanwhile are
 * queued. Errors are logged under PAINT_X11DEBUG=1 and are never fatal.
 *
 * Also here, for x11_linux.c and portal_linux.c (the Linux host's private
 * parts): the keysym table, the modal wait the pickers use, and file:// URIs.
 */
#ifndef HOST_X11_WIRE_H
#define HOST_X11_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct
{
    int fd, local, dead;            /* local: a Unix socket, so MIT-SHM can work */
    uint32_t root, visual, colormap;    /* a 24-bit TrueColor visual, 0xRRGGBB in 32-bit pixels */
    int depth, screen_w, screen_h;
    int min_keycode, max_keycode;
    size_t max_req;                 /* the largest request, bytes */
    unsigned seq;                   /* the last request's sequence number */
    int swap_images;                /* the server wants pixels in the other byte order */
} xw_display;

extern xw_display xw;

enum
{
    XA_ATOM = 4, XA_CARDINAL = 6, XA_INTEGER = 19, XA_RESOURCE_MANAGER = 23, XA_STRING = 31, XA_WINDOW = 33,
    XA_WM_HINTS = 35, XA_WM_CLIENT_MACHINE = 36, XA_WM_NAME = 39, XA_WM_NORMAL_HINTS = 40, XA_WM_SIZE_HINTS = 41,
    XA_WM_CLASS = 67
};

/* Core events (the type byte without the SendEvent bit) */
enum
{
    XE_ERROR = 0, XE_REPLY = 1, XE_KEY_PRESS = 2, XE_KEY_RELEASE, XE_BUTTON_PRESS, XE_BUTTON_RELEASE, XE_MOTION,
    XE_ENTER, XE_LEAVE, XE_FOCUS_IN, XE_FOCUS_OUT, XE_EXPOSE = 12, XE_UNMAP = 18, XE_MAP = 19, XE_CONFIGURE = 22,
    XE_PROPERTY = 28, XE_SELECTION_CLEAR, XE_SELECTION_REQUEST, XE_SELECTION_NOTIFY, XE_CLIENT_MESSAGE = 33,
    XE_MAPPING
};

static inline void xw_put16(uint8_t *at, unsigned value)
{
    uint16_t half = (uint16_t)value;
    memcpy(at, &half, 2);
}
static inline void xw_put32(uint8_t *at, uint32_t value)
{
    memcpy(at, &value, 4);
}
static inline unsigned xw_get16(const uint8_t *at)
{
    uint16_t half;
    memcpy(&half, at, 2);
    return half;
}
static inline uint32_t xw_get32(const uint8_t *at)
{
    uint32_t word;
    memcpy(&word, at, 4);
    return word;
}

/* ---- the connection ---- */
int xw_open(void);                      /* 0, or -1 (and why on stderr) */
uint32_t xw_new_id(void);
/* A request of len bytes (rounded up to 4), zeroed, its opcode, data byte and
 * length written: fields go from offset 4. Valid until the next xw_ call. */
uint8_t *xw_req(int opcode, int data, size_t len);
void xw_req32(int opcode, uint32_t value);  /* one 32-bit field: MapWindow, FreePixmap... */
void xw_flush(void);
/* The reply to request seq (valid until the next xw_ call; *len its size), or NULL on an error */
const uint8_t *xw_reply(unsigned seq, size_t *len);
int xw_sync(void);                      /* a round trip; the code of the first error since the last one, or 0 */
int xw_ext(const char *name, int *first_event);    /* an extension's major opcode, or 0 */

/* ---- events: 32 bytes each ---- */
int xw_read(void);                      /* queue whatever has arrived, without waiting; -1 if the connection is gone */
/* Take the first queued event match accepts (NULL: any): 1, else 0 */
int xw_take(uint8_t ev[32], int (*match)(const uint8_t *ev, void *arg), void *arg);
const uint8_t *xw_peek(void);           /* the first queued event, left there; NULL if none */
/* Flush and wait for the display or fd (-1: none) to have input: 1 the display, 2 fd, 0 timeout, -1 gone */
int xw_wait(int timeout_ms, int fd);

/* ---- atoms, properties, events ---- */
uint32_t xw_atom(const char *name);
void xw_atoms(const char *const *names, int count, uint32_t *out);     /* in one round trip */
/* ChangeProperty (Replace), in pieces when it is larger than a request */
void xw_set_prop(uint32_t win, uint32_t prop, uint32_t type, int format, const void *data, size_t count);
/* The whole property (malloc'd, NUL-terminated; *bytes its size), or NULL when it is not there.
 * type 0: any; *type_out gets its type. del: delete it once read. */
uint8_t *xw_get_prop(uint32_t win, uint32_t prop, uint32_t type, int del, size_t *bytes, uint32_t *type_out);
uint32_t xw_get_prop32(uint32_t win, uint32_t prop, uint32_t type, size_t index, uint32_t fallback);
void xw_send_event(uint32_t dest, uint32_t mask, const uint8_t ev[32]);
/* A format-32 ClientMessage about win, sent to dest */
void xw_client_message(uint32_t dest, uint32_t mask, uint32_t win, uint32_t type, const uint32_t data[5]);
/* PutImage of 32-bit pixels (stride in pixels), in strips that fit a request */
void xw_put_image(uint32_t drawable, uint32_t gc, const uint32_t *px, int stride, int x, int y, int w, int h);

/* ---- shared with portal_linux.c ---- */
uint32_t keysym_unicode(uint32_t sym);  /* x11_keysym_linux.c; 0: not a character */
uint32_t x11_host_window(void);         /* the main window's id (the pickers' parent) */
/* Wait for fd to be readable (1) or timeout_ms to pass (0) while a picker is open: the window
 * repaints from its back buffer and the clipboard is served; input is dropped. -1: the display is gone. */
int x11_modal_wait(int fd, int timeout_ms);
char *uri_to_path(const char *uri, size_t len);    /* a file:// URI's local path (malloc'd), or NULL */

#endif
