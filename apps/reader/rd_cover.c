/* rd_cover.c — see rd_cover.h */
#include <string.h>
#include "rd_cover.h"
#include "rd_image.h"

#define COVER_MAX_IMAGE (8u * 1024u * 1024u)

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int rd_cover_none(uint8_t *rec, uint32_t size_tag)
{
    rec[0] = 'C';
    rec[1] = 'V';
    rec[2] = 0;
    rec[3] = 0;
    put32(rec + 4, size_tag);
    return RD_COVER_HDR;
}

int rd_cover_from_image(uint8_t *img, uint32_t len, uint32_t size_tag, uint32_t bg,
                        uint8_t *rec, uint8_t *work, uint32_t work_cap, uint8_t *window)
{
    int w = 0, h = 0;
    int rc = rd_image_decode(img, len, (uint16_t *)(void *)(rec + RD_COVER_HDR), RD_COVER_W,
                             RD_COVER_H, bg, work, work_cap, window, &w, &h);
    if (rc != 0 || w < 1 || h < 1 || w > RD_COVER_W || h > RD_COVER_H) return rd_cover_none(rec, size_tag);
    rec[0] = 'C';
    rec[1] = 'V';
    rec[2] = (uint8_t)w;
    rec[3] = (uint8_t)h;
    put32(rec + 4, size_tag);
    return RD_COVER_HDR + w * h * 2;
}

struct membuf { uint8_t *p; uint32_t cap, len; int over; };

static int mem_sink(void *c, const uint8_t *d, uint32_t n)
{
    struct membuf *m = (struct membuf *)c;
    if (n > m->cap - m->len) { m->over = 1; return 1; }
    memcpy(m->p + m->len, d, n);
    m->len += n;
    return 0;
}

int rd_cover_from_epub(const uint8_t *epub, uint32_t epub_size, uint32_t size_tag, uint32_t bg,
                       uint8_t *rec, const rd_epub_mem_t *mem, uint8_t *work, uint32_t work_cap,
                       void *(*alloc)(uint32_t), void (*dealloc)(void *))
{
    rd_zip_t z;
    if (rd_zip_open(&z, epub, epub_size)) return rd_cover_none(rec, size_tag);
    char path[200];
    int pl = rd_epub_cover(&z, mem, path, (int)sizeof path);
    if (pl <= 0) return rd_cover_none(rec, size_tag);
    rd_zip_ent_t e;
    if (rd_zip_find(&z, path, (uint32_t)pl, &e) || e.usize == 0 || e.usize > COVER_MAX_IMAGE)
        return rd_cover_none(rec, size_tag);
    uint8_t *tmp = (uint8_t *)alloc(e.usize);
    if (!tmp) return -1;
    struct membuf m = { tmp, e.usize, 0, 0 };
    int r = rd_zip_read(&z, &e, mem->window, mem_sink, &m);
    if (r || m.over || m.len == 0) {
        dealloc(tmp);
        return rd_cover_none(rec, size_tag);
    }
    int n = rd_cover_from_image(tmp, m.len, size_tag, bg, rec, work, work_cap, mem->window);
    dealloc(tmp);
    return n;
}

int rd_cover_peek(const uint8_t *hdr, uint32_t n, uint32_t size_tag)
{
    return n >= RD_COVER_HDR && hdr[0] == 'C' && hdr[1] == 'V' && get32(hdr + 4) == size_tag;
}

int rd_cover_check(const uint8_t *rec, uint32_t n, uint32_t size_tag, int *w, int *h)
{
    if (!rd_cover_peek(rec, n, size_tag)) return -1;
    int cw = rec[2], ch = rec[3];
    if (cw == 0 && ch == 0) return 0;
    if (cw < 1 || ch < 1 || cw > RD_COVER_W || ch > RD_COVER_H) return -1;
    if (n != (uint32_t)(RD_COVER_HDR + cw * ch * 2)) return -1;
    *w = cw;
    *h = ch;
    return 1;
}
