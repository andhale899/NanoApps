/*
 * rd_epub — read an EPUB that is already in memory.
 *
 * META-INF/container.xml -> OPF -> manifest + spine, then every spine document is
 * streamed straight from the zip through rd_markup into the text blob. Chapters come
 * from the headings the parser sees (or one per spine file when a book has none).
 * Images stay in the archive; the text carries markers and rd_zip_find() fetches the
 * bytes later, when a page needs them.
 */
#ifndef RD_EPUB_H
#define RD_EPUB_H

#include "rd_markup.h"
#include "rd_zip.h"

#define RD_EPUB_ERR_ZIP    (-20)   /* not a readable zip */
#define RD_EPUB_ERR_DRM    (-21)   /* encrypted content */
#define RD_EPUB_ERR_EMPTY  (-22)   /* nothing readable inside */

typedef struct {
    uint8_t *window;        /* RD_WINDOW_SIZE bytes of inflate history */
    uint8_t *scratch;       /* OPF + manifest tables; 16-byte aligned */
    uint32_t scratch_cap;   /* at least 64 KB, 256 KB is comfortable */
} rd_epub_mem_t;

/* Append the book's text to `doc`. On success *zip is left open on `file`. */
int rd_epub_load(rd_zip_t *zip, rd_doc_t *doc, const uint8_t *file, uint32_t size,
                 const rd_epub_mem_t *mem);

/* Archive path of the book's cover image (see rd_epub.c for the lookup order), or 0 if none.
 * Uses mem->scratch and mem->window; `zip` must already be open. */
int rd_epub_cover(const rd_zip_t *zip, const rd_epub_mem_t *mem, char *out, int cap);

#endif
