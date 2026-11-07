# Reader

An ereader for the iPod nano (7th generation), part of [NanoApps](https://github.com/nfzerox/NanoApps).
It grew out of the Books app and reads EPUB, FB2, HTML, Markdown and plain text, with pictures.

## What it does

- **Formats:** EPUB (the main one), FB2, HTML/XHTML, Markdown, plain text. Put the files in `/Data/Books/`.
- **Pictures:** JPEG and PNG illustrations in EPUB and FB2 books, each on its own page, scaled to fit.
- **Library:** portrait tiles with the book's cover and a progress bar, most recently read first.
  Covers are made from the EPUB's own cover image and cached on the iPod.
- **Reading:** tap the left third for the previous page and the rest for the next. Reopening the app
  returns to the book and page you left. Bookmarks, a chapter list and a position slider are in the menu.
- **Settings** (gear button): font size, day / night / auto page colour, line spacing, margins,
  toolbar auto-hide, keep screen on, brightness, reopen last book. The same screen has an **About** page
  with book details, free memory and battery.

## Install

You need NanoApps already installed on the iPod (firmware first, see the main README).

1. Unzip `reader.zip` and copy the `Reader.nanoapp` folder into the `Apps` folder on the iPod.
2. Copy books into `Data/Books` on the iPod, then eject it. Reader appears on the Home Screen.

## Limits

- An EPUB is read into memory whole (the SDK cannot read part of a file). Very large illustrated books
  can be refused with "Not enough memory"; the About page shows how much the iPod has free.
- The font is ASCII only: accents are folded to plain letters, Cyrillic and Greek are transliterated,
  other scripts are dropped.
- Not supported: DRM-protected books, progressive JPEG, GIF, WebP, SVG, `.fb2.zip`.
- Plain text, Markdown and HTML are converted with simple rules and are the least reliable formats.

## Build from source

```sh
./start build reader        # compile
./start install reader      # build, install to the connected iPod
./start package reader      # build/packages/reader.zip, for sharing
```

The parsing, decoding and paging code is plain C with no LVGL dependency, and is tested on a computer
with address and undefined-behaviour sanitizers, including fuzzers:

```sh
sh apps/reader/test/run_tests.sh /folder/with/epubs     # IMAGES=/folder/with/images adds image tests
```

| File | Role |
| --- | --- |
| `reader.c` | the screens: library, reader, menu, settings, About |
| `rd_inflate.c`, `rd_zip.c`, `rd_epub.c` | deflate, zip and EPUB reading |
| `rd_markup.c` | HTML / FB2 / Markdown / text to the page text, chapters and picture markers |
| `rd_page.c` | single-pass page layout |
| `rd_image.c`, `rd_cover.c` | JPEG / PNG decoding and library thumbnails |
| `tjpgd/` | vendored JPEG decoder |

## Credits and licences

- NanoApps by nfzerox, MIT licence (see the repository `LICENSE`). Reader started from its Books app.
- [LVGL](https://lvgl.io), MIT licence.
- TJpgDec by ChaN, vendored in `tjpgd/`; its copyright notice is in `tjpgd/LICENSE.txt`.
- Montserrat font (SIL Open Font License), compiled in through LVGL.
