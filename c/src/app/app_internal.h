/*
 * Paint (C port) - what the app's own files share with each other (not for the views)
 */
#ifndef APP_APP_INTERNAL_H
#define APP_APP_INTERNAL_H

#include "app/app.h"

static inline int app_busy_dialog(const app *a)
{
    return a->confirm.kind || a->confirm_everyone.kind || a->resize || a->properties || a->about_open || a->edit_colors
        || a->error || a->join_dialog || a->ask_kind;
}

/* app.c */
void app_mark(app *a, irect r);
void app_record(app *a, change *c);                 /* a finished edit: one undo step, and sent to friends */
void app_set_error(app *a, const char *fmt, ...);
void pending_set(pending *p, int kind, const char *arg);
void pending_clear(pending *p);
void app_op_clear(app *a);
void app_selection_free(selection *s);
void app_text_box_free(text_box *t);
int app_tool_pointer(app_tool tool);                /* ICON_* drawn as the pointer, or -1 */
void app_text_family(app *a);                       /* find the fonts, the first time */
int app_text_hit(const app *a, pt p);               /* a press there lands on the text box or its handles */
int app_text_handle_at(const app *a, pt p);
int app_live_shape_hit(const app *a, pt p);
void app_spray_frame(app *a, float dt);

/* app_files.c */
void app_perform(app *a, pending *p);               /* uses up p */
void app_load_recent(app *a);

/* app_together.c */
void app_step_mine(app *a, int back);
void app_rebase(app *a, region rg);
void app_toast(app *a, const char *text);
void app_advertise(app *a);
void app_handover_clear(app *a);
int app_mark_get(const app *a, uint64_t id, uint64_t *foreign);
void app_mark_set(app *a, uint64_t id, uint64_t foreign);
void app_save_me(const app *a);
int app_load_me(app *a);

#endif
