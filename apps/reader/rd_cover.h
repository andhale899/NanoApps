/*
 * rd_cover — library thumbnails.
 *
 * A cover is cached on the iPod as one small file per book (/Data/Books/.cov.<name>):
 *
 *     'C' 'V' w h  size(le32)  w*h RGB565 pixels
 *
 * `size` is the book file's size when the thumbnail was made, so a replaced file gets a
 * fresh cover. w = h = 0 is a deliberate "this book has no usable cover" marker, so books
 * without one are not retried every time the library opens.
 */
#ifndef RD_COVER_INC_H
#define RD_COVER_INC_H

#include <stdint.h>
#include "rd_epub.h"

#define RD_COVER_W    108
#define RD_COVER_H    142
#define RD_COVER_HDR  8
#define RD_COVER_BUF  (RD_COVER_HDR + RD_COVER_W * RD_COVER_H * 2)

/* Decode an image file into a record at `rec` (RD_COVER_BUF bytes, 2-byte aligned).
 * `img` is clobbered (PNG). Returns the record length; a failed decode yields the 8-byte
 * "no cover" record. */
int rd_cover_from_image(uint8_t *img, uint32_t len, uint32_t size_tag, uint32_t bg,
                        uint8_t *rec, uint8_t *work, uint32_t work_cap, uint8_t *window);

/* Find the cover inside an EPUB held in memory and build its record. Returns the record
 * length (8 bytes = no cover found), or -1 if memory was short and the caller should retry
 * later. `alloc`/`dealloc` supply the temporary buffer for the extracted image. */
int rd_cover_from_epub(const uint8_t *epub, uint32_t epub_size, uint32_t size_tag, uint32_t bg,
                       uint8_t *rec, const rd_epub_mem_t *mem, uint8_t *work, uint32_t work_cap,
                       void *(*alloc)(uint32_t), void (*dealloc)(void *));

/* The 8-byte "this book has no usable cover" record. */
int rd_cover_none(uint8_t *rec, uint32_t size_tag);

/* Is `rec` (n bytes) a usable cache record for a book of `size_tag` bytes?
 *   1  a thumbnail  (*w, *h set)      0  a "no cover" marker      -1  missing, damaged or stale */
int rd_cover_check(const uint8_t *rec, uint32_t n, uint32_t size_tag, int *w, int *h);

/* Same, but looking only at the 8-byte header: is there a record (cover or marker) that is
 * current for this book? 1 yes, 0 no. */
int rd_cover_peek(const uint8_t *hdr, uint32_t n, uint32_t size_tag);

#endif
