/*
 * Paint (C port) - the shape gallery (Rust: shapes.rs)
 *
 * Every shape is a polygon fitted to the dragged box, so the same outline
 * drives rasterization and the toolbar icons. Outlines are written into a
 * caller's array of SHAPE_MAX_POINTS points; nothing is allocated.
 */
#ifndef PAINT_SHAPES_H
#define PAINT_SHAPES_H

#include "paint/raster.h"

typedef enum
{
    SHAPE_LINE,
    SHAPE_OVAL,
    SHAPE_RECTANGLE,
    SHAPE_ROUNDED_RECTANGLE,
    SHAPE_TRIANGLE,
    SHAPE_RIGHT_TRIANGLE,
    SHAPE_DIAMOND,
    SHAPE_PENTAGON,
    SHAPE_HEXAGON,
    SHAPE_RIGHT_ARROW,
    SHAPE_LEFT_ARROW,
    SHAPE_UP_ARROW,
    SHAPE_DOWN_ARROW,
    SHAPE_STAR4,
    SHAPE_STAR5,
    SHAPE_STAR6,
    SHAPE_ROUNDED_CALLOUT,
    SHAPE_OVAL_CALLOUT,
    SHAPE_CLOUD_CALLOUT,
    SHAPE_HEART,
    SHAPE_LIGHTNING,
    SHAPE_COUNT         /* in the order of Paint's shape gallery */
} shape_kind;

#define SHAPE_MAX_POINTS 360    /* the most any outline has (a big oval) */
#define SHAPE_MAX_CONTOURS 3

const char *shape_name(shape_kind k);
/* Closed outline fitted to the box spanned by a and b; for a line, just its two ends.
 * Returns the number of points written to out[SHAPE_MAX_POINTS]. */
int shape_outline(shape_kind k, pt a, pt b, pt *out);
/* Every closed outline of the shape: the main one plus any detached parts (the
 * cloud callout's bubbles). out holds SHAPE_MAX_CONTOURS * SHAPE_MAX_POINTS points;
 * contour i starts at out + i * SHAPE_MAX_POINTS and has counts[i] points.
 * Returns the number of contours. */
int shape_contours(shape_kind k, pt a, pt b, pt *out, int *counts);
/* With Shift held: lines snap to 45 degree steps, everything else becomes square */
pt shape_constrain(shape_kind k, pt a, pt b, int shift);

#endif
