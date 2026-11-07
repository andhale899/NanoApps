/*
 * rd_page — single-pass page layout.
 *
 * The old pager asked LVGL to measure candidate text ~11 times per page, for every
 * page of the book, on open and again on every font change. This one walks the text
 * once with a 95-entry glyph-width table (ASCII 32..126, taken from the same LVGL font
 * the label draws with) and greedy-wraps it: O(characters), a few instructions each.
 *
 * It decides the line breaks itself, so the page is handed to the label already broken
 * into lines (label in CLIP mode) and what is measured is exactly what is drawn. Widths
 * ignore kerning, which can only make a line shorter than predicted, never longer.
 */
#ifndef RD_PAGE_H
#define RD_PAGE_H

#include <stdint.h>

typedef struct {
    uint8_t w[95];          /* advance in px of ASCII 32..126 */
    int line_w;             /* usable px per line */
    int max_lines;          /* lines per page */
} rd_metrics_t;

/* Lay out one page starting at `start`. Returns where the next page begins (len when
 * the text is exhausted). If `out` is non-NULL the page text is written to it, one line
 * per '\n'-separated row, NUL-terminated and clipped to cap-1 bytes. A page that begins
 * with an image marker is just that marker (out is left empty). */
uint32_t rd_page_layout(const char *text, uint32_t len, uint32_t start,
                        const rd_metrics_t *m, char *out, uint32_t cap);

/* Image index if the page starting at `start` is an image page, else -1. */
int rd_page_image(const char *text, uint32_t len, uint32_t start);

/* Fill starts[] with the start offset of every page. Returns the page count (<= cap). */
int rd_paginate(const char *text, uint32_t len, const rd_metrics_t *m,
                uint32_t *starts, int cap);

#endif
