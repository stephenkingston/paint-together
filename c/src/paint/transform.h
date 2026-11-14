/*
 * Paint (C port) - whole-picture transforms: rotate, flip, resize (Rust: the image crate's imageops)
 *
 * Each returns a new picture (NULL if out of memory) and leaves the source alone,
 * so any of the one-argument ones can be passed where app.rs passes
 * imageops::rotate90 and friends.
 */
#ifndef PAINT_TRANSFORM_H
#define PAINT_TRANSFORM_H

#include "core/img.h"

img *img_rotate90(const img *src);      /* clockwise ("Rotate right 90") */
img *img_rotate180(const img *src);
img *img_rotate270(const img *src);     /* counter-clockwise ("Rotate left 90") */
img *img_flip_horizontal(const img *src);
img *img_flip_vertical(const img *src);

typedef enum
{
    RESIZE_TRIANGLE,    /* bilinear: the Resize and Skew dialog */
    RESIZE_LANCZOS3,    /* the app icon's small sizes */
} resize_filter;

/* imageops::resize: separable resampling, columns first, with the image crate's
 * kernels, weights and rounding; straight (not premultiplied) alpha. */
img *img_resize(const img *src, int w, int h, resize_filter filter);

#endif
