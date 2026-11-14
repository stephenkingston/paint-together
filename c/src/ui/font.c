/*
 * Paint (C port) - the fonts the window's own text is drawn with
 *
 * The faces are found when the program starts (fc-match on Linux, as the
 * Rust build does, else a search of the font folders; the Windows font
 * folder there) but each file is read only when text first needs it. A
 * character none of the four UI faces has comes from a fallback face, also
 * read on first need.
 *
 * Glyphs are rendered unhinted, like egui's, and kept in one fixed table
 * shared by every size: a slot is replaced when another glyph hashes to it,
 * so the cache never grows. Coverage is darkened with 2c - c^2 (what egui
 * does for dark text on light backgrounds) so small text isn't thin and grey.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "ui/font.h"
#include "stb_truetype.h"

enum
{
    FACE_UI = 0,        /* REGULAR, BOLD, SERIF, SERIF|BOLD index faces[] directly */
    FIRST_FALLBACK = 4,
    MAX_FACES = 10,
    SLOTS = 1024,       /* glyph cache entries */
    BOLD_IN_COLLECTION = -2,    /* face index: the one named Bold, found when read */
};

typedef struct
{
    char *path;
    int index;          /* face inside a .ttc */
    int state;          /* 0 not read yet, 1 ready, -1 unusable */
    unsigned char *data;
    stbtt_fontinfo info;
} face;

struct font
{
    float px;
    int style;
    int ascent, height;
    float asc;
    font *next;
};

typedef struct
{
    const font *f;      /* NULL: empty */
    uint32_t code;
    int16_t x0, y0, w, h;
    int8_t face;
    int gi;
    float advance;
    uint8_t *bitmap;    /* rendered on first draw */
} glyph;

static face faces[MAX_FACES];
static int n_faces = FIRST_FALLBACK;
static int fallbacks_found;
static font *fonts;
static glyph slots[SLOTS];
static uint8_t darken[256];

/* ---- finding the files ---- */

static int file_exists(const char *path)
{
    int64_t size;
    return path && os_file_size(path, &size, NULL) == 0 && size > 0;
}

static void set_face(int which, const char *path, int index)
{
    if (faces[which].path || !file_exists(path))
        return;
    faces[which].path = strdup(path);
    faces[which].index = index;
}

#ifdef _WIN32
static void find_faces(void)
{
    const char *windir = getenv("WINDIR");
    char *dir = os_path_join(windir ? windir : "C:\\Windows", "Fonts");
    /* Segoe UI from Vista on; Tahoma on XP */
    const char *names[4][2] = { { "segoeui.ttf", "tahoma.ttf" }, { "segoeuib.ttf", "tahomabd.ttf" },
                                { "times.ttf", "georgia.ttf" }, { "timesbd.ttf", "georgiab.ttf" } };
    for (int w = 0; w < 4; w++)
        for (int k = 0; k < 2; k++)
        {
            char *p = os_path_join(dir, names[w][k]);
            set_face(w, p, 0);
            free(p);
        }
    free(dir);
}

static void find_fallbacks(void)
{
    const char *windir = getenv("WINDIR");
    char *dir = os_path_join(windir ? windir : "C:\\Windows", "Fonts");
    const char *names[] = { "arial.ttf", "seguisym.ttf", "msyh.ttc", "msgothic.ttc", "malgun.ttf", "simsun.ttc", "mingliu.ttc" };
    for (size_t i = 0; i < sizeof names / sizeof *names && n_faces < MAX_FACES; i++)
    {
        char *p = os_path_join(dir, names[i]);
        if (file_exists(p))
        {
            faces[n_faces].path = p;
            faces[n_faces++].index = 0;
        }
        else
            free(p);
    }
    free(dir);
}
#elif defined(__APPLE__)
static void find_faces(void)
{
    /* As theme.rs: San Francisco (its default instance: stb_truetype doesn't apply
     * variations), Helvetica Neue Bold for bold, Times New Roman for the serif */
    set_face(FONT_REGULAR, "/System/Library/Fonts/SFNS.ttf", 0);
    set_face(FONT_REGULAR, "/System/Library/Fonts/HelveticaNeue.ttc", 0);
    set_face(FONT_BOLD, "/System/Library/Fonts/HelveticaNeue.ttc", BOLD_IN_COLLECTION);
    set_face(FONT_SERIF, "/System/Library/Fonts/Supplemental/Times New Roman.ttf", 0);
    set_face(FONT_SERIF | FONT_BOLD, "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf", 0);
}

static void find_fallbacks(void)
{
    static const char *const paths[] = { "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/System/Library/Fonts/Apple Symbols.ttf",
                                         "/System/Library/Fonts/Hiragino Sans GB.ttc" };
    for (int i = 0; i < 3 && n_faces < MAX_FACES; i++)
        if (file_exists(paths[i]))
        {
            faces[n_faces].path = strdup(paths[i]);
            faces[n_faces++].index = 0;
        }
}
#else
/* What the desktop's fontconfig prefers for a pattern */
static void fc_match(int which, const char *pattern)
{
    char *argv[] = { "fc-match", "--format=%{file}\t%{index}", (char *)pattern, NULL };
    int status = 0;
    char *out = os_run_capture(argv, &status);
    char *tab = out ? strchr(out, '\t') : NULL;
    if (tab && status == 0)
    {
        *tab = 0;
        set_face(which, out, atoi(tab + 1));
    }
    free(out);
}

typedef struct
{
    const char *const *names;   /* best first */
    int n;
    int best;
    char *path;
} search;

static void search_dir(const char *dir, search *s, int depth);

typedef struct
{
    const char *dir;
    search *s;
    int depth;
} visit;

static void search_entry(const char *name, int is_dir, void *user)
{
    visit *v = user;
    char *p = os_path_join(v->dir, name);
    if (is_dir)
    {
        if (v->depth < 4)
            search_dir(p, v->s, v->depth + 1);
    }
    else
        for (int i = 0; i < v->s->best; i++)
            if (!strcmp(name, v->s->names[i]))
            {
                free(v->s->path);
                v->s->path = strdup(p);
                v->s->best = i;
                break;
            }
    free(p);
}

static void search_dir(const char *dir, search *s, int depth)
{
    visit v = { dir, s, depth };
    os_list_dir(dir, search_entry, &v);
}

/* The best-ranked of `names` found anywhere under the usual font folders (malloc'd) */
static char *find_file(const char *const *names, int n)
{
    search s = { names, n, n, NULL };
    const char *home = os_home_dir();
    char *user[2] = { home ? os_path_join(home, ".local/share/fonts") : NULL, home ? os_path_join(home, ".fonts") : NULL };
    const char *dirs[] = { "/usr/share/fonts", "/usr/local/share/fonts", user[0], user[1] };
    for (int i = 0; i < 4; i++)
        if (dirs[i])
            search_dir(dirs[i], &s, 0);
    free(user[0]);
    free(user[1]);
    return s.path;
}

static void find_faces(void)
{
    fc_match(FONT_REGULAR, "sans-serif");
    fc_match(FONT_BOLD, "sans-serif:bold");
    fc_match(FONT_SERIF, "serif");
    fc_match(FONT_SERIF | FONT_BOLD, "serif:bold");
    /* No fontconfig: the common faces, best first */
    static const char *const sans[] = { "NotoSans-Regular.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf", "Arimo-Regular.ttf",
                                        "Cantarell-Regular.otf", "Ubuntu-R.ttf", "FreeSans.ttf", "AdwaitaSans-Regular.ttf" };
    static const char *const sans_bold[] = { "NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf", "Arimo-Bold.ttf",
                                             "Cantarell-Bold.otf", "Ubuntu-B.ttf", "FreeSansBold.ttf" };
    static const char *const serif[] = { "NotoSerif-Regular.ttf", "DejaVuSerif.ttf", "LiberationSerif-Regular.ttf", "Tinos-Regular.ttf",
                                         "FreeSerif.ttf" };
    static const char *const serif_bold[] = { "NotoSerif-Bold.ttf", "DejaVuSerif-Bold.ttf", "LiberationSerif-Bold.ttf", "Tinos-Bold.ttf",
                                              "FreeSerifBold.ttf" };
    const char *const *lists[4] = { sans, sans_bold, serif, serif_bold };
    int counts[4] = { 8, 7, 5, 5 };
    for (int w = 0; w < 4; w++)
        if (!faces[w].path)
        {
            char *p = find_file(lists[w], counts[w]);
            set_face(w, p, 0);
            free(p);
        }
}

static void find_fallbacks(void)
{
    static const char *const names[] = { "DejaVuSans.ttf", "NotoSans-Regular.ttf", "NotoSansSymbols2-Regular.ttf", "NotoSansSymbols-Regular.ttf",
                                         "NotoSansCJK-Regular.ttc", "DroidSansFallbackFull.ttf", "Symbola.ttf" };
    for (int i = 0; i < 7 && n_faces < MAX_FACES; i++)
    {
        char *p = find_file(&names[i], 1);
        if (p)
        {
            faces[n_faces].path = p;
            faces[n_faces++].index = 0;
        }
    }
}
#endif

/* The face of a collection whose style (name 2) is "Bold", else the first */
static int bold_index(const unsigned char *data)
{
    int n = stbtt_GetNumberOfFonts(data);
    for (int i = 0; i < n; i++)
    {
        stbtt_fontinfo info;
        int len = 0;
        if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, i)))
            continue;
        const char *name = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 2);
        /* UTF-16BE */
        if (name && len == 8 && !memcmp(name, "\0B\0o\0l\0d", 8))
            return i;
    }
    return 0;
}

static int load(int which)
{
    face *fc = &faces[which];
    if (fc->state)
        return fc->state > 0;
    fc->state = -1;
    size_t len = 0;
    if (!fc->path || !(fc->data = (unsigned char *)os_read_file(fc->path, &len)))
        return 0;
    if (fc->index == BOLD_IN_COLLECTION)
        fc->index = bold_index(fc->data);
    int offset = stbtt_GetFontOffsetForIndex(fc->data, fc->index);
    if (offset < 0 || !stbtt_InitFont(&fc->info, fc->data, offset))
    {
        free(fc->data);
        fc->data = NULL;
        return 0;
    }
    fc->state = 1;
    return 1;
}

int font_init(void)
{
    for (int i = 0; i < 256; i++)
        darken[i] = (uint8_t)((2 * 255 * i - i * i + 127) / 255);
    find_faces();
    /* A missing style stands in with what there is */
    if (!faces[FONT_REGULAR].path)
        set_face(FONT_REGULAR, faces[FONT_SERIF].path, faces[FONT_SERIF].index);
    for (int w = 1; w < 4; w++)
    {
        int base = faces[w & FONT_SERIF].path ? w & FONT_SERIF : FONT_REGULAR;
        if (!faces[w].path)
            set_face(w, faces[base].path, faces[base].index);
    }
    return load(FONT_REGULAR) ? 0 : -1;
}

/* The face of `style` itself, or the regular one if it can't be read */
static int main_face(int style)
{
    return load(style & 3) ? style & 3 : FONT_REGULAR;
}

font *font_get(float px, int style)
{
    style &= 3;
    for (font *f = fonts; f; f = f->next)
        if (f->px == px && f->style == style)
            return f;
    font *f = calloc(1, sizeof *f);
    f->px = px;
    f->style = style;
    const stbtt_fontinfo *info = &faces[main_face(style)].info;
    int asc = 0, desc = 0, gap = 0;
    if (faces[main_face(style)].state > 0)
        stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
    float scale = faces[main_face(style)].state > 0 ? stbtt_ScaleForMappingEmToPixels(info, px) : 0;
    f->asc = asc * scale;
    f->ascent = (int)(f->asc + 0.5f);
    f->height = (int)ceilf((asc - desc + gap) * scale - 0.01f);
    f->next = fonts;
    fonts = f;
    return f;
}

int font_height(const font *f)
{
    return f->height;
}

int font_ascent(const font *f)
{
    return f->ascent;
}

/* ---- glyphs ---- */

static int face_for(const font *f, uint32_t code, int *gi)
{
    int first = main_face(f->style);
    int order[2] = { first, FONT_REGULAR };
    for (int k = 0; k < 2; k++)
        if (load(order[k]) && (*gi = stbtt_FindGlyphIndex(&faces[order[k]].info, (int)code)))
            return order[k];
    if (!fallbacks_found)
    {
        fallbacks_found = 1;
        find_fallbacks();
    }
    for (int w = FIRST_FALLBACK; w < n_faces; w++)
        if (load(w) && (*gi = stbtt_FindGlyphIndex(&faces[w].info, (int)code)))
            return w;
    *gi = 0;    /* the main face's "missing" box */
    return first;
}

static glyph *glyph_get(const font *f, uint32_t code)
{
    uint32_t h = (code * 2654435761u) ^ (uint32_t)((uintptr_t)f >> 4) * 40503u;
    glyph *g = &slots[(h ^ h >> 15) % SLOTS];
    if (g->f == f && g->code == code)
        return g;
    free(g->bitmap);
    memset(g, 0, sizeof *g);
    g->f = f;
    g->code = code;
    g->face = (int8_t)face_for(f, code, &g->gi);
    face *fc = &faces[g->face];
    if (fc->state <= 0)
        return g;
    float scale = stbtt_ScaleForMappingEmToPixels(&fc->info, f->px);
    int adv = 0, lsb = 0, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&fc->info, g->gi, &adv, &lsb);
    g->advance = adv * scale;
    stbtt_GetGlyphBitmapBox(&fc->info, g->gi, scale, scale, &x0, &y0, &x1, &y1);
    g->x0 = (int16_t)x0;
    g->y0 = (int16_t)y0;
    g->w = (int16_t)(x1 - x0);
    g->h = (int16_t)(y1 - y0);
    return g;
}

static void glyph_render(glyph *g)
{
    if (g->bitmap || g->w <= 0 || g->h <= 0)
        return;
    face *fc = &faces[g->face];
    float scale = stbtt_ScaleForMappingEmToPixels(&fc->info, g->f->px);
    g->bitmap = malloc((size_t)g->w * (size_t)g->h);
    stbtt_MakeGlyphBitmap(&fc->info, g->bitmap, g->w, g->h, g->w, scale, scale, g->gi);
    for (int i = 0; i < g->w * g->h; i++)
        g->bitmap[i] = darken[g->bitmap[i]];
}

static float kern(const font *f, const glyph *a, const glyph *b)
{
    if (!a || a->face != b->face || faces[a->face].state <= 0)
        return 0;
    const stbtt_fontinfo *info = &faces[a->face].info;
    return stbtt_GetGlyphKernAdvance(info, a->gi, b->gi) * stbtt_ScaleForMappingEmToPixels(info, f->px);
}

/* Next code point of UTF-8 text (a bad byte stands for itself) */
static uint32_t next_code(const char **s, const char *end)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (more)
    {
        c &= 0x3F >> more;
        for (int i = 0; i < more && (const char *)p < end && (*p & 0xC0) == 0x80; i++)
            c = c << 6 | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    return c;
}

/* Width of text as a float (what layout sums) */
static float measure(font *f, const char *s, int len)
{
    const char *end = s + (len < 0 ? (int)strlen(s) : len);
    float x = 0;
    glyph prev = { 0 };
    int have_prev = 0;
    while (s < end)
    {
        uint32_t c = next_code(&s, end);
        if (c == '\n')
            continue;
        glyph *g = glyph_get(f, c);
        if (have_prev)
            x += kern(f, &prev, g);
        x += g->advance;
        prev = *g;
        have_prev = 1;
    }
    return x;
}

int text_width(font *f, const char *utf8, int len)
{
    return (int)(measure(f, utf8, len) + 0.5f);
}

int text_draw(surface *dst, font *f, int x, int y, const char *utf8, int len, color c)
{
    const char *s = utf8, *end = utf8 + (len < 0 ? (int)strlen(utf8) : len);
    float pen = (float)x;
    int base = y + f->ascent;
    glyph prev = { 0 };
    int have_prev = 0;
    while (s < end)
    {
        uint32_t code = next_code(&s, end);
        if (code == '\n')
            continue;
        glyph *g = glyph_get(f, code);
        if (have_prev)
            pen += kern(f, &prev, g);
        int gx = (int)floorf(pen + 0.5f) + g->x0;
        if (g->w > 0 && gx < dst->clip.x + dst->clip.w && gx + g->w > dst->clip.x)
        {
            glyph_render(g);
            if (g->bitmap)
                gfx_mask(dst, gx, base + g->y0, g->bitmap, g->w, g->h, g->w, c);
        }
        pen += g->advance;
        prev = *g;
        have_prev = 1;
    }
    return (int)(pen + 0.5f);
}

/* ---- lines ---- */

/* Bytes of text that fit in `max` pixels */
static int fit(font *f, const char *s, int len, float max)
{
    const char *p = s, *end = s + len;
    float x = 0;
    glyph prev = { 0 };
    int have_prev = 0;
    while (p < end)
    {
        const char *q = p;
        glyph *g = glyph_get(f, next_code(&q, end));
        x += (have_prev ? kern(f, &prev, g) : 0) + g->advance;
        if (x > max + 0.05f)
            break;
        prev = *g;
        have_prev = 1;
        p = q;
    }
    return (int)(p - s);
}

void text_in(surface *dst, font *f, rect r, int align, const char *utf8, color c)
{
    int len = (int)strlen(utf8);
    float w = measure(f, utf8, len);
    int y = r.y + (r.h - f->height + 1) / 2;
    if (w > r.w + 0.05f)
    {
        /* Cut with an ellipsis */
        const char *dots = faces[main_face(f->style)].state > 0 && stbtt_FindGlyphIndex(&faces[main_face(f->style)].info, 0x2026) ? "\xE2\x80\xA6" : "...";
        float dw = measure(f, dots, -1);
        int n = fit(f, utf8, len, r.w - dw);
        while (n > 0 && utf8[n - 1] == ' ')
            n--;
        int x = text_draw(dst, f, r.x, y, utf8, n, c);
        text_draw(dst, f, x, y, dots, -1, c);
        return;
    }
    int x = align == ALIGN_CENTER ? r.x + (int)floorf((r.w - w) / 2 + 0.5f) : align == ALIGN_RIGHT ? r.x + r.w - (int)ceilf(w - 0.05f) : r.x;
    text_draw(dst, f, x, y, utf8, len, c);
}

int text_wrap(surface *dst, font *f, rect r, int align, const char *utf8, color c)
{
    const char *s = utf8;
    int y = r.y;
    while (*s)
    {
        /* The longest run of whole words that fits, else as many characters as fit */
        const char *nl = strchr(s, '\n');
        int line = nl ? (int)(nl - s) : (int)strlen(s);
        int n = fit(f, s, line, (float)r.w);
        if (n < line)
        {
            int k = n;
            while (k > 0 && s[k] != ' ')
                k--;
            n = k > 0 ? k : n > 0 ? n : 1;
            while (n < line && (s[n] & 0xC0) == 0x80)
                n++;
        }
        int shown = n;
        while (shown > 0 && s[shown - 1] == ' ')
            shown--;
        if (dst)
        {
            int w = (int)ceilf(measure(f, s, shown) - 0.05f);
            int x = align == ALIGN_CENTER ? r.x + (r.w - w) / 2 : align == ALIGN_RIGHT ? r.x + r.w - w : r.x;
            text_draw(dst, f, x, y, s, shown, c);
        }
        y += f->height;
        s += n;
        while (*s == ' ')
            s++;
        if (s == nl)
            s++;
    }
    return y - r.y;
}
