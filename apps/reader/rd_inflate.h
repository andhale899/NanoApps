/*
 * rd_inflate — small streaming DEFLATE (RFC 1951) decoder.
 *
 * Output is pushed to a sink callback in chunks, so a chapter or an image can be
 * decoded without holding the whole uncompressed result. The caller supplies the
 * 32 KB history window. Compact canonical-Huffman decoder after Mark Adler's puff.
 */
#ifndef RD_INFLATE_H
#define RD_INFLATE_H

#include <stdint.h>

#define RD_WINDOW_SIZE 32768u

/* Return 0 to keep going, non-zero to abort decoding. */
typedef int (*rd_sink_fn)(void *ctx, const uint8_t *data, uint32_t len);

#define RD_OK            0
#define RD_ABORTED       1     /* the sink asked to stop */
#define RD_ERR_DATA     (-1)   /* corrupt stream */
#define RD_ERR_TRUNC    (-2)   /* input ended early */
#define RD_ERR_UNSUPP   (-3)

/* Decode a raw DEFLATE stream. `window` must point at RD_WINDOW_SIZE bytes. */
int rd_inflate(const uint8_t *src, uint32_t src_len, uint8_t *window,
               rd_sink_fn sink, void *ctx);

#endif
