/* rd_markup.c — see rd_markup.h */
#include <string.h>
#include "rd_markup.h"
#include "rd_tables.h"

enum { S_TEXT = 0, S_TAG, S_COMMENT, S_ENT };
enum { ENC_UTF8 = 0, ENC_1252, ENC_1251 };

static const char BLOCK_HTML[] =
    ",p,div,br,li,tr,h1,h2,h3,h4,h5,h6,blockquote,section,article,pre,hr,table,ul,ol,dl,dt,dd,"
    "figure,figcaption,body,header,footer,address,aside,main,caption,";
static const char SKIP_HTML[] = ",script,style,head,nav,noscript,template,";
static const char BLOCK_FB2[] =
    ",p,v,subtitle,text-author,section,epigraph,poem,stanza,cite,annotation,body,table,tr,";

/* ---- tiny helpers ---- */

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_alnum(int c) { return is_alpha(c) || (c >= '0' && c <= '9'); }
static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int is_namech(int c) { return is_alnum(c) || c == '-' || c == '_' || c == ':' || c == '.'; }
static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static void tidy_chapters(rd_doc_t *d, uint32_t end_off);

static int in_list(const char *nm, const char *list)
{
    int nl = (int)strlen(nm);
    if (nl == 0) return 0;
    for (const char *s = list; *s; s++) {
        if (*s != ',') continue;
        const char *a = s + 1;
        int k = 0;
        while (k < nl && a[k] == nm[k]) k++;
        if (k == nl && a[k] == ',') return 1;
    }
    return 0;
}

void rd_doc_init(rd_doc_t *d, char *buf, uint32_t cap)
{
    d->buf = buf;
    d->cap = cap;
    d->len = 0;
    d->overflow = 0;
    d->nchap = 0;
    d->ipool_len = 0;
    d->nimg = 0;
    d->nbin = 0;
    d->title[0] = 0;
    d->author[0] = 0;
    d->slack = 0;
    d->no_images = 0;
    if (cap) buf[0] = 0;
}

/* Drop headings that have almost no text before the next one: contents lists, title stacks. */
static void tidy_chapters(rd_doc_t *d, uint32_t end_off)
{
    int w = 0;
    for (int i = 0; i < d->nchap; i++) {
        uint32_t next = (i + 1 < d->nchap) ? d->chap[i + 1].off : end_off;
        if (next >= d->chap[i].off && next - d->chap[i].off < 100u) continue;
        if (w != i) d->chap[w] = d->chap[i];
        w++;
    }
    d->nchap = w;
}

void rd_doc_finish(rd_doc_t *d)
{
    tidy_chapters(d, d->len);
    if (d->cap) d->buf[d->len < d->cap ? d->len : d->cap - 1] = 0;
}

/* ---- output ---- */

static void put_raw(rd_parser_t *p, char c)
{
    rd_doc_t *d = p->doc;
    if (d->len + 1 < d->cap) d->buf[d->len++] = c;
    else d->overflow = 1;
    p->last = c;
}

static void par_break(rd_parser_t *p)
{
    if (p->doc->len && p->last != '\n') put_raw(p, '\n');
    p->pend_space = 0;
}

static void cap_begin(rd_parser_t *p, char *dst, int max, int append)
{
    p->cap_dst = dst;
    p->cap_max = max;
    if (append) {
        p->cap_len = (int)strlen(dst);
        p->cap_sp = p->cap_len > 0;
    } else {
        dst[0] = 0;
        p->cap_len = 0;
        p->cap_sp = 0;
    }
}

static void cap_end(rd_parser_t *p) { p->cap_dst = 0; }

static void cap_add(rd_parser_t *p, int c)
{
    if (!p->cap_dst || p->cap_pause) return;
    if (c == ' ') { if (p->cap_len > 0) p->cap_sp = 1; return; }
    if (p->cap_sp && p->cap_len < p->cap_max) p->cap_dst[p->cap_len++] = ' ';
    p->cap_sp = 0;
    if (p->cap_len < p->cap_max) p->cap_dst[p->cap_len++] = (char)c;
    p->cap_dst[p->cap_len] = 0;
}

static void emit_ascii(rd_parser_t *p, int c)
{
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        cap_add(p, ' ');
        if (!p->skip && !p->in_desc && p->doc->len && p->last != '\n') p->pend_space = 1;
        return;
    }
    if (c < 32 || c > 126) return;
    cap_add(p, c);
    if (p->skip || p->in_desc) return;
    if (p->pend_space) { put_raw(p, ' '); p->pend_space = 0; }
    if (p->heading && c >= 'a' && c <= 'z') c -= 32;
    put_raw(p, (char)c);
}

static void emit_str(rd_parser_t *p, const char *s)
{
    while (*s) emit_ascii(p, (unsigned char)*s++);
}

static void emit_cp(rd_parser_t *p, uint32_t cp)
{
    if (cp < 128) { emit_ascii(p, (int)cp); return; }
    for (int r = 0; r < RD_FOLD_NRANGES; r++) {
        if (cp >= rd_fold_range[r][0] && cp < rd_fold_range[r][1]) {
            const char *s = rd_fold_str + rd_fold_off[rd_fold_range[r][2] + (cp - rd_fold_range[r][0])];
            while (*s) emit_ascii(p, (unsigned char)*s++);
            return;
        }
    }
    /* anything else cannot be drawn: drop it */
}

static void text_byte(rd_parser_t *p, uint8_t b)
{
    if (p->enc == ENC_UTF8) {
        if (p->unneed) {
            if ((b & 0xC0) == 0x80) {
                p->ucp = (p->ucp << 6) | (b & 0x3Fu);
                if (--p->unneed == 0) emit_cp(p, p->ucp);
                return;
            }
            p->unneed = 0;                      /* broken sequence: treat b afresh */
        }
        if (b < 0x80) emit_cp(p, b);
        else if ((b & 0xE0) == 0xC0) { p->ucp = b & 0x1Fu; p->unneed = 1; }
        else if ((b & 0xF0) == 0xE0) { p->ucp = b & 0x0Fu; p->unneed = 2; }
        else if ((b & 0xF8) == 0xF0) { p->ucp = b & 0x07u; p->unneed = 3; }
    } else if (p->enc == ENC_1252) {
        emit_cp(p, b < 0x80 ? b : (b < 0xA0 ? rd_cp1252_hi[b - 0x80] : b));
    } else {
        emit_cp(p, b < 0x80 ? b : rd_cp1251_hi[b - 0x80]);
    }
}

/* ---- paths and attributes ---- */

int rd_resolve_path(const char *base, const char *src, int src_len, char *out, int cap)
{
    char tmp[256];
    int tl = 0;
    for (int i = 0; i < src_len; i++)
        if (src[i] == '#' || src[i] == '?') { src_len = i; break; }
    if (src_len > 0 && src[0] == '/') { src++; src_len--; }
    else { for (int i = 0; base[i] && tl < 200; i++) tmp[tl++] = base[i]; }
    for (int i = 0; i < src_len && tl < 255; i++) {
        char c = src[i];
        if (c == '%' && i + 2 < src_len) {
            int h = 0, ok = 1;
            for (int k = 1; k <= 2; k++) {
                char x = src[i + k];
                int v = (x >= '0' && x <= '9') ? x - '0' : (x >= 'a' && x <= 'f') ? x - 'a' + 10
                      : (x >= 'A' && x <= 'F') ? x - 'A' + 10 : -1;
                if (v < 0) { ok = 0; break; }
                h = h * 16 + v;
            }
            if (ok) { tmp[tl++] = (char)h; i += 2; continue; }
        }
        tmp[tl++] = c;
    }
    int ol = 0, i = 0;
    while (i < tl) {
        int s = i;
        while (i < tl && tmp[i] != '/') i++;
        int seg = i - s;
        if (i < tl) i++;
        if (seg == 0 || (seg == 1 && tmp[s] == '.')) continue;
        if (seg == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            if (ol > 0) { ol--; while (ol > 0 && out[ol - 1] != '/') ol--; }
            continue;
        }
        if (ol + seg + 1 >= cap) return 0;
        memcpy(out + ol, tmp + s, (size_t)seg);
        ol += seg;
        out[ol++] = '/';
    }
    if (ol > 0) ol--;
    out[ol] = 0;
    return ol;
}

static int name_matches(const char *a, int alen, const char *want, int suffix_ok)
{
    int wl = (int)strlen(want);
    const char *s = a;
    int n = alen;
    if (suffix_ok && n > wl && a[n - wl - 1] == ':') { s = a + n - wl; n = wl; }
    if (n != wl) return 0;
    for (int i = 0; i < n; i++)
        if (lowc(s[i]) != want[i]) return 0;
    return 1;
}

/* Locate attribute `want` in tag text `t` (length n). On success sets the value's
 * offset inside `t` and its length, and returns 1. */
int rd_attr_find(const char *t, int n, const char *want, int suffix_ok, int *voff, int *vlen)
{
    int i = 0;
    while (i < n && !is_ws(t[i]) && t[i] != '/') i++;
    while (i < n) {
        while (i < n && (is_ws(t[i]) || t[i] == '/')) i++;
        int ns = i;
        while (i < n && !is_ws(t[i]) && t[i] != '=' && t[i] != '/') i++;
        int nl = i - ns;
        while (i < n && is_ws(t[i])) i++;
        int vs = 0, ve = 0, has = 0;
        if (i < n && t[i] == '=') {
            i++;
            while (i < n && is_ws(t[i])) i++;
            if (i < n && (t[i] == '"' || t[i] == '\'')) {
                char q = t[i++];
                vs = i;
                while (i < n && t[i] != q) i++;
                ve = i;
                if (i < n) i++;
            } else {
                vs = i;
                while (i < n && !is_ws(t[i])) i++;
                ve = i;
            }
            has = 1;
        }
        if (nl > 0 && has && name_matches(t + ns, nl, want, suffix_ok)) {
            *voff = vs;
            *vlen = ve - vs;
            return 1;
        }
    }
    return 0;
}

/* Copying form of rd_attr_find. Returns the length or -1. */
int rd_attr_get(const char *t, int n, const char *want, int suffix_ok, char *out, int cap)
{
    int vo, vl;
    if (!rd_attr_find(t, n, want, suffix_ok, &vo, &vl)) return -1;
    if (vl > cap - 1) vl = cap - 1;
    memcpy(out, t + vo, (size_t)vl);
    out[vl] = 0;
    return vl;
}

/* ---- images, chapters ---- */

static int pool_add(rd_doc_t *d, const char *s, int len)
{
    if (d->ipool_len + (uint32_t)len + 1 > RD_IPOOL) return -1;
    uint32_t off = d->ipool_len;
    memcpy(d->ipool + off, s, (size_t)len);
    d->ipool[off + (uint32_t)len] = 0;
    d->ipool_len += (uint32_t)len + 1;
    return (int)off;
}

static void add_image(rd_parser_t *p, const char *ref, int len)
{
    rd_doc_t *d = p->doc;
    int idx = -1;
    for (int i = 0; i < d->nimg; i++)
        if (streq(d->ipool + d->img_off[i], ref)) { idx = i; break; }
    if (idx < 0) {
        if (d->nimg >= RD_MAX_IMAGES) return;
        int off = pool_add(d, ref, len);
        if (off < 0) return;
        d->img_off[d->nimg] = (uint32_t)off;
        idx = d->nimg++;
    }
    par_break(p);
    put_raw(p, RD_MARK_IMG);
    put_raw(p, (char)('0' + (idx >> 6)));
    put_raw(p, (char)('0' + (idx & 63)));
    put_raw(p, '\n');
    p->pend_space = 0;
}

static int dim_px(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; if (v > 100000) return 0; }
    return *s == '%' ? 0 : v;
}

static void do_image(rd_parser_t *p, const char *t, int n, int is_img)
{
    if (p->doc->no_images) return;
    char src[200];
    int l = rd_attr_get(t, n, is_img ? "src" : "href", !is_img, src, sizeof src);
    if (l <= 0) return;
    if (p->fmt == RD_FMT_FB2) {
        if (src[0] == '#') add_image(p, src, l);
        return;
    }
    if ((l > 5 && src[4] == ':') || (l > 6 && src[5] == ':') || (l > 7 && src[6] == ':')) return;  /* data:, http:, https: */
    if (is_img) {
        /* A one or two letter alt text is a drop cap: print the letter, do not make a page. */
        char alt[8];
        int al = rd_attr_get(t, n, "alt", 0, alt, sizeof alt);
        if (al >= 1 && al <= 2) {
            for (int i = 0; i < al; i++) text_byte(p, (uint8_t)alt[i]);
            return;
        }
        /* Tiny declared sizes are icons and ornaments. */
        char dim[12];
        int w = 0, h = 0;
        if (rd_attr_get(t, n, "width", 0, dim, sizeof dim) > 0) w = dim_px(dim);
        if (rd_attr_get(t, n, "height", 0, dim, sizeof dim) > 0) h = dim_px(dim);
        if ((w > 0 && w <= 64 && (h == 0 || h <= 64)) || (h > 0 && h <= 64 && w == 0)) return;
    }
    char path[200];
    int pl = rd_resolve_path(p->base, src, l, path, sizeof path);
    if (pl > 0) add_image(p, path, pl);
}

static void heading_begin(rd_parser_t *p, int level)
{
    par_break(p);
    p->heading++;
    if (level <= 2 && !p->ch_pending && p->doc->nchap >= RD_MAX_CHAPTERS)
        tidy_chapters(p->doc, p->doc->len);          /* table full: drop contents-list entries first */
    if (level <= 2 && !p->ch_pending && p->doc->nchap < RD_MAX_CHAPTERS) {
        p->ch_pending = 1;
        p->ch_off = p->doc->len;
        cap_begin(p, p->chtitle, RD_TITLE_LEN - 1, 0);
    }
}

static void chapter_commit(rd_parser_t *p)
{
    if (!p->ch_pending) return;
    cap_end(p);
    p->ch_pending = 0;
    rd_doc_t *d = p->doc;
    if (d->nchap >= RD_MAX_CHAPTERS) tidy_chapters(d, p->ch_off);     /* make room if we can */
    if (p->chtitle[0] && d->nchap < RD_MAX_CHAPTERS) {
        d->chap[d->nchap].off = p->ch_off;
        memcpy(d->chap[d->nchap].title, p->chtitle, RD_TITLE_LEN);
        d->nchap++;
    }
}

static void heading_end(rd_parser_t *p)
{
    if (p->heading) p->heading--;
    par_break(p);
    if (p->heading == 0) chapter_commit(p);
}

/* ---- character set sniffing ---- */

static void charset_from(rd_parser_t *p, const char *t, int n)
{
    static const char *keys[2] = { "charset=", "encoding=" };
    for (int k = 0; k < 2; k++) {
        int kl = (int)strlen(keys[k]);
        for (int i = 0; i + kl < n; i++) {
            int j = 0;
            while (j < kl && lowc(t[i + j]) == keys[k][j]) j++;
            if (j != kl) continue;
            int v = i + kl;
            if (v < n && (t[v] == '"' || t[v] == '\'')) v++;
            char w[16];
            int wl = 0;
            while (v < n && wl < 15 && (is_alnum(t[v]) || t[v] == '-' || t[v] == '_')) w[wl++] = (char)lowc(t[v++]);
            w[wl] = 0;
            if (streq(w, "windows-1251") || streq(w, "cp1251")) {
                p->enc = ENC_1251;
            } else if (streq(w, "windows-1252") || streq(w, "iso-8859-1") || streq(w, "latin1")) {
                p->enc = ENC_1252;
            } else if (streq(w, "utf-8") || streq(w, "utf8")) {
                p->enc = ENC_UTF8;
            }
            return;
        }
    }
}

/* ---- tags ---- */

static void end_skip(rd_parser_t *p, const char *nm)
{
    if (streq(nm, "binary") && p->in_bin) {
        rd_doc_t *d = p->doc;
        if (d->nbin < RD_MAX_BINS && p->bin_id_off != 0xFFFFFFFFu) {
            d->bin[d->nbin].id_off = p->bin_id_off;
            d->bin[d->nbin].data_off = p->bin_start;
            d->bin[d->nbin].data_len = p->tag_pos > p->bin_start ? p->tag_pos - p->bin_start : 0;
            d->nbin++;
        }
        p->in_bin = 0;
    }
    if (streq(nm, "title")) cap_end(p);
    p->skip = 0;
}

static void start_skip(rd_parser_t *p, const char *nm)
{
    p->skip = 1;
    size_t l = strlen(nm);
    if (l > 15) l = 15;
    memcpy(p->skip_name, nm, l);
    p->skip_name[l] = 0;
}

static int has_caption_class(const char *t, int n)
{
    char cls[80];
    int l = rd_attr_get(t, n, "class", 0, cls, sizeof cls);
    for (int i = 0; i + 7 <= l; i++) {
        int k = 0;
        while (k < 7 && lowc(cls[i + k]) == "caption"[k]) k++;
        if (k == 7) return 1;
    }
    return 0;
}

static void html_tag(rd_parser_t *p, const char *nm, int closing, int selfclose, const char *t, int n)
{
    if (p->cap_pause) {
        if (closing && streq(nm, p->cap_pause_name)) p->cap_pause = 0;
    } else if (p->cap_dst && !closing && !selfclose && has_caption_class(t, n)) {
        p->cap_pause = 1;
        size_t cl = strlen(nm);
        if (cl > 15) cl = 15;
        memcpy(p->cap_pause_name, nm, cl);
        p->cap_pause_name[cl] = 0;
    }
    if (!closing && in_list(nm, SKIP_HTML)) {
        if (!selfclose) start_skip(p, nm);
        return;
    }
    if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && nm[2] == 0) {
        if (!closing) heading_begin(p, nm[1] - '0');
        else heading_end(p);
        return;
    }
    if (streq(nm, "img") || streq(nm, "image")) {
        if (!closing) do_image(p, t, n, nm[0] == 'i' && nm[3] == 0);
        return;
    }
    if (streq(nm, "br")) { par_break(p); return; }
    if (streq(nm, "hr")) {
        if (!closing) { par_break(p); emit_str(p, "* * *"); par_break(p); }
        return;
    }
    if (streq(nm, "li") && !closing) { par_break(p); emit_str(p, "- "); return; }
    if (streq(nm, "meta") && !closing) { charset_from(p, t, n); return; }
    if (in_list(nm, BLOCK_HTML)) par_break(p);
}

static void fb2_tag(rd_parser_t *p, const char *nm, int closing, int selfclose, const char *t, int n)
{
    rd_doc_t *d = p->doc;
    if (streq(nm, "description")) { p->in_desc = closing ? 0 : 1; return; }
    if (streq(nm, "book-title")) {
        if (!closing) { if (!d->title[0]) cap_begin(p, d->title, (int)sizeof d->title - 1, 0); }
        else cap_end(p);
        return;
    }
    if (streq(nm, "author")) {
        p->cap_is_author = (!closing && !d->author[0]) ? 1 : 0;
        return;
    }
    if (streq(nm, "first-name") || streq(nm, "middle-name") || streq(nm, "last-name")) {
        if (!closing) { if (p->cap_is_author) cap_begin(p, d->author, (int)sizeof d->author - 1, 1); }
        else cap_end(p);
        return;
    }
    if (!closing && streq(nm, "binary")) {
        if (selfclose) return;
        char id[64];
        int l = rd_attr_get(t, n, "id", 0, id, sizeof id);
        p->bin_id_off = 0xFFFFFFFFu;
        if (l > 0) {
            char key[66];
            key[0] = '#';
            memcpy(key + 1, id, (size_t)l + 1);
            int off = pool_add(d, key, l + 1);
            if (off >= 0) p->bin_id_off = (uint32_t)off;
        }
        p->in_bin = 1;
        p->bin_start = p->pos;
        start_skip(p, "binary");
        return;
    }
    if (!closing && streq(nm, "stylesheet")) { if (!selfclose) start_skip(p, nm); return; }
    if (!closing && streq(nm, "body")) {
        char nmv[16];
        if (rd_attr_get(t, n, "name", 0, nmv, sizeof nmv) > 0 && streq(nmv, "notes") && !selfclose) {
            start_skip(p, "body");
            return;
        }
    }
    if (streq(nm, "image")) { if (!closing) do_image(p, t, n, 0); return; }
    if (streq(nm, "title")) {
        if (!closing) heading_begin(p, 2);
        else heading_end(p);
        return;
    }
    if (streq(nm, "empty-line")) { par_break(p); return; }
    if (in_list(nm, BLOCK_FB2)) par_break(p);
}

static void tag_end(rd_parser_t *p)
{
    char *t = p->tag;
    int n = p->taglen;
    t[n] = 0;
    if (n == 0) return;
    if (t[0] == '?') { charset_from(p, t, n); return; }
    if (t[0] == '!') return;
    int closing = (t[0] == '/');
    int i = closing, nl = 0;
    char name[16];
    while (i < n && nl < 15 && is_namech((unsigned char)t[i])) name[nl++] = (char)lowc(t[i++]);
    name[nl] = 0;
    const char *nm = name;
    for (int k = 0; k < nl; k++) if (name[k] == ':') nm = name + k + 1;
    int selfclose = (t[n - 1] == '/');

    if (p->skip) {
        if (!closing && p->fmt == RD_FMT_HTML && streq(nm, "meta")) charset_from(p, t, n);   /* lives in <head> */
        if (!closing && streq(nm, "title") && !p->doc->title[0] && p->fmt == RD_FMT_HTML && !p->cap_dst)
            cap_begin(p, p->doc->title, (int)sizeof p->doc->title - 1, 0);
        else if (closing && streq(nm, "title") && p->fmt == RD_FMT_HTML)
            cap_end(p);
        if (closing && streq(nm, p->skip_name)) end_skip(p, nm);
        return;
    }
    if (!closing && p->fmt == RD_FMT_HTML && streq(nm, "title") && !selfclose) {
        start_skip(p, "title");
        if (!p->doc->title[0]) cap_begin(p, p->doc->title, (int)sizeof p->doc->title - 1, 0);
        return;
    }
    if (p->fmt == RD_FMT_FB2) fb2_tag(p, nm, closing, selfclose, t, n);
    else html_tag(p, nm, closing, selfclose, t, n);
}

/* ---- entities ---- */

static void ent_done(rd_parser_t *p)
{
    p->ent[p->entlen] = 0;
    uint32_t cp = 0;
    if (p->ent[0] == '#') {
        int hex = (p->ent[1] == 'x' || p->ent[1] == 'X');
        for (int i = 1 + hex; i < p->entlen; i++) {
            int c = p->ent[i];
            int v = (c >= '0' && c <= '9') ? c - '0' : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (v < 0) return;
            cp = cp * (hex ? 16u : 10u) + (uint32_t)v;
            if (cp > 0x10FFFFu) return;
        }
        if (cp) emit_cp(p, cp);
        return;
    }
    const char *s = rd_ent_names;
    for (int i = 0; i < RD_ENT_COUNT; i++) {
        if (streq(s, p->ent)) { emit_cp(p, rd_ent_cp[i]); return; }
        s += strlen(s) + 1;
    }
}

/* ---- the state machine ---- */

static void feed_byte(rd_parser_t *p, uint8_t b)
{
    for (;;) {
        switch (p->st) {
        case S_TEXT:
            if (b == '<') {
                p->st = S_TAG; p->taglen = 0; p->quote = 0;
                p->tag_pos = p->pos - 1;
                return;
            }
            if (p->skip && !p->cap_dst) return;
            if (b == '&') { p->st = S_ENT; p->entlen = 0; return; }
            text_byte(p, b);
            return;
        case S_TAG:
            if (p->taglen == 0 && !(b == '/' || b == '!' || b == '?' || is_alpha(b))) {
                p->st = S_TEXT;
                text_byte(p, '<');
                continue;                       /* reprocess b as text */
            }
            if (p->quote) { if (b == p->quote) p->quote = 0; }
            else if (b == '"' || b == '\'') p->quote = b;
            else if (b == '>') { p->st = S_TEXT; tag_end(p); return; }
            if (p->taglen < RD_TAGMAX) p->tag[p->taglen++] = (char)b;
            if (p->taglen == 3 && p->tag[0] == '!' && p->tag[1] == '-' && p->tag[2] == '-') {
                p->st = S_COMMENT; p->quote = '-'; p->cmt = 0;
            } else if (p->taglen == 8 && memcmp(p->tag, "![CDATA[", 8) == 0) {
                p->st = S_COMMENT; p->quote = ']'; p->cmt = 0;
            }
            return;
        case S_COMMENT:
            if (b == p->quote) { if (p->cmt < 2) p->cmt++; }
            else if (b == '>' && p->cmt >= 2) p->st = S_TEXT;
            else p->cmt = 0;
            return;
        default: /* S_ENT */
            if (b == ';') { p->st = S_TEXT; ent_done(p); return; }
            if ((is_alnum(b) || b == '#') && p->entlen < 10) { p->ent[p->entlen++] = (char)b; return; }
            p->st = S_TEXT;
            text_byte(p, '&');
            for (int i = 0; i < p->entlen; i++) text_byte(p, (uint8_t)p->ent[i]);
            continue;                           /* reprocess b as text */
        }
    }
}

void rd_parser_begin(rd_parser_t *p, rd_doc_t *d, rd_fmt_t fmt, const char *base)
{
    memset(p, 0, sizeof *p);
    p->doc = d;
    p->fmt = fmt;
    p->enc = ENC_UTF8;
    p->last = '\n';
    p->bin_id_off = 0xFFFFFFFFu;
    size_t l = strlen(base);
    if (l > sizeof p->base - 1) l = sizeof p->base - 1;
    memcpy(p->base, base, l);
    p->base[l] = 0;
    par_break(p);
}

void rd_parser_feed(rd_parser_t *p, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (p->doc->overflow) return;
        p->pos++;
        if (p->doc->slack) p->doc->cap = p->doc->slack + p->pos;
        feed_byte(p, data[i]);
    }
}

void rd_parser_end(rd_parser_t *p)
{
    if (p->st == S_ENT) { p->st = S_TEXT; }
    p->heading = 0;
    par_break(p);
    chapter_commit(p);
    cap_end(p);
}

/* Fold a short UTF-8 / entity-encoded string (an OPF title, say) to ASCII. */
void rd_capture_text(const uint8_t *s, uint32_t n, char *out, int cap)
{
    rd_parser_t p;
    memset(&p, 0, sizeof p);
    p.doc = 0;                          /* never dereferenced: nothing is emitted while skipping */
    p.skip = 1;
    p.enc = ENC_UTF8;
    p.last = '\n';
    cap_begin(&p, out, cap - 1, 0);
    for (uint32_t i = 0; i < n; i++) feed_byte(&p, s[i]);
    if (p.st == S_ENT) { p.st = S_TEXT; }
    cap_end(&p);
}

/* ---- plain text ---- */

/* Length of the heading line at s ("CHAPTER IV.", "Part 2", "LETTER 3 - To Mrs. S"), or 0. */
static uint32_t txt_heading_len(const uint8_t *s, uint32_t n)
{
    static const char *const kw[] = { "chapter", "book", "part", "letter", "section", "act",
                                      "stave", "canto", "volume" };
    uint32_t e = 0;
    while (e < n && e < 140 && s[e] != '\n') e++;
    if (e >= 140 || e == 0) return 0;
    uint32_t end = e;
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\r' || s[end - 1] == '\t')) end--;
    if (end == 0 || end > 130) return 0;
    for (unsigned k = 0; k < sizeof kw / sizeof kw[0]; k++) {
        uint32_t l = (uint32_t)strlen(kw[k]), i = 0;
        while (i < l && i < end && lowc(s[i]) == kw[k][i]) i++;
        if (i != l || i >= end || s[i] != ' ') continue;
        i++;
        uint32_t t = i;
        while (t < end && ((s[t] >= '0' && s[t] <= '9') || strchr("IVXLCDMivxlcdm", s[t]) != 0)) t++;
        if (t == i) return 0;
        if (t == end || s[t] == '.' || s[t] == ':' || s[t] == ',' || s[t] == ';' || s[t] == '-' || s[t] == ' ')
            return end;
        return 0;
    }
    /* Roman numerals with no keyword: "I. A SCANDAL IN BOHEMIA" (all caps) or a bare "XII". */
    uint32_t r = 0;
    if (end > 60) return 0;
    while (r < end && strchr("IVXLCDM", s[r]) != 0) r++;
    if (r == 0 || r > 7) return 0;
    if (r + 2 < end && s[r] == '.' && s[r + 1] == ' ') {
        int caps = 1, letters = 0;
        for (uint32_t k = r + 2; k < end; k++) {
            if (s[k] >= 'a' && s[k] <= 'z') { caps = 0; break; }
            if (s[k] >= 'A' && s[k] <= 'Z') letters++;
        }
        return (caps && letters >= 3) ? end : 0;
    }
    if (r == end || (r + 1 == end && s[r] == '.')) {
        /* A bare numeral: alone on its line, or followed by one short title line, then a blank. */
        uint32_t q = e;
        if (q < n && s[q] == '\n') q++;
        uint32_t ls = q;
        while (ls < n && (s[ls] == ' ' || s[ls] == '\t' || s[ls] == '\r')) ls++;
        if (ls >= n || s[ls] == '\n') return end;
        uint32_t le = ls;
        while (le < n && s[le] != '\n' && le - ls < 80) le++;
        if (le - ls > 60 || (le < n && s[le] != '\n')) return 0;
        uint32_t te = le;
        while (te > ls && (s[te - 1] == ' ' || s[te - 1] == '\r' || s[te - 1] == '\t')) te--;
        uint32_t q2 = le;
        if (q2 < n && s[q2] == '\n') q2++;
        while (q2 < n && (s[q2] == ' ' || s[q2] == '\r' || s[q2] == '\t')) q2++;
        return (q2 >= n || s[q2] == '\n') ? te : 0;
    }
    return 0;
}

void rd_txt_convert(rd_doc_t *d, const uint8_t *s, uint32_t n)
{
    rd_parser_t p;
    rd_parser_begin(&p, d, RD_FMT_HTML, "");
    int nl = 2, first = 1;
    uint32_t i = 0;
    while (i < n && !d->overflow) {
        if (d->slack) d->cap = d->slack + i;
        uint8_t c = s[i];
        if (c == '\r') { i++; continue; }
        if (c == '\n') {
            nl = first ? nl + 1 : 1;                   /* blank line adds one, a text line resets to one */
            first = 1;
            emit_ascii(&p, ' ');
            i++;
            continue;
        }
        if (c == ' ' || c == '\t') { text_byte(&p, c); i++; continue; }
        if (first) {                                   /* first visible character of a line */
            first = 0;
            if (nl >= 2) {                             /* only after a blank line */
                uint32_t hl = txt_heading_len(s + i, n - i);
                if (hl) {
                    heading_begin(&p, 2);
                    for (uint32_t k = 0; k < hl; k++) text_byte(&p, s[i + k]);
                    heading_end(&p);
                    i += hl;
                    nl = 0;
                    continue;
                }
                par_break(&p);
            }
            nl = 0;
        }
        text_byte(&p, c);
        i++;
    }
    if (d->slack) d->cap = d->slack + n;
    rd_parser_end(&p);
    rd_doc_finish(d);
}

/* ---- Markdown ---- */

static int md_is_hr(const uint8_t *s, uint32_t n)
{
    int c = 0, ch = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == '\t') continue;
        if (s[i] != '-' && s[i] != '*' && s[i] != '_') return 0;
        if (ch && s[i] != ch) return 0;
        ch = s[i];
        c++;
    }
    return c >= 3;
}

static void md_inline(rd_parser_t *p, const uint8_t *s, uint32_t a, uint32_t b)
{
    uint32_t i = a;
    while (i < b) {
        uint8_t c = s[i];
        if (c == '\\' && i + 1 < b) { text_byte(p, s[i + 1]); i += 2; continue; }
        if (c == '`' || c == '*') { i++; continue; }
        if (c == '_') {
            int prev_w = i > a && is_alnum(s[i - 1]);
            int next_w = i + 1 < b && is_alnum(s[i + 1]);
            if (!(prev_w && next_w)) { i++; continue; }
        }
        int img = (c == '!' && i + 1 < b && s[i + 1] == '[');
        if (c == '[' || img) {
            uint32_t j = i + (img ? 2u : 1u), close = j;
            while (close < b && s[close] != ']') close++;
            if (close < b && close + 1 < b && s[close + 1] == '(') {
                uint32_t k = close + 2;
                while (k < b && s[k] != ')') k++;
                if (k < b) {
                    for (uint32_t q = j; q < close; q++) text_byte(p, s[q]);
                    i = k + 1;
                    continue;
                }
            }
        }
        if (c == '<' && i + 1 < b && (is_alpha(s[i + 1]) || s[i + 1] == '/')) {
            uint32_t k = i + 1;
            while (k < b && s[k] != '>') k++;
            if (k < b) { i = k + 1; continue; }
        }
        text_byte(p, c);
        i++;
    }
}

void rd_md_convert(rd_doc_t *d, const uint8_t *s, uint32_t n)
{
    rd_parser_t p;
    rd_parser_begin(&p, d, RD_FMT_HTML, "");
    int in_code = 0;
    uint32_t i = 0;
    while (i < n && !d->overflow) {
        if (d->slack) d->cap = d->slack + i;
        uint32_t e = i;
        while (e < n && s[e] != '\n') e++;
        uint32_t a = i, b = e;
        if (b > a && s[b - 1] == '\r') b--;
        i = e + 1;
        uint32_t k = a;
        while (k < b && (s[k] == ' ' || s[k] == '\t')) k++;
        if (b - k >= 3 && ((s[k] == '`' && s[k + 1] == '`' && s[k + 2] == '`') ||
                           (s[k] == '~' && s[k + 1] == '~' && s[k + 2] == '~'))) {
            in_code = !in_code;
            par_break(&p);
            continue;
        }
        if (k == b) { par_break(&p); continue; }
        if (!in_code) {
            uint32_t h = 0;
            while (k + h < b && s[k + h] == '#') h++;
            if (h >= 1 && h <= 6 && k + h < b && s[k + h] == ' ') {
                uint32_t hb = b;
                while (hb > k + h && (s[hb - 1] == '#' || s[hb - 1] == ' ')) hb--;
                heading_begin(&p, (int)h);
                md_inline(&p, s, k + h + 1, hb);
                heading_end(&p);
                continue;
            }
            if (md_is_hr(s + k, b - k)) {
                par_break(&p);
                emit_str(&p, "* * *");
                par_break(&p);
                continue;
            }
            int all_eq = 1;
            for (uint32_t q = k; q < b; q++) if (s[q] != '=') { all_eq = 0; break; }
            if (all_eq) continue;               /* setext underline */
            while (k < b && s[k] == '>') { k++; while (k < b && s[k] == ' ') k++; }
            if (k + 1 < b && (s[k] == '-' || s[k] == '*' || s[k] == '+') && s[k + 1] == ' ') {
                par_break(&p);
                emit_str(&p, "- ");
                k += 2;
            } else {
                uint32_t q = k;
                while (q < b && s[q] >= '0' && s[q] <= '9') q++;
                if (q > k && q + 1 < b && (s[q] == '.' || s[q] == ')') && s[q + 1] == ' ') {
                    par_break(&p);
                    for (uint32_t z = k; z < q; z++) emit_ascii(&p, s[z]);
                    emit_str(&p, ". ");
                    k = q + 2;
                }
            }
        } else {
            par_break(&p);
        }
        md_inline(&p, s, k, b);
        emit_ascii(&p, ' ');
    }
    if (d->slack) d->cap = d->slack + n;
    rd_parser_end(&p);
    rd_doc_finish(d);
}
