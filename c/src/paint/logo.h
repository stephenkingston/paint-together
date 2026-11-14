/*
 * Paint (C port) - the app icon (Rust: logo.rs)
 *
 * A macOS-style rounded tile holding an artist's palette and a brush, drawn
 * with the canvas rasteriser. Sizes under 512 are drawn at 512 and scaled
 * down with Lanczos3, as the Rust does.
 */
#ifndef PAINT_LOGO_H
#define PAINT_LOGO_H

#include "core/img.h"

img *logo_render(int size);     /* size x size, transparent around the tile; NULL if out of memory */

#endif
