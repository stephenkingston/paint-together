/*
 * Paint (C port) - Paint's brush types, each a way of laying coverage along a stroke segment (Rust: brush.rs)
 *
 * The textured brushes use stable per-pixel noise, so a stroke looks the same
 * however it is redrawn; only the airbrush is random, from a caller-held
 * xorshift state (seed it once, e.g. 0x2545F4914F6CDD1D, as the Rust does).
 */
#ifndef PAINT_BRUSH_H
#define PAINT_BRUSH_H

#include <stdint.h>
#include "paint/raster.h"

typedef enum
{
    BRUSH_BRUSH,
    BRUSH_CALLIGRAPHY1,
    BRUSH_CALLIGRAPHY2,
    BRUSH_AIRBRUSH,
    BRUSH_OIL,
    BRUSH_CRAYON,
    BRUSH_MARKER,
    BRUSH_NATURAL_PENCIL,
    BRUSH_WATERCOLOR,
    BRUSH_COUNT         /* the gallery shows them in this order */
} brush_kind;

const char *brush_name(brush_kind k);       /* "Calligraphy brush 1" ... */
float brush_opacity(brush_kind k);          /* alpha applied to the whole stroke */
/* Lay coverage for the stroke segment a-b into m; returns the pixels touched */
irect brush_segment(brush_kind k, mask *m, pt a, pt b, float size, uint64_t *rng);
/* Scatter n single-pixel dots in a disc of diameter size around c (the airbrush,
 * also called on a timer while the button is held) */
irect brush_spray(mask *m, pt c, float size, uint64_t *rng, int n);

#endif
