/* Host test for the cover pipeline: find the cover in each EPUB, build the cache record,
 * validate it, and write a PPM contact sheet.
 *   ./cover_test sheet.ppm a.epub b.epub ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd_cover.h"
#include "rd_inflate.h"
#include "rd_image.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void *xalloc(uint32_t n) { return malloc(n); }
static void xfree(void *p) { free(p); }

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    int n_books = argc - 2;
    uint8_t *window = malloc(RD_WINDOW_SIZE);
    uint8_t *scratch = aligned_alloc(16, 256 * 1024);
    uint8_t *rec = aligned_alloc(16, (RD_COVER_BUF + 31u) & ~15u);
    int sheet_w = n_books * (RD_COVER_W + 8) + 8, sheet_h = RD_COVER_H + 16;
    uint8_t *sheet = calloc((size_t)sheet_w * sheet_h, 3);
    for (size_t i = 0; i < (size_t)sheet_w * sheet_h; i++) sheet[i * 3] = 0x30;

    for (int b = 0; b < n_books; b++) {
        FILE *f = fopen(argv[2 + b], "rb");
        if (!f) { perror(argv[2 + b]); return 2; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *epub = malloc((size_t)sz);
        if (fread(epub, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);

        rd_epub_mem_t mem = { window, scratch, 256 * 1024 };
        int n = rd_cover_from_epub(epub, (uint32_t)sz, (uint32_t)sz, 0x2f6f9f, rec, &mem, scratch, 256 * 1024, xalloc, xfree);
        int w = 0, h = 0;
        int v = n > 0 ? rd_cover_check(rec, (uint32_t)n, (uint32_t)sz, &w, &h) : -2;
        printf("%-46s record=%d bytes  check=%d  thumb=%dx%d\n", strrchr(argv[2 + b], '/') ? strrchr(argv[2 + b], '/') + 1 : argv[2 + b], n, v, w, h);
        CHECK(n >= RD_COVER_HDR, "no record");
        CHECK(rd_cover_peek(rec, (uint32_t)n, (uint32_t)sz) == 1, "peek should accept the record");
        CHECK(rd_cover_check(rec, (uint32_t)n, (uint32_t)sz + 1, &w, &h) == -1, "a different file size must make it stale");
        if (n > RD_COVER_HDR) CHECK(rd_cover_check(rec, (uint32_t)n - 2, (uint32_t)sz, &w, &h) == -1, "truncated record must be rejected");
        if (v == 1) {
            const uint16_t *px = (const uint16_t *)(const void *)(rec + RD_COVER_HDR);
            int ox = 8 + b * (RD_COVER_W + 8), oy = 8;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                uint16_t c = px[y * w + x];
                uint8_t *d = sheet + ((size_t)(oy + y) * sheet_w + ox + x) * 3;
                d[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); d[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); d[2] = (uint8_t)((c & 31) * 255 / 31);
            }
        }
        free(epub);
    }
    /* garbage and hostile input must produce a clean "no cover" record, never a crash */
    uint8_t junk[2048];
    for (int i = 0; i < (int)sizeof junk; i++) junk[i] = (uint8_t)(i * 31 + 7);
    rd_epub_mem_t mem = { window, scratch, 256 * 1024 };
    int n = rd_cover_from_epub(junk, sizeof junk, 2048, 0, rec, &mem, scratch, 256 * 1024, xalloc, xfree);
    CHECK(n == RD_COVER_HDR && rec[2] == 0 && rec[3] == 0, "garbage input should give the no-cover record, got %d", n);
    int w, h;
    CHECK(rd_cover_check(rec, (uint32_t)n, 2048, &w, &h) == 0, "no-cover marker should check as 0");
    CHECK(rd_cover_check(junk, 3, 2048, &w, &h) == -1, "short record");
    FILE *o = fopen(argv[1], "wb");
    fprintf(o, "P6\n%d %d\n255\n", sheet_w, sheet_h);
    fwrite(sheet, 3, (size_t)sheet_w * sheet_h, o);
    fclose(o);
    printf(fails ? "FAILED: %d\n" : "cover test passed\n", fails);
    return fails != 0;
}
