/*
 * Paint (C port) - friendly names and colours for kids drawing together
 * (Rust: collab/names.rs)
 */
#include "collab/names.h"

#include <stdio.h>
#include <string.h>
#include "os/os.h"

const uint8_t KID_COLORS[N_KID_COLORS][3] = {
    { 230, 57, 70 }, { 244, 140, 6 }, { 233, 196, 0 }, { 42, 157, 70 },   { 0, 150, 199 }, { 67, 97, 238 },
    { 131, 56, 236 }, { 222, 49, 155 }, { 0, 168, 150 }, { 158, 98, 50 }, { 112, 193, 25 }, { 255, 92, 138 },
};

static const char *const ADJECTIVES[16] = {
    "Happy", "Brave", "Sunny", "Clever", "Bouncy", "Giggly", "Sparkly", "Speedy",
    "Gentle", "Jolly", "Lucky", "Cosy", "Fluffy", "Zippy", "Wiggly", "Mighty",
};

static const char *const ANIMALS[16] = {
    "Panda", "Tiger", "Penguin", "Fox", "Otter", "Koala", "Dolphin", "Owl",
    "Bunny", "Turtle", "Kitten", "Puppy", "Llama", "Hedgehog", "Frog", "Lion",
};

static uint64_t roll(uint64_t *x, uint64_t n)
{
    *x ^= *x << 13;
    *x ^= *x >> 7;
    *x ^= *x << 17;
    return *x % n;
}

void names_random(char *name, size_t cap, uint8_t color[3])
{
    uint64_t x;
    os_random(&x, sizeof x);
    x |= 1;
    const char *adj = ADJECTIVES[roll(&x, 16)];
    const char *animal = ANIMALS[roll(&x, 16)];
    snprintf(name, cap, "%s %s", adj, animal);
    memcpy(color, KID_COLORS[roll(&x, N_KID_COLORS)], 3);
}

/* One code point of UTF-8 at s (its length in *len; a stray byte counts as one) */
static uint32_t next_cp(const char *s, size_t *len)
{
    const unsigned char *u = (const unsigned char *)s;
    int n = u[0] < 0x80 ? 1 : (u[0] & 0xE0) == 0xC0 ? 2 : (u[0] & 0xF0) == 0xE0 ? 3 : (u[0] & 0xF8) == 0xF0 ? 4 : 1;
    uint32_t cp = n == 1 ? u[0] : (uint32_t)(u[0] & (0x7F >> n));
    for (int i = 1; i < n; i++)
    {
        if ((u[i] & 0xC0) != 0x80)
        {
            *len = 1;
            return u[0];
        }
        cp = cp << 6 | (u[i] & 0x3F);
    }
    *len = (size_t)n;
    return cp;
}

/* Rust's char::is_whitespace */
static int is_space(uint32_t c)
{
    return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028
           || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* Rust's char::is_control */
static int is_control(uint32_t c)
{
    return c < 0x20 || (c >= 0x7F && c < 0xA0);
}

void utf8_prefix(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; *in && i < n; i++)
    {
        size_t len;
        next_cp(in, &len);
        if (o + len >= cap)
            break;
        memcpy(out + o, in, len);
        o += len;
        in += len;
    }
    if (cap)
        out[o] = 0;
}

void names_tidy(const char *in, char *out, size_t cap)
{
    /* Trim: find the last non-space character */
    const char *end = in, *p = in;
    size_t len;
    while (*p && is_space(next_cp(p, &len)))
        p += len;
    in = p;
    for (; *p; p += len)
        if (!is_space(next_cp(p, &len)))
            end = p + len;
    size_t o = 0;
    int kept = 0;
    for (p = in; p < end && kept < 20; p += len)
    {
        uint32_t c = next_cp(p, &len);
        if (is_control(c))
            continue;
        if (o + len >= cap)
            break;
        memcpy(out + o, p, len);
        o += len;
        kept++;
    }
    out[o] = 0;
    if (!o)
        snprintf(out, cap, "Friend");
}
