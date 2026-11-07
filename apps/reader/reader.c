/*
 * reader — ereader for the iPod nano.
 *
 * Formats: EPUB (the main one), FB2, HTML/XHTML, Markdown and plain text, read from
 * /Data/Books/. EPUB/FB2 illustrations (JPEG, PNG) show on pages of their own.
 *
 * Library : a grid of book tiles, 2 x 2 per page, most recently read first. A tile shows the
 *           book's cover (cached in /Data/Books/.cov.<name>) and a progress bar. Swipe or use
 *           the arrows for more.
 * Reader  : tap the left third for the previous page, the rest for the next.
 *           Header: back | Aa (font size) | eye (day/night) | save (bookmark) | menu.
 * Menu    : position slider, chapters list, bookmarks list.
 *
 * Reopening the app returns to the book and page you left (/Data/Books/.last).
 *
 * Plain-text dot files in /Data/Books (the library scan skips them):
 *   .reader.cfg  font index + night flag     .last    name of the open book
 *   .recent      most recently read first    .rd.<n>  "offset total" reading position
 *   .bm.<n>      bookmark offsets            .meta.<n> title / author
 *
 * Positions and bookmarks are byte offsets into the converted text, so they survive a
 * font-size change.
 *
 * Memory: the book file, its converted text, the page table, scratch space, the inflate
 * window and the illustration buffer share ONE OS-heap block registered as the app's aux
 * block, so the resident frees it if the user quits mid-book.
 *
 * The text is ASCII only (the bundled font has no other glyphs): accents are folded,
 * Cyrillic and Greek transliterated, anything else dropped. See rd_markup.c.
 */
#include <string.h>
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_heap.h"
#include "lvgl/lvgl.h"
#include "rd_epub.h"
#include "rd_image.h"
#include "rd_page.h"
#include "rd_cover.h"

#define DATA_DIR       "/Data/Books"
#define CFG_PATH       DATA_DIR "/.reader.cfg"
#define LAST_PATH      DATA_DIR "/.last"
#define RECENT_PATH    DATA_DIR "/.recent"
#define POS_PREFIX     DATA_DIR "/.rd."
#define BM_PREFIX      DATA_DIR "/.bm."
#define META_PREFIX    DATA_DIR "/.meta."
#define COVER_PREFIX   DATA_DIR "/.cov."
#define COVER_MAX_BOOK (16u * 1024u * 1024u)

#define MAX_BOOKS      64
#define MAX_FILENAME   64
#define MAX_RECENT     32
#define MAX_BOOKMARKS  20
#define MAX_TEXT       (3u * 1024u * 1024u)
#define OS_RESERVE     (320u * 1024u)       /* heap the OS keeps for itself while a book is open */
#define TEXT_SLACK     (128u * 1024u)       /* working room (and page-table room) around a book's text */
#define SCRATCH_SZ     (160u * 1024u)
#define MAX_IMG_BYTES  (8u * 1024u * 1024u)

#define SCR_W          240
#define SCR_H          432
#define PAGE_TOP       40
#define PAGE_X         10
#define PAGE_W         220
#define PAGE_H         366
#define IMG_W          PAGE_W
#define IMG_H          PAGE_H

#define TILES          4
#define MROWS          5
#define ALIGN16(x)     (((x) + 15u) & ~15u)

enum { K_TXT, K_EPUB, K_FB2, K_HTML, K_MD };
enum { BTN_BACK, BTN_FONT, BTN_NIGHT, BTN_BM, BTN_MENU, BTN_COUNT };

typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
    uint8_t kind;
} book_t;

/* ---- library ---- */
static book_t s_books[MAX_BOOKS];
static int    s_n_books = 0;
static int    s_order[MAX_BOOKS];           /* display order: recent first */
static char   s_recent[MAX_RECENT][MAX_FILENAME];
static int    s_n_recent = 0;
static int    s_lib_page = 0;

/* ---- open book ---- */
static char     s_open_name[MAX_FILENAME];
static int      s_open_idx = -1;
static uint8_t *s_file;                     /* the book file as read from disk */
static uint32_t s_file_size;
static int      s_have_raw;                 /* s_file is a plain FB2 whose <binary> offsets are valid */
static char    *s_text;                     /* converted text (the rd_doc buffer) */
static uint32_t s_text_len;
static uint32_t *s_starts;
static int      s_page_cap, s_n_pages, s_cur;
static uint8_t *s_scratch, *s_window;
static uint16_t *s_imgbuf;
static rd_zip_t s_zip;
static int      s_have_zip;
static rd_doc_t s_doc;
static uint32_t s_session;                  /* bumped per opened book */
static int      s_inplace;                  /* the text was converted in place inside the file buffer */
static uint8_t *s_region_end;               /* end of the text region; the page table fills what the text leaves */
static uint32_t s_need_kb, s_have_kb;       /* the last memory shortfall, for the About page */
static char     s_problem[160];             /* the last thing that went wrong, for the About page */
static int      s_truncated;                /* the open book did not fit and was cut short */

static uint32_t s_bm[MAX_BOOKMARKS];
static int      s_n_bm = 0;

/* ---- settings ---- */
static int s_font_idx = 0;                  /* 0=14pt 1=16pt 2=20pt */
static int s_night = 0;
static rd_metrics_t s_metrics;

/* ---- image page state ---- */
static int      s_img_cached = -1;
static int      s_img_w, s_img_h;
static int      s_img_page;
static uint32_t s_img_session;

/* ---- widgets ---- */
static lv_obj_t *s_scr;
static lv_obj_t *s_lib_view, *s_lib_count, *s_lib_notice, *s_lib_empty;
static lv_obj_t *s_tile_area;
static lv_obj_t *s_tile[TILES], *s_tile_title[TILES], *s_tile_stat[TILES], *s_tile_kind[TILES], *s_tile_bar[TILES];
static lv_obj_t *s_tile_cv[TILES], *s_tile_band[TILES];
static int       s_slot_book[TILES];
static uint8_t  *s_thumb[TILES];             /* cover cache records, one per visible tile */
static uint8_t   s_cover_todo[TILES];
static int       s_cover_timer;
static int       s_in_library;               /* the library is on screen: cover work may run */
static int       s_lib_mem;                  /* the aux block currently holds library thumbnails */
static lv_obj_t *s_lib_prev, *s_lib_next, *s_lib_pg;
static lv_obj_t *s_rd_view, *s_page_lbl, *s_canvas, *s_foot_ch, *s_foot_pg;
static lv_obj_t *s_hbtn[BTN_COUNT], *s_hlbl[BTN_COUNT];
static lv_obj_t *s_menu_view, *s_menu_title, *s_menu_close, *s_menu_close_l, *s_menu_cap, *s_menu_pct, *s_menu_slider;
static lv_obj_t *s_menu_tab[2], *s_menu_tab_l[2], *s_menu_empty;
static lv_obj_t *s_mrow[MROWS], *s_mrow_l[MROWS], *s_mrow_r[MROWS];
static lv_obj_t *s_menu_prev, *s_menu_prev_l, *s_menu_next, *s_menu_next_l, *s_menu_pg;
static lv_obj_t *s_busy, *s_busy_lbl;
static int       s_menu_tab_sel = 0, s_menu_page = 0;
static lv_obj_t *s_prog;                    /* thin progress line shown while the toolbar is hidden */
static lv_obj_t *s_menu_gear, *s_menu_gear_l, *s_lib_gear;
static uint32_t  s_swipe_ms = 0;

/* ---- small helpers (no libc in the app image beyond string.h basics) ---- */

static int s_len(const char *s) { return (int)strlen(s); }
static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static void make_path(const char *prefix, const char *name, char *out, int cap)
{
    int k = 0;
    while (*prefix && k < cap - 1) out[k++] = *prefix++;
    for (int i = 0; name[i] && k < cap - 1; i++) out[k++] = name[i];
    out[k] = 0;
}

static uint32_t parse_u32(const char *buf, uint32_t n, uint32_t *i)
{
    uint32_t v = 0;
    while (*i < n && buf[*i] >= '0' && buf[*i] <= '9') { v = v * 10 + (uint32_t)(buf[*i] - '0'); (*i)++; }
    return v;
}

static const lv_font_t *cur_font(void)
{
    switch (s_font_idx) {
        case 1:  return &lv_font_montserrat_16;
        case 2:  return &lv_font_montserrat_20;
        default: return &lv_font_montserrat_14;
    }
}

/* Reader colours (independent of the system theme: sepia by day, dark by night). */
static uint32_t col_bg(void)  { return s_night ? 0x101010u : 0xf5f0e0u; }
static uint32_t col_fg(void)  { return s_night ? 0xd0d0d0u : 0x222222u; }
static uint32_t col_dim(void) { return s_night ? 0x808080u : 0x776f5cu; }
static uint32_t col_btn(void) { return s_night ? 0x2a2a2au : 0xdcd5c0u; }

/* ---- settings ---- */

typedef struct {
    uint8_t font;        /* 0 small (14 pt), 1 medium (16), 2 large (20) */
    uint8_t theme;       /* 0 day, 1 night, 2 auto (follows the time of day) */
    uint8_t spacing;     /* 0 tight, 1 normal, 2 loose */
    uint8_t margin;      /* 0 narrow, 1 normal, 2 wide */
    uint8_t hidebar;     /* the toolbar hides itself while you read */
    uint8_t resume;      /* reopen the last book at start */
} settings_t;

static settings_t s_set;

static void seg_refresh(void);                       /* settings screen: show the current choices */
static void settings_open(lv_event_t *e);
static void toolbar_show(void);
static lv_obj_t *s_set_view;                         /* the settings screen (built later) */

static uint32_t heap_avail(void);

static void settings_derive(void)
{
    s_font_idx = s_set.font;
    s_night = (s_set.theme == 1) ? 1 : (s_set.theme == 0) ? 0 : (hb_theme_is_dark() ? 1 : 0);
}

static void load_cfg(void)
{
    uint8_t b[16];
    uint32_t n = hb_fs_read(CFG_PATH, b, sizeof b);
    s_set.font = 0; s_set.theme = 2; s_set.spacing = 1; s_set.margin = 1;
    s_set.hidebar = 1; s_set.resume = 1;
    if (n >= 10 && b[0] == 'R' && b[1] == '2') {
        s_set.font = b[2] <= 2 ? b[2] : 0;
        s_set.theme = b[3] <= 2 ? b[3] : 2;
        s_set.spacing = b[4] <= 2 ? b[4] : 1;
        s_set.margin = b[5] <= 2 ? b[5] : 1;
        s_set.hidebar = b[6] ? 1 : 0;
        s_set.resume = b[8] ? 1 : 0;
    } else if (n >= 2) {                             /* the first version stored two characters */
        s_set.font = (b[0] >= '0' && b[0] <= '2') ? (uint8_t)(b[0] - '0') : 0;
        s_set.theme = (b[1] == '1') ? 1 : 0;
    }
    settings_derive();
}

static void save_cfg(void)
{
    uint8_t b[10] = { 'R', '2', s_set.font, s_set.theme, s_set.spacing, s_set.margin,
                      s_set.hidebar, 0 /* unused */, s_set.resume, 0 /* unused */ };
    hb_fs_write(CFG_PATH, b, sizeof b);
}

static const int k_line_extra[3] = { 0, 2, 5 };      /* extra px between lines */
static const int k_margin_x[3]   = { 6, 10, 22 };    /* px either side of the text */

/* Glyph widths and page geometry for the current font, spacing and margins. */
static void build_metrics(void)
{
    const lv_font_t *f = cur_font();
    for (int c = 32; c < 127; c++) {
        uint32_t w = lv_font_get_glyph_width(f, (uint32_t)c, 0);
        s_metrics.w[c - 32] = (uint8_t)(w > 255 ? 255 : w);
    }
    int lh = (int)lv_font_get_line_height(f);
    if (lh < 8) lh = 16;
    int extra = k_line_extra[s_set.spacing > 2 ? 1 : s_set.spacing];
    int mx = k_margin_x[s_set.margin > 2 ? 1 : s_set.margin];
    s_metrics.line_w = SCR_W - 2 * mx - 2;
    s_metrics.max_lines = PAGE_H / (lh + extra);
    lv_obj_set_pos(s_page_lbl, mx, PAGE_TOP);
    lv_obj_set_size(s_page_lbl, SCR_W - 2 * mx, PAGE_H);
    lv_obj_set_style_text_line_space(s_page_lbl, extra, 0);
}

/* How much one allocation can safely take: the largest free block, less what the OS must keep. */
static uint32_t heap_avail(void)
{
    uint32_t largest = hb_os_heap_largest(), freeb = hb_os_heap_free();
    uint32_t keep = freeb > OS_RESERVE ? freeb - OS_RESERVE : 0;
    return largest < keep ? largest : keep;
}

/* ---- busy overlay and deferred work ----
 * Heavy steps run from a one-shot timer started right after the overlay is shown, so
 * LVGL gets a frame in first and the "Loading" message is actually on screen. */

static void (*s_deferred)(void);

static void deferred_tick(lv_timer_t *t)
{
    (void)t;
    void (*f)(void) = s_deferred;
    s_deferred = 0;
    if (f) f();
}

static void defer(void (*f)(void))
{
    s_deferred = f;
    lv_timer_t *t = lv_timer_create(deferred_tick, 60, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void busy_show(const char *msg)
{
    lv_label_set_text(s_busy_lbl, msg);
    lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_busy);
}

static void busy_hide(void) { lv_obj_add_flag(s_busy, LV_OBJ_FLAG_HIDDEN); }

/* ---- progress, recents, titles ---- */

static int load_progress(const char *name, uint32_t *off, uint32_t *total)
{
    char path[260], buf[28];
    make_path(POS_PREFIX, name, path, sizeof path);
    uint32_t n = hb_fs_read(path, buf, sizeof buf - 1);
    if (n == 0) return 0;
    buf[n] = 0;
    uint32_t i = 0;
    *off = parse_u32(buf, n, &i);
    while (i < n && buf[i] == ' ') i++;
    *total = parse_u32(buf, n, &i);
    return 1;
}

static void load_recent(void)
{
    static char buf[MAX_RECENT * (MAX_FILENAME + 1) + 8];
    s_n_recent = 0;
    uint32_t n = hb_fs_read(RECENT_PATH, buf, sizeof buf - 1);
    buf[n] = 0;
    uint32_t i = 0;
    while (i < n && s_n_recent < MAX_RECENT) {
        int k = 0;
        while (i < n && buf[i] != '\n') { if (k < MAX_FILENAME - 1) s_recent[s_n_recent][k++] = buf[i]; i++; }
        i++;
        s_recent[s_n_recent][k] = 0;
        if (k) s_n_recent++;
    }
}

static void touch_recent(const char *name)
{
    int at = -1;
    for (int i = 0; i < s_n_recent; i++) if (strcmp(s_recent[i], name) == 0) { at = i; break; }
    if (at < 0) {
        if (s_n_recent < MAX_RECENT) s_n_recent++;
        at = s_n_recent - 1;
    }
    for (int i = at; i > 0; i--) memcpy(s_recent[i], s_recent[i - 1], MAX_FILENAME);
    int k = 0;
    while (name[k] && k < MAX_FILENAME - 1) { s_recent[0][k] = name[k]; k++; }
    s_recent[0][k] = 0;

    static char out[MAX_RECENT * (MAX_FILENAME + 1) + 8];
    uint32_t o = 0;
    for (int i = 0; i < s_n_recent; i++) {
        int l = s_len(s_recent[i]);
        memcpy(out + o, s_recent[i], (size_t)l);
        o += (uint32_t)l;
        out[o++] = '\n';
    }
    hb_fs_write(RECENT_PATH, out, o);
}

static void build_order(void)
{
    uint8_t used[MAX_BOOKS];
    memset(used, 0, sizeof used);
    int n = 0;
    for (int r = 0; r < s_n_recent; r++)
        for (int i = 0; i < s_n_books; i++)
            if (!used[i] && strcmp(s_books[i].name, s_recent[r]) == 0) { used[i] = 1; s_order[n++] = i; break; }
    for (int i = 0; i < s_n_books; i++) if (!used[i]) s_order[n++] = i;
}

static const char *kind_label(int k)
{
    switch (k) {
        case K_EPUB: return "EPUB";
        case K_FB2: return "FB2";
        case K_HTML: return "HTML";
        case K_MD: return "MD";
        default: return "TXT";
    }
}

/* Display title: the title cached from the book's metadata, else the file name. */
static void book_title(const book_t *b, char *out, int cap)
{
    char path[260], buf[100];
    make_path(META_PREFIX, b->name, path, sizeof path);
    uint32_t n = hb_fs_read(path, buf, sizeof buf - 1);
    int k = 0;
    if (n) {
        while ((uint32_t)k < n && buf[k] != '\n' && k < cap - 1) { out[k] = buf[k]; k++; }
        out[k] = 0;
        if (k) return;
    }
    int len = s_len(b->name);
    int cut = len;
    for (int i = len - 1; i > 0; i--) if (b->name[i] == '.') { cut = i; break; }
    for (k = 0; k < cut && k < cap - 1; k++) out[k] = b->name[k] == '_' ? ' ' : b->name[k];
    out[k] = 0;
}

static void save_meta(void)
{
    if (!s_doc.title[0]) return;
    char path[260], buf[130];
    make_path(META_PREFIX, s_open_name, path, sizeof path);
    int k = 0;
    for (int i = 0; s_doc.title[i] && k < 70; i++) buf[k++] = s_doc.title[i];
    buf[k++] = '\n';
    for (int i = 0; s_doc.author[i] && k < 125; i++) buf[k++] = s_doc.author[i];
    buf[k++] = '\n';
    hb_fs_write(path, buf, (uint32_t)k);
}

/* ---- cover cache ---- */

static uint32_t tile_color(const char *name);

static void cover_path(const book_t *b, char *out, int cap) { make_path(COVER_PREFIX, b->name, out, cap); }

static void cover_save(const book_t *b, const uint8_t *rec, uint32_t n)
{
    char path[260];
    cover_path(b, path, sizeof path);
    hb_fs_write(path, rec, n);
}

/* ---- library scan ---- */

static int kind_of(const char *n)
{
    int len = s_len(n);
    int dot = -1;
    for (int i = 0; i < len; i++) if (n[i] == '.') dot = i;
    if (dot < 0 || len - dot > 9) return -1;
    char e[10];
    int el = 0;
    for (int j = dot + 1; n[j]; j++) e[el++] = (char)lowc(n[j]);
    e[el] = 0;
    if (!strcmp(e, "txt") || !strcmp(e, "text")) return K_TXT;
    if (!strcmp(e, "epub")) return K_EPUB;
    if (!strcmp(e, "fb2")) return K_FB2;
    if (!strcmp(e, "md") || !strcmp(e, "markdown")) return K_MD;
    if (!strcmp(e, "html") || !strcmp(e, "htm") || !strcmp(e, "xhtml")) return K_HTML;
    return -1;
}

static void scan_books(void)
{
    s_n_books = 0;
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char fn[MAX_FILENAME];
    bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (is_dir || fn[0] == '.') continue;
        int kind = kind_of(fn);
        if (kind < 0) continue;
        if (s_n_books >= MAX_BOOKS) break;
        book_t *b = &s_books[s_n_books];
        int i = 0;
        while (fn[i] && i < MAX_FILENAME - 1) { b->name[i] = fn[i]; i++; }
        b->name[i] = 0;
        char path[256];
        make_path(DATA_DIR "/", fn, path, sizeof path);
        b->size = hb_fs_size(path);
        b->kind = (uint8_t)kind;
        s_n_books++;
    }
    hb_fs_dir_close(&d);
}

/* ---- reading position and bookmarks ---- */

static int page_for_offset(uint32_t off)
{
    if (s_n_pages <= 0) return 0;
    int lo = 0, hi = s_n_pages - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (s_starts[mid] <= off) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static uint32_t page_end_offset(int page)
{
    return (page + 1 < s_n_pages) ? s_starts[page + 1] : s_text_len;
}

static void save_position(void)
{
    if (!s_open_name[0] || s_n_pages == 0) return;
    char path[260], buf[28];
    make_path(POS_PREFIX, s_open_name, path, sizeof path);
    int k = lv_snprintf(buf, sizeof buf, "%u %u", (unsigned)s_starts[s_cur], (unsigned)s_text_len);
    hb_fs_write(path, buf, (uint32_t)k);
}

static void load_bookmarks(void)
{
    s_n_bm = 0;
    char path[260];
    static char buf[256];
    make_path(BM_PREFIX, s_open_name, path, sizeof path);
    uint32_t n = hb_fs_read(path, buf, sizeof buf);
    uint32_t i = 0;
    while (i < n && s_n_bm < MAX_BOOKMARKS) {
        if (buf[i] < '0' || buf[i] > '9') { i++; continue; }
        uint32_t v = parse_u32(buf, n, &i);
        if (v < s_text_len) s_bm[s_n_bm++] = v;
    }
}

static void save_bookmarks(void)
{
    char path[260];
    static char buf[256];
    make_path(BM_PREFIX, s_open_name, path, sizeof path);
    uint32_t k = 0;
    for (int i = 0; i < s_n_bm && k + 12 < sizeof buf; i++) {
        k += (uint32_t)lv_snprintf(buf + k, sizeof buf - k, "%u\n", (unsigned)s_bm[i]);
    }
    hb_fs_write(path, buf, k);
}

static int page_bm_index(int page)
{
    if (s_n_pages == 0) return -1;
    uint32_t a = s_starts[page], b = page_end_offset(page);
    for (int i = 0; i < s_n_bm; i++) if (s_bm[i] >= a && s_bm[i] < b) return i;
    return -1;
}

static void toggle_bookmark(void)
{
    if (s_n_pages == 0) return;
    if (page_bm_index(s_cur) >= 0) {
        uint32_t a = s_starts[s_cur], b = page_end_offset(s_cur);
        int w = 0;
        for (int i = 0; i < s_n_bm; i++) if (!(s_bm[i] >= a && s_bm[i] < b)) s_bm[w++] = s_bm[i];
        s_n_bm = w;
    } else {
        if (s_n_bm >= MAX_BOOKMARKS) return;
        uint32_t v = s_starts[s_cur];
        int i = s_n_bm++;
        while (i > 0 && s_bm[i - 1] > v) { s_bm[i] = s_bm[i - 1]; i--; }
        s_bm[i] = v;
    }
    save_bookmarks();
}

static int chapter_for_offset(uint32_t off)
{
    int lo = 0, hi = s_doc.nchap - 1, r = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_doc.chap[mid].off <= off) { r = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return r;
}

/* ---- the book's memory ---- */

static void book_free(void)
{
    if (s_canvas) lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < TILES; i++) {
        s_thumb[i] = 0;
        s_cover_todo[i] = 0;
        if (s_tile_cv[i]) lv_obj_add_flag(s_tile_cv[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_lib_mem = 0;
    if (HB_APP_AUX_BLOCK) hb_os_free(HB_APP_AUX_BLOCK);
    HB_APP_AUX_BLOCK = 0;
    s_file = 0; s_text = 0; s_starts = 0; s_scratch = 0; s_window = 0; s_imgbuf = 0;
    s_n_pages = 0; s_page_cap = 0; s_text_len = 0; s_cur = 0;
    s_have_zip = 0; s_have_raw = 0; s_img_cached = -1;
    s_region_end = 0; s_inplace = 0; s_truncated = 0;
    s_session++;
}

/* Memory for one open book, in a single aux block (the resident frees it if we are quit).
 *   TXT / Markdown / HTML :  [ output ... | input file ]    converted IN PLACE (rd_doc_t.slack)
 *   FB2 / EPUB            :  [ file ][ text ][ scratch | window | picture ]
 * The page table is not reserved up front: it goes in whatever the text leaves free.
 * `t_req` is the text size wanted for an EPUB / FB2. Returns the text-region size, or -1. */
static int book_alloc(const book_t *b, uint32_t t_req)
{
    uint32_t F = b->size;
    int pics = (b->kind == K_EPUB || b->kind == K_FB2);
    s_inplace = (b->kind == K_TXT || b->kind == K_MD || b->kind == K_HTML);
    uint32_t extra = pics ? ALIGN16(SCRATCH_SZ) + ALIGN16(RD_WINDOW_SIZE) + ALIGN16(IMG_W * IMG_H * 2u) : 0;
    uint32_t avail = heap_avail();
    uint32_t T = s_inplace ? ALIGN16(F) + TEXT_SLACK : t_req;
    uint32_t total;
    for (;;) {
        total = (s_inplace ? 0u : ALIGN16(F)) + ALIGN16(T) + extra + 64u;
        if (total <= avail) break;
        if (b->kind != K_EPUB || T <= 2u * TEXT_SLACK) {     /* only an EPUB's text room can shrink */
            s_need_kb = total / 1024u;
            s_have_kb = avail / 1024u;
            return -1;
        }
        T -= T / 4u;
    }
    uint8_t *raw = (uint8_t *)hb_os_alloc(total + 16u);
    if (!raw) { s_need_kb = total / 1024u; s_have_kb = avail / 1024u; return -1; }
    HB_APP_AUX_BLOCK = raw;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + 15u) & ~(uintptr_t)15u);
    if (s_inplace) {
        s_text = (char *)p;
        s_file = p + TEXT_SLACK;
    } else {
        s_file = p;
        p += ALIGN16(F);
        s_text = (char *)p;
    }
    s_region_end = (uint8_t *)s_text + ALIGN16(T);
    p = s_region_end;
    if (pics) {
        s_scratch = p;  p += ALIGN16(SCRATCH_SZ);
        s_window = p;   p += ALIGN16(RD_WINDOW_SIZE);
        s_imgbuf = (uint16_t *)(void *)p;
    }
    s_file_size = F;
    return (int)T;
}

/* ---- rendering ---- */

static void apply_theme(void)
{
    lv_obj_set_style_bg_color(s_rd_view, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_text_color(s_page_lbl, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_text_color(s_foot_ch, lv_color_hex(col_dim()), 0);
    lv_obj_set_style_text_color(s_foot_pg, lv_color_hex(col_dim()), 0);
    lv_obj_set_style_bg_color(s_prog, lv_color_hex(col_dim()), LV_PART_INDICATOR);
    for (int i = 0; i < BTN_COUNT; i++) {
        lv_obj_set_style_bg_color(s_hbtn[i], lv_color_hex(col_btn()), 0);
        lv_obj_set_style_text_color(s_hlbl[i], lv_color_hex(col_fg()), 0);
    }
}

static void refresh_bm_button(void)
{
    bool on = s_n_pages > 0 && page_bm_index(s_cur) >= 0;
    lv_obj_set_style_bg_color(s_hbtn[BTN_BM], lv_color_hex(on ? hb_color_primary() : col_btn()), 0);
    lv_obj_set_style_text_color(s_hlbl[BTN_BM], lv_color_hex(on ? hb_color_on_primary() : col_fg()), 0);
}

static void update_footer(void)
{
    char foot[24];
    lv_snprintf(foot, sizeof foot, "%d / %d", s_cur + 1, s_n_pages);
    lv_label_set_text(s_foot_pg, foot);
    int ch = chapter_for_offset(s_starts[s_cur]);
    lv_label_set_text(s_foot_ch, ch >= 0 ? s_doc.chap[ch].title : "");
    lv_bar_set_value(s_prog, s_n_pages > 0 ? (s_cur + 1) * 1000 / s_n_pages : 0, LV_ANIM_OFF);
    refresh_bm_button();
}

static void show_text(const char *txt)
{
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_page_lbl, cur_font(), 0);
    lv_label_set_text(s_page_lbl, txt);
}

struct memw { uint8_t *p; uint32_t cap, len; int over; };

static int memw_sink(void *c, const uint8_t *d, uint32_t n)
{
    struct memw *m = (struct memw *)c;
    if (n > m->cap - m->len) { m->over = 1; return 1; }
    memcpy(m->p + m->len, d, n);
    m->len += n;
    return 0;
}

/* Pull illustration `idx` out of the book into a fresh OS-heap buffer; the caller frees it. */
static uint8_t *extract_image(int idx, uint32_t *len_out)
{
    if (idx < 0 || idx >= s_doc.nimg) return 0;
    const char *ref = s_doc.ipool + s_doc.img_off[idx];
    uint8_t *tmp = 0;
    if (ref[0] == '#') {                                    /* FB2 <binary> block */
        if (!s_have_raw) return 0;
        for (int i = 0; i < s_doc.nbin; i++) {
            if (strcmp(s_doc.ipool + s_doc.bin[i].id_off, ref) != 0) continue;
            uint32_t dl = s_doc.bin[i].data_len, off = s_doc.bin[i].data_off;
            if (dl == 0 || off > s_file_size || dl > s_file_size - off) return 0;
            uint32_t cap = dl / 4u * 3u + 8u;
            if (cap > MAX_IMG_BYTES) return 0;
            tmp = (uint8_t *)hb_os_alloc(cap);
            if (!tmp) return 0;
            int n = rd_base64_decode(s_file + off, dl, tmp, cap);
            if (n <= 0) { hb_os_free(tmp); return 0; }
            *len_out = (uint32_t)n;
            return tmp;
        }
        return 0;
    }
    rd_zip_ent_t e;                                         /* a file inside the EPUB */
    if (!s_have_zip || rd_zip_find(&s_zip, ref, (uint32_t)strlen(ref), &e)) return 0;
    if (e.usize == 0 || e.usize > MAX_IMG_BYTES) return 0;
    tmp = (uint8_t *)hb_os_alloc(e.usize);
    if (!tmp) return 0;
    struct memw m = { tmp, e.usize, 0, 0 };
    if (rd_zip_read(&s_zip, &e, s_window, memw_sink, &m) || m.over || m.len == 0) {
        hb_os_free(tmp);
        return 0;
    }
    *len_out = m.len;
    return tmp;
}

/* Decode illustration `idx` into s_imgbuf. Returns 0 on success. */
static int load_image(int idx)
{
    uint32_t tlen = 0;
    uint8_t *tmp = extract_image(idx, &tlen);
    if (!tmp) return -1;
    int w = 0, h = 0;
    int rc = rd_image_decode(tmp, tlen, s_imgbuf, IMG_W, IMG_H, col_bg(), s_scratch, SCRATCH_SZ, s_window, &w, &h);
    hb_os_free(tmp);
    if (rc) return rc;
    s_img_w = w;
    s_img_h = h;
    return 0;
}

/* A library thumbnail made from one of the book's own pictures. */
static int cover_from_doc_image(int idx, uint8_t *rec, uint32_t size_tag, uint32_t bg)
{
    uint32_t tlen = 0;
    uint8_t *tmp = extract_image(idx, &tlen);
    if (!tmp) return -1;
    int n = rd_cover_from_image(tmp, tlen, size_tag, bg, rec, s_scratch, SCRATCH_SZ, s_window);
    hb_os_free(tmp);
    return n;
}

/* The book you just opened gets a cached cover for the library, if it has none yet. */
static void cache_cover_at_open(const book_t *b, uint32_t rd)
{
    if (b->kind != K_EPUB && b->kind != K_FB2) return;
    char path[260];
    cover_path(b, path, sizeof path);
    uint8_t hdr[RD_COVER_HDR];
    uint32_t n = hb_fs_read(path, hdr, sizeof hdr);
    if (rd_cover_peek(hdr, n, b->size)) return;             /* a cover or a "none" marker is already cached */
    uint8_t *rec = (uint8_t *)s_imgbuf;                     /* idle until the first picture page, and roomy enough */
    uint32_t bg = tile_color(b->name);
    int len = -1;
    if (b->kind == K_EPUB) {
        rd_epub_mem_t mem = { s_window, s_scratch, SCRATCH_SZ };
        len = rd_cover_from_epub(s_file, rd, b->size, bg, rec, &mem, s_scratch, SCRATCH_SZ, hb_os_alloc, hb_os_free);
    }
    if ((len < 0 || len == RD_COVER_HDR) && rd_page_image(s_text, s_text_len, 0) >= 0) {
        int n2 = cover_from_doc_image(0, rec, b->size, bg); /* the book opens on a picture: that is its cover */
        if (n2 >= 0) len = n2;
    }
    if (len >= 0) cover_save(b, rec, (uint32_t)len);
    s_img_cached = -1;
}

static void present_image(void)
{
    lv_canvas_set_buffer(s_canvas, s_imgbuf, s_img_w, s_img_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, PAGE_TOP + (PAGE_H - s_img_h) / 2);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_canvas);
    lv_label_set_text(s_page_lbl, "");
}

static void image_tick(lv_timer_t *t)
{
    (void)t;
    if (s_img_session != s_session || s_img_page != s_cur || !s_text) return;   /* page moved on */
    int idx = rd_page_image(s_text, s_text_len, s_starts[s_cur]);
    if (idx < 0) return;
    if (load_image(idx) == 0) {
        s_img_cached = idx;
        present_image();
    } else {
        s_img_cached = -1;
        lv_label_set_text(s_page_lbl, "\n\n\n   [ illustration cannot be shown ]");
    }
}

static void show_image_page(int idx)
{
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    if (idx == s_img_cached) { present_image(); return; }
    lv_obj_set_style_text_font(s_page_lbl, cur_font(), 0);
    lv_label_set_text(s_page_lbl, "\n\n\n   Loading picture...");
    s_img_page = s_cur;
    s_img_session = s_session;
    lv_timer_t *t = lv_timer_create(image_tick, 40, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void render_page(void)
{
    if (!s_text) return;
    if (s_n_pages == 0) {
        show_text("(this book has no text)");
        lv_label_set_text(s_foot_pg, "");
        lv_label_set_text(s_foot_ch, "");
        return;
    }
    if (s_cur < 0) s_cur = 0;
    if (s_cur >= s_n_pages) s_cur = s_n_pages - 1;
    static char pagebuf[4096];
    uint32_t start = s_starts[s_cur];
    int img = rd_page_image(s_text, s_text_len, start);
    if (img >= 0) {
        show_image_page(img);
    } else {
        rd_page_layout(s_text, s_text_len, start, &s_metrics, pagebuf, sizeof pagebuf);
        show_text(pagebuf);
    }
    update_footer();
}

static void go_to_page(int page)
{
    if (s_n_pages == 0) return;
    if (page < 0) page = 0;
    if (page >= s_n_pages) page = s_n_pages - 1;
    if (page == s_cur) return;
    s_cur = page;
    save_position();
    render_page();
}

static void go_to_offset(uint32_t off)
{
    if (s_n_pages == 0) return;
    int p = page_for_offset(off);
    if (p == s_cur) return;
    s_cur = p;
    save_position();
    render_page();
}

/* ---- library screen ---- */

static uint32_t tile_color(const char *name)
{
    uint32_t h = 5381;
    for (const char *p = name; *p; p++) h = h * 33u + (uint8_t)*p;
    switch (h % 8u) {
        case 0: return 0x2f6f9f;
        case 1: return 0x8a4f9e;
        case 2: return 0xb5533c;
        case 3: return 0x3f8f6b;
        case 4: return 0xa8741a;
        case 5: return 0x4a5568;
        case 6: return 0xa63d6e;
        default: return 0x2d8a8f;
    }
}

/* ---- notices: the header's right-hand text ---- */

static int s_notice_err;

static void notice_show(int on)
{
    if (on) {
        lv_obj_remove_flag(s_lib_notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lib_count, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lib_notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lib_count, LV_OBJ_FLAG_HIDDEN);
    }
}

static void notice(const char *msg)                 /* an error: stays until the next book opens */
{
    s_notice_err = msg[0] != 0;
    lv_obj_set_style_text_color(s_lib_notice, lv_color_hex(hb_color_danger()), 0);
    lv_label_set_text(s_lib_notice, msg);
    notice_show(msg[0] != 0);
}

static void notice_info(const char *msg)            /* progress text; never covers up an error */
{
    if (s_notice_err) return;
    lv_obj_set_style_text_color(s_lib_notice, lv_color_hex(hb_color_text_dim()), 0);
    lv_label_set_text(s_lib_notice, msg);
    notice_show(msg[0] != 0);
}

/* ---- library memory and covers ----
 * With no book open, the aux block holds one cover record per visible tile plus the scratch
 * space for making covers. Opening a book swaps it for the book's own block (book_free). */

static void lib_alloc(void)
{
    if (HB_APP_AUX_BLOCK) return;
    uint32_t rec = ALIGN16(RD_COVER_BUF);
    uint32_t total = rec * TILES + ALIGN16(SCRATCH_SZ) + ALIGN16(RD_WINDOW_SIZE) + 64u;
    if (total > heap_avail()) return;
    uint8_t *raw = (uint8_t *)hb_os_alloc(total + 16u);
    if (!raw) return;
    HB_APP_AUX_BLOCK = raw;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + 15u) & ~(uintptr_t)15u);
    for (int i = 0; i < TILES; i++) { s_thumb[i] = p; p += rec; }
    s_scratch = p;
    p += ALIGN16(SCRATCH_SZ);
    s_window = p;
    s_lib_mem = 1;
}

static void tile_hide_cover(int slot)
{
    lv_obj_add_flag(s_tile_cv[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_tile_title[slot], LV_OBJ_FLAG_HIDDEN);
}

static void tile_show_cover(int slot, int w, int h)
{
    lv_canvas_set_buffer(s_tile_cv[slot], s_thumb[slot] + RD_COVER_HDR, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_tile_cv[slot], LV_ALIGN_TOP_MID, 0, (RD_COVER_H - h) / 2);
    lv_obj_remove_flag(s_tile_cv[slot], LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_tile_cv[slot]);
    lv_obj_add_flag(s_tile_title[slot], LV_OBJ_FLAG_HIDDEN);
}

/* Make and cache the cover of an EPUB that is not open: read the file, find the cover in its
 * metadata, shrink it. Leaves things as they are (to retry later) if memory is short. */
static void gen_cover(int slot, const book_t *b)
{
    uint32_t F = b->size;
    uint8_t *rec = s_thumb[slot];
    if (!rec || F == 0) return;
    int n;
    if (F > COVER_MAX_BOOK) {
        n = rd_cover_none(rec, F);                          /* too big to read just for a thumbnail */
    } else {
        if (F + 256u * 1024u > heap_avail()) return;
        uint8_t *file = (uint8_t *)hb_os_alloc(F);
        if (!file) return;
        char path[256];
        make_path(DATA_DIR "/", b->name, path, sizeof path);
        uint32_t rd = hb_fs_read(path, file, F);
        rd_epub_mem_t mem = { s_window, s_scratch, SCRATCH_SZ };
        n = rd ? rd_cover_from_epub(file, rd, F, tile_color(b->name), rec, &mem, s_scratch, SCRATCH_SZ,
                                    hb_os_alloc, hb_os_free) : -1;
        hb_os_free(file);
    }
    if (n < 0) return;
    cover_save(b, rec, (uint32_t)n);
    int w = 0, h = 0;
    if (rd_cover_check(rec, (uint32_t)n, F, &w, &h) == 1) tile_show_cover(slot, w, h);
}

static void cover_tick(lv_timer_t *t);

static int cover_pending(void)
{
    for (int i = 0; i < TILES; i++) if (s_cover_todo[i]) return 1;
    return 0;
}

static void cover_schedule(void)
{
    s_cover_timer = 1;
    lv_timer_t *t = lv_timer_create(cover_tick, 80, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void cover_kick(void)
{
    if (s_cover_timer || !s_in_library || !s_lib_mem || !cover_pending()) return;
    notice_info("Preparing covers...");
    cover_schedule();
}

/* One cover per tick, so the library stays usable while they appear. */
static void cover_tick(lv_timer_t *t)
{
    (void)t;
    s_cover_timer = 0;
    if (!s_in_library || !s_lib_mem) return;
    int slot = -1;
    for (int i = 0; i < TILES; i++) if (s_cover_todo[i]) { slot = i; break; }
    if (slot < 0) { notice_info(""); return; }
    s_cover_todo[slot] = 0;
    int book = s_slot_book[slot];
    if (book >= 0 && book < s_n_books) gen_cover(slot, &s_books[book]);
    if (cover_pending()) cover_schedule();
    else notice_info("");
}

static void tile_update(int slot, int book)
{
    lv_obj_t *t = s_tile[slot];
    s_cover_todo[slot] = 0;
    tile_hide_cover(slot);
    if (book < 0) { lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN); return; }
    const book_t *b = &s_books[book];
    s_slot_book[slot] = book;
    char title[100];
    book_title(b, title, sizeof title);
    lv_label_set_text(s_tile_title[slot], title);
    lv_label_set_text(s_tile_kind[slot], kind_label(b->kind));
    lv_obj_set_style_bg_color(t, lv_color_hex(tile_color(b->name)), 0);

    uint32_t off = 0, total = 0;
    int have = load_progress(b->name, &off, &total);
    if (have && total == 0) total = b->size;
    int pct = (have && total) ? (int)((off > total ? total : off) * 100u / total) : 0;
    char stat[12];
    if (!have) lv_snprintf(stat, sizeof stat, "NEW");
    else if (pct >= 98) lv_snprintf(stat, sizeof stat, "DONE");
    else lv_snprintf(stat, sizeof stat, "%d%%", pct);
    lv_label_set_text(s_tile_stat[slot], stat);
    lv_bar_set_value(s_tile_bar[slot], pct >= 98 ? 100 : pct, LV_ANIM_OFF);

    /* the cover, if one is cached; an EPUB with no record yet gets one made in the background */
    if (s_lib_mem && s_thumb[slot]) {
        char cp[260];
        cover_path(b, cp, sizeof cp);
        uint32_t n = hb_fs_read(cp, s_thumb[slot], RD_COVER_BUF);
        int cw = 0, ch = 0;
        int v = rd_cover_check(s_thumb[slot], n, b->size, &cw, &ch);
        if (v == 1) tile_show_cover(slot, cw, ch);
        else if (v < 0 && b->kind == K_EPUB) s_cover_todo[slot] = 1;
    }
    lv_obj_remove_flag(t, LV_OBJ_FLAG_HIDDEN);
}

static void library_refresh(void)
{
    notice_info("");
    build_order();
    int pages = (s_n_books + TILES - 1) / TILES;
    if (pages < 1) pages = 1;
    if (s_lib_page >= pages) s_lib_page = pages - 1;
    if (s_lib_page < 0) s_lib_page = 0;
    for (int s = 0; s < TILES; s++) {
        int k = s_lib_page * TILES + s;
        tile_update(s, k < s_n_books ? s_order[k] : -1);
    }
    char pg[16];
    lv_snprintf(pg, sizeof pg, "%d / %d", s_lib_page + 1, pages);
    lv_label_set_text(s_lib_pg, pg);
    bool multi = pages > 1;
    lv_obj_set_x(s_lib_gear, multi ? 116 : (SCR_W - 56) / 2);
    if (multi) {
        lv_obj_remove_flag(s_lib_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lib_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lib_pg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lib_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lib_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lib_pg, LV_OBJ_FLAG_HIDDEN);
    }
    char cnt[20];
    lv_snprintf(cnt, sizeof cnt, "%d book%s", s_n_books, s_n_books == 1 ? "" : "s");
    lv_label_set_text(s_lib_count, cnt);
    if (s_n_books == 0) lv_obj_remove_flag(s_lib_empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_lib_empty, LV_OBJ_FLAG_HIDDEN);
    cover_kick();
}

static void show_library(void)
{
    s_in_library = 1;
    lv_obj_add_flag(s_rd_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_menu_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lib_view, LV_OBJ_FLAG_HIDDEN);
    library_refresh();
}

/* ---- opening a book ---- */

static const char *open_error(int rc)
{
    switch (rc) {
        case RD_EPUB_ERR_ZIP:   return "Not a valid book file";
        case RD_EPUB_ERR_DRM:   return "DRM-protected book";
        case RD_EPUB_ERR_EMPTY: return "No readable text";
        default:                return "Could not open book";
    }
}

static void fail_open(const char *msg)
{
    lv_snprintf(s_problem, sizeof s_problem, "%s: %s", s_open_name, msg);
    book_free();
    s_open_name[0] = 0;
    busy_hide();
    notice(msg);
    lib_alloc();
    show_library();
}

/* Turn the book in s_file into text in s_doc. Returns 0 or an RD_EPUB_ERR_* code. */
static int convert_book(const book_t *b, uint32_t rd)
{
    int rc = 0;
    switch (b->kind) {
    case K_TXT:  rd_txt_convert(&s_doc, s_file, rd); break;
    case K_MD:   rd_md_convert(&s_doc, s_file, rd); break;
    case K_HTML: {
        rd_parser_t p;
        rd_parser_begin(&p, &s_doc, RD_FMT_HTML, "");
        rd_parser_feed(&p, s_file, rd);
        rd_parser_end(&p);
        rd_doc_finish(&s_doc);
        break;
    }
    case K_FB2: {
        rd_parser_t p;
        rd_parser_begin(&p, &s_doc, RD_FMT_FB2, "");
        rd_parser_feed(&p, s_file, rd);
        rd_parser_end(&p);
        rd_doc_finish(&s_doc);
        s_have_raw = 1;                                      /* the file stays: pictures are decoded from it */
        break;
    }
    default: {
        rd_epub_mem_t mem = { s_window, s_scratch, SCRATCH_SZ };
        rc = rd_epub_load(&s_zip, &s_doc, s_file, rd, &mem);
        s_have_zip = (rc == 0);
        break;
    }
    }
    return rc;
}

static const char k_trunc_note[] = "\n[ The rest of this book did not fit in memory. ]";

static void do_open(void)
{
    if (s_open_idx < 0 || s_open_idx >= s_n_books) { busy_hide(); return; }
    const book_t *b = &s_books[s_open_idx];
    s_in_library = 0;
    if (b->size == 0) { book_free(); memcpy(s_open_name, b->name, MAX_FILENAME); fail_open("Empty file"); return; }
    char path[256];
    make_path(DATA_DIR "/", b->name, path, sizeof path);

    /* How much text room to ask for: an EPUB's text is a few times its zipped size (retried bigger if
     * that turns out too small); an FB2's text never exceeds the file; the rest convert in place. */
    uint32_t t_req = 0;
    if (b->kind == K_EPUB) {
        t_req = b->size * 3u + 192u * 1024u;
        if (t_req > MAX_TEXT) t_req = MAX_TEXT;
    } else if (b->kind == K_FB2) {
        t_req = ALIGN16(b->size) + TEXT_SLACK;
    }
    uint32_t rd = 0;
    int T = 0;
    for (int attempt = 0;; attempt++) {
        book_free();
        memcpy(s_open_name, b->name, MAX_FILENAME);
        T = book_alloc(b, t_req);
        if (T < 0) {
            fail_open("Not enough memory");
            lv_snprintf(s_problem, sizeof s_problem, "%s needs about %u KB but only %u KB can be used",
                        b->name, (unsigned)s_need_kb, (unsigned)s_have_kb);
            return;
        }
        rd = hb_fs_read(path, s_file, b->size);
        if (rd == 0) { fail_open("Cannot read file"); return; }

        if (s_inplace) {
            rd_doc_init(&s_doc, s_text, TEXT_SLACK);
            s_doc.slack = TEXT_SLACK;                        /* output may grow as the input is consumed */
            s_doc.no_images = (b->kind == K_HTML);
        } else {
            rd_doc_init(&s_doc, s_text, (uint32_t)T - TEXT_SLACK);   /* the tail is kept for the page table */
        }
        int rc = convert_book(b, rd);
        if (rc) { fail_open(open_error(rc)); return; }
        if (b->kind == K_EPUB && s_doc.overflow && attempt < 2 && (uint32_t)T < MAX_TEXT) {
            t_req = (uint32_t)T * 2u;                        /* more text than guessed: go again with room */
            if (t_req > MAX_TEXT) t_req = MAX_TEXT;
            continue;
        }
        break;
    }
    s_text_len = s_doc.len;
    if (s_text_len < 4 && s_doc.nimg == 0) { fail_open("No readable text"); return; }
    if (s_doc.overflow && (uint8_t *)s_text + s_text_len + sizeof k_trunc_note + 64u < s_region_end) {
        memcpy(s_text + s_text_len, k_trunc_note, sizeof k_trunc_note);
        s_text_len += (uint32_t)sizeof k_trunc_note - 1u;
        s_truncated = 1;
        lv_snprintf(s_problem, sizeof s_problem, "%s was cut short: not enough memory for all of it", b->name);
    }

    /* the page table goes in the room the text leaves free */
    uint8_t *tbl = (uint8_t *)(((uintptr_t)(s_text + s_text_len + 1) + 15u) & ~(uintptr_t)15u);
    if (tbl + 1024u > s_region_end) { fail_open("Book too large to format"); return; }
    s_starts = (uint32_t *)(void *)tbl;
    s_page_cap = (int)((s_region_end - tbl) / 4u);

    settings_derive();                                       /* "auto" colour follows the clock */
    build_metrics();
    s_n_pages = rd_paginate(s_text, s_text_len, &s_metrics, s_starts, s_page_cap);

    uint32_t off = 0, total = 0;
    if (load_progress(b->name, &off, &total)) s_cur = page_for_offset(off);
    else s_cur = 0;
    load_bookmarks();
    save_meta();
    cache_cover_at_open(b, rd);
    touch_recent(b->name);
    hb_fs_write(LAST_PATH, b->name, (uint32_t)s_len(b->name));

    notice("");
    lv_obj_add_flag(s_lib_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_rd_view, LV_OBJ_FLAG_HIDDEN);
    apply_theme();
    toolbar_show();
    render_page();
    busy_hide();
}

static void open_book(int idx)
{
    s_open_idx = idx;
    busy_show("Opening...");
    defer(do_open);
}

/* ---- reader events ---- */

/* ---- toolbar: hides itself while you read ---- */

static int s_tb_visible = 1;

static void toolbar_show(void)
{
    for (int i = 0; i < BTN_COUNT; i++) lv_obj_remove_flag(s_hbtn[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_prog, LV_OBJ_FLAG_HIDDEN);
    s_tb_visible = 1;
}

static void toolbar_hide(void)
{
    for (int i = 0; i < BTN_COUNT; i++) lv_obj_add_flag(s_hbtn[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_prog, LV_OBJ_FLAG_HIDDEN);
    s_tb_visible = 0;
}

/* Twice a second: tuck the toolbar away once you have stopped touching the screen. */
static void toolbar_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_set.hidebar || !s_tb_visible || !s_text) return;
    if (!lv_obj_has_flag(s_menu_view, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(s_set_view, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_display_get_inactive_time(NULL) > 3500u) toolbar_hide();
}

static void on_reader_tap(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active();
    if (!ind) return;
    lv_point_t p;
    lv_indev_get_point(ind, &p);
    if (p.y < PAGE_TOP) {                        /* the top strip: bring the toolbar back */
        if (!s_tb_visible) toolbar_show();
        return;
    }
    if (s_set.hidebar && s_tb_visible) toolbar_hide();   /* reading: get it out of the way */
    if (p.x < 80) go_to_page(s_cur - 1);
    else          go_to_page(s_cur + 1);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    save_position();
    book_free();
    s_open_name[0] = 0;
    hb_fs_remove(LAST_PATH);                    /* leaving a book on purpose: next launch shows the library */
    lib_alloc();
    show_library();
}

static void do_reformat(void)
{
    uint32_t off = s_n_pages ? s_starts[s_cur] : 0;
    build_metrics();
    s_n_pages = rd_paginate(s_text, s_text_len, &s_metrics, s_starts, s_page_cap);
    s_cur = page_for_offset(off);
    save_position();
    render_page();
    busy_hide();
}

static void on_font(lv_event_t *e)
{
    (void)e;
    if (s_n_pages == 0) return;
    s_set.font = (uint8_t)((s_set.font + 1) % 3);
    settings_derive();
    save_cfg();
    seg_refresh();
    busy_show("Formatting...");
    defer(do_reformat);
}

static void on_night(lv_event_t *e)
{
    (void)e;
    s_set.theme = s_night ? 0 : 1;               /* an explicit choice replaces "auto" */
    settings_derive();
    save_cfg();
    seg_refresh();
    apply_theme();
    s_img_cached = -1;                           /* transparent images were blended on the old colour */
    render_page();
}

static void on_bookmark(lv_event_t *e)
{
    (void)e;
    toggle_bookmark();
    refresh_bm_button();
}

/* ---- menu: position, chapters, bookmarks ---- */

static void menu_fill(void)
{
    int total = s_menu_tab_sel == 0 ? s_doc.nchap : s_n_bm;
    int pages = (total + MROWS - 1) / MROWS;
    if (pages < 1) pages = 1;
    if (s_menu_page >= pages) s_menu_page = pages - 1;
    if (s_menu_page < 0) s_menu_page = 0;
    int cur_ch = s_n_pages ? chapter_for_offset(s_starts[s_cur]) : -1;

    for (int r = 0; r < MROWS; r++) {
        int i = s_menu_page * MROWS + r;
        if (i >= total) { lv_obj_add_flag(s_mrow[r], LV_OBJ_FLAG_HIDDEN); continue; }
        uint32_t off = s_menu_tab_sel == 0 ? s_doc.chap[i].off : s_bm[i];
        char left[48], right[8];
        if (s_menu_tab_sel == 0) {
            int k = 0;
            for (; s_doc.chap[i].title[k] && k < 40; k++) left[k] = s_doc.chap[i].title[k];
            left[k] = 0;
        } else {
            int k = 0;
            for (uint32_t j = off; j < s_text_len && k < 36 && s_text[j]; j++) {
                char c = s_text[j];
                if (c == RD_MARK_IMG) { j += RD_MARK_LEN - 1; continue; }
                left[k++] = (c == '\n') ? ' ' : c;
            }
            left[k] = 0;
        }
        lv_snprintf(right, sizeof right, "%d%%", s_text_len ? (int)(off * 100u / s_text_len) : 0);
        lv_label_set_text(s_mrow_l[r], left);
        lv_label_set_text(s_mrow_r[r], right);
        bool hi = (s_menu_tab_sel == 0 && i == cur_ch);
        lv_obj_set_style_bg_color(s_mrow[r], lv_color_hex(hi ? hb_color_primary() : col_btn()), 0);
        lv_obj_set_style_text_color(s_mrow_l[r], lv_color_hex(hi ? hb_color_on_primary() : col_fg()), 0);
        lv_obj_set_style_text_color(s_mrow_r[r], lv_color_hex(hi ? hb_color_on_primary() : col_dim()), 0);
        lv_obj_remove_flag(s_mrow[r], LV_OBJ_FLAG_HIDDEN);
    }
    if (total == 0) {
        lv_label_set_text(s_menu_empty, s_menu_tab_sel == 0 ? "No chapters found in this book."
                                                              : "No bookmarks yet.\nTap the save button on a page.");
        lv_obj_remove_flag(s_menu_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_menu_empty, LV_OBJ_FLAG_HIDDEN);
    }
    char pg[16];
    lv_snprintf(pg, sizeof pg, "%d / %d", s_menu_page + 1, pages);
    lv_label_set_text(s_menu_pg, pg);
    for (int t = 0; t < 2; t++) {
        bool on = (t == s_menu_tab_sel);
        lv_obj_set_style_bg_color(s_menu_tab[t], lv_color_hex(on ? hb_color_primary() : col_btn()), 0);
        lv_obj_set_style_text_color(s_menu_tab_l[t], lv_color_hex(on ? hb_color_on_primary() : col_fg()), 0);
    }
}

static void menu_theme(void)
{
    lv_obj_set_style_bg_color(s_menu_view, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_text_color(s_menu_title, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_text_color(s_menu_cap, lv_color_hex(col_dim()), 0);
    lv_obj_set_style_text_color(s_menu_pct, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_text_color(s_menu_empty, lv_color_hex(col_dim()), 0);
    lv_obj_set_style_text_color(s_menu_pg, lv_color_hex(col_dim()), 0);
    lv_obj_set_style_bg_color(s_menu_close, lv_color_hex(col_btn()), 0);
    lv_obj_set_style_text_color(s_menu_close_l, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_bg_color(s_menu_gear, lv_color_hex(col_btn()), 0);
    lv_obj_set_style_text_color(s_menu_gear_l, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_bg_color(s_menu_prev, lv_color_hex(col_btn()), 0);
    lv_obj_set_style_bg_color(s_menu_next, lv_color_hex(col_btn()), 0);
    lv_obj_set_style_text_color(s_menu_prev_l, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_text_color(s_menu_next_l, lv_color_hex(col_fg()), 0);
}

static void on_menu_open(lv_event_t *e)
{
    (void)e;
    if (s_n_pages == 0) return;
    menu_theme();
    int pct = s_n_pages > 1 ? s_cur * 100 / (s_n_pages - 1) : 0;
    char b[12];
    lv_snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(s_menu_pct, b);
    lv_slider_set_value(s_menu_slider, pct, LV_ANIM_OFF);
    s_menu_page = 0;
    int cur = chapter_for_offset(s_starts[s_cur]);
    if (s_menu_tab_sel == 0 && cur >= 0) s_menu_page = cur / MROWS;
    menu_fill();
    lv_obj_remove_flag(s_menu_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_menu_view);
}

static void menu_close(void) { lv_obj_add_flag(s_menu_view, LV_OBJ_FLAG_HIDDEN); }
static void on_menu_close(lv_event_t *e) { (void)e; menu_close(); }

static void on_slider_changed(lv_event_t *e)
{
    char b[12];
    lv_snprintf(b, sizeof b, "%d%%", (int)lv_slider_get_value(lv_event_get_target(e)));
    lv_label_set_text(s_menu_pct, b);
}

static void on_slider_released(lv_event_t *e)
{
    int pct = (int)lv_slider_get_value(lv_event_get_target(e));
    if (s_n_pages > 0) go_to_page((pct * (s_n_pages - 1) + 50) / 100);
    menu_close();
}

static void on_menu_tab(lv_event_t *e)
{
    s_menu_tab_sel = (int)(uintptr_t)lv_event_get_user_data(e);
    s_menu_page = 0;
    menu_fill();
}

static void on_menu_page(lv_event_t *e)
{
    s_menu_page += (int)(intptr_t)lv_event_get_user_data(e);
    menu_fill();
}

static void on_menu_row(lv_event_t *e)
{
    int i = s_menu_page * MROWS + (int)(uintptr_t)lv_event_get_user_data(e);
    uint32_t off;
    if (s_menu_tab_sel == 0) { if (i >= s_doc.nchap) return; off = s_doc.chap[i].off; }
    else                     { if (i >= s_n_bm) return; off = s_bm[i]; }
    go_to_offset(off);
    menu_close();
}

/* ---- settings and about ---- */

#define READER_VERSION "2.2"

enum { SEG_FONT, SEG_THEME, SEG_SPACE, SEG_MARGIN, SEG_BAR, SEG_RESUME, SEG_COUNT };

static lv_obj_t *s_set_view, *s_set_title, *s_set_close, *s_set_close_l, *s_set_scroll;
static lv_obj_t *s_seg[SEG_COUNT][3], *s_seg_l[SEG_COUNT][3], *s_seg_cap[SEG_COUNT];
static lv_obj_t *s_about_lbl;
static uint8_t   s_seg_n[SEG_COUNT];

static int seg_value(int id)
{
    switch (id) {
    case SEG_FONT:   return s_set.font;
    case SEG_THEME:  return s_set.theme;
    case SEG_SPACE:  return s_set.spacing;
    case SEG_MARGIN: return s_set.margin;
    case SEG_BAR:    return s_set.hidebar ? 0 : 1;
    default:         return s_set.resume ? 0 : 1;
    }
}

static void seg_refresh(void)
{
    if (!s_set_view) return;
    for (int id = 0; id < SEG_COUNT; id++) {
        int cur = seg_value(id);
        for (int v = 0; v < s_seg_n[id]; v++) {
            bool on = (v == cur);
            lv_obj_set_style_bg_color(s_seg[id][v], lv_color_hex(on ? hb_color_primary() : col_btn()), 0);
            lv_obj_set_style_text_color(s_seg_l[id][v], lv_color_hex(on ? hb_color_on_primary() : col_fg()), 0);
        }
    }
}

static void settings_theme(void)
{
    lv_obj_set_style_bg_color(s_set_view, lv_color_hex(col_bg()), 0);
    lv_obj_set_style_text_color(s_set_title, lv_color_hex(col_fg()), 0);
    lv_obj_set_style_bg_color(s_set_close, lv_color_hex(col_btn()), 0);
    lv_obj_set_style_text_color(s_set_close_l, lv_color_hex(col_fg()), 0);
    for (int id = 0; id < SEG_COUNT; id++) lv_obj_set_style_text_color(s_seg_cap[id], lv_color_hex(col_dim()), 0);
    lv_obj_set_style_text_color(s_about_lbl, lv_color_hex(col_fg()), 0);
    seg_refresh();
}

/* One tap on a segmented control. */
static void settings_change(int id, int v)
{
    int relayout = 0, retheme = 0;
    switch (id) {
    case SEG_FONT:   if (s_set.font != v)    { s_set.font = (uint8_t)v;    relayout = 1; } break;
    case SEG_THEME:  if (s_set.theme != v)   { s_set.theme = (uint8_t)v;   retheme = 1; } break;
    case SEG_SPACE:  if (s_set.spacing != v) { s_set.spacing = (uint8_t)v; relayout = 1; } break;
    case SEG_MARGIN: if (s_set.margin != v)  { s_set.margin = (uint8_t)v;  relayout = 1; } break;
    case SEG_BAR:    s_set.hidebar = (v == 0); if (!s_set.hidebar && s_text) toolbar_show(); break;
    default:         s_set.resume = (v == 0); break;
    }
    settings_derive();
    save_cfg();
    seg_refresh();
    if (retheme) {
        apply_theme();
        menu_theme();
        settings_theme();
        s_img_cached = -1;
        if (s_text) render_page();
    }
    if (relayout && s_text) {
        busy_show("Formatting...");
        defer(do_reformat);
    }
}

static void on_seg(lv_event_t *e)
{
    int code = (int)(uintptr_t)lv_event_get_user_data(e);
    settings_change(code >> 4, code & 15);
}

static void about_refresh(void)
{
    static char t[1700];
    int k = 0;
#define AP(...) do { if (k < (int)sizeof t - 1) k += lv_snprintf(t + k, sizeof t - (size_t)k, __VA_ARGS__); } while (0)
    AP("READER %s\nBuilt %s\n", READER_VERSION, __DATE__);
    if (s_text && s_open_idx >= 0 && s_open_idx < s_n_books) {
        AP("\nTHIS BOOK\n%s\n", s_doc.title[0] ? s_doc.title : s_open_name);
        if (s_doc.author[0]) AP("by %s\n", s_doc.author);
        AP("Format: %s, %u KB\n", kind_label(s_books[s_open_idx].kind), (unsigned)(s_file_size / 1024u));
        AP("Text: %u thousand characters\n", (unsigned)(s_text_len / 1000u));
        AP("Pages: %d at this font\n", s_n_pages);
        AP("Chapters: %d\nPictures: %d\nBookmarks: %d\n", s_doc.nchap, s_doc.nimg, s_n_bm);
        AP("Read: %d%%\n", s_n_pages > 1 ? s_cur * 100 / (s_n_pages - 1) : 0);
        if (s_truncated) AP("Cut short: out of memory\n");
    }
    AP("\nLIBRARY\n%d book%s\n", s_n_books, s_n_books == 1 ? "" : "s");
    unsigned mv = (unsigned)hb_battery_voltage_mv(), lvl = (unsigned)hb_battery_level_0_to_15();
    AP("\nDEVICE\nMemory free: %u KB\nLargest block: %u KB\nA book can use: %u KB\n",
       (unsigned)(hb_os_heap_free() / 1024u), (unsigned)(hb_os_heap_largest() / 1024u), (unsigned)(heap_avail() / 1024u));
    AP("Battery: %u%% (%u.%02u V)%s\n", lvl * 100u / 15u, mv / 1000u, (mv % 1000u) / 10u,
       hb_battery_is_charging() ? ", charging" : "");
    AP("\nFORMATS\nEPUB, FB2, HTML, Markdown and plain text. Pictures: JPEG and PNG.\n");
    AP("Not supported: DRM books, progressive JPEG, GIF, WebP.\n");
    if (s_problem[0]) AP("\nLAST PROBLEM\n%s\n", s_problem);
    AP("\nCREDITS\nNanoApps by nfzerox\nLVGL (MIT licence)\nTJpgDec by ChaN\nMontserrat font (SIL OFL)\n");
#undef AP
    lv_label_set_text(s_about_lbl, t);
}

static void settings_open(lv_event_t *e)
{
    (void)e;
    settings_derive();
    settings_theme();
    about_refresh();
    lv_obj_scroll_to_y(s_set_scroll, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(s_set_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_set_view);
}

static void settings_close(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_set_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- library events ---- */

static void on_tile(lv_event_t *e)
{
    if (lv_tick_get() - s_swipe_ms < 450u) return;           /* the tail of a swipe, not a tap */
    int slot = (int)(uintptr_t)lv_event_get_user_data(e);
    open_book(s_slot_book[slot]);
}

static void lib_step(int d)
{
    s_lib_page += d;
    library_refresh();
}

static void on_lib_page(lv_event_t *e) { lib_step((int)(intptr_t)lv_event_get_user_data(e)); }

static void on_lib_gesture(lv_event_t *e)
{
    (void)e;
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_LEFT) { s_swipe_ms = lv_tick_get(); lib_step(1); }
    else if (d == LV_DIR_RIGHT) { s_swipe_ms = lv_tick_get(); lib_step(-1); }
}

/* ---- construction ---- */

static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *parent, int x, int y, int w, int h, const char *txt,
                           lv_event_cb_t cb, void *ud, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    if (lbl_out) *lbl_out = l;
    return b;
}

static lv_obj_t *mk_view(void)
{
    lv_obj_t *v = lv_obj_create(s_scr);
    lv_obj_set_size(v, SCR_W, SCR_H);
    lv_obj_set_pos(v, 0, 0);
    lv_obj_set_style_radius(v, 0, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    return v;
}

static void build_library(void)
{
    s_lib_view = mk_view();
    lv_obj_set_style_bg_color(s_lib_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_add_event_cb(s_lib_view, on_lib_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *hdr = mk_label(s_lib_view, "Library", &lv_font_montserrat_24, hb_color_primary());
    lv_obj_set_pos(hdr, 12, 8);
    s_lib_count = mk_label(s_lib_view, "", &lv_font_montserrat_14, hb_color_text_dim());
    lv_obj_set_width(s_lib_count, 100);
    lv_obj_set_style_text_align(s_lib_count, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_lib_count, SCR_W - 12 - 100, 16);
    s_lib_notice = mk_label(s_lib_view, "", &lv_font_montserrat_14, hb_color_danger());
    lv_label_set_long_mode(s_lib_notice, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_lib_notice, 130, 18);
    lv_obj_set_pos(s_lib_notice, SCR_W - 12 - 130, 16);
    lv_obj_set_style_text_align(s_lib_notice, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(s_lib_notice, LV_OBJ_FLAG_HIDDEN);

    s_tile_area = lv_obj_create(s_lib_view);
    lv_obj_set_size(s_tile_area, 224, 340);
    lv_obj_set_pos(s_tile_area, 8, 46);
    lv_obj_set_style_bg_opa(s_tile_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tile_area, 0, 0);
    lv_obj_set_style_pad_all(s_tile_area, 0, 0);
    lv_obj_remove_flag(s_tile_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_tile_area, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int i = 0; i < TILES; i++) {
        lv_obj_t *t = lv_obj_create(s_tile_area);
        s_tile[i] = t;
        lv_obj_set_size(t, 108, 164);
        lv_obj_set_pos(t, (i % 2) * 116, (i / 2) * 172);
        lv_obj_set_style_radius(t, 10, 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_set_style_shadow_width(t, 0, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_clip_corner(t, true, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(t, on_tile, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        s_tile_cv[i] = lv_canvas_create(t);                 /* the cover, when there is one */
        lv_obj_remove_flag(s_tile_cv[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_tile_cv[i], LV_OBJ_FLAG_HIDDEN);

        s_tile_title[i] = mk_label(t, "", &lv_font_montserrat_14, 0xFFFFFF);
        lv_label_set_long_mode(s_tile_title[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s_tile_title[i], 92, 120);
        lv_obj_set_pos(s_tile_title[i], 8, 8);

        s_tile_band[i] = lv_obj_create(t);                  /* dark strip: status stays readable on any cover */
        lv_obj_set_size(s_tile_band[i], 108, 28);
        lv_obj_set_pos(s_tile_band[i], 0, 136);
        lv_obj_set_style_radius(s_tile_band[i], 0, 0);
        lv_obj_set_style_border_width(s_tile_band[i], 0, 0);
        lv_obj_set_style_pad_all(s_tile_band[i], 0, 0);
        lv_obj_set_style_shadow_width(s_tile_band[i], 0, 0);
        lv_obj_set_style_bg_color(s_tile_band[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_tile_band[i], LV_OPA_50, 0);
        lv_obj_remove_flag(s_tile_band[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_tile_band[i], LV_OBJ_FLAG_SCROLLABLE);

        s_tile_stat[i] = mk_label(t, "", &lv_font_montserrat_14, 0xFFFFFF);
        lv_obj_set_pos(s_tile_stat[i], 8, 138);
        s_tile_kind[i] = mk_label(t, "", &lv_font_montserrat_14, 0xE0E0E0);
        lv_obj_set_width(s_tile_kind[i], 92);
        lv_obj_set_style_text_align(s_tile_kind[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_tile_kind[i], 8, 138);
        s_tile_bar[i] = lv_bar_create(t);
        lv_obj_remove_flag(s_tile_bar[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(s_tile_bar[i], 92, 5);
        lv_obj_set_pos(s_tile_bar[i], 8, 156);
        lv_bar_set_range(s_tile_bar[i], 0, 100);
        lv_obj_set_style_bg_color(s_tile_bar[i], lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_tile_bar[i], LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_tile_bar[i], lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(s_tile_bar[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_tile_bar[i], 3, LV_PART_MAIN);
        lv_obj_set_style_radius(s_tile_bar[i], 3, LV_PART_INDICATOR);
    }

    s_lib_prev = mk_button(s_lib_view, 8, 394, 48, 30, LV_SYMBOL_LEFT, on_lib_page, (void *)(intptr_t)-1, NULL);
    s_lib_next = mk_button(s_lib_view, SCR_W - 8 - 48, 394, 48, 30, LV_SYMBOL_RIGHT, on_lib_page, (void *)(intptr_t)1, NULL);
    s_lib_pg = mk_label(s_lib_view, "", &lv_font_montserrat_14, hb_color_text_dim());
    lv_obj_set_width(s_lib_pg, 52);
    lv_obj_set_style_text_align(s_lib_pg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lib_pg, 60, 401);
    s_lib_gear = mk_button(s_lib_view, 116, 394, 56, 30, LV_SYMBOL_SETTINGS, settings_open, NULL, NULL);

    s_lib_empty = mk_label(s_lib_view, "No books yet.\nCopy .epub, .fb2, .html,\n.md or .txt files into\n/Data/Books/", &lv_font_montserrat_14, hb_color_text_dim());
    lv_obj_set_width(s_lib_empty, 220);
    lv_obj_set_style_text_align(s_lib_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lib_empty, 10, 170);
    lv_obj_add_flag(s_lib_empty, LV_OBJ_FLAG_HIDDEN);
}

static void build_reader(void)
{
    s_rd_view = mk_view();
    lv_obj_add_flag(s_rd_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rd_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rd_view, on_reader_tap, LV_EVENT_PRESSED, NULL);

    static const char *const glyph[BTN_COUNT] = { LV_SYMBOL_LEFT, "Aa", LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_SAVE, LV_SYMBOL_LIST };
    lv_event_cb_t cbs[BTN_COUNT] = { on_back, on_font, on_night, on_bookmark, on_menu_open };
    for (int i = 0; i < BTN_COUNT; i++)
        s_hbtn[i] = mk_button(s_rd_view, 4 + i * 46, 5, 42, 30, glyph[i], cbs[i], NULL, &s_hlbl[i]);

    s_page_lbl = lv_label_create(s_rd_view);
    lv_label_set_text(s_page_lbl, "");
    lv_label_set_long_mode(s_page_lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(s_page_lbl, PAGE_W, PAGE_H);
    lv_obj_set_pos(s_page_lbl, PAGE_X, PAGE_TOP);
    lv_obj_set_style_text_font(s_page_lbl, cur_font(), 0);
    lv_obj_set_style_text_line_space(s_page_lbl, 0, 0);
    lv_obj_set_style_text_letter_space(s_page_lbl, 0, 0);

    s_canvas = lv_canvas_create(s_rd_view);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);

    s_prog = lv_bar_create(s_rd_view);                    /* shows where you are while the toolbar is tucked away */
    lv_obj_remove_flag(s_prog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_prog, SCR_W, 3);
    lv_obj_set_pos(s_prog, 0, 0);
    lv_bar_set_range(s_prog, 0, 1000);
    lv_obj_set_style_bg_opa(s_prog, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_prog, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_prog, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_prog, 0, LV_PART_INDICATOR);
    lv_obj_add_flag(s_prog, LV_OBJ_FLAG_HIDDEN);
    lv_timer_create(toolbar_tick, 500, NULL);

    s_foot_ch = mk_label(s_rd_view, "", &lv_font_montserrat_14, col_dim());
    lv_label_set_long_mode(s_foot_ch, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(s_foot_ch, 150, 18);
    lv_obj_set_pos(s_foot_ch, 10, SCR_H - 22);
    s_foot_pg = mk_label(s_rd_view, "", &lv_font_montserrat_14, col_dim());
    lv_obj_set_width(s_foot_pg, 70);
    lv_obj_set_style_text_align(s_foot_pg, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_foot_pg, SCR_W - 10 - 70, SCR_H - 22);
    apply_theme();
}

static void build_menu(void)
{
    s_menu_view = mk_view();
    lv_obj_add_flag(s_menu_view, LV_OBJ_FLAG_HIDDEN);

    s_menu_title = mk_label(s_menu_view, "Menu", &lv_font_montserrat_24, col_fg());
    lv_obj_set_pos(s_menu_title, 12, 8);
    s_menu_close = mk_button(s_menu_view, SCR_W - 8 - 42, 5, 42, 30, LV_SYMBOL_CLOSE, on_menu_close, NULL, &s_menu_close_l);
    s_menu_gear = mk_button(s_menu_view, SCR_W - 8 - 42 - 46, 5, 42, 30, LV_SYMBOL_SETTINGS, settings_open, NULL, &s_menu_gear_l);

    s_menu_cap = mk_label(s_menu_view, "Position", &lv_font_montserrat_14, col_dim());
    lv_obj_set_pos(s_menu_cap, 14, 50);
    s_menu_pct = mk_label(s_menu_view, "", &lv_font_montserrat_14, col_fg());
    lv_obj_set_width(s_menu_pct, 50);
    lv_obj_set_style_text_align(s_menu_pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_menu_pct, SCR_W - 14 - 50, 50);
    s_menu_slider = lv_slider_create(s_menu_view);
    lv_obj_set_size(s_menu_slider, 200, 12);
    lv_obj_set_pos(s_menu_slider, 20, 82);
    lv_slider_set_range(s_menu_slider, 0, 100);
    lv_obj_add_event_cb(s_menu_slider, on_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_menu_slider, on_slider_released, LV_EVENT_RELEASED, NULL);

    s_menu_tab[0] = mk_button(s_menu_view, 8, 108, 108, 30, "Chapters", on_menu_tab, (void *)(uintptr_t)0, &s_menu_tab_l[0]);
    s_menu_tab[1] = mk_button(s_menu_view, 124, 108, 108, 30, "Bookmarks", on_menu_tab, (void *)(uintptr_t)1, &s_menu_tab_l[1]);

    for (int r = 0; r < MROWS; r++) {
        s_mrow[r] = mk_button(s_menu_view, 8, 146 + r * 46, 224, 42, "", on_menu_row, (void *)(uintptr_t)r, NULL);
        lv_obj_set_style_pad_all(s_mrow[r], 0, 0);      /* the theme pads buttons ~13 px a side: the two labels collided */
        lv_obj_t *old = lv_obj_get_child(s_mrow[r], 0);          /* mk_button's placeholder label */
        if (old) lv_obj_delete(old);
        s_mrow_l[r] = mk_label(s_mrow[r], "", &lv_font_montserrat_14, col_fg());
        lv_label_set_long_mode(s_mrow_l[r], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(s_mrow_l[r], 164, 18);
        lv_obj_align(s_mrow_l[r], LV_ALIGN_LEFT_MID, 6, 0);
        s_mrow_r[r] = mk_label(s_mrow[r], "", &lv_font_montserrat_14, col_dim());
        lv_obj_set_width(s_mrow_r[r], 44);
        lv_obj_set_style_text_align(s_mrow_r[r], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_mrow_r[r], LV_ALIGN_RIGHT_MID, -6, 0);
    }
    s_menu_empty = mk_label(s_menu_view, "", &lv_font_montserrat_14, col_dim());
    lv_obj_set_width(s_menu_empty, 210);
    lv_obj_set_pos(s_menu_empty, 15, 160);
    lv_obj_add_flag(s_menu_empty, LV_OBJ_FLAG_HIDDEN);

    s_menu_prev = mk_button(s_menu_view, 8, 384, 56, 30, LV_SYMBOL_LEFT, on_menu_page, (void *)(intptr_t)-1, &s_menu_prev_l);
    s_menu_next = mk_button(s_menu_view, SCR_W - 8 - 56, 384, 56, 30, LV_SYMBOL_RIGHT, on_menu_page, (void *)(intptr_t)1, &s_menu_next_l);
    s_menu_pg = mk_label(s_menu_view, "", &lv_font_montserrat_14, col_dim());
    lv_obj_set_width(s_menu_pg, 80);
    lv_obj_set_style_text_align(s_menu_pg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_menu_pg, (SCR_W - 80) / 2, 391);
}

/* One caption and a row of 2-3 buttons; the button picked is highlighted by seg_refresh(). */
static void set_row(int id, int y, const char *cap, int n, const char *l0, const char *l1, const char *l2)
{
    s_seg_n[id] = (uint8_t)n;
    s_seg_cap[id] = mk_label(s_set_scroll, cap, &lv_font_montserrat_14, col_dim());
    lv_obj_set_pos(s_seg_cap[id], 12, y);
    const char *lab[3] = { l0, l1, l2 };
    int bw = (216 - (n - 1) * 4) / n;
    for (int v = 0; v < n; v++)
        s_seg[id][v] = mk_button(s_set_scroll, 12 + v * (bw + 4), y + 20, bw, 30, lab[v], on_seg,
                                 (void *)(uintptr_t)(id * 16 + v), &s_seg_l[id][v]);
}

static void build_settings(void)
{
    s_set_view = mk_view();
    lv_obj_add_flag(s_set_view, LV_OBJ_FLAG_HIDDEN);
    s_set_title = mk_label(s_set_view, "Settings", &lv_font_montserrat_24, col_fg());
    lv_obj_set_pos(s_set_title, 12, 8);
    s_set_close = mk_button(s_set_view, SCR_W - 8 - 42, 5, 42, 30, LV_SYMBOL_CLOSE, settings_close, NULL, &s_set_close_l);

    s_set_scroll = lv_obj_create(s_set_view);             /* everything below the title scrolls */
    lv_obj_set_size(s_set_scroll, SCR_W, SCR_H - 44);
    lv_obj_set_pos(s_set_scroll, 0, 44);
    lv_obj_set_style_bg_opa(s_set_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_set_scroll, 0, 0);
    lv_obj_set_style_radius(s_set_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_set_scroll, 0, 0);
    lv_obj_set_style_pad_bottom(s_set_scroll, 24, 0);
    lv_obj_set_scroll_dir(s_set_scroll, LV_DIR_VER);

    int y = 4;
    set_row(SEG_FONT,   y, "Font size",               3, "Small", "Medium", "Large");        y += 58;
    set_row(SEG_THEME,  y, "Page colour",             3, "Day", "Night", "Auto");            y += 58;
    set_row(SEG_SPACE,  y, "Line spacing",            3, "Tight", "Normal", "Loose");        y += 58;
    set_row(SEG_MARGIN, y, "Margins",                 3, "Narrow", "Normal", "Wide");        y += 58;
    set_row(SEG_BAR,    y, "Toolbar while reading",   2, "Auto-hide", "Always shown", NULL); y += 58;
    set_row(SEG_RESUME, y, "Open last book at start", 2, "Yes", "No", NULL);                 y += 58;
    s_about_lbl = mk_label(s_set_scroll, "", &lv_font_montserrat_14, col_fg());
    lv_label_set_long_mode(s_about_lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_about_lbl, 216);
    lv_obj_set_pos(s_about_lbl, 12, y + 4);
    seg_refresh();
}

static void build_busy(void)
{
    s_busy = lv_obj_create(s_scr);
    lv_obj_set_size(s_busy, SCR_W, SCR_H);
    lv_obj_set_pos(s_busy, 0, 0);
    lv_obj_set_style_radius(s_busy, 0, 0);
    lv_obj_set_style_border_width(s_busy, 0, 0);
    lv_obj_set_style_bg_color(s_busy, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_busy, LV_OPA_90, 0);
    lv_obj_remove_flag(s_busy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_busy, LV_OBJ_FLAG_CLICKABLE);              /* swallow taps while busy */
    lv_obj_add_flag(s_busy, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *sp = lv_spinner_create(s_busy);
    lv_obj_set_size(sp, 56, 56);
    lv_obj_set_pos(sp, (SCR_W - 56) / 2, 160);
    lv_obj_set_style_arc_color(sp, lv_color_hex(hb_color_primary()), LV_PART_INDICATOR);
    s_busy_lbl = mk_label(s_busy, "", &lv_font_montserrat_16, hb_color_text());
    lv_obj_set_width(s_busy_lbl, 200);
    lv_obj_set_style_text_align(s_busy_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_busy_lbl, 20, 232);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_cfg();
    scan_books();
    load_recent();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_library();
    build_reader();
    build_menu();
    build_settings();
    build_busy();
    lib_alloc();
    show_library();

    /* Reopen where we left off. */
    char last[MAX_FILENAME];
    uint32_t n = hb_fs_read(LAST_PATH, last, sizeof last - 1);
    if (n > 0 && s_set.resume) {
        last[n] = 0;
        for (int i = 0; i < s_n_books; i++)
            if (strcmp(s_books[i].name, last) == 0) { open_book(i); break; }
    }
}
