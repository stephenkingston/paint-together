/*
 * Paint (C port) - the UI scale (Rust: theme.rs)
 */
#include "ui/theme.h"

int ui_scale = 100;

void theme_set_scale(int percent)
{
    ui_scale = percent < 50 ? 50 : percent > 400 ? 400 : percent;
    /* The canvas checkerboard is 8 points a square */
    gfx_canvas_checker(S(8));
}

font *ui_font(float pt, int style)
{
    return font_get(SF(pt), style);
}
