/* Fuzz rd_image_decode with mutated and truncated PNG/JPEG files (run under the sanitizers).
 *   cc -g -fsanitize=address,undefined -I.. image_fuzz.c ../rd_image.c ../rd_inflate.c \
 *      ../tjpgd/tjpgd.c -o image_fuzz && ./image_fuzz seed1 seed2 ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd_image.h"
#include "rd_inflate.h"

static uint32_t rng = 987654321u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

int main(int argc, char **argv)
{
    uint16_t *out = malloc(220 * 366 * 2);
    uint8_t *work = aligned_alloc(16, RD_IMG_WORK_MIN);
    uint8_t *win = malloc(RD_WINDOW_SIZE);
    long total = 0, ok = 0, partial_ok = 0;
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) { perror(argv[a]); return 2; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *seed = malloc((size_t)sz), *buf = malloc((size_t)sz);
        if (fread(seed, 1, (size_t)sz, f) != (size_t)sz) return 2;
        fclose(f);
        for (int it = 0; it < 3000; it++) {
            memcpy(buf, seed, (size_t)sz);
            int flips = 1 + (int)(rnd() % 8);
            for (int k = 0; k < flips; k++) {
                long at = (rnd() & 1) ? (long)(rnd() % (sz < 120 ? sz : 120)) : (long)(rnd() % (uint32_t)sz);
                buf[at] = (uint8_t)rnd();
            }
            uint32_t len = (rnd() % 5 == 0) ? 1 + rnd() % (uint32_t)sz : (uint32_t)sz;
            int ow, oh;
            int r = rd_image_decode(buf, len, out, 220, 366, 0xFFFFFF, work, RD_IMG_WORK_MIN, win, &ow, &oh);
            total++;
            if (r == 0) {
                ok++;
                if (ow < 1 || oh < 1 || ow > 220 || oh > 366) { printf("BAD DIMS %dx%d\n", ow, oh); return 1; }
                if (len < (uint32_t)sz) partial_ok++;
            }
        }
        free(seed); free(buf);
    }
    printf("image fuzz: %ld runs, %ld decoded (%ld from truncated data), no crashes\n", total, ok, partial_ok);
    return 0;
}
