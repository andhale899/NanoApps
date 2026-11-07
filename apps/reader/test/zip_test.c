/* Host test: every entry of an archive must inflate to its recorded CRC-32 and size.
 *   cc -g -fsanitize=address,undefined -I.. zip_test.c ../rd_zip.c ../rd_inflate.c -o zip_test
 *   ./zip_test book.epub ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd_zip.h"

static uint32_t crc_tab[256];
static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
struct acc { uint32_t crc, n; };
static int sink(void *ctx, const uint8_t *d, uint32_t len)
{
    struct acc *a = ctx;
    for (uint32_t i = 0; i < len; i++) a->crc = crc_tab[(a->crc ^ d[i]) & 0xFF] ^ (a->crc >> 8);
    a->n += len;
    return 0;
}

int main(int argc, char **argv)
{
    crc_init();
    static uint8_t window[RD_WINDOW_SIZE];
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); return 2; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)sz);
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);

        rd_zip_t z;
        int r = rd_zip_open(&z, buf, (uint32_t)sz);
        if (r) { printf("%s: open failed %d\n", argv[a], r); bad++; continue; }
        uint32_t pos = 0, idx = 0, ok = 0, total = 0;
        rd_zip_ent_t e;
        while (rd_zip_next(&z, &pos, &idx, &e) == 0) {
            if (e.nlen && e.name[e.nlen - 1] == '/') continue;   /* directory */
            struct acc ac = { 0xFFFFFFFFu, 0 };
            r = rd_zip_read(&z, &e, window, sink, &ac);
            ac.crc ^= 0xFFFFFFFFu;
            if (r || ac.n != e.usize || ac.crc != e.crc) {
                printf("  BAD %.*s r=%d n=%u/%u crc=%08x/%08x\n", e.nlen, e.name, r,
                       ac.n, e.usize, ac.crc, e.crc);
                bad++;
            } else { ok++; total += ac.n; }
        }
        printf("%s: %u entries OK, %u bytes inflated, size on disk %ld\n", argv[a], ok, total, sz);
        rd_zip_ent_t m;
        if (rd_zip_find(&z, "mimetype", 8, &m) == 0) printf("  mimetype method=%u usize=%u\n", m.method, m.usize);
        free(buf);
    }
    printf(bad ? "FAILED: %d problems\n" : "all good\n", bad);
    return bad != 0;
}
