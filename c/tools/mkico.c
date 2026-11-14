/*
 * Paint (C port) - writes res/paint.ico, the Windows icon, from the logo (Rust: the packaging's
 * `paint --export-icon`, which can't run when cross-compiling)
 *
 * Built for the build machine and run there by the Makefile:
 *   mkico OUT.ico
 * The icon holds 16, 32 and 48 pixel images as 32-bit DIBs (what every
 * Windows from XP on reads); Explorer scales the 48 for its bigger views.
 * Bigger sizes would cost the .exe more bytes than they're worth.
 */
#include <stdio.h>
#include <stdlib.h>
#include "paint/logo.h"

static void put16(FILE *f, unsigned v)
{
    fputc((int)(v & 255), f);
    fputc((int)(v >> 8 & 255), f);
}

static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFF));
    put16(f, (unsigned)(v >> 16));
}

int main(int argc, char **argv)
{
    static const int sizes[3] = { 16, 32, 48 };
    if (argc != 2)
    {
        fprintf(stderr, "usage: mkico OUT.ico\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    /* ICONDIR, then one ICONDIRENTRY per image, then the images */
    put16(f, 0);
    put16(f, 1);
    put16(f, 3);
    unsigned long offset = 6 + 16 * 3;
    for (int i = 0; i < 3; i++)
    {
        int s = sizes[i];
        unsigned long bytes = 40 + 4ul * s * s + (unsigned long)((s + 31) / 32 * 4) * s;
        fputc(s, f);
        fputc(s, f);
        fputc(0, f);        /* colours in a palette: none */
        fputc(0, f);
        put16(f, 1);        /* planes */
        put16(f, 32);       /* bits per pixel */
        put32(f, bytes);
        put32(f, offset);
        offset += bytes;
    }
    for (int i = 0; i < 3; i++)
    {
        int s = sizes[i];
        img *im = logo_render(s);
        if (!im)
            return 1;
        /* BITMAPINFOHEADER: the height counts the colour rows and the AND mask's */
        put32(f, 40);
        put32(f, (unsigned long)s);
        put32(f, (unsigned long)s * 2);
        put16(f, 1);
        put16(f, 32);
        for (int k = 0; k < 6; k++)
            put32(f, 0);
        /* BGRA rows, bottom up */
        for (int y = s - 1; y >= 0; y--)
            for (int x = 0; x < s; x++)
            {
                const uint8_t *p = img_at(im, x, y);
                fputc(p[2], f), fputc(p[1], f), fputc(p[0], f), fputc(p[3], f);
            }
        /* The AND mask: all zero (the alpha channel says what shows) */
        for (int k = 0; k < (s + 31) / 32 * 4 * s; k++)
            fputc(0, f);
        img_free(im);
    }
    return fclose(f) == 0 ? 0 : 1;
}
