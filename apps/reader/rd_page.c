/* rd_page.c — see rd_page.h */
#include "rd_page.h"
#include "rd_markup.h"          /* RD_MARK_IMG / RD_MARK_LEN */

static int is_alnum_c(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

/* One line starting at `pos`. Returns where the next line begins; *le is where this
 * line's text ends. A return pointing at '\n' / a marker / len means the paragraph ended. */
static uint32_t next_line(const char *t, uint32_t len, uint32_t pos, const rd_metrics_t *m,
                          uint32_t *le)
{
    uint32_t i = pos, brk_end = 0, brk_next = 0;
    int have = 0, x = 0;
    for (;;) {
        if (i >= len || t[i] == '\n' || t[i] == RD_MARK_IMG) { *le = i; return i; }
        unsigned char c = (unsigned char)t[i];
        int w = (c >= 32 && c < 127) ? m->w[c - 32] : 0;
        if (x + w > m->line_w) {
            if (c == ' ') {                         /* the overflow is a space: break right here */
                *le = i;
                while (i < len && t[i] == ' ') i++;
                return i;
            }
            if (have) {
                *le = brk_end;
                uint32_t n = brk_next;
                while (n < len && t[n] == ' ') n++;
                return n;
            }
            if (i == pos) i++;                      /* a glyph wider than the line: take it anyway */
            *le = i;                                /* a word longer than the line: split it */
            return i;
        }
        x += w;
        if (c == ' ') {
            brk_end = i;
            brk_next = i + 1;
            have = 1;
        } else if (c == '-' && i + 1 < len && is_alnum_c((unsigned char)t[i + 1])) {
            brk_end = i + 1;                        /* may wrap after a hyphen or dash */
            brk_next = i + 1;
            have = 1;
        }
        i++;
    }
}

int rd_page_image(const char *text, uint32_t len, uint32_t start)
{
    if (start + RD_MARK_LEN > len || text[start] != RD_MARK_IMG) return -1;
    int idx = (text[start + 1] - '0') * 64 + (text[start + 2] - '0');
    return idx < 0 ? -1 : idx;
}

uint32_t rd_page_layout(const char *text, uint32_t len, uint32_t start,
                        const rd_metrics_t *m, char *out, uint32_t cap)
{
    uint32_t pos = start, o = 0;
    int lines = 0;
    if (out && cap) out[0] = 0;
    if (pos >= len) return len;

    if (text[pos] == RD_MARK_IMG) {                 /* an image is a page of its own */
        pos += RD_MARK_LEN;
        if (pos < len && text[pos] == '\n') pos++;
        return pos > len ? len : pos;
    }
    while (pos < len && lines < m->max_lines) {
        if (text[pos] == RD_MARK_IMG) break;        /* the page ends before an image */
        if (text[pos] == '\n') { pos++; continue; }
        uint32_t le;
        uint32_t np = next_line(text, len, pos, m, &le);
        if (out) {
            if (lines > 0 && o + 1 < cap) out[o++] = '\n';
            for (uint32_t k = pos; k < le && o + 1 < cap; k++) out[o++] = text[k];
            out[o] = 0;
        }
        lines++;
        pos = np;
        if (pos < len && text[pos] == '\n') pos++;
    }
    return pos;
}

int rd_paginate(const char *text, uint32_t len, const rd_metrics_t *m,
                uint32_t *starts, int cap)
{
    int n = 0;
    uint32_t pos = 0;
    while (n < cap) {
        while (pos < len && (text[pos] == '\n' || text[pos] == ' ')) pos++;
        if (pos >= len) break;
        starts[n++] = pos;
        uint32_t np = rd_page_layout(text, len, pos, m, 0, 0);
        pos = np > pos ? np : pos + 1;
    }
    return n;
}
