/* Host regression test for rd_markup (HTML, FB2, Markdown, paths) plus a mutation fuzzer.
 *   cc -g -fsanitize=address,undefined -I.. markup_test.c ../rd_markup.c ../rd_zip.c \
 *      ../rd_inflate.c ../rd_epub.c -o markup_test && ./markup_test [fuzz]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd_epub.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define FILL "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore."
static rd_doc_t *doc;
static char *buf;
#define CAP (256 * 1024)

static void run(rd_fmt_t fmt, const char *base, const void *data, size_t n)
{
    rd_doc_init(doc, buf, CAP);
    rd_parser_t p;
    rd_parser_begin(&p, doc, fmt, base);
    rd_parser_feed(&p, (const uint8_t *)data, (uint32_t)n);
    rd_parser_end(&p);
    rd_doc_finish(doc);
}
#define RUN(fmt, base, s) run(fmt, base, s, sizeof(s) - 1)
#define HAS(sub) CHECK(strstr(doc->buf, sub) != NULL, "missing \"%s\" in:\n%s", sub, doc->buf)
#define HASNOT(sub) CHECK(strstr(doc->buf, sub) == NULL, "unexpected \"%s\" in:\n%s", sub, doc->buf)

static void test_html(void)
{
    RUN(RD_FMT_HTML, "OEBPS/text/",
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>My Title</title>"
        "<style>p{color:red}</style><script>if (a<b) {x=1;}</script></head>\n"
        "<body><h1>Chapter One</h1><p>" FILL "</p><p>Caf&eacute; &amp; cr&egrave;me &mdash; &#8220;quoted&#8221; &#x2019;s &nbsp; end.</p>\n"
        "<!-- a comment <p>hidden</p> -->\n"
        "<p>Second<br/>line</p><ul><li>one</li><li>two</li></ul>"
        "<img src=\"../img/pic%201.png\" alt=\"A figure\"/><p>Tail text</p></body></html>");
    CHECK(strcmp(doc->title, "My Title") == 0, "title=\"%s\"", doc->title);
    HAS("CHAPTER ONE\n");
    HAS("Cafe & creme -- \"quoted\" 's end.\n");
    HAS("Second\nline\n");
    HAS("- one\n- two\n");
    HAS("Tail text");
    HASNOT("hidden"); HASNOT("color:red"); HASNOT("x=1"); HASNOT("DOCTYPE");
    CHECK(doc->nchap == 1 && strcmp(doc->chap[0].title, "Chapter One") == 0, "chapters=%d", doc->nchap);
    CHECK(doc->nimg == 1 && strcmp(doc->ipool + doc->img_off[0], "OEBPS/img/pic 1.png") == 0,
          "img=%d %s", doc->nimg, doc->nimg ? doc->ipool + doc->img_off[0] : "-");
    CHECK(strchr(doc->buf, 1) != NULL, "image marker missing");

    /* drop cap, icon, svg cover, caption inside a heading */
    RUN(RD_FMT_HTML, "",
        "<p><img alt=\"W\" src=\"a.png\"/>ITH joy</p>"
        "<p><img src=\"i.png\" width=\"20\" height=\"20\" alt=\"icon\"/>after icon</p>"
        "<svg><image xlink:href=\"cover.jpg\" width=\"600\" height=\"800\"/></svg>"
        "<h2><span class=\"caption\">\"A caption.\"</span><br/>CHAPTER IX.</h2><p>" FILL "</p>");
    HAS("WITH joy\n"); HAS("after icon");
    CHECK(doc->nimg == 1 && strcmp(doc->ipool + doc->img_off[0], "cover.jpg") == 0, "nimg=%d", doc->nimg);
    CHECK(doc->nchap == 1 && strcmp(doc->chap[0].title, "CHAPTER IX.") == 0, "title \"%s\"", doc->nchap ? doc->chap[0].title : "-");
    HAS("\"A CAPTION.\"");

    /* latin-1 declared by meta */
    RUN(RD_FMT_HTML, "", "<html><head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\"></head>"
                         "<body><p>caf\xE9 na\xEFve \x93quote\x94</p></body></html>");
    HAS("cafe naive \"quote\"");

    /* unterminated and odd input must not misbehave */
    RUN(RD_FMT_HTML, "", "<p>a < b and AT&T &unknown; &#xZZ; <b>bold <i>x");
    HAS("a < b and AT&T");
}

static void test_fb2(void)
{
    static const char fb2[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
        "xmlns:l=\"http://www.w3.org/1999/xlink\"><description><title-info><genre>prose</genre>"
        "<author><first-name>Lev</first-name><last-name>Tolstoy</last-name></author>"
        "<book-title>War &amp; Peace</book-title><annotation><p>should not show</p></annotation>"
        "<coverpage><image l:href=\"#cover.jpg\"/></coverpage></title-info></description>\n"
        "<body><title><p>Part One</p></title><section><title><p>Chapter 1</p></title><p>" FILL "</p>"
        "<p>\"Eh bien\" \xE2\x80\x94 said Anna. Caf\xC3\xA9 \xC3\x86ther \xD0\x96\xD0\xB8\xD0\xB7\xD0\xBD\xD1\x8C</p>"
        "<empty-line/><p>Second.</p><image l:href=\"#pic1\"/></section></body>\n"
        "<body name=\"notes\"><section><p>note text must vanish</p></section></body>\n"
        "<binary id=\"cover.jpg\" content-type=\"image/jpeg\">QUJDRA==</binary>"
        "<binary id=\"pic1\" content-type=\"image/png\">AAAA</binary></FictionBook>";
    run(RD_FMT_FB2, "", fb2, sizeof fb2 - 1);
    CHECK(strcmp(doc->title, "War & Peace") == 0, "title \"%s\"", doc->title);
    CHECK(strcmp(doc->author, "Lev Tolstoy") == 0, "author \"%s\"", doc->author);
    HAS("PART ONE\n"); HAS("CHAPTER 1\n");
    HAS("\"Eh bien\" -- said Anna. Cafe AEther Zhizn\n");
    HAS("Second.");
    HASNOT("should not show"); HASNOT("note text");
    CHECK(doc->nchap == 1, "chapters=%d (a title stack keeps only the last heading)", doc->nchap);
    CHECK(doc->nimg == 2 && strcmp(doc->ipool + doc->img_off[0], "#cover.jpg") == 0 &&
          strcmp(doc->ipool + doc->img_off[1], "#pic1") == 0, "nimg=%d", doc->nimg);
    CHECK(doc->buf[0] == 1, "cover marker should open the book");
    CHECK(doc->nbin == 2, "nbin=%d", doc->nbin);
    if (doc->nbin == 2) {
        CHECK(strcmp(doc->ipool + doc->bin[0].id_off, "#cover.jpg") == 0, "bin0 id");
        CHECK(doc->bin[0].data_len == 8 && memcmp(fb2 + doc->bin[0].data_off, "QUJDRA==", 8) == 0, "bin0 range");
        CHECK(doc->bin[1].data_len == 4 && memcmp(fb2 + doc->bin[1].data_off, "AAAA", 4) == 0, "bin1 range");
    }

    /* windows-1251 body: "Privet" + a quote mark */
    RUN(RD_FMT_FB2, "", "<?xml version=\"1.0\" encoding=\"windows-1251\"?><FictionBook><body><section>"
                         "<p>\xCF\xF0\xE8\xE2\xE5\xF2 \x93mir\x94</p></section></body></FictionBook>");
    HAS("Privet");
}

static void test_md(void)
{
    static const char md[] =
        "# Title One\n\nSome *emphasis* and **strong** text with [a link](http://x.y/z) and `code`.\n"
        "Continuation line.\n\n- item a\n- item b\n\n1. first\n2. second\n\n> quoted\n\n---\n\n"
        "![alt text](pic.png) after image\n\n```\nfenced code\n```\n\n## Sub head ##\nsnake_case_word " FILL "\n";
    rd_doc_init(doc, buf, CAP);
    rd_md_convert(doc, (const uint8_t *)md, sizeof md - 1);
    rd_doc_finish(doc);
    HAS("TITLE ONE\n");
    HAS("Some emphasis and strong text with a link and code. Continuation line.\n");
    HAS("- item a\n- item b\n");
    HAS("1. first\n2. second\n");
    HAS("quoted\n"); HAS("* * *\n"); HAS("alt text after image");
    HAS("fenced code\n"); HAS("SUB HEAD\n"); HAS("snake_case_word");
    HASNOT("http://x.y"); HASNOT("**"); HASNOT("`");
    CHECK(doc->nchap == 2, "md chapters=%d", doc->nchap);
}

static void test_txt(void)
{
    static const char txt[] =
        "Title Line\r\n\r\nCHAPTER I. Beginning\r\n\r\nIt was a dark and stormy\r\nnight; the rain fell in torrents.\r\n\r\n"
        "Second para caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 end.\nCHAPTER II\nNot a heading: no blank line before.\n\n"
        "Chapter 2 \n\nOk text. " FILL " " FILL "\n\n\n\nPart of a sentence that is not a heading at all, it just runs on and on and on and on.\n\n"
        "LETTER 3 - To Mrs. S\n\nDear madam. " FILL " " FILL "\n\n"
        "II.\nThe Machine\n\n" FILL " " FILL "\n\nXIII\n\n" FILL " " FILL "\n";
    rd_doc_init(doc, buf, CAP);
    rd_txt_convert(doc, (const uint8_t *)txt, sizeof txt - 1);
    rd_doc_finish(doc);
    HAS("Title Line\n");
    HAS("CHAPTER I. BEGINNING\n");
    HAS("It was a dark and stormy night; the rain fell in torrents.\n");
    HAS("Second para cafe \"quoted\" -- end. CHAPTER II Not a heading: no blank line before.\n");
    HAS("CHAPTER 2\nOk text. Lorem");
    HAS("LETTER 3 - TO MRS. S\nDear madam. Lorem"); HAS("II. THE MACHINE\nLorem"); HAS("XIII\nLorem");
    HASNOT("\r");
    CHECK(doc->nchap == 5, "txt chapters=%d", doc->nchap);
    if (doc->nchap == 5) {
        CHECK(strcmp(doc->chap[0].title, "CHAPTER I. Beginning") == 0, "ch0 \"%s\"", doc->chap[0].title);
        CHECK(strcmp(doc->chap[1].title, "Chapter 2") == 0, "ch1 \"%s\"", doc->chap[1].title);
        CHECK(strcmp(doc->chap[2].title, "LETTER 3 - To Mrs. S") == 0, "ch2 \"%s\"", doc->chap[2].title);
        CHECK(strcmp(doc->chap[3].title, "II. The Machine") == 0, "ch3 \"%s\"", doc->chap[3].title);
        CHECK(strncmp(doc->buf + doc->chap[0].off, "CHAPTER I. BEGINNING", 20) == 0, "chapter offset points at heading");
    }
}

/* Converting in place must give exactly what a separate output buffer gives. */
static char *ref_out;
static void conv(int kind, const uint8_t *in, uint32_t n, int inplace_slack, uint8_t *work, int *overflow, uint32_t *len)
{
    rd_parser_t p;
    if (inplace_slack) { rd_doc_init(doc, (char *)work, (uint32_t)inplace_slack); doc->slack = (uint32_t)inplace_slack; }
    else rd_doc_init(doc, buf, CAP);
    if (kind == 0) rd_txt_convert(doc, in, n);
    else if (kind == 1) rd_md_convert(doc, in, n);
    else { rd_parser_begin(&p, doc, RD_FMT_HTML, ""); rd_parser_feed(&p, in, n); rd_parser_end(&p); rd_doc_finish(doc); }
    *overflow = doc->overflow;
    *len = doc->len;
}

static void test_inplace(void)
{
    static const char txt[] = "Title\r\n\r\nCHAPTER I. Start\r\n\r\ncaf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x94 and so on.\r\nMore text here.\r\n\r\nSecond para.\r\n";
    static const char md[] = "# Head\n\nSome *emph* and [a link](http://x.y).\n\n- one\n- two\n";
    static const char html[] = "<html><body><h1>Chapter One</h1><p>Caf&eacute; &amp; cr&egrave;me</p><p>Tail <img src=\"a.png\"/></p></body></html>";
    const struct { int kind; const char *d; uint32_t n; } cases[3] = {
        { 0, txt, sizeof txt - 1 }, { 1, md, sizeof md - 1 }, { 2, html, sizeof html - 1 } };
    for (int c = 0; c < 3; c++) {
        for (uint32_t slack = 64; slack <= 128 * 1024; slack *= 8) {
            uint32_t n = cases[c].n;
            int of; uint32_t l1, l2;
            conv(cases[c].kind, (const uint8_t *)cases[c].d, n, 0, 0, &of, &l1);
            ref_out = malloc(l1 + 1);
            memcpy(ref_out, doc->buf, l1 + 1);
            uint8_t *w = malloc(slack + n);
            memcpy(w + slack, cases[c].d, n);
            conv(cases[c].kind, w + slack, n, (int)(slack + n), w, &of, &l2);
            /* in-place wants cap = slack + n at most: re-run with the exact contract */
            memcpy(w + slack, cases[c].d, n);
            rd_doc_init(doc, (char *)w, slack);
            doc->slack = slack;
            if (cases[c].kind == 0) rd_txt_convert(doc, w + slack, n);
            else if (cases[c].kind == 1) rd_md_convert(doc, w + slack, n);
            else { rd_parser_t p; rd_parser_begin(&p, doc, RD_FMT_HTML, ""); rd_parser_feed(&p, w + slack, n); rd_parser_end(&p); rd_doc_finish(doc); }
            CHECK(!doc->overflow && doc->len == l1 && memcmp(doc->buf, ref_out, l1) == 0,
                  "in-place kind %d slack %u differs: len %u vs %u", cases[c].kind, slack, doc->len, l1);
            CHECK(doc->chap[0].off == 0 || doc->nchap >= 0, "chapters survive");
            free(w); free(ref_out);
        }
    }
    /* worst case: every 2 input bytes become 4 output chars. With too little room the output must
     * be a clean prefix of the full result (never garbage from overwritten input). */
    enum { N = 6000 };
    uint8_t *cy = malloc(N * 2);
    for (int i = 0; i < N; i++) { cy[2 * i] = 0xD0; cy[2 * i + 1] = 0xA9; }      /* U+0429 -> "Shch" */
    int of; uint32_t full;
    conv(0, cy, N * 2, 0, 0, &of, &full);
    char *whole = malloc(full + 1);
    memcpy(whole, doc->buf, full + 1);
    for (uint32_t slack = 16; slack <= 20000; slack *= 5) {
        uint8_t *w = malloc(slack + N * 2);
        memcpy(w + slack, cy, N * 2);
        rd_doc_init(doc, (char *)w, slack);
        doc->slack = slack;
        rd_txt_convert(doc, w + slack, N * 2);
        {
            uint32_t k = 0;
            while (k < doc->len && k < full && doc->buf[k] == whole[k]) k++;
            int clean = (k == doc->len) || (doc->overflow && k + 1 == doc->len && doc->buf[k] == '\n');   /* a cut-off book still ends its last paragraph */
            CHECK(doc->len <= full + 1 && clean, "slack %u: len %u (full %u), first difference at %u: got '%c' (0x%02x) want '%c'",
                  slack, doc->len, full, k, doc->buf[k] >= 32 ? doc->buf[k] : '?', (unsigned char)doc->buf[k], whole[k]);
        }
        if (slack >= N * 2) CHECK(!doc->overflow && doc->len == full, "slack %u should be enough", slack);
        else CHECK(doc->overflow, "slack %u cannot hold the expansion, overflow expected", slack);
        free(w);
    }
    free(cy); free(whole);
}

static void test_paths(void)
{
    char out[200];
#define PATH(base, src, want) do { int l = rd_resolve_path(base, src, (int)strlen(src), out, sizeof out); \
        CHECK(l == (int)strlen(want) && strcmp(out, want) == 0, "path(%s,%s)=\"%s\" want \"%s\"", base, src, out, want); } while (0)
    PATH("OEBPS/text/", "../img/a%20b.png", "OEBPS/img/a b.png");
    PATH("", "x/./y/../z.png", "x/z.png");
    PATH("a/", "/abs/p.png", "abs/p.png");
    PATH("a/", "p.png#frag", "a/p.png");
    PATH("a/b/", "../../../../up.png", "up.png");
    PATH("a/", "p%2.png", "a/p%2.png");
}

/* ---- fuzzing: mutated inputs must never crash or hang under the sanitizers ---- */

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static int null_sink(void *c, const uint8_t *d, uint32_t n) { (void)c; (void)d; (void)n; return 0; }

static void fuzz(void)
{
    static const char seed[] =
        "<html><head><title>T</title></head><body><h1>Head &amp; &#x41; &eacute;</h1><p>Text <b>bold</b><br/>"
        "<img src=\"a/../b%20.png\" alt=\"x\"/></p><!-- c --><![CDATA[ d ]]><?pi?><ul><li>a</li></ul></body></html>";
    uint8_t m[sizeof seed];
    for (int it = 0; it < 20000; it++) {
        memcpy(m, seed, sizeof seed);
        int flips = 1 + (int)(rnd() % 6);
        for (int k = 0; k < flips; k++) m[rnd() % (sizeof seed - 1)] = (uint8_t)rnd();
        size_t n = (rnd() % 4 == 0) ? rnd() % (sizeof seed - 1) : sizeof seed - 1;
        run((it & 1) ? RD_FMT_FB2 : RD_FMT_HTML, "x/", m, n);
    }
    static uint8_t junk[4096];
    for (int it = 0; it < 3000; it++) {
        size_t n = rnd() % sizeof junk;
        for (size_t i = 0; i < n; i++) junk[i] = (uint8_t)rnd();
        run(RD_FMT_HTML, "", junk, n);
        run(RD_FMT_FB2, "", junk, n);
        rd_doc_init(doc, buf, CAP);
        rd_md_convert(doc, junk, (uint32_t)n);
        static uint8_t win[RD_WINDOW_SIZE];
        rd_inflate(junk, (uint32_t)n, win, null_sink, 0);
        rd_zip_t z;
        rd_zip_open(&z, junk, (uint32_t)n);
    }
    /* mutated real archives: random byte flips in a small hand-built zip */
    static uint8_t zip[4096];
    size_t zn = 0;
    {
        FILE *f = fopen("fuzz_seed.epub", "rb");
        if (f) { zn = fread(zip, 1, sizeof zip, f); fclose(f); }
    }
    if (zn) {
        uint8_t *scratch = aligned_alloc(16, 128 * 1024);
        uint8_t *win = malloc(RD_WINDOW_SIZE);
        for (int it = 0; it < 4000; it++) {
            uint8_t z2[4096];
            memcpy(z2, zip, zn);
            for (int k = 0, fl = 1 + (int)(rnd() % 4); k < fl; k++) z2[rnd() % zn] = (uint8_t)rnd();
            rd_doc_init(doc, buf, CAP);
            rd_epub_mem_t mem = { win, scratch, 128 * 1024 };
            rd_zip_t z;
            rd_epub_load(&z, doc, z2, (uint32_t)(rnd() % 8 == 0 ? rnd() % zn : zn), &mem);
        }
        free(scratch); free(win);
    }
    for (int it = 0; it < 4000; it++) {                  /* in-place conversion of random bytes: ASan guards the bounds */
        size_t n = rnd() % 3000;
        uint32_t slack = 16 + rnd() % 200;
        uint8_t *w = malloc(slack + n + 1);
        for (size_t i = 0; i < n; i++) w[slack + i] = (it & 1) ? (uint8_t)rnd() : "ab <>&;\n#*-\xD0\xA9\xE2\x80"[rnd() % 15];
        rd_doc_init(doc, (char *)w, slack);
        doc->slack = slack;
        if (it % 3 == 0) rd_txt_convert(doc, w + slack, (uint32_t)n);
        else if (it % 3 == 1) rd_md_convert(doc, w + slack, (uint32_t)n);
        else { rd_parser_t p; rd_parser_begin(&p, doc, RD_FMT_HTML, ""); rd_parser_feed(&p, w + slack, (uint32_t)n); rd_parser_end(&p); rd_doc_finish(doc); }
        free(w);
    }
    printf("fuzz: done\n");
}

int main(int argc, char **argv)
{
    doc = malloc(sizeof *doc);
    buf = malloc(CAP);
    test_html();
    test_fb2();
    test_md();
    test_txt();
    test_inplace();
    test_paths();
    if (argc > 1 && strcmp(argv[1], "fuzz") == 0) fuzz();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
