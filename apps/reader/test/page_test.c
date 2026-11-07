/* Host test for rd_page: no text lost or repeated, no line too wide, images alone on a
 * page, and how long a whole book takes to paginate.
 *   cc -g -O2 -fsanitize=address,undefined -I.. page_test.c ../rd_page.c -o page_test
 *   ./page_test book.txt [font_size_class]      (book.txt = output of epub_test / rd_markup)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rd_page.h"
#include "rd_markup.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; if (fails < 12) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void make_metrics(rd_metrics_t *m, int scale)
{
    /* rough Montserrat-like widths, scaled by font class (0:14pt 1:16pt 2:20pt) */
    for (int c = 32; c < 127; c++) {
        int w = 8;
        if (c == ' ') w = 4;
        else if (c == 'i' || c == 'l' || c == 'j' || c == '.' || c == ',' || c == '\'' || c == ':' || c == ';' || c == '!' || c == '|') w = 4;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') w = 13;
        else if (c >= 'A' && c <= 'Z') w = 10;
        m->w[c - 32] = (uint8_t)(w + scale * w / 6);
    }
    m->line_w = 218;
    m->max_lines = 366 / (17 + scale * 3);
}

static int line_px(const rd_metrics_t *m, const char *s, int n)
{
    int x = 0;
    for (int i = 0; i < n; i++) x += m->w[(unsigned char)s[i] - 32];
    return x;
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (fread(text, 1, (size_t)n, f) != (size_t)n) return 2;
    text[n] = 0;
    fclose(f);
    uint32_t len = (uint32_t)n;

    for (int scale = 0; scale < 3; scale++) {
        rd_metrics_t m;
        make_metrics(&m, scale);
        uint32_t *starts = malloc(sizeof(uint32_t) * (len / 48 + 64));
        clock_t t0 = clock();
        int np = rd_paginate(text, len, &m, starts, (int)(len / 48 + 64));
        double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        printf("font class %d: %d pages for %u chars (%d lines/page) in %.2f ms\n", scale, np, len, m.max_lines, ms);

        char page[8192];
        uint64_t words_seen = 0, images = 0;
        const char *rd = text;          /* walk the source in step with the pages */
        for (int p = 0; p < np; p++) {
            CHECK(p == 0 || starts[p] > starts[p - 1], "starts not increasing at %d", p);
            uint32_t nx = rd_page_layout(text, len, starts[p], &m, page, sizeof page);
            int img = rd_page_image(text, len, starts[p]);
            if (img >= 0) {
                images++;
                CHECK(page[0] == 0, "image page has text");
                CHECK(nx - starts[p] >= RD_MARK_LEN, "image page too short");
                while (*rd == ' ' || *rd == '\n') rd++;
                CHECK(*rd == 1, "source out of step at image page %d", p);
                if (*rd == 1) rd += RD_MARK_LEN;
                continue;
            }
            CHECK(!strchr(page, 1), "marker inside text page %d", p);
            int lines = 1, ls = 0;
            for (int i = 0;; i++) {
                if (page[i] == '\n' || page[i] == 0) {
                    int px = line_px(&m, page + ls, i - ls);
                    int over_ok = (i - ls) <= 1;    /* one glyph can be wider than the line */
                    CHECK(px <= m.line_w || over_ok, "page %d line too wide: %d px: \"%.*s\"", p, px, i - ls, page + ls);
                    if (page[i] == 0) break;
                    lines++;
                    ls = i + 1;
                }
            }
            CHECK(lines <= m.max_lines, "page %d has %d lines (max %d)", p, lines, m.max_lines);
            /* every word on the page must be the next word in the source */
            const char *q = page;
            while (*q) {
                while (*q == ' ' || *q == '\n') q++;
                if (!*q) break;
                while (*rd == ' ' || *rd == '\n') rd++;
                const char *qs = q;
                while (*q && *q != ' ' && *q != '\n') q++;
                int wl = (int)(q - qs);
                /* a split long word / hyphen break shows up as consecutive pieces */
                CHECK(strncmp(rd, qs, (size_t)wl) == 0, "page %d: text diverges at \"%.*s\" vs source \"%.12s\"", p, wl, qs, rd);
                rd += wl;
                words_seen++;
                if (*rd == 1) rd += RD_MARK_LEN;
            }
        }
        while (*rd == ' ' || *rd == '\n') rd++;
        while (*rd == 1) { rd += RD_MARK_LEN; while (*rd == ' ' || *rd == '\n') rd++; }
        CHECK(*rd == 0, "source not fully consumed, left: \"%.30s\"", rd);
        printf("  words seen %llu, image pages %llu\n", (unsigned long long)words_seen, (unsigned long long)images);
        free(starts);
    }
    printf(fails ? "FAILED: %d problems\n" : "page test passed\n", fails);
    return fails != 0;
}
