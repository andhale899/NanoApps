APP_NAME   := reader
SRCS       := reader.c rd_inflate.c rd_zip.c rd_markup.c rd_epub.c rd_image.c rd_page.c rd_cover.c tjpgd/tjpgd.c
LV_SURFACE := 1
# tjpgd's IDCT arithmetic can overflow on damaged JPEGs; wrapping is the behaviour we want.
EXTRA_CFLAGS := -fwrapv
include ../../sdk/hb_app.mk
