#!/bin/sh
# Host tests for the Reader's portable code (inflate, zip, EPUB/FB2/HTML/Markdown, images, pager),
# built with AddressSanitizer + UBSan. The LVGL screens in reader.c can only be tested on the iPod.
#
#   sh run_tests.sh                      markup tests + fuzz, always
#   sh run_tests.sh /dir/with/epubs      also: zip CRC check, EPUB parse, pager on each book
#   IMAGES=/dir/with/images sh ...       also: decode every .jpg/.png there and fuzz the decoders
set -e
cd "$(dirname "$0")"
OUT="${TMPDIR:-/tmp}/reader_host_tests"
mkdir -p "$OUT"
CC="cc -g -O1 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -I.."
LIB="../rd_zip.c ../rd_inflate.c ../rd_markup.c ../rd_epub.c"

echo "== markup, EPUB loader, paths, fuzz"
$CC markup_test.c $LIB -o "$OUT/markup_test"
"$OUT/markup_test" fuzz

if [ -n "$1" ]; then
    echo "== zip/inflate CRC check on every entry"
    $CC zip_test.c ../rd_zip.c ../rd_inflate.c -o "$OUT/zip_test"
    "$OUT/zip_test" "$1"/*.epub
    $CC epub_test.c $LIB -o "$OUT/epub_test"
    $CC page_test.c ../rd_page.c -o "$OUT/page_test"
    cc -c -g -O1 -Wno-everything -fwrapv -fsanitize=address -I../tjpgd ../tjpgd/tjpgd.c -o "$OUT/tjpgd.o"
    $CC cover_test.c ../rd_cover.c ../rd_image.c $LIB "$OUT/tjpgd.o" -o "$OUT/cover_test"
    echo "== library covers (found via the EPUB's own metadata)"
    "$OUT/cover_test" "$OUT/covers.ppm" "$1"/*.epub fuzz_seed.epub
    for b in "$1"/*.epub; do
        n=$(basename "$b" .epub)
        echo "== $n"
        "$OUT/epub_test" "$b" "$OUT/$n.txt" | head -3
        "$OUT/page_test" "$OUT/$n.txt" | tail -1
    done
fi

if [ -n "$IMAGES" ]; then
    echo "== images (tjpgd is built with -fwrapv, as in the app)"
    cc -c -g -O1 -Wno-everything -fwrapv -fsanitize=address -I../tjpgd ../tjpgd/tjpgd.c -o "$OUT/tjpgd.o"
    $CC image_test.c ../rd_image.c ../rd_inflate.c "$OUT/tjpgd.o" -o "$OUT/image_test"
    $CC image_fuzz.c ../rd_image.c ../rd_inflate.c "$OUT/tjpgd.o" -o "$OUT/image_fuzz"
    for f in "$IMAGES"/*.jpg "$IMAGES"/*.png; do
        [ -f "$f" ] && "$OUT/image_test" "$f" "$OUT/img.ppm" || true
    done | awk '{print} /result=0/ {ok++} END {print ok " decoded"}' | tail -3
    "$OUT/image_fuzz" $(ls "$IMAGES"/*.jpg "$IMAGES"/*.png 2>/dev/null | head -6)
fi
echo "all host tests done"
