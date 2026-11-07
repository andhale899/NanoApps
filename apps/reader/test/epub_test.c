/* Host test: load an EPUB, print what the parser found, optionally dump the text.
 *   cc -g -fsanitize=address,undefined -I.. epub_test.c ../rd_epub.c ../rd_markup.c \
 *      ../rd_zip.c ../rd_inflate.c -o epub_test
 *   ./epub_test book.epub [out.txt]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rd_epub.h"

static void show(const char *s, size_t n)
{
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] == '\n') fputs("\\n\n", stdout);
        else if (s[i] == 1) printf("<IMG %d>", (s[i + 1] - '0') * 64 + (s[i + 2] - '0')), i += 2;
        else putchar(s[i]);
    }
    putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc((size_t)sz);
    if (fread(file, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);

    uint32_t cap = 2u * 1024 * 1024;
    char *text = malloc(cap);
    rd_doc_t *doc = malloc(sizeof *doc);
    uint8_t *window = malloc(RD_WINDOW_SIZE);
    uint8_t *scratch = aligned_alloc(16, 256 * 1024);
    rd_doc_init(doc, text, cap);
    rd_epub_mem_t mem = { window, scratch, 256 * 1024 };
    rd_zip_t z;
    int r = rd_epub_load(&z, doc, file, (uint32_t)sz, &mem);

    printf("result=%d  title=\"%s\"  author=\"%s\"\n", r, doc->title, doc->author);
    printf("text=%u bytes (overflow=%d)  chapters=%d  images=%d\n", doc->len, doc->overflow, doc->nchap, doc->nimg);
    for (int i = 0; i < doc->nchap && i < 12; i++)
        printf("  chapter %2d @%-7u %s\n", i, doc->chap[i].off, doc->chap[i].title);
    for (int i = 0; i < doc->nimg && i < 6; i++)
        printf("  image %d: %s\n", i, doc->ipool + doc->img_off[i]);
    printf("---- first 700 chars ----\n");
    show(doc->buf, 700);
    if (doc->len > 1400) {
        printf("---- chars at 40%% ----\n");
        show(doc->buf + doc->len * 4 / 10, 500);
    }
    int nonascii = 0;
    for (uint32_t i = 0; i < doc->len; i++) {
        unsigned char c = (unsigned char)doc->buf[i];
        if ((c < 32 && c != '\n' && c != 1) || c > 126) nonascii++;
    }
    printf("non-printable bytes in output: %d\n", nonascii);
    if (argc > 2) {
        FILE *o = fopen(argv[2], "wb");
        fwrite(doc->buf, 1, doc->len, o);
        fclose(o);
    }
    return r != 0;
}
