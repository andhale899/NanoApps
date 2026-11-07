/*
 * rd_zip — read-only access to a ZIP archive that is already in memory.
 *
 * Entries are looked up by walking the central directory (no allocation).
 * Content streams through rd_sink_fn, stored or deflated. Every offset is bounds
 * checked: the archive is untrusted input.
 */
#ifndef RD_ZIP_H
#define RD_ZIP_H

#include <stdint.h>
#include "rd_inflate.h"

typedef struct {
    const uint8_t *d;
    uint32_t size;
    uint32_t cd;        /* central directory offset */
    uint32_t cd_size;
    uint32_t n;         /* entry count */
} rd_zip_t;

typedef struct {
    uint32_t lho;       /* local header offset */
    uint32_t csize, usize, crc;
    uint16_t method, flags;
    const uint8_t *name;
    uint16_t nlen;
} rd_zip_ent_t;

#define RD_ZIP_ERR_FORMAT  (-10)
#define RD_ZIP_ERR_NOENT   (-11)
#define RD_ZIP_ERR_CRYPT   (-12)
#define RD_ZIP_ERR_METHOD  (-13)
#define RD_ZIP_ERR_SIZE    (-14)

int rd_zip_open(rd_zip_t *z, const uint8_t *data, uint32_t size);

/* Walk entries: start with *pos = 0 (set by caller), call until it returns non-zero. */
int rd_zip_next(const rd_zip_t *z, uint32_t *pos, uint32_t *idx, rd_zip_ent_t *e);

/* Find by path (exact match first, then case-insensitive). */
int rd_zip_find(const rd_zip_t *z, const char *name, uint32_t nlen, rd_zip_ent_t *e);

/* Stream the uncompressed bytes of an entry to `sink`. Returns RD_* / RD_ZIP_ERR_*. */
int rd_zip_read(const rd_zip_t *z, const rd_zip_ent_t *e, uint8_t *window,
                rd_sink_fn sink, void *ctx);

#endif
