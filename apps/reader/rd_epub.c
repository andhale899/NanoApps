/* rd_epub.c — see rd_epub.h */
#include <string.h>
#include "rd_epub.h"

struct item {
    uint32_t id_off, href_off;      /* offsets into the OPF buffer */
    uint16_t id_len, href_len;
    uint8_t nav, pad0;
    uint16_t pad1;
};

struct sref {
    uint32_t id_off;
    uint16_t id_len;
    uint8_t linear, pad;
};

struct membuf { uint8_t *p; uint32_t cap, len; int over; };

static int mem_sink(void *c, const uint8_t *d, uint32_t n)
{
    struct membuf *m = (struct membuf *)c;
    if (n > m->cap - m->len) { m->over = 1; return 1; }
    memcpy(m->p + m->len, d, n);
    m->len += n;
    return 0;
}

/* Inflate one archive entry into dst. Returns 0 on success. */
static int read_entry(const rd_zip_t *z, const char *name, uint8_t *window,
                      uint8_t *dst, uint32_t cap, uint32_t *outlen)
{
    rd_zip_ent_t e;
    if (rd_zip_find(z, name, (uint32_t)strlen(name), &e)) return -1;
    struct membuf m = { dst, cap, 0, 0 };
    int r = rd_zip_read(z, &e, window, mem_sink, &m);
    if (r || m.over) return -1;
    *outlen = m.len;
    return 0;
}

static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Next tag in an XML buffer; skips comments, PIs and declarations. */
static int next_tag(const char *b, uint32_t n, uint32_t *pos, const char **t, uint32_t *tl)
{
    uint32_t i = *pos;
    for (;;) {
        while (i < n && b[i] != '<') i++;
        if (i >= n) return 0;
        i++;
        if (i + 2 < n && b[i] == '!' && b[i + 1] == '-' && b[i + 2] == '-') {
            i += 3;
            while (i + 2 < n && !(b[i] == '-' && b[i + 1] == '-' && b[i + 2] == '>')) i++;
            i += 3;
            continue;
        }
        if (i < n && (b[i] == '?' || b[i] == '!')) {
            while (i < n && b[i] != '>') i++;
            i++;
            continue;
        }
        uint32_t s = i;
        char q = 0;
        while (i < n) {
            char c = b[i];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '>') break;
            i++;
        }
        if (i >= n) return 0;
        *t = b + s;
        *tl = i - s;
        *pos = i + 1;
        return 1;
    }
}

/* Lower-case local name (namespace prefix removed). Returns 1 for a closing tag. */
static int tag_name(const char *t, uint32_t tl, char *name)
{
    uint32_t i = 0;
    int closing = 0, nl = 0, last = 0;
    if (tl && t[0] == '/') { closing = 1; i = 1; }
    char tmp[24];
    while (i < tl && nl < 23) {
        char c = t[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == ':' || c == '.')) break;
        tmp[nl] = (char)lowc(c);
        if (c == ':') last = nl + 1;
        nl++;
        i++;
    }
    tmp[nl] = 0;
    memcpy(name, tmp + last, (size_t)(nl - last) + 1);
    return closing;
}

static int has_token(const char *s, int n, const char *tok)
{
    int tl = (int)strlen(tok);
    for (int i = 0; i + tl <= n; i++) {
        if (memcmp(s + i, tok, (size_t)tl) != 0) continue;
        if ((i == 0 || s[i - 1] == ' ') && (i + tl == n || s[i + tl] == ' ')) return 1;
    }
    return 0;
}

/* Reject books whose content is encrypted. Font obfuscation alone is fine. */
static int check_drm(const rd_zip_t *z, uint8_t *window, uint8_t *scratch, uint32_t cap)
{
    rd_zip_ent_t e;
    if (rd_zip_find(z, "META-INF/rights.xml", 19, &e) == 0) return RD_EPUB_ERR_DRM;
    uint32_t len;
    if (read_entry(z, "META-INF/encryption.xml", window, scratch, cap, &len)) return 0;
    const char *s = (const char *)scratch;
    for (uint32_t i = 0; i + 10 < len; i++) {
        if (memcmp(s + i, "Algorithm=", 10) != 0) continue;
        uint32_t v = i + 10;
        if (v >= len || (s[v] != '"' && s[v] != '\'')) continue;
        char q = s[v++];
        uint32_t e2 = v;
        while (e2 < len && s[e2] != q) e2++;
        uint32_t vl = e2 - v;
        int font = (vl == 34 && memcmp(s + v, "http://www.idpf.org/2008/embedding", 34) == 0) ||
                   (vl == 29 && memcmp(s + v, "http://ns.adobe.com/pdf/enc#RC", 29) == 0);
        if (!font) return RD_EPUB_ERR_DRM;
    }
    return 0;
}

static int feed_sink(void *c, const uint8_t *d, uint32_t n)
{
    rd_parser_t *p = (rd_parser_t *)c;
    rd_parser_feed(p, d, n);
    return p->doc->overflow;
}

static int feed_entry(const rd_zip_t *z, rd_zip_ent_t *e, const char *path, int plen,
                      uint8_t *window, rd_doc_t *doc)
{
    char base[160];
    int last = -1;
    for (int i = 0; i < plen; i++) if (path[i] == '/') last = i;
    int bl = last + 1;
    if (bl > (int)sizeof base - 1) bl = 0;
    memcpy(base, path, (size_t)bl);
    base[bl] = 0;
    rd_parser_t p;
    rd_parser_begin(&p, doc, RD_FMT_HTML, base);
    int r = rd_zip_read(z, e, window, feed_sink, &p);
    rd_parser_end(&p);
    return r;
}

int rd_epub_load(rd_zip_t *z, rd_doc_t *doc, const uint8_t *file, uint32_t size,
                 const rd_epub_mem_t *mem)
{
    if (rd_zip_open(z, file, size)) return RD_EPUB_ERR_ZIP;
    if (mem->scratch_cap < 65536u) return RD_EPUB_ERR_EMPTY;

    uint32_t opf_cap = (mem->scratch_cap / 4u) * 3u;
    opf_cap &= ~15u;
    uint8_t *opf = mem->scratch;
    uint32_t tail = mem->scratch_cap - opf_cap;
    struct item *items = (struct item *)(void *)(mem->scratch + opf_cap);
    uint32_t items_cap = (tail / 4u * 3u) / (uint32_t)sizeof(struct item);
    struct sref *spine = (struct sref *)(void *)(mem->scratch + opf_cap + items_cap * sizeof(struct item));
    uint32_t spine_cap = (mem->scratch_cap - opf_cap - items_cap * (uint32_t)sizeof(struct item)) / (uint32_t)sizeof(struct sref);

    int drm = check_drm(z, mem->window, opf, opf_cap);
    if (drm) return drm;

    /* container.xml names the OPF */
    char opf_path[200];
    opf_path[0] = 0;
    uint32_t len = 0;
    if (read_entry(z, "META-INF/container.xml", mem->window, opf, opf_cap, &len) == 0) {
        uint32_t pos = 0, tl;
        const char *t;
        while (next_tag((const char *)opf, len, &pos, &t, &tl)) {
            char nm[24];
            if (tag_name(t, tl, nm) || strcmp(nm, "rootfile") != 0) continue;
            if (rd_attr_get(t, (int)tl, "full-path", 0, opf_path, (int)sizeof opf_path) > 0) break;
        }
    }

    uint32_t n_items = 0, n_spine = 0;
    char opf_dir[160];
    opf_dir[0] = 0;
    uint32_t spine_docs_start[RD_MAX_CHAPTERS];
    uint32_t n_starts = 0;

    if (opf_path[0] && read_entry(z, opf_path, mem->window, opf, opf_cap, &len) == 0) {
        int last = -1;
        for (int i = 0; opf_path[i]; i++) if (opf_path[i] == '/') last = i;
        if (last >= 0 && last < (int)sizeof opf_dir - 1) {
            memcpy(opf_dir, opf_path, (size_t)last + 1);
            opf_dir[last + 1] = 0;
        }
        const char *ob = (const char *)opf;
        uint32_t pos = 0, tl;
        const char *t;
        while (next_tag(ob, len, &pos, &t, &tl)) {
            char nm[24];
            if (tag_name(t, tl, nm)) continue;
            int vo, vl;
            if (strcmp(nm, "item") == 0 && n_items < items_cap) {
                struct item *it = &items[n_items];
                memset(it, 0, sizeof *it);
                if (!rd_attr_find(t, (int)tl, "id", 0, &vo, &vl) || vl <= 0 || vl > 255) continue;
                it->id_off = (uint32_t)(t - ob) + (uint32_t)vo;
                it->id_len = (uint16_t)vl;
                if (!rd_attr_find(t, (int)tl, "href", 0, &vo, &vl) || vl <= 0 || vl > 400) continue;
                it->href_off = (uint32_t)(t - ob) + (uint32_t)vo;
                it->href_len = (uint16_t)vl;
                if (rd_attr_find(t, (int)tl, "properties", 0, &vo, &vl))
                    it->nav = (uint8_t)has_token(t + vo, vl, "nav");
                n_items++;
            } else if (strcmp(nm, "itemref") == 0 && n_spine < spine_cap) {
                if (!rd_attr_find(t, (int)tl, "idref", 0, &vo, &vl) || vl <= 0 || vl > 255) continue;
                struct sref *sr = &spine[n_spine];
                sr->id_off = (uint32_t)(t - ob) + (uint32_t)vo;
                sr->id_len = (uint16_t)vl;
                sr->linear = 1;
                sr->pad = 0;
                if (rd_attr_find(t, (int)tl, "linear", 0, &vo, &vl) && vl == 2 && memcmp(t + vo, "no", 2) == 0)
                    sr->linear = 0;
                n_spine++;
            } else if ((strcmp(nm, "title") == 0 && !doc->title[0]) ||
                       (strcmp(nm, "creator") == 0 && !doc->author[0])) {
                uint32_t e = pos;
                while (e < len && ob[e] != '<') e++;
                rd_capture_text((const uint8_t *)ob + pos, e - pos,
                                nm[0] == 't' ? doc->title : doc->author,
                                nm[0] == 't' ? (int)sizeof doc->title : (int)sizeof doc->author);
            }
        }
    }

    /* Read the spine in order. Without a usable OPF, fall back to every html file. */
    int fed = 0;
    if (n_spine) {
        for (uint32_t s = 0; s < n_spine && !doc->overflow; s++) {
            if (!spine[s].linear) continue;
            const char *id = (const char *)opf + spine[s].id_off;
            struct item *it = 0;
            for (uint32_t k = 0; k < n_items; k++)
                if (items[k].id_len == spine[s].id_len && memcmp((const char *)opf + items[k].id_off, id, spine[s].id_len) == 0) {
                    it = &items[k];
                    break;
                }
            if (!it || it->nav) continue;
            char path[200];
            int pl = rd_resolve_path(opf_dir, (const char *)opf + it->href_off, it->href_len, path, sizeof path);
            if (pl <= 0) continue;
            rd_zip_ent_t e;
            if (rd_zip_find(z, path, (uint32_t)pl, &e)) continue;
            if (n_starts < RD_MAX_CHAPTERS) spine_docs_start[n_starts++] = doc->len;
            feed_entry(z, &e, path, pl, mem->window, doc);
            fed++;
        }
    } else {
        uint32_t pos = 0, idx = 0;
        rd_zip_ent_t e;
        while (rd_zip_next(z, &pos, &idx, &e) == 0 && !doc->overflow) {
            int l = e.nlen;
            if (l < 5 || l > 190) continue;
            const char *nmx = (const char *)e.name;
            int html = (l > 6 && lowc(nmx[l - 1]) == 'l' && lowc(nmx[l - 2]) == 'm' && lowc(nmx[l - 3]) == 't' && lowc(nmx[l - 4]) == 'h' && nmx[l - 5] == '.') ||
                       (l > 5 && lowc(nmx[l - 1]) == 'm' && lowc(nmx[l - 2]) == 't' && lowc(nmx[l - 3]) == 'h' && nmx[l - 4] == '.') ||
                       (l > 7 && lowc(nmx[l - 1]) == 'l' && lowc(nmx[l - 2]) == 'm' && lowc(nmx[l - 3]) == 't' && lowc(nmx[l - 4]) == 'h' && lowc(nmx[l - 5]) == 'x' && nmx[l - 6] == '.');
            if (!html) continue;
            char path[200];
            memcpy(path, nmx, (size_t)l);
            path[l] = 0;
            if (n_starts < RD_MAX_CHAPTERS) spine_docs_start[n_starts++] = doc->len;
            feed_entry(z, &e, path, l, mem->window, doc);
            fed++;
        }
    }

    /* A book with fewer than two headings gets one chapter per spine file. */
    if (doc->nchap < 2 && n_starts > 1) {
        doc->nchap = 0;
        for (uint32_t i = 0; i < n_starts && doc->nchap < RD_MAX_CHAPTERS; i++) {
            rd_chapter_t *c = &doc->chap[doc->nchap++];
            c->off = spine_docs_start[i];
            char tmp[12];
            int nl = 0;
            uint32_t v = i + 1;
            while (v) { tmp[nl++] = (char)('0' + v % 10); v /= 10; }
            memcpy(c->title, "Part ", 5);
            int k = 5;
            while (nl && k < RD_TITLE_LEN - 1) c->title[k++] = tmp[--nl];
            c->title[k] = 0;
        }
    }

    rd_doc_finish(doc);
    if (!fed || (doc->len < 40 && doc->nimg == 0)) return RD_EPUB_ERR_EMPTY;
    return 0;
}

/* ---- cover lookup (for library thumbnails) ---- */

static int contains_ci(const char *s, int n, const char *needle)
{
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= n; i++) {
        int k = 0;
        while (k < nl && lowc(s[i + k]) == needle[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

/* Which archive entry is the book's cover image? Preference order:
 *   1. manifest item with properties="cover-image"            (EPUB 3)
 *   2. the item named by <meta name="cover" content="id">      (EPUB 2)
 *   3. an image item whose id or href contains "cover"
 * Returns the path length (0 if none) and writes the archive path to `out`. */
int rd_epub_cover(const rd_zip_t *z, const rd_epub_mem_t *mem, char *out, int cap)
{
    if (mem->scratch_cap < 65536u) return 0;
    uint32_t opf_cap = (mem->scratch_cap / 4u) * 3u;
    opf_cap &= ~15u;
    uint8_t *opf = mem->scratch;
    uint32_t len = 0;
    char opf_path[200];
    opf_path[0] = 0;
    if (read_entry(z, "META-INF/container.xml", mem->window, opf, opf_cap, &len)) return 0;
    {
        uint32_t pos = 0, tl;
        const char *t;
        while (next_tag((const char *)opf, len, &pos, &t, &tl)) {
            char nm[24];
            if (tag_name(t, tl, nm) || strcmp(nm, "rootfile") != 0) continue;
            if (rd_attr_get(t, (int)tl, "full-path", 0, opf_path, (int)sizeof opf_path) > 0) break;
        }
    }
    if (!opf_path[0] || read_entry(z, opf_path, mem->window, opf, opf_cap, &len)) return 0;
    char opf_dir[160];
    opf_dir[0] = 0;
    int last = -1;
    for (int i = 0; opf_path[i]; i++) if (opf_path[i] == '/') last = i;
    if (last >= 0 && last < (int)sizeof opf_dir - 1) {
        memcpy(opf_dir, opf_path, (size_t)last + 1);
        opf_dir[last + 1] = 0;
    }

    const char *ob = (const char *)opf;
    char coverid[128];
    coverid[0] = 0;
    uint32_t pos = 0, tl;
    const char *t;
    while (next_tag(ob, len, &pos, &t, &tl)) {                 /* pass 1: <meta name="cover" content="id"> */
        char nm[24], v[16];
        if (tag_name(t, (uint32_t)tl, nm) || strcmp(nm, "meta") != 0) continue;
        if (rd_attr_get(t, (int)tl, "name", 0, v, (int)sizeof v) > 0 && strcmp(v, "cover") == 0)
            rd_attr_get(t, (int)tl, "content", 0, coverid, (int)sizeof coverid);
    }
    int best = 0;
    char href[300], bestref[300];
    bestref[0] = 0;
    pos = 0;
    while (next_tag(ob, len, &pos, &t, &tl)) {                 /* pass 2: manifest items */
        char nm[24], id[128], mt[40], props[80];
        if (tag_name(t, (uint32_t)tl, nm) || strcmp(nm, "item") != 0) continue;
        int hl = rd_attr_get(t, (int)tl, "href", 0, href, (int)sizeof href);
        if (hl <= 0) continue;
        int il = rd_attr_get(t, (int)tl, "id", 0, id, (int)sizeof id);
        int ml = rd_attr_get(t, (int)tl, "media-type", 0, mt, (int)sizeof mt);
        int image = ml >= 6 && memcmp(mt, "image/", 6) == 0;
        int pl = rd_attr_get(t, (int)tl, "properties", 0, props, (int)sizeof props);
        int pri = 0;
        if (pl > 0 && has_token(props, pl, "cover-image")) pri = 3;
        else if (image && coverid[0] && il > 0 && strcmp(id, coverid) == 0) pri = 2;
        else if (image && ((il > 0 && contains_ci(id, il, "cover")) || contains_ci(href, hl, "cover"))) pri = 1;
        if (pri > best) {
            best = pri;
            memcpy(bestref, href, (size_t)hl + 1);
        }
    }
    if (!best) return 0;
    int pl = rd_resolve_path(opf_dir, bestref, (int)strlen(bestref), out, cap);
    if (pl <= 0) return 0;
    rd_zip_ent_t e;
    if (rd_zip_find(z, out, (uint32_t)pl, &e)) return 0;
    return pl;
}
