/*
 * Paint (C port) - the kernels of kernels.h in C: what the assembly does, for
 * other CPUs and for the tests
 */
#include <string.h>
#include "core/kernels.h"

#if defined(PAINT_ASM) && UINTPTR_MAX == UINT64_MAX
/* The assembly reads the structs at these offsets */
_Static_assert(offsetof(k_deflate_t, bits) == 24 && offsetof(k_deflate_t, nbits) == 32 && offsetof(k_deflate_t, i) == 36
                   && offsetof(k_deflate_t, len) == 40 && offsetof(k_deflate_t, cap) == 44 && offsetof(k_deflate_t, chains) == 48
                   && offsetof(k_deflate_t, counts) == 56,
               "k_deflate_t");
_Static_assert(offsetof(k_inflate_t, lengths) == 40 && offsetof(k_inflate_t, distances) == 48 && offsetof(k_inflate_t, bits) == 56
                   && offsetof(k_inflate_t, nbits) == 60,
               "k_inflate_t");
#endif

static uint64_t load64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;      /* little-endian: every CPU Paint is built for */
}

/* ---- deflate ---- */

static uint32_t zhash(const uint8_t *d)
{
    uint32_t h = d[0] + (d[1] << 8) + (d[2] << 16);
    h ^= h << 3;
    h += h >> 5;
    h ^= h << 4;
    h += h >> 17;
    h ^= h << 25;
    h += h >> 6;
    return h & (K_ZHASH - 1);
}

/* How many bytes from a and b match, up to limit */
static uint32_t match(const uint8_t *a, const uint8_t *b, uint32_t limit)
{
    uint32_t k = 0;
    while (k + 8 <= limit)
    {
        uint64_t x = load64(a + k) ^ load64(b + k);
        if (x)
            return k + (uint32_t)__builtin_ctzll(x) / 8;
        k += 8;
    }
    while (k < limit && a[k] == b[k])
        k++;
    return k;
}

static uint32_t rev(uint32_t v, int n)
{
    uint32_t r = 0;
    while (n--)
        r = r << 1 | (v & 1), v >>= 1;
    return r;
}

static uint32_t bsr(uint32_t v)
{
    return 31 - (uint32_t)__builtin_clz(v);
}

int k_deflate_c(k_deflate_t *s)
{
    const uint8_t *d = s->data;
    int32_t i = s->i, len = s->len;
    uint64_t bits = s->bits;
    uint32_t nbits = s->nbits, cap = s->cap;
    uint8_t *out = s->out;
    int done = 0;
    for (;;)
    {
        if (s->end - out < K_ROOM)
            break;
        if (i >= len)
        {
            nbits += 7;     /* the end of the block: code 256, seven 0 bits */
            done = 1;
            break;
        }
        uint32_t best = 3;
        int32_t at = -1;
        if (i < len - 3)    /* (stb codes the last 3 bytes as they are) */
        {
            uint32_t h = zhash(d + i), n = s->counts[h];
            uint32_t *chain = s->chains + h * cap;
            uint32_t limit = len - i < 258 ? (uint32_t)(len - i) : 258;
            /* stb looks at every position; one can only be as long as the best so far if its last byte matches too */
            for (uint32_t j = 0; j < n; j++)
            {
                int32_t p = (int32_t)chain[j];
                if (p > i - 32768 && d[p + best - 1] == d[i + best - 1])
                {
                    uint32_t m = match(d + p, d + i, limit);
                    if (m >= best)
                        best = m, at = p;
                }
            }
            if (n == cap)
            {
                memmove(chain, chain + cap / 2, cap / 2 * sizeof *chain);
                n = cap / 2;
            }
            chain[n] = (uint32_t)i;
            s->counts[h] = (uint16_t)(n + 1);
            /* a longer match at the next byte? (only possible if this one isn't the longest there can be) */
            uint32_t limit2 = len - i - 1 < 258 ? (uint32_t)(len - i - 1) : 258;
            if (at >= 0 && best < limit2)
            {
                uint32_t h2 = zhash(d + i + 1), n2 = s->counts[h2];
                const uint32_t *chain2 = s->chains + h2 * cap;
                for (uint32_t j = 0; j < n2; j++)
                {
                    int32_t p = (int32_t)chain2[j];
                    if (p > i - 32767 && d[p + best] == d[i + 1 + best] && match(d + p, d + i + 1, limit2) > best)
                    {
                        at = -1;
                        break;
                    }
                }
            }
        }
        if (at >= 0)
        {
            uint32_t l = best - 3, sym, e = 0;
            if (best == 258)
                sym = 285;
            else if (l < 8)
                sym = 257 + l;
            else
                e = bsr(l) - 2, sym = 257 + 4 * (e + 1) + ((l >> e) & 3);
            if (sym <= 279)
                bits |= (uint64_t)rev(sym - 256, 7) << nbits, nbits += 7;
            else
                bits |= (uint64_t)rev(sym - 280 + 0xC0, 8) << nbits, nbits += 8;
            bits |= (uint64_t)(l & ((1u << e) - 1)) << nbits, nbits += e;
            uint32_t dd = (uint32_t)(i - at) - 1, code;
            e = 0;
            if (dd < 2)
                code = dd;
            else
                e = bsr(dd) - 1, code = 2 * (e + 1) + ((dd >> e) & 1);
            bits |= (uint64_t)rev(code, 5) << nbits, nbits += 5;
            bits |= (uint64_t)(dd & ((1u << e) - 1)) << nbits, nbits += e;
            i += (int32_t)best;
        }
        else
        {
            uint32_t c = d[i++];
            if (c < 144)
                bits |= (uint64_t)rev(c + 0x30, 8) << nbits, nbits += 8;
            else
                bits |= (uint64_t)rev(c - 144 + 0x190, 9) << nbits, nbits += 9;
        }
        memcpy(out, &bits, 8);
        out += nbits >> 3;
        bits >>= nbits & ~7u;
        nbits &= 7;
    }
    s->i = i;
    s->out = out;
    s->bits = bits;
    s->nbits = nbits;
    return done;
}

/* ---- checksums ---- */

uint32_t k_crc32_c(uint32_t crc, const uint8_t *p, size_t n)
{
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = c >> 1 ^ (0xEDB88320u & -(c & 1));
        table[i] = c;
    }
    crc = ~crc;
    while (n--)
        crc = crc >> 8 ^ table[(crc ^ *p++) & 255];
    return ~crc;
}

uint32_t k_adler32_c(uint32_t adler, const uint8_t *p, size_t n)
{
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (n)
    {
        size_t k = n < 5552 ? n : 5552;     /* the most bytes before b can overflow */
        n -= k;
        while (k--)
            a += *p++, b += a;
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* ---- PNG rows ---- */

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static uint8_t filter1(int f, int x, int a, int b, int c)
{
    switch (f)
    {
    case 0:
        return (uint8_t)x;
    case 1:
        return (uint8_t)(x - a);
    case 2:
        return (uint8_t)(x - b);
    case 3:
        return (uint8_t)(x - ((a + b) >> 1));
    default:
        return (uint8_t)(x - paeth(a, b, c));
    }
}

void k_png_costs_c(const uint8_t *row, const uint8_t *prev, size_t n, uint32_t cost[5])
{
    for (int f = 0; f < 5; f++)
        cost[f] = 0;
    for (size_t i = 0; i < n; i++)
    {
        int a = i >= 4 ? row[i - 4] : 0, c = i >= 4 ? prev[i - 4] : 0;
        for (int f = 0; f < 5; f++)
        {
            int v = (int8_t)filter1(f, row[i], a, prev[i], c);
            cost[f] += (uint32_t)(v < 0 ? -v : v);
        }
    }
}

void k_png_filter_c(uint8_t *out, const uint8_t *row, const uint8_t *prev, size_t n, int filter)
{
    for (size_t i = 0; i < n; i++)
        out[i] = filter1(filter, row[i], i >= 4 ? row[i - 4] : 0, prev[i], i >= 4 ? prev[i - 4] : 0);
}

void k_unfilter_c(uint8_t *cur, const uint8_t *raw, const uint8_t *prior, size_t bpp, size_t n, int filter)
{
    size_t k;
    switch (filter)
    {
    case 0:
        memcpy(cur, raw, n);
        break;
    case 1:
        for (k = 0; k < n; k++)
            cur[k] = (uint8_t)(raw[k] + (k >= bpp ? cur[k - bpp] : 0));
        break;
    case 2:
        for (k = 0; k < n; k++)
            cur[k] = (uint8_t)(raw[k] + prior[k]);
        break;
    case 3:
        for (k = 0; k < n; k++)
            cur[k] = (uint8_t)(raw[k] + ((prior[k] + (k >= bpp ? cur[k - bpp] : 0)) >> 1));
        break;
    case 4:
        for (k = 0; k < n; k++)
            cur[k] = (uint8_t)(raw[k] + (k >= bpp ? paeth(cur[k - bpp], prior[k], prior[k - bpp]) : prior[k]));
        break;
    default:
        for (k = 0; k < n; k++)
            cur[k] = (uint8_t)(raw[k] + (k >= bpp ? cur[k - bpp] >> 1 : 0));
        break;
    }
}

/* ---- inflate ---- */

/* stb refills its 32-bit code buffer a byte at a time, to more than 24 bits, when it
 * holds fewer than `need`; the kernel keeps 56 or more but counts what stb would hold */
static int32_t stb_fill(int32_t held, int32_t need)
{
    return held < need ? held + ((32 - held) & ~7) : held;
}

/* A symbol, as stbi__zhuffman_decode: -1 if the code is bad */
static int decode(const uint8_t *z, uint64_t *bits, int32_t *nb, int32_t *held)
{
    const uint16_t *fast = (const uint16_t *)z;
    *held = stb_fill(*held, 16);
    int b = fast[*bits & 511], s;
    if (b)
    {
        s = b >> 9;
        b &= 511;
    }
    else
    {
        int k = (int)rev((uint32_t)*bits & 0xFFFF, 16);
        const int *maxcode = (const int *)(z + K_ZH_MAXCODE);
        for (s = 10; k >= maxcode[s]; s++)
            ;
        if (s >= 16)
            return -1;
        b = (k >> (16 - s)) - ((const uint16_t *)(z + K_ZH_FIRSTCODE))[s] + ((const uint16_t *)(z + K_ZH_FIRSTSYMBOL))[s];
        if (b >= 288 || z[K_ZH_SIZE + b] != s)
            return -1;
        b = ((const uint16_t *)(z + K_ZH_VALUE))[b];
    }
    *bits >>= s;
    *nb -= s;
    *held -= s;
    return b;
}

static uint32_t take(int e, uint64_t *bits, int32_t *nb, int32_t *held)
{
    *held = stb_fill(*held, e);
    uint32_t v = (uint32_t)*bits & ((1u << e) - 1);
    *bits >>= e;
    *nb -= e;
    *held -= e;
    return v;
}

int k_inflate_c(k_inflate_t *s)
{
    const uint8_t *in = s->in;
    uint8_t *out = s->out;
    uint64_t bits = s->bits;
    int32_t nb = s->nbits, held = s->nbits;
    int r;
    for (;;)
    {
        /* two refills of 8 bytes in a symbol, and the longest match, with 16 bytes over for copying in 16s */
        if (s->in_end - in < 16 || s->out_end - out < 258 + 16)
        {
            r = K_INFLATE_MORE;
            break;
        }
        bits |= load64(in) << nb;
        in += (63 - nb) >> 3;
        nb |= 56;
        int z = decode(s->lengths, &bits, &nb, &held);
        if (z < 0)
        {
            r = K_INFLATE_BAD_CODE;
            break;
        }
        if (z < 256)
        {
            *out++ = (uint8_t)z;
            continue;
        }
        if (z == 256)
        {
            r = K_INFLATE_END;
            break;
        }
        if (z >= 286)
        {
            r = K_INFLATE_BAD_CODE;
            break;
        }
        z -= 257;
        uint32_t len, dist;
        if (z < 8)
            len = 3 + z;
        else if (z == 28)
            len = 258;
        else
        {
            int e = (z >> 2) - 1;
            len = 3 + ((4u + (z & 3)) << e) + take(e, &bits, &nb, &held);
        }
        bits |= load64(in) << nb;
        in += (63 - nb) >> 3;
        nb |= 56;
        z = decode(s->distances, &bits, &nb, &held);
        if (z < 0 || z >= 30)
        {
            r = K_INFLATE_BAD_CODE;
            break;
        }
        if (z < 4)
            dist = 1 + z;
        else
        {
            int e = (z >> 1) - 1;
            dist = 1 + ((2u + (z & 1)) << e) + take(e, &bits, &nb, &held);
        }
        if ((size_t)(out - s->out_start) < dist)
        {
            r = K_INFLATE_BAD_DIST;
            break;
        }
        const uint8_t *p = out - dist;
        if (dist >= 8)
        {
            /* in 8s, up to 7 bytes past the end (there's room, and they're written again later) */
            uint8_t *end = out + len;
            do
            {
                memcpy(out, p, 8);
                out += 8, p += 8;
            } while (out < end);
            out = end;
        }
        else
        {
            while (len--)
                *out++ = *p++;
        }
    }
    s->in = in - ((nb - held) >> 3);
    s->out = out;
    s->bits = (uint32_t)(held >= 32 ? bits : bits & ((1ull << held) - 1));
    s->nbits = held;
    return r;
}
