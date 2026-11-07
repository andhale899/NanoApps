/* rd_zip.c — see rd_zip.h */
#include "rd_zip.h"

static uint32_t u16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rd_zip_open(rd_zip_t *z, const uint8_t *d, uint32_t size)
{
    if (size < 22) return RD_ZIP_ERR_FORMAT;
    z->d = d;
    z->size = size;
    /* End of central directory record: scan back through the optional comment. */
    uint32_t lo = size > 22u + 65535u ? size - 22u - 65535u : 0;
    uint32_t p = size - 22;
    for (;;) {
        if (u32(d + p) == 0x06054b50u) break;
        if (p == lo) return RD_ZIP_ERR_FORMAT;
        p--;
    }
    z->n = u16(d + p + 10);
    z->cd_size = u32(d + p + 12);
    z->cd = u32(d + p + 16);
    if (z->n == 0xFFFFu || z->cd == 0xFFFFFFFFu) return RD_ZIP_ERR_FORMAT;   /* zip64 */
    if (z->cd > size || z->cd_size > size - z->cd) return RD_ZIP_ERR_FORMAT;
    return 0;
}

int rd_zip_next(const rd_zip_t *z, uint32_t *pos, uint32_t *idx, rd_zip_ent_t *e)
{
    if (*idx >= z->n) return RD_ZIP_ERR_NOENT;
    uint32_t p = z->cd + *pos;
    if (*pos + 46 > z->cd_size) return RD_ZIP_ERR_FORMAT;
    const uint8_t *h = z->d + p;
    if (u32(h) != 0x02014b50u) return RD_ZIP_ERR_FORMAT;
    uint32_t nlen = u16(h + 28), elen = u16(h + 30), clen = u16(h + 32);
    if (*pos + 46u + nlen + elen + clen > z->cd_size) return RD_ZIP_ERR_FORMAT;
    e->flags = (uint16_t)u16(h + 8);
    e->method = (uint16_t)u16(h + 10);
    e->crc = u32(h + 16);
    e->csize = u32(h + 20);
    e->usize = u32(h + 24);
    e->nlen = (uint16_t)nlen;
    e->lho = u32(h + 42);
    e->name = h + 46;
    *pos += 46u + nlen + elen + clen;
    (*idx)++;
    return 0;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int name_eq(const uint8_t *a, uint32_t alen, const char *b, uint32_t blen, int fold)
{
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        int x = a[i], y = (uint8_t)b[i];
        if (fold) { x = lower(x); y = lower(y); }
        if (x != y) return 0;
    }
    return 1;
}

int rd_zip_find(const rd_zip_t *z, const char *name, uint32_t nlen, rd_zip_ent_t *e)
{
    for (int fold = 0; fold < 2; fold++) {
        uint32_t pos = 0, idx = 0;
        while (rd_zip_next(z, &pos, &idx, e) == 0)
            if (name_eq(e->name, e->nlen, name, nlen, fold)) return 0;
    }
    return RD_ZIP_ERR_NOENT;
}

struct guard {
    rd_sink_fn sink;
    void *ctx;
    uint32_t left;      /* bytes the entry is declared to hold */
    int over;
};

static int guard_sink(void *c, const uint8_t *data, uint32_t len)
{
    struct guard *g = (struct guard *)c;
    if (len > g->left) { g->over = 1; return 1; }   /* header lied: zip bomb or corruption */
    g->left -= len;
    return g->sink(g->ctx, data, len);
}

int rd_zip_read(const rd_zip_t *z, const rd_zip_ent_t *e, uint8_t *window,
                rd_sink_fn sink, void *ctx)
{
    if (e->flags & 1u) return RD_ZIP_ERR_CRYPT;
    if (e->method != 0 && e->method != 8) return RD_ZIP_ERR_METHOD;
    if (e->lho > z->size || z->size - e->lho < 30u) return RD_ZIP_ERR_FORMAT;
    const uint8_t *h = z->d + e->lho;
    if (u32(h) != 0x04034b50u) return RD_ZIP_ERR_FORMAT;
    uint32_t start = e->lho + 30u + u16(h + 26) + u16(h + 28);
    if (start > z->size || e->csize > z->size - start) return RD_ZIP_ERR_FORMAT;
    const uint8_t *data = z->d + start;

    struct guard g = { sink, ctx, e->usize, 0 };
    if (e->method == 0) {
        if (e->csize != e->usize) return RD_ZIP_ERR_SIZE;
        uint32_t off = 0;
        while (off < e->csize) {
            uint32_t n = e->csize - off;
            if (n > 8192u) n = 8192u;
            if (sink(ctx, data + off, n)) return RD_ABORTED;
            off += n;
        }
        return RD_OK;
    }
    int r = rd_inflate(data, e->csize, window, guard_sink, &g);
    if (g.over) return RD_ZIP_ERR_SIZE;
    return r;
}
