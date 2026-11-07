/*
 * rd_image — decode a book illustration (JPEG or PNG) to RGB565, scaled to fit the page.
 *
 * Streaming and small: PNG rows are inflated and unfiltered one at a time, JPEG goes
 * through tjpgd one MCU at a time, and both feed a box-filter downscaler that only ever
 * holds a few output rows. Nothing proportional to the source image size is allocated,
 * so a 3000x2000 photo costs the same memory as a thumbnail.
 *
 * Not supported (rd_image_decode returns RD_IMG_ERR_UNSUPPORTED): progressive JPEG,
 * interlaced PNG, GIF, WebP, SVG. The caller should show a placeholder.
 */
#ifndef RD_IMAGE_H
#define RD_IMAGE_H

#include <stdint.h>

#define RD_IMG_OK               0
#define RD_IMG_ERR_FORMAT     (-30)
#define RD_IMG_ERR_UNSUPPORTED (-31)
#define RD_IMG_ERR_MEM        (-32)
#define RD_IMG_ERR_DATA       (-33)

/* Work memory needed by rd_image_decode (excluding the inflate window). */
#define RD_IMG_WORK_MIN       (192u * 1024u)

/*
 * data / len  the whole image file. A PNG's IDAT chunks are compacted in place, so the
 *             buffer is clobbered and must be writable.
 * out         max_w * max_h RGB565 pixels, row-major, stride = *ow.
 * bg          0xRRGGBB, transparent pixels are blended onto it.
 * work        scratch, 8-byte aligned, RD_IMG_WORK_MIN bytes or more.
 * window      RD_WINDOW_SIZE bytes (PNG inflate history).
 * The image is scaled down to fit max_w x max_h, never enlarged. A damaged file that
 * yields at least one row returns RD_IMG_OK with the rest left as background.
 */
int rd_image_decode(uint8_t *data, uint32_t len, uint16_t *out, int max_w, int max_h,
                    uint32_t bg, uint8_t *work, uint32_t work_cap, uint8_t *window,
                    int *ow, int *oh);

/* Base64 (whitespace tolerant, '=' padding optional). Returns bytes written, or -1. */
int rd_base64_decode(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap);

#endif
