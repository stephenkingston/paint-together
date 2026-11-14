/*
 * Paint (C port) - the Text tool's fonts, layout and rasterization (Rust: text.rs)
 *
 * Finding fonts. The Rust asks fontconfig on Linux and reads name tables on
 * Windows; here both read name tables. On Linux and macOS a face is filed the way
 * fontconfig names it (typographic family and style, name IDs 16/17, else
 * 1/2): Regular/Book/Normal/Roman, Bold, Italic/Oblique and Bold Italic fill
 * a family's four slots, and other weights (Light, Medium...) stand in as the
 * regular face only for families that have none. On Windows a face is filed
 * under its name ID 1 family with the OS/2 bold and italic flags, as the Rust
 * does there.
 *
 * Metrics follow ab_glyph (through ttf-parser): the pixel size is the height
 * from descender to ascender, the ascender and descender come from OS/2 when
 * its USE_TYPO_METRICS flag is set and from hhea otherwise, and kerning comes
 * from the 'kern' table only. stb_truetype draws the glyphs.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "paint/text.h"
#include "stb_truetype.h"

#define ITALIC_SHEAR 0.2f

/* ---- UTF-8 ---- */

int utf8_decode(const char *str, int *len)
{
    const unsigned char *s = (const unsigned char *)str;
    if (s[0] < 0x80)
    {
        *len = 1;
        return s[0];
    }
    int n = s[0] >= 0xF0 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 0;
    if (!n || s[0] > 0xF4)
    {
        *len = 1;
        return 0xFFFD;
    }
    int cp = s[0] & (0x7F >> n);
    for (int i = 1; i < n; i++)
    {
        if ((s[i] & 0xC0) != 0x80)
        {
            *len = i;
            return 0xFFFD;
        }
        cp = cp << 6 | (s[i] & 0x3F);
    }
    *len = n;
    return cp;
}

int utf8_count(const char *s)
{
    int n = 0, len;
    while (*s)
    {
        utf8_decode(s, &len);
        s += len;
        n++;
    }
    return n;
}

size_t utf8_byte(const char *s, int index)
{
    size_t b = 0;
    int len;
    for (int i = 0; i < index && s[b]; i++)
    {
        utf8_decode(s + b, &len);
        b += (size_t)len;
    }
    return b;
}

/* ---- the font list ---- */

typedef struct
{
    uint32_t path;      /* offset + 1 into the string pool; 0 = no face */
    int index;          /* face within a collection */
} face_ref;

typedef struct
{
    uint32_t name;      /* offset into the pool */
    face_ref slot[4];   /* regular, bold, italic, bold italic: bold | italic << 1 */
} family;

static struct
{
    int scanned;
    char *pool;
    size_t pool_n, pool_cap;
    family *fams;
    int n, cap;
    int *bucket, *chain;        /* name hash, while scanning */
} lib;

#define NBUCKETS 1024

static uint32_t pool_add(const char *s)
{
    size_t len = strlen(s) + 1;
    if (lib.pool_n + len > lib.pool_cap)
    {
        size_t cap = lib.pool_cap ? lib.pool_cap * 2 : 16384;
        while (cap < lib.pool_n + len)
            cap *= 2;
        lib.pool = realloc(lib.pool, cap);
        lib.pool_cap = cap;
    }
    memcpy(lib.pool + lib.pool_n, s, len);
    lib.pool_n += len;
    return (uint32_t)(lib.pool_n - len);
}

static unsigned name_hash(const char *s)
{
    unsigned h = 2166136261u;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 16777619u;
    return h % NBUCKETS;
}

static family *family_get(const char *name)
{
    unsigned h = name_hash(name);
    for (int i = lib.bucket[h]; i >= 0; i = lib.chain[i])
        if (!strcmp(lib.pool + lib.fams[i].name, name))
            return &lib.fams[i];
    if (lib.n == lib.cap)
    {
        lib.cap = lib.cap ? lib.cap * 2 : 256;
        lib.fams = realloc(lib.fams, (size_t)lib.cap * sizeof(family));
        lib.chain = realloc(lib.chain, (size_t)lib.cap * sizeof(int));
    }
    family *f = &lib.fams[lib.n];
    memset(f, 0, sizeof *f);
    f->name = pool_add(name);
    lib.chain[lib.n] = lib.bucket[h];
    lib.bucket[h] = lib.n++;
    return f;
}

/* The first face for a slot wins */
static void add_face(const char *name, int bold, int italic, uint32_t path, int index)
{
    family *f = family_get(name);
    face_ref *s = &f->slot[(bold ? 1 : 0) | (italic ? 2 : 0)];
    if (!s->path)
        *s = (face_ref){ path, index };
}

/* A font file, mapped: only the pages of the tables read are touched */
typedef struct
{
    const char *mem;
    size_t len;
} src;

static int src_read(src *s, uint32_t off, void *buf, size_t n)
{
    if (off > s->len || n > s->len - off)
        return -1;
    memcpy(buf, s->mem + off, n);
    return 0;
}

static unsigned u16be(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static uint32_t u32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The best record for name id `id` as UTF-8: Windows US English first, then any
 * Unicode record, then Mac Roman. 0 if there is none. */
static int name_get(const uint8_t *t, size_t len, unsigned id, char *out, size_t cap)
{
    if (len < 6)
        return 0;
    unsigned count = u16be(t + 2), strings = u16be(t + 4);
    int best = -1, best_score = 0;
    for (unsigned i = 0; i < count && 6 + 12 * (size_t)(i + 1) <= len; i++)
    {
        const uint8_t *r = t + 6 + 12 * i;
        unsigned plat = u16be(r), enc = u16be(r + 2), lang = u16be(r + 4);
        if (u16be(r + 6) != id)
            continue;
        int score = plat == 3 && (enc == 0 || enc == 1 || enc == 10) ? (lang == 0x409 ? 4 : 3) : plat == 0 ? 3 : plat == 1 && enc == 0 ? 1 : 0;
        if (score > best_score)
            best = (int)i, best_score = score;
    }
    if (best < 0)
        return 0;
    const uint8_t *r = t + 6 + 12 * best;
    size_t slen = u16be(r + 8), soff = (size_t)strings + u16be(r + 10);
    if (soff > len || slen > len - soff)
        return 0;
    const uint8_t *s = t + soff;
    size_t o = 0;
    if (best_score == 1)
    {
        for (size_t i = 0; i < slen && o + 1 < cap; i++)
            out[o++] = s[i] < 0x80 ? (char)s[i] : '?';
    }
    else
    {
        for (size_t i = 0; i + 1 < slen && o + 5 < cap; i += 2)
        {
            unsigned c = u16be(s + i);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < slen)
            {
                unsigned lo = u16be(s + i + 2);
                if (lo >= 0xDC00 && lo < 0xE000)
                {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (c < 0x80)
                out[o++] = (char)c;
            else if (c < 0x800)
                out[o++] = (char)(0xC0 | c >> 6), out[o++] = (char)(0x80 | (c & 63));
            else if (c < 0x10000)
                out[o++] = (char)(0xE0 | c >> 12), out[o++] = (char)(0x80 | (c >> 6 & 63)), out[o++] = (char)(0x80 | (c & 63));
            else
                out[o++] = (char)(0xF0 | c >> 18), out[o++] = (char)(0x80 | (c >> 12 & 63)), out[o++] = (char)(0x80 | (c >> 6 & 63)),
                out[o++] = (char)(0x80 | (c & 63));
        }
    }
    out[o] = 0;
    return o > 0;
}

#ifndef _WIN32
/* Faces with other styles, used on Linux only for families with no regular face */
typedef struct
{
    uint32_t name, path;
    int index;
} fallback;

static struct
{
    fallback *v;
    int n, cap;
} fallbacks;
#endif

static void scan_face(src *s, uint32_t start, uint32_t path, int index)
{
    uint8_t head[12];
    if (src_read(s, start, head, 12))
        return;
    /* TrueType, OpenType-CFF or Apple 'true' outlines; not Type 1 wrapped in sfnt ('typ1') */
    if (memcmp(head, "\0\1\0\0", 4) && memcmp(head, "OTTO", 4) && memcmp(head, "true", 4))
        return;
    unsigned ntables = u16be(head + 4);
    if (ntables == 0 || ntables > 200)
        return;
    uint8_t dir[200 * 16];
    if (src_read(s, start + 12, dir, (size_t)ntables * 16))
        return;
    uint32_t name_off = 0, name_len = 0, os2_off = 0, os2_len = 0, post_off = 0;
    int outlines = 0;
    for (unsigned i = 0; i < ntables; i++)
    {
        const uint8_t *e = dir + 16 * i;
        if (!memcmp(e, "name", 4))
            name_off = u32be(e + 8), name_len = u32be(e + 12);
        else if (!memcmp(e, "OS/2", 4))
            os2_off = u32be(e + 8), os2_len = u32be(e + 12);
        else if (!memcmp(e, "post", 4))
            post_off = u32be(e + 8);
        else if (!memcmp(e, "glyf", 4) || !memcmp(e, "CFF ", 4))
            outlines |= 1;
        else if (!memcmp(e, "sbix", 4) || !memcmp(e, "CBDT", 4))
            outlines |= 2;          /* colour emoji: pictures, with empty outlines if any */
    }
    /* Bitmap-only faces (colour emoji) and CFF2 variable fonts have nothing stb_truetype can draw */
    if (outlines != 1 || !name_off || name_len < 6 || name_len > (1u << 20))
        return;
    if (name_off > s->len || name_len > s->len - name_off)
        return;
    const uint8_t *names = (const uint8_t *)s->mem + name_off;
    char fam[256];
#ifdef _WIN32
    /* Windows: name ID 1 and the bold/italic flags */
    int ok = name_get(names, name_len, 1, fam, sizeof fam);
    if (!ok)
        return;
    uint8_t os2[64], post[8];
    unsigned fs = os2_off && os2_len >= 64 && !src_read(s, os2_off, os2, 64) ? u16be(os2 + 62) : 0;
    int slant = post_off && !src_read(s, post_off, post, 8) && u32be(post + 4) != 0;
    add_face(fam, fs & 0x20, (fs & 1) || slant, path, index);
#else
    /* Linux and macOS: fontconfig's first family and style */
    char style[128];
    int ok = name_get(names, name_len, 16, fam, sizeof fam) || name_get(names, name_len, 1, fam, sizeof fam);
    if (ok && !name_get(names, name_len, 17, style, sizeof style) && !name_get(names, name_len, 2, style, sizeof style))
        style[0] = 0;
    (void)os2_off, (void)os2_len, (void)post_off;
    if (!ok)
        return;
    for (char *c = style; *c; c++)
        if (*c >= 'A' && *c <= 'Z')
            *c += 32;
    if (!strcmp(style, "regular") || !strcmp(style, "book") || !strcmp(style, "normal") || !strcmp(style, "roman"))
        add_face(fam, 0, 0, path, index);
    else if (!strcmp(style, "bold"))
        add_face(fam, 1, 0, path, index);
    else if (!strcmp(style, "italic") || !strcmp(style, "oblique"))
        add_face(fam, 0, 1, path, index);
    else if (!strcmp(style, "bold italic") || !strcmp(style, "bold oblique"))
        add_face(fam, 1, 1, path, index);
    else
    {
        if (fallbacks.n == fallbacks.cap)
        {
            fallbacks.cap = fallbacks.cap ? fallbacks.cap * 2 : 64;
            fallbacks.v = realloc(fallbacks.v, (size_t)fallbacks.cap * sizeof(fallback));
        }
        fallbacks.v[fallbacks.n++] = (fallback){ pool_add(fam), path, index };
    }
#endif
}

static int is_font_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || name[0] == '.')
        return 0;
    char ext[5] = { 0 };
    for (int i = 0; i < 4 && dot[1 + i]; i++)
        ext[i] = (char)(dot[1 + i] | 0x20);
    return !strcmp(ext, "ttf") || !strcmp(ext, "otf") || !strcmp(ext, "ttc");
}

static void scan_file(const char *path)
{
    src s = { NULL, 0 };
    if (!(s.mem = os_map_file(path, &s.len)))
        return;
    uint8_t head[12];
    uint32_t p = 0;
    if (!src_read(&s, 0, head, 12))
    {
        if (!memcmp(head, "ttcf", 4))
        {
            uint32_t n = u32be(head + 8);
            uint8_t offs[4 * 64];
            if (n > 64)
                n = 64;
            if (n && !src_read(&s, 12, offs, 4 * (size_t)n))
                for (uint32_t i = 0; i < n; i++)
                {
                    if (!p)
                        p = pool_add(path) + 1;
                    scan_face(&s, u32be(offs + 4 * i), p, (int)i);
                }
        }
        else
            scan_face(&s, 0, pool_add(path) + 1, 0);
    }
    os_unmap_file(s.mem, s.len);
}

typedef struct
{
    const char *dir;
    int depth;
} walk;

static void scan_dir(const char *dir, int depth);

static void on_entry(const char *name, int is_dir, void *user)
{
    const walk *w = user;
    char *path = os_path_join(w->dir, name);
    if (is_dir)
    {
        if (w->depth > 0)
            scan_dir(path, w->depth - 1);
    }
    else if (is_font_file(name))
        scan_file(path);
    free(path);
}

static void scan_dir(const char *dir, int depth)
{
    walk w = { dir, depth };
    os_list_dir(dir, on_entry, &w);
}

static int cmp_family(const void *pa, const void *pb)
{
    const family *a = pa, *b = pb;
    const char *x = lib.pool + a->name, *y = lib.pool + b->name;
    for (;; x++, y++)
    {
        int cx = *x >= 'A' && *x <= 'Z' ? *x + 32 : (unsigned char)*x;
        int cy = *y >= 'A' && *y <= 'Z' ? *y + 32 : (unsigned char)*y;
        if (cx != cy || !cx)
            return cx != cy ? cx - cy : strcmp(lib.pool + a->name, lib.pool + b->name);
    }
}

static void scan(void)
{
    if (lib.scanned)
        return;
    lib.scanned = 1;
    lib.bucket = malloc(NBUCKETS * sizeof(int));
    memset(lib.bucket, 0xFF, NBUCKETS * sizeof(int));
#ifdef _WIN32
    const char *windir = getenv("WINDIR");
    char *fonts = os_path_join(windir && *windir ? windir : "C:\\Windows", "Fonts");
    scan_dir(fonts, 0);
    free(fonts);
    const char *local = getenv("LOCALAPPDATA");
    if (local && *local)
    {
        char *user = os_path_join(local, "Microsoft\\Windows\\Fonts");
        scan_dir(user, 0);
        free(user);
    }
#else
#ifdef __APPLE__
    /* /System/Library/Fonts holds Supplemental/ and other subfolders */
    scan_dir("/System/Library/Fonts", 8);
    scan_dir("/Library/Fonts", 8);
    const char *mine[] = { "Library/Fonts" };
#else
    scan_dir("/usr/share/fonts", 8);
    scan_dir("/usr/local/share/fonts", 8);
    const char *mine[] = { ".local/share/fonts", ".fonts" };
#endif
    const char *home = os_home_dir();
    if (home && *home)
    {
        for (size_t i = 0; i < sizeof mine / sizeof *mine; i++)
        {
            char *d = os_path_join(home, mine[i]);
            scan_dir(d, 8);
            free(d);
        }
    }
    for (int i = 0; i < fallbacks.n; i++)
    {
        family *f = family_get(lib.pool + fallbacks.v[i].name);
        if (!f->slot[0].path)
            f->slot[0] = (face_ref){ fallbacks.v[i].path, fallbacks.v[i].index };
    }
    free(fallbacks.v);
    fallbacks.v = NULL;
    fallbacks.n = fallbacks.cap = 0;
#endif
    free(lib.bucket);
    free(lib.chain);
    lib.bucket = lib.chain = NULL;
    /* Only families with a regular face, sorted by name ignoring case; names starting
     * with a dot are macOS's hidden system faces */
    int n = 0;
    for (int i = 0; i < lib.n; i++)
        if (lib.fams[i].slot[0].path && lib.pool[lib.fams[i].name] != '.')
            lib.fams[n++] = lib.fams[i];
    lib.n = n;
    qsort(lib.fams, (size_t)n, sizeof(family), cmp_family);
}

int fonts_count(void)
{
    scan();
    return lib.n;
}

const char *fonts_name(int family)
{
    scan();
    return family >= 0 && family < lib.n ? lib.pool + lib.fams[family].name : "";
}

int fonts_find(const char *name)
{
    scan();
    for (int i = 0; i < lib.n; i++)
        if (!strcmp(lib.pool + lib.fams[i].name, name))
            return i;
    return -1;
}

int fonts_default_family(void)
{
    static const char *const wanted[] = { "Calibri", "Arial", "Helvetica", "Verdana", "Liberation Sans", "DejaVu Sans", "Noto Sans",
                                          "Cantarell", "Ubuntu" };
    for (size_t i = 0; i < sizeof wanted / sizeof *wanted; i++)
    {
        int f = fonts_find(wanted[i]);
        if (f >= 0)
            return f;
    }
    return 0;
}

/* ---- loaded faces ---- */

struct font_face
{
    const char *data;           /* the whole font file, mapped */
    size_t len;
    stbtt_fontinfo info;
    float asc, desc, gap;       /* unscaled, as ttf-parser reports them */
    uint32_t path;
    int index;
    int symbol;                 /* only a Windows Symbol cmap: characters live at U+F000 + code */
};

static font_face *loaded;

void fonts_release(void)
{
    if (loaded)
    {
        os_unmap_file(loaded->data, loaded->len);
        free(loaded);
        loaded = NULL;
    }
}

static int16_t s16(const uint8_t *p)
{
    return (int16_t)u16be(p);
}

/* A table of the face starting at `start`, from its directory; 0 if absent */
static uint32_t find_table(const uint8_t *d, size_t len, uint32_t start, const char *tag, uint32_t *tlen)
{
    if (start > len || len - start < 12)
        return 0;
    unsigned n = u16be(d + start + 4);
    for (unsigned i = 0; i < n && start + 12 + 16 * (size_t)(i + 1) <= len; i++)
    {
        const uint8_t *e = d + start + 12 + 16 * i;
        if (!memcmp(e, tag, 4))
        {
            uint32_t off = u32be(e + 8);
            *tlen = u32be(e + 12);
            return off <= len && *tlen <= len - off ? off : 0;
        }
    }
    return 0;
}

/* Ascender, descender and line gap by ttf-parser's rules */
static void vmetrics(font_face *f, size_t len)
{
    const uint8_t *d = (const uint8_t *)f->data;
    uint32_t hl = 0, ol = 0;
    uint32_t hhea = find_table(d, len, (uint32_t)f->info.fontstart, "hhea", &hl);
    uint32_t os2 = find_table(d, len, (uint32_t)f->info.fontstart, "OS/2", &ol);
    int ha = hhea && hl >= 10 ? s16(d + hhea + 4) : 0, hd = hhea && hl >= 10 ? s16(d + hhea + 6) : 0;
    int hg = hhea && hl >= 10 ? s16(d + hhea + 8) : 0;
    int has_os2 = os2 && ol >= 78;
    int ta = has_os2 ? s16(d + os2 + 68) : 0, td = has_os2 ? s16(d + os2 + 70) : 0, tg = has_os2 ? s16(d + os2 + 72) : 0;
    int wa = has_os2 ? s16(d + os2 + 74) : 0, wd = has_os2 ? -s16(d + os2 + 76) : 0;
    if (has_os2 && u16be(d + os2) >= 4 && (u16be(d + os2 + 62) & 0x80))
    {
        f->asc = (float)ta;
        f->desc = (float)td;
        f->gap = (float)tg;
        return;
    }
    int a = ha, de = hd, g = hg;
    if (a == 0 && has_os2)
        a = ta != 0 ? ta : wa;
    if (de == 0 && has_os2)
        de = td != 0 ? td : wd;
    if ((ha == 0 || hd == 0) && has_os2)
        g = ta != 0 || td != 0 ? tg : 0;
    f->asc = (float)a;
    f->desc = (float)de;
    f->gap = (float)g;
}

int fonts_load(int fam_index, int bold, int italic, paint_font *out)
{
    scan();
    if (fam_index < 0 || fam_index >= lib.n)
        return -1;
    const family *fam = &lib.fams[fam_index];
    face_ref face = fam->slot[(bold ? 1 : 0) | (italic ? 2 : 0)];
    int fake_bold = 0, fake_italic = 0;
    if (!face.path)
    {
        if (bold && italic && fam->slot[1].path)
            face = fam->slot[1], fake_italic = 1;
        else if (bold && italic && fam->slot[2].path)
            face = fam->slot[2], fake_bold = 1;
        else
            face = fam->slot[0], fake_bold = bold, fake_italic = italic;
    }
    if (!loaded || loaded->path != face.path || loaded->index != face.index)
    {
        fonts_release();
        font_face *f = calloc(1, sizeof *f);
        if (!f || !(f->data = os_map_file(lib.pool + face.path - 1, &f->len)))
        {
            free(f);
            return -1;
        }
        size_t len = f->len;
        const unsigned char *d = (const unsigned char *)f->data;
        int off = stbtt_GetFontOffsetForIndex(d, face.index);
        int ok = off >= 0 && stbtt_InitFont(&f->info, d, off);
        if (!ok && off >= 0 && f->info.head)
        {
            /* Symbol fonts (Wingdings, Webdings...) have only a (3, 0) cmap, which stb_truetype
             * skips; it reads the same way, with the characters moved up to U+F0xx as Windows does */
            uint32_t cl = 0, cmap = find_table(d, len, (uint32_t)off, "cmap", &cl);
            for (unsigned i = 0; cmap && cl >= 4 && i < u16be(d + cmap + 2) && 4 + 8 * (size_t)(i + 1) <= cl; i++)
            {
                const uint8_t *e = d + cmap + 4 + 8 * i;
                if (u16be(e) == 3 && u16be(e + 2) == 0 && u32be(e + 4) < cl)
                {
                    f->info.index_map = (int)(cmap + u32be(e + 4));
                    f->info.indexToLocFormat = (int)u16be(d + f->info.head + 50);
                    f->symbol = ok = 1;
                    break;
                }
            }
        }
        if (!ok)
        {
            os_unmap_file(f->data, len);
            free(f);
            return -1;
        }
        /* Kerning from the 'kern' table only, as ab_glyph does */
        f->info.gpos = 0;
        vmetrics(f, len);
        if (f->asc - f->desc <= 0.0f)
        {
            os_unmap_file(f->data, len);
            free(f);
            return -1;
        }
        f->path = face.path;
        f->index = face.index;
        loaded = f;
    }
    out->face = loaded;
    out->fake_bold = fake_bold;
    out->fake_italic = fake_italic;
    return 0;
}

/* ---- layout ---- */

typedef struct
{
    const font_face *f;
    float scale, ascent, line_h;
} scaled;

static scaled scale_of(const paint_font *pf, float px)
{
    scaled s;
    s.f = pf->face;
    s.scale = px / (s.f->asc - s.f->desc);
    s.ascent = s.f->asc * s.scale;
    s.line_h = s.ascent - s.f->desc * s.scale + s.f->gap * s.scale;
    return s;
}

static int glyph_of(const font_face *f, int cp)
{
    int g = stbtt_FindGlyphIndex(&f->info, cp);
    return g || !f->symbol || cp >= 0x100 ? g : stbtt_FindGlyphIndex(&f->info, 0xF000 + cp);
}

static float h_advance(const scaled *s, int g)
{
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&s->f->info, g, &adv, &lsb);
    return (float)adv * s->scale;
}

static float kern(const scaled *s, int a, int b)
{
    return s->f->info.kern ? (float)stbtt_GetGlyphKernAdvance(&s->f->info, a, b) * s->scale : 0.0f;
}

/* Width of the line starting at text up to '\n' or the end; *end gets where it stopped */
static float line_width(const scaled *s, const char *text, const char **end)
{
    float x = 0.0f;
    int prev = -1, len;
    while (*text && *text != '\n')
    {
        int id = glyph_of(s->f, utf8_decode(text, &len));
        text += len;
        if (prev >= 0)
            x += kern(s, prev, id);
        x += h_advance(s, id);
        prev = id;
    }
    *end = text;
    return x;
}

static int bold_offset(float px)
{
    return (int)fmaxf(ceilf(px / 28.0f), 1.0f);
}

text_size ptext_measure(const paint_font *f, float px, const char *text)
{
    scaled s = scale_of(f, px);
    float width = 0.0f;
    int lines = 0;
    for (;;)
    {
        lines++;
        width = fmaxf(width, line_width(&s, text, &text));
        if (!*text)
            break;
        text++;
    }
    if (f->fake_italic)
        width += px * ITALIC_SHEAR;
    if (f->fake_bold)
        width += (float)bold_offset(px);
    return (text_size){ width, (float)lines * s.line_h };
}

static int shear_of(const paint_font *f, float baseline, int py)
{
    return f->fake_italic ? (int)roundf((baseline - (float)py) * ITALIC_SHEAR) : 0;
}

irect ptext_render(mask *m, const paint_font *f, float px, const char *text, pt origin, int underline, int strike)
{
    scaled s = scale_of(f, px);
    const stbtt_fontinfo *info = &f->face->info;
    int bold = f->fake_bold ? bold_offset(px) : 0;
    irect out = IRECT_EMPTY;
    unsigned char *bmp = NULL;
    size_t bmp_cap = 0;
    for (int li = 0;; li++)
    {
        float baseline = origin.y + s.ascent + (float)li * s.line_h;
        float x = origin.x;
        int prev = -1, len;
        while (*text && *text != '\n')
        {
            int id = glyph_of(f->face, utf8_decode(text, &len));
            text += len;
            if (prev >= 0)
                x += kern(&s, prev, id);
            float gx = x;
            x += h_advance(&s, id);
            prev = id;
            if (stbtt_IsGlyphEmpty(info, id))
                continue;
            float fx = floorf(gx), fy = floorf(baseline);
            int ix0, iy0, ix1, iy1;
            stbtt_GetGlyphBitmapBoxSubpixel(info, id, s.scale, s.scale, gx - fx, baseline - fy, &ix0, &iy0, &ix1, &iy1);
            int w = ix1 - ix0, h = iy1 - iy0;
            if (w <= 0 || h <= 0)
                continue;
            if ((size_t)w * h > bmp_cap)
            {
                bmp_cap = (size_t)w * h;
                free(bmp);
                bmp = malloc(bmp_cap);
                if (!bmp)
                {
                    bmp_cap = 0;
                    continue;
                }
            }
            stbtt_MakeGlyphBitmapSubpixel(info, bmp, w, h, w, s.scale, s.scale, gx - fx, baseline - fy, id);
            int bx = (int)fx + ix0, by = (int)fy + iy0;
            for (int y = 0; y < h; y++)
            {
                int py = by + y;
                int sx = bx + shear_of(f, baseline, py);
                for (int xx = 0; xx < w; xx++)
                {
                    unsigned char c = bmp[(size_t)y * w + xx];
                    if (!c)
                        continue;
                    for (int k = 0; k <= bold; k++)
                        mask_max_at(m, sx + xx + k, py, (float)c / 255.0f);
                }
            }
            int top = shear_of(f, baseline, by), bottom = shear_of(f, baseline, by + h);
            out = irect_union(out, irect_make(bx + (top < bottom ? top : bottom) - 1, by - 1, bx + w + (top > bottom ? top : bottom) + bold + 1, by + h + 1));
        }
        float thick = fmaxf(px / 16.0f, 1.0f);
        float rules[2];
        int nrules = 0;
        if (underline && x > origin.x)
            rules[nrules++] = baseline + fmaxf(px * 0.1f, 1.0f);
        if (strike && x > origin.x)
            rules[nrules++] = baseline - px * 0.3f;
        for (int i = 0; i < nrules; i++)
        {
            float y = rules[i];
            pt quad[4] = { { origin.x, y }, { x, y }, { x, y + thick }, { origin.x, y + thick } };
            out = irect_union(out, mask_fill_polygon(m, quad, 4, 1.0f));
        }
        if (!*text)
            break;
        text++;
    }
    free(bmp);
    return irect_intersect(out, mask_bounds(m));
}

/* ---- caret geometry ---- */

static void caret_set(caret_layout *c, int nlines, int nxs)
{
    free(c->start);
    free(c->xs);
    c->nlines = nlines;
    c->start = malloc(((size_t)nlines + 1) * sizeof(int));
    c->xs = malloc((size_t)nxs * sizeof(float));
}

void caret_layout_init(caret_layout *c)
{
    c->start = NULL;
    c->xs = NULL;
    caret_set(c, 1, 1);
    c->start[0] = 0;
    c->start[1] = 1;
    c->xs[0] = 0.0f;
    c->line_h = 16.0f;
}

void caret_layout_free(caret_layout *c)
{
    free(c->start);
    free(c->xs);
    c->start = NULL;
    c->xs = NULL;
    c->nlines = 0;
}

void caret_layout_build(caret_layout *c, const paint_font *f, float px, const char *text)
{
    scaled s = scale_of(f, px);
    int chars = utf8_count(text), lines = 1;
    for (const char *p = text; *p; p++)
        lines += *p == '\n';
    caret_set(c, lines, chars + 1);
    c->line_h = s.line_h;
    int n = 0, len;
    for (int l = 0; l < lines; l++)
    {
        c->start[l] = n;
        c->xs[n++] = 0.0f;
        float x = 0.0f;
        int prev = -1;
        while (*text && *text != '\n')
        {
            int id = glyph_of(s.f, utf8_decode(text, &len));
            text += len;
            if (prev >= 0)
                x += kern(&s, prev, id);
            x += h_advance(&s, id);
            c->xs[n++] = x;
            prev = id;
        }
        if (*text)
            text++;
    }
    c->start[lines] = n;
}

void caret_line_col(const caret_layout *c, int index, int *line, int *col)
{
    int i = index;
    for (int l = 0; l < c->nlines; l++)
    {
        int len = caret_line_len(c, l) - 1;
        if (i <= len)
        {
            *line = l;
            *col = i;
            return;
        }
        i -= len + 1;
    }
    *line = c->nlines - 1;
    *col = caret_line_len(c, c->nlines - 1) - 1;
}

int caret_index(const caret_layout *c, int line, int col)
{
    return c->start[line] + col;
}

pt caret_pos(const caret_layout *c, int index)
{
    int l, col;
    caret_line_col(c, index, &l, &col);
    return pt_make(c->xs[c->start[l] + col], (float)l * c->line_h);
}

int caret_col_at(const caret_layout *c, int line, float x)
{
    const float *xs = c->xs + c->start[line];
    int best = 0;
    for (int i = 1; i < caret_line_len(c, line); i++)
        if (fabsf(xs[i] - x) < fabsf(xs[best] - x))
            best = i;
    return best;
}

int caret_index_at(const caret_layout *c, float x, float y)
{
    float l = floorf(fmaxf(y / c->line_h, 0.0f));
    int line = l >= (float)(c->nlines - 1) ? c->nlines - 1 : (int)l;
    return caret_index(c, line, caret_col_at(c, line, x));
}
