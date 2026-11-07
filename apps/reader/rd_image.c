/* rd_image.c — see rd_image.h */
#include <string.h>
#include "rd_image.h"
#include "rd_inflate.h"
#include "tjpgd/tjpgd.h"

#define PNG_MAX_W    4096u
#define PNG_MAX_H    16384u
#define JPG_MAX_DIM  8192u
#define MAX_PIXELS   50000000u
#define SAMPLE_CAP   2000000u     /* source pixels actually read per image */
#define JPG_POOL     16384u

struct arena { uint8_t *p; uint32_t left; };

static void *arena_alloc(struct arena *a, uint32_t n)
{
    n = (n + 7u) & ~7u;
    if (n > a->left) return 0;
    void *r = a->p;
    a->p += n;
    a->left -= n;
    return r;
}

/* ---- box-filter downscaler ---------------------------------------------------
 * Every sampled source pixel lands in exactly one output cell (floor mapping), and
 * a cell is the mean of its pixels. Only a band of output rows is accumulated, so
 * JPEG's MCU-by-MCU order and PNG's row-by-row order both work. */

struct scaler {
    uint16_t *out;
    int sw, sh, ow, oh, step;
    uint16_t *xm;               /* source x -> output column */
    uint32_t *acc;              /* band_rows * ow * {r,g,b,count} */
    int band_rows, base;        /* output row held in acc row 0 */
    uint16_t bg565;
};

static uint16_t pack565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static int sc_init(struct scaler *s, struct arena *ar, uint16_t *out, int sw, int sh,
                   int max_w, int max_h, uint32_t bg, int mcu_rows)
{
    s->out = out;
    s->sw = sw;
    s->sh = sh;
    if (sw <= max_w && sh <= max_h) {
        s->ow = sw;
        s->oh = sh;
    } else if ((uint32_t)sw * (uint32_t)max_h > (uint32_t)sh * (uint32_t)max_w) {
        s->ow = max_w;
        s->oh = (int)((uint32_t)sh * (uint32_t)max_w / (uint32_t)sw);
    } else {
        s->oh = max_h;
        s->ow = (int)((uint32_t)sw * (uint32_t)max_h / (uint32_t)sh);
    }
    if (s->ow < 1) s->ow = 1;
    if (s->oh < 1) s->oh = 1;

    /* Skip pixels on very large images, but never so many that a cell is left empty. */
    s->step = 1;
    while ((uint32_t)(sw / s->step) * (uint32_t)(sh / s->step) > SAMPLE_CAP) s->step++;
    int rmax = sw / s->ow;
    if (sh / s->oh < rmax) rmax = sh / s->oh;
    if (rmax < 1) rmax = 1;
    if (s->step > rmax) s->step = rmax;

    s->bg565 = pack565((bg >> 16) & 255u, (bg >> 8) & 255u, bg & 255u);
    s->band_rows = (mcu_rows * s->oh + sh - 1) / sh + 2;
    s->base = 0;
    s->xm = (uint16_t *)arena_alloc(ar, (uint32_t)sw * 2u);
    s->acc = (uint32_t *)arena_alloc(ar, (uint32_t)s->band_rows * (uint32_t)s->ow * 16u);
    if (!s->xm || !s->acc) return RD_IMG_ERR_MEM;
    memset(s->acc, 0, (size_t)s->band_rows * (size_t)s->ow * 16u);
    for (int x = 0; x < sw; x++) s->xm[x] = (uint16_t)((uint32_t)x * (uint32_t)s->ow / (uint32_t)sw);
    for (uint32_t i = 0, n = (uint32_t)s->ow * (uint32_t)s->oh; i < n; i++) out[i] = s->bg565;
    return 0;
}

/* Write output rows [base, upto) and slide the band down. */
static void sc_flush_rows(struct scaler *s, int upto)
{
    if (upto > s->oh) upto = s->oh;
    int n = upto - s->base;
    if (n <= 0) return;
    int rows = n < s->band_rows ? n : s->band_rows;
    for (int i = 0; i < rows; i++) {
        int row = s->base + i;
        uint32_t *a = s->acc + (size_t)i * (size_t)s->ow * 4u;
        uint16_t *o = s->out + (size_t)row * (size_t)s->ow;
        int any = 0;
        for (int x = 0; x < s->ow; x++) if (a[x * 4 + 3]) { any = 1; break; }
        if (!any) {
            if (row > 0) memcpy(o, o - s->ow, (size_t)s->ow * 2u);
            continue;
        }
        for (int x = 0; x < s->ow; x++) {
            uint32_t c = a[x * 4 + 3];
            if (c) o[x] = pack565(a[x * 4] / c, a[x * 4 + 1] / c, a[x * 4 + 2] / c);
            else if (x > 0) o[x] = o[x - 1];
            else if (row > 0) o[x] = o[x - s->ow];
        }
    }
    for (int row = s->base + rows; row < upto; row++)          /* gap bigger than the band */
        if (row > 0) memcpy(s->out + (size_t)row * (size_t)s->ow, s->out + (size_t)(row - 1) * (size_t)s->ow, (size_t)s->ow * 2u);
    size_t rowu32 = (size_t)s->ow * 4u;
    if (n >= s->band_rows) {
        memset(s->acc, 0, (size_t)s->band_rows * rowu32 * 4u);
    } else {
        memmove(s->acc, s->acc + (size_t)n * rowu32, (size_t)(s->band_rows - n) * rowu32 * 4u);
        memset(s->acc + (size_t)(s->band_rows - n) * rowu32, 0, (size_t)n * rowu32 * 4u);
    }
    s->base = upto;
}

/* Called with the first source row of a band (a PNG row, or a JPEG MCU row). */
static void sc_band(struct scaler *s, int y)
{
    int dy = (int)((uint32_t)y * (uint32_t)s->oh / (uint32_t)s->sh);
    if (dy > s->base) sc_flush_rows(s, dy);
}

static void sc_add(struct scaler *s, int x, int dy, uint32_t r, uint32_t g, uint32_t b)
{
    int row = dy - s->base;
    if (row < 0 || row >= s->band_rows) return;
    uint32_t *a = s->acc + ((size_t)row * (size_t)s->ow + s->xm[x]) * 4u;
    a[0] += r;
    a[1] += g;
    a[2] += b;
    a[3]++;
}

static void sc_finish(struct scaler *s) { sc_flush_rows(s, s->base + s->band_rows); }

/* ---- PNG ---------------------------------------------------------------------- */

struct png {
    struct scaler sc;
    uint32_t w, h, rowbytes, y, fill;
    uint8_t depth, ctype, filt, have_filt, done;
    int bpp;
    uint8_t *prev, *cur;
    uint8_t pal[768];
    uint8_t trns[256];
    uint32_t bgr, bgg, bgb;
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

static void png_unfilter(struct png *p)
{
    uint8_t *cur = p->cur;
    const uint8_t *prev = p->prev;
    uint32_t n = p->rowbytes;
    uint32_t bpp = (uint32_t)p->bpp;
    switch (p->filt) {
    case 1:
        for (uint32_t i = bpp; i < n; i++) cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
        break;
    case 2:
        for (uint32_t i = 0; i < n; i++) cur[i] = (uint8_t)(cur[i] + prev[i]);
        break;
    case 3:
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = i >= bpp ? cur[i - bpp] : 0;
            cur[i] = (uint8_t)(cur[i] + ((a + prev[i]) >> 1));
        }
        break;
    case 4:
        for (uint32_t i = 0; i < n; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int c = i >= bpp ? prev[i - bpp] : 0;
            cur[i] = (uint8_t)(cur[i] + paeth(a, prev[i], c));
        }
        break;
    default:
        break;
    }
}

/* Sample x of a 1/2/4/8/16-bit single-channel row. */
static uint32_t png_sample(const uint8_t *r, uint32_t x, int depth)
{
    if (depth == 8) return r[x];
    if (depth == 16) return r[x * 2];
    uint32_t bit = x * (uint32_t)depth;
    uint32_t v = (uint32_t)(r[bit >> 3] >> (8 - depth - (int)(bit & 7))) & ((1u << depth) - 1u);
    return v;
}

static void png_emit(struct png *p)
{
    struct scaler *s = &p->sc;
    uint32_t step = (uint32_t)s->step;
    if (p->y % step) return;
    int dy = (int)(p->y * (uint32_t)s->oh / (uint32_t)s->sh);
    sc_band(s, (int)p->y);
    const uint8_t *r = p->cur;
    for (uint32_t x = 0; x < p->w; x += step) {
        uint32_t R, G, B, A = 255;
        const uint8_t *q;
        switch (p->ctype) {
        case 2:
            if (p->depth == 8) { q = r + x * 3; R = q[0]; G = q[1]; B = q[2]; }
            else               { q = r + x * 6; R = q[0]; G = q[2]; B = q[4]; }
            break;
        case 6:
            if (p->depth == 8) { q = r + x * 4; R = q[0]; G = q[1]; B = q[2]; A = q[3]; }
            else               { q = r + x * 8; R = q[0]; G = q[2]; B = q[4]; A = q[6]; }
            break;
        case 0: {
            uint32_t v = png_sample(r, x, p->depth);
            if (p->depth < 8) v = v * 255u / ((1u << p->depth) - 1u);
            R = G = B = v;
            break;
        }
        case 4:
            if (p->depth == 8) { q = r + x * 2; R = G = B = q[0]; A = q[1]; }
            else               { q = r + x * 4; R = G = B = q[0]; A = q[2]; }
            break;
        default: {
            uint32_t i = png_sample(r, x, p->depth);
            R = p->pal[i * 3]; G = p->pal[i * 3 + 1]; B = p->pal[i * 3 + 2]; A = p->trns[i];
            break;
        }
        }
        if (A != 255) {
            R = (R * A + p->bgr * (255u - A) + 127u) / 255u;
            G = (G * A + p->bgg * (255u - A) + 127u) / 255u;
            B = (B * A + p->bgb * (255u - A) + 127u) / 255u;
        }
        sc_add(s, (int)x, dy, R, G, B);
    }
}

static int png_sink(void *ctx, const uint8_t *d, uint32_t n)
{
    struct png *p = (struct png *)ctx;
    while (n) {
        if (!p->have_filt) {
            p->filt = *d++;
            n--;
            p->have_filt = 1;
            p->fill = 0;
            if (p->filt > 4) return 1;
            continue;
        }
        uint32_t take = p->rowbytes - p->fill;
        if (take > n) take = n;
        memcpy(p->cur + p->fill, d, take);
        p->fill += take;
        d += take;
        n -= take;
        if (p->fill == p->rowbytes) {
            png_unfilter(p);
            png_emit(p);
            uint8_t *t = p->prev;
            p->prev = p->cur;
            p->cur = t;
            p->y++;
            p->have_filt = 0;
            if (p->y >= p->h) { p->done = 1; return 1; }
        }
    }
    return 0;
}

static int png_decode(uint8_t *data, uint32_t len, uint16_t *out, int max_w, int max_h,
                      uint32_t bg, struct arena *ar, uint8_t *window, int *ow, int *oh)
{
    struct png p;
    memset(&p, 0, sizeof p);
    memset(p.trns, 255, sizeof p.trns);
    p.bgr = (bg >> 16) & 255u;
    p.bgg = (bg >> 8) & 255u;
    p.bgb = bg & 255u;

    uint32_t pos = 8, idat_start = 0, idat_end = 0;
    int have_ihdr = 0, have_idat = 0;
    while (pos + 12 <= len) {
        uint32_t clen = be32(data + pos);
        const uint8_t *type = data + pos + 4;
        if (clen > len - pos - 12) break;
        uint32_t body = pos + 8;
        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            p.w = be32(data + body);
            p.h = be32(data + body + 4);
            p.depth = data[body + 8];
            p.ctype = data[body + 9];
            if (data[body + 10] != 0 || data[body + 11] != 0) return RD_IMG_ERR_UNSUPPORTED;
            if (data[body + 12] != 0) return RD_IMG_ERR_UNSUPPORTED;     /* Adam7 interlacing */
            have_ihdr = 1;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            uint32_t n = clen < 768 ? clen : 768;
            memcpy(p.pal, data + body, n);
        } else if (memcmp(type, "tRNS", 4) == 0 && have_ihdr && p.ctype == 3) {
            uint32_t n = clen < 256 ? clen : 256;
            memcpy(p.trns, data + body, n);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (!have_idat) {
                idat_start = body;
                idat_end = body + clen;
                have_idat = 1;
            } else {
                memmove(data + idat_end, data + body, clen);
                idat_end += clen;
            }
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }
    if (!have_ihdr || !have_idat || idat_end - idat_start < 3) return RD_IMG_ERR_DATA;

    int channels;
    switch (p.ctype) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: return RD_IMG_ERR_UNSUPPORTED;
    }
    int ok = (p.depth == 1 || p.depth == 2 || p.depth == 4 || p.depth == 8 || p.depth == 16);
    if (p.ctype == 2 || p.ctype == 4 || p.ctype == 6) ok = ok && (p.depth == 8 || p.depth == 16);
    if (p.ctype == 3) ok = ok && p.depth <= 8;
    if (!ok) return RD_IMG_ERR_UNSUPPORTED;
    if (p.w == 0 || p.h == 0 || p.w > PNG_MAX_W || p.h > PNG_MAX_H) return RD_IMG_ERR_UNSUPPORTED;
    if ((uint64_t)p.w * p.h > MAX_PIXELS) return RD_IMG_ERR_UNSUPPORTED;

    uint32_t bits = (uint32_t)p.depth * (uint32_t)channels;
    p.rowbytes = (p.w * bits + 7u) / 8u;
    p.bpp = (int)(bits / 8u);
    if (p.bpp < 1) p.bpp = 1;
    p.prev = (uint8_t *)arena_alloc(ar, p.rowbytes);
    p.cur = (uint8_t *)arena_alloc(ar, p.rowbytes);
    if (!p.prev || !p.cur) return RD_IMG_ERR_MEM;
    memset(p.prev, 0, p.rowbytes);

    int e = sc_init(&p.sc, ar, out, (int)p.w, (int)p.h, max_w, max_h, bg, 1);
    if (e) return e;

    uint8_t cmf = data[idat_start], flg = data[idat_start + 1];
    if ((cmf & 0x0Fu) != 8 || (((uint32_t)cmf << 8) | flg) % 31u != 0 || (flg & 0x20u)) return RD_IMG_ERR_DATA;
    rd_inflate(data + idat_start + 2, idat_end - idat_start - 2, window, png_sink, &p);
    sc_finish(&p.sc);
    if (p.y == 0) return RD_IMG_ERR_DATA;
    *ow = p.sc.ow;
    *oh = p.sc.oh;
    return RD_IMG_OK;
}

/* ---- JPEG --------------------------------------------------------------------- */

struct jpg {
    struct scaler sc;
    const uint8_t *p;
    uint32_t left;
    int got;
};

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t n)
{
    struct jpg *j = (struct jpg *)jd->device;
    if (n > j->left) n = j->left;
    if (buf) memcpy(buf, j->p, n);
    j->p += n;
    j->left -= (uint32_t)n;
    return n;
}

static int jpg_out(JDEC *jd, void *bm, JRECT *rc)
{
    struct jpg *j = (struct jpg *)jd->device;
    struct scaler *s = &j->sc;
    int w = rc->right - rc->left + 1;
    int step = s->step;
    const uint8_t *src = (const uint8_t *)bm;
    sc_band(s, rc->top);
    int first = (step - rc->left % step) % step;
    for (int row = rc->top; row <= rc->bottom; row++, src += w * 3) {
        if (row % step) continue;
        int dy = (int)((uint32_t)row * (uint32_t)s->oh / (uint32_t)s->sh);
        for (int i = first; i < w; i += step) {
            const uint8_t *q = src + i * 3;                 /* this tjpgd emits B, G, R */
            sc_add(s, rc->left + i, dy, q[2], q[1], q[0]);
        }
    }
    j->got = 1;
    return 1;
}

static int jpg_decode(uint8_t *data, uint32_t len, uint16_t *out, int max_w, int max_h,
                      uint32_t bg, struct arena *ar, int *ow, int *oh)
{
    struct jpg j;
    memset(&j, 0, sizeof j);
    j.p = data;
    j.left = len;
    void *pool = arena_alloc(ar, JPG_POOL);
    if (!pool) return RD_IMG_ERR_MEM;
    JDEC jd;
    JRESULT r = jd_prepare(&jd, jpg_in, pool, JPG_POOL, &j);
    if (r == JDR_FMT3 || r == JDR_FMT2) return RD_IMG_ERR_UNSUPPORTED;     /* progressive etc. */
    if (r == JDR_MEM1 || r == JDR_MEM2) return RD_IMG_ERR_MEM;
    if (r != JDR_OK) return RD_IMG_ERR_DATA;
    if (jd.width == 0 || jd.height == 0 || jd.width > JPG_MAX_DIM || jd.height > JPG_MAX_DIM)
        return RD_IMG_ERR_UNSUPPORTED;
    if ((uint32_t)jd.width * jd.height > MAX_PIXELS) return RD_IMG_ERR_UNSUPPORTED;
    int e = sc_init(&j.sc, ar, out, jd.width, jd.height, max_w, max_h, bg, 16);
    if (e) return e;
    jd_decomp(&jd, jpg_out, 0);
    sc_finish(&j.sc);
    if (!j.got) return RD_IMG_ERR_DATA;
    *ow = j.sc.ow;
    *oh = j.sc.oh;
    return RD_IMG_OK;
}

int rd_image_decode(uint8_t *data, uint32_t len, uint16_t *out, int max_w, int max_h,
                    uint32_t bg, uint8_t *work, uint32_t work_cap, uint8_t *window,
                    int *ow, int *oh)
{
    *ow = *oh = 0;
    if (max_w < 1 || max_h < 1 || max_w > 1024 || max_h > 1024) return RD_IMG_ERR_FORMAT;
    uintptr_t a = ((uintptr_t)work + 7u) & ~(uintptr_t)7u;
    struct arena ar = { (uint8_t *)a, work_cap - (uint32_t)(a - (uintptr_t)work) };
    if (len >= 4 && data[0] == 0xFF && data[1] == 0xD8)
        return jpg_decode(data, len, out, max_w, max_h, bg, &ar, ow, oh);
    if (len >= 33 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        return png_decode(data, len, out, max_w, max_h, bg, &ar, window, ow, oh);
    return RD_IMG_ERR_FORMAT;
}

int rd_base64_decode(const uint8_t *s, uint32_t n, uint8_t *dst, uint32_t cap)
{
    uint32_t acc = 0, o = 0;
    int bits = 0;
    for (uint32_t i = 0; i < n; i++) {
        int c = s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            dst[o++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return (int)o;
}
