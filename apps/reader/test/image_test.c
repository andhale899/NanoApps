/* Host test: decode an image, report size/time, write a PPM you can look at.
 *   cc -g -fsanitize=address,undefined -I.. image_test.c ../rd_image.c ../rd_inflate.c \
 *      ../tjpgd/tjpgd.c -o image_test
 *   ./image_test in.jpg out.ppm [max_w max_h [truncate_percent]]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rd_image.h"
#include "rd_inflate.h"

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    int max_w = argc > 4 ? atoi(argv[3]) : 220, max_h = argc > 4 ? atoi(argv[4]) : 366;
    int trunc = argc > 5 ? atoi(argv[5]) : 100;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)sz);
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    uint32_t len = (uint32_t)((uint64_t)sz * (uint32_t)trunc / 100u);

    uint16_t *out = calloc((size_t)max_w * max_h, 2);
    uint8_t *work = aligned_alloc(16, RD_IMG_WORK_MIN);
    uint8_t *win = malloc(RD_WINDOW_SIZE);
    int ow, oh;
    clock_t t0 = clock();
    int r = rd_image_decode(data, len, out, max_w, max_h, 0xFFFFFF, work, RD_IMG_WORK_MIN, win, &ow, &oh);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("%s: result=%d  out=%dx%d  %.1f ms  (%ld bytes in, %d%% used)\n", argv[1], r, ow, oh, ms, sz, trunc);
    if (r == 0) {
        FILE *o = fopen(argv[2], "wb");
        fprintf(o, "P6\n%d %d\n255\n", ow, oh);
        for (int i = 0; i < ow * oh; i++) {
            uint16_t v = out[i];
            uint8_t px[3] = { (uint8_t)(((v >> 11) & 31) * 255 / 31), (uint8_t)(((v >> 5) & 63) * 255 / 63), (uint8_t)((v & 31) * 255 / 31) };
            fwrite(px, 1, 3, o);
        }
        fclose(o);
    }
    return r != 0;
}
