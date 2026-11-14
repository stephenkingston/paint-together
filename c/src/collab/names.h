/*
 * Paint (C port) - friendly names and colours for kids drawing together
 * (Rust: collab/names.rs)
 */
#ifndef COLLAB_NAMES_H
#define COLLAB_NAMES_H

#include <stddef.h>
#include <stdint.h>

/* Bright colours that are easy to tell apart, for each kid's pointer and name tag */
#define N_KID_COLORS 12
extern const uint8_t KID_COLORS[N_KID_COLORS][3];

/* A fresh fun name ("Happy Panda") and colour, different each time */
void names_random(char *name, size_t cap, uint8_t color[3]);
/* What a typed name becomes: trimmed, at most 20 characters, never empty */
void names_tidy(const char *in, char *out, size_t cap);
/* The first `n` characters (not bytes) of UTF-8 text, into out */
void utf8_prefix(const char *in, size_t n, char *out, size_t cap);

#endif
