/*
 * Paint (C port) - writes the Windows icon from the logo (Rust: the packaging's
 * `paint --export-icon`, which can't run when cross-compiling)
 *
 * Built for the build machine and run there by the Makefile:
 *   mkico [--png] OUT.ico
 * The icon holds 16, 32 and 48 pixel images; Explorer scales the 48 for its
 * bigger views. Bigger sizes would cost the .exe more bytes than they're worth.
 * The images are 32-bit DIBs, which every Windows from XP on reads, or with
 * --png PNGs, less than half the bytes, which Windows reads from Vista on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* A BITMAPINFOHEADER, BGRA rows bottom up, and an AND mask of zeros (the alpha
 * channel says what shows); the height counts the colour rows and the mask's */
static uint8_t *dib(const img *im, size_t *len)
{
    int s = im->w, mask = (s + 31) / 32 * 4 * s;
    *len = 40 + 4ul * s * s + (size_t)mask;
    uint8_t *d = calloc(1, *len), *o = d + 40;
    uint32_t head[3] = { 40, (uint32_t)s, (uint32_t)s * 2 };
    for (int k = 0; k < 12; k++)
        d[k] = (uint8_t)(head[k / 4] >> (k % 4 * 8));
    d[12] = 1;      /* planes */
    d[14] = 32;     /* bits per pixel */
    for (int y = s - 1; y >= 0; y--)
        for (int x = 0; x < s; x++, o += 4)
        {
            const uint8_t *p = img_at(im, x, y);
            o[0] = p[2], o[1] = p[1], o[2] = p[0], o[3] = p[3];
        }
    return d;
}

int main(int argc, char **argv)
{
    static const int sizes[3] = { 16, 32, 48 };
    int png = argc == 3 && !strcmp(argv[1], "--png");
    if (argc != 2 + png)
    {
        fprintf(stderr, "usage: mkico [--png] OUT.ico\n");
        return 2;
    }
    uint8_t *data[3];
    size_t len[3];
    for (int i = 0; i < 3; i++)
    {
        img *im = logo_render(sizes[i]);
        data[i] = im ? png ? img_png(im, &len[i]) : dib(im, &len[i]) : NULL;
        img_free(im);
        if (!data[i])
            return 1;
    }
    FILE *f = fopen(argv[1 + png], "wb");
    if (!f)
    {
        perror(argv[1 + png]);
        return 1;
    }
    /* ICONDIR, then one ICONDIRENTRY per image, then the images */
    put16(f, 0);
    put16(f, 1);
    put16(f, 3);
    unsigned long offset = 6 + 16 * 3;
    for (int i = 0; i < 3; i++)
    {
        fputc(sizes[i], f);
        fputc(sizes[i], f);
        fputc(0, f);        /* colours in a palette: none */
        fputc(0, f);
        put16(f, 1);        /* planes */
        put16(f, 32);       /* bits per pixel */
        put32(f, (unsigned long)len[i]);
        put32(f, offset);
        offset += (unsigned long)len[i];
    }
    for (int i = 0; i < 3; i++)
    {
        fwrite(data[i], 1, len[i], f);
        free(data[i]);
    }
    return fclose(f) == 0 ? 0 : 1;
}
