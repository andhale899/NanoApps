/*
 * rd_markup — turns HTML/XHTML, FB2 and Markdown into the Reader's text blob.
 *
 * Output convention (shared with the on-device pager):
 *   - plain ASCII; paragraphs separated by a single '\n'; words by single spaces
 *   - headings are upper-cased and recorded as chapters (h1/h2, FB2 <title>)
 *   - an image is a 3-byte marker  0x01, '0'+(idx>>6), '0'+(idx&63)  on its own
 *     line, where idx indexes doc->img_off[] (resolved archive path, or "#id" for FB2)
 *
 * The parser is a byte-at-a-time state machine, so it can be fed straight from the
 * inflate sink with no temporary copy of the chapter.
 */
#ifndef RD_MARKUP_H
#define RD_MARKUP_H

#include <stdint.h>

#define RD_MARK_IMG       0x01
#define RD_MARK_LEN       3
#define RD_MAX_IMAGES     512
#define RD_MAX_CHAPTERS   256
#define RD_TITLE_LEN      28
#define RD_IPOOL          16384
#define RD_MAX_BINS       64

typedef struct {
    uint32_t off;                   /* text offset where the chapter starts */
    char title[RD_TITLE_LEN];
} rd_chapter_t;

typedef struct {
    uint32_t id_off;                /* id string in ipool */
    uint32_t data_off, data_len;    /* base64 payload inside the fed FB2 stream */
} rd_bin_t;

typedef struct {
    char *buf;                      /* output text (caller owns, NUL-terminated by rd_doc_finish) */
    uint32_t cap, len;
    int overflow;
    rd_chapter_t chap[RD_MAX_CHAPTERS];
    int nchap;
    char ipool[RD_IPOOL];           /* NUL-separated image references */
    uint32_t ipool_len;
    uint32_t img_off[RD_MAX_IMAGES];
    int nimg;
    rd_bin_t bin[RD_MAX_BINS];      /* FB2 <binary> blocks */
    int nbin;
    char title[64];
    char author[48];
    /* In-place conversion: the input sits `slack` bytes after the start of the output buffer,
     * so output may only extend as far as the input already consumed (cap is raised as the
     * converters advance). 0 = a separate output buffer. */
    uint32_t slack;
    uint8_t no_images;              /* leave <img> out (a loose HTML/Markdown file has no archive to read) */
} rd_doc_t;

typedef enum { RD_FMT_HTML = 0, RD_FMT_FB2 = 1 } rd_fmt_t;

#define RD_TAGMAX 400

typedef struct {
    rd_doc_t *doc;
    rd_fmt_t fmt;
    char base[160];                 /* directory of the current document inside the archive */
    uint8_t st;
    uint8_t quote, enc, unneed, cmt;
    uint32_t ucp;
    uint16_t taglen;
    char tag[RD_TAGMAX + 1];
    char ent[12];
    uint8_t entlen;
    /* output state */
    char last;
    uint8_t pend_space, skip, in_desc, heading, ch_pending;
    char skip_name[16];
    /* text capture (chapter title, book title, author) */
    char *cap_dst;
    int cap_len, cap_max;
    uint8_t cap_sp, cap_is_author, cap_pause;
    char cap_pause_name[16];
    char chtitle[RD_TITLE_LEN];
    uint32_t ch_off;
    /* stream position, for FB2 <binary> offsets */
    uint32_t pos, tag_pos, bin_start;
    uint32_t bin_id_off;
    uint8_t in_bin;
} rd_parser_t;

void rd_doc_init(rd_doc_t *d, char *buf, uint32_t cap);
void rd_doc_finish(rd_doc_t *d);                       /* NUL-terminate */

/* One document. `base` is the archive directory of this file ("" for a loose file). */
void rd_parser_begin(rd_parser_t *p, rd_doc_t *d, rd_fmt_t fmt, const char *base);
void rd_parser_feed(rd_parser_t *p, const uint8_t *data, uint32_t len);
void rd_parser_end(rd_parser_t *p);

/* Attribute lookup in the text of one tag (name + attributes, no angle brackets). */
int rd_attr_find(const char *t, int n, const char *want, int suffix_ok, int *voff, int *vlen);
int rd_attr_get(const char *t, int n, const char *want, int suffix_ok, char *out, int cap);

/* Decode UTF-8 and entities, fold to ASCII, collapse spaces. `out` is NUL-terminated. */
void rd_capture_text(const uint8_t *s, uint32_t n, char *out, int cap);

/* Whole-buffer plain text: blank line = paragraph, single newline = space, and lines like
 * "CHAPTER IV." become chapters. Accents and curly quotes are folded to ASCII. */
void rd_txt_convert(rd_doc_t *d, const uint8_t *buf, uint32_t len);

/* Whole-buffer Markdown. */
void rd_md_convert(rd_doc_t *d, const uint8_t *buf, uint32_t len);

/* Join `base` + percent-decoded `src` and normalise "./" and "../". Returns length or 0. */
int rd_resolve_path(const char *base, const char *src, int src_len, char *out, int cap);

/* 1 if the document should stop being fed (output full). */
static inline int rd_doc_full(const rd_doc_t *d) { return d->overflow; }

#endif
