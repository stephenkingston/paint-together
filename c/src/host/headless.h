/*
 * Paint (C port) - what tests can do with the headless host (host/headless.c)
 *
 * The headless host draws into memory and never makes events: a test calls
 * the app's handler itself (or headless_send). These let it set up what the
 * desktop would answer and see what the app asked of it.
 */
#ifndef HOST_HEADLESS_H
#define HOST_HEADLESS_H

#include "ui/host.h"

/* The next host_open_file / host_save_file answers, in order: a path, or NULL for
 * Cancel. With none queued the pickers say they are unavailable. */
void headless_queue_pick(const char *path);
void headless_resize(host_win *win, int w, int h);      /* resizes the surface and sends EV_SIZE */
void headless_send(host_win *win, const ui_event *ev);  /* to the window's handler */
void headless_set_scale(int percent);

typedef struct
{
    int quit, wakes, cursor, captured, fullscreen, maximized, shown, min_w, min_h;
    rect presented;             /* the union of host_present's areas since the last headless_state */
    char title[256], url[512];  /* the last host_title / host_open_url */
    char picker_title[256], picker_name[256];   /* the last picker's, and the Save picker's name */
} headless_info;

/* What the app has asked for; resets `presented` */
headless_info headless_state(void);

#endif
