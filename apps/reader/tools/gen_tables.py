#!/usr/bin/env python3
"""Generate rd_tables.h for the Reader app (dev-time only; the output is committed).

  python3 gen_tables.py > ../rd_tables.h

The nano font is ASCII only, so every non-ASCII code point is folded to a short
ASCII string (accents dropped, Cyrillic/Greek transliterated, punctuation mapped).
Tables hold offsets, never pointers, so the app image needs no data relocations.
"""
import html.entities
import unicodedata

# (first, last+1) code point ranges that get a fold entry
RANGES = [(0x00A0, 0x0250), (0x0370, 0x0400), (0x0400, 0x0500), (0x2000, 0x2070),
          (0x20A0, 0x20C0), (0x2100, 0x2190), (0xFB00, 0xFB07)]

OVERRIDE = {
    0x00A0: " ", 0x00AD: "", 0x00A1: "!", 0x00BF: "?", 0x00AB: '"', 0x00BB: '"',
    0x00A9: "(c)", 0x00AE: "(R)", 0x00B0: " deg", 0x00B1: "+/-", 0x00D7: "x",
    0x00F7: "/", 0x00B5: "u", 0x00BC: "1/4", 0x00BD: "1/2", 0x00BE: "3/4",
    0x00A7: "S", 0x00B6: "", 0x00A2: "c", 0x00A3: "GBP ", 0x00A5: "JPY ",
    0x00A4: "", 0x00A8: "", 0x00B4: "", 0x00B8: "", 0x00AF: "", 0x00B7: "*",
    0x00C6: "AE", 0x00E6: "ae", 0x0152: "OE", 0x0153: "oe", 0x00DF: "ss",
    0x00D8: "O", 0x00F8: "o", 0x0110: "D", 0x0111: "d", 0x0141: "L", 0x0142: "l",
    0x00DE: "Th", 0x00FE: "th", 0x00D0: "D", 0x00F0: "d", 0x0131: "i", 0x0138: "k",
    0x0149: "'n", 0x017F: "s", 0x014A: "Ng", 0x014B: "ng", 0x0126: "H", 0x0127: "h",
    0x2018: "'", 0x2019: "'", 0x201A: ",", 0x201B: "'", 0x201C: '"', 0x201D: '"',
    0x201E: '"', 0x201F: '"', 0x2032: "'", 0x2033: '"', 0x2010: "-", 0x2011: "-",
    0x2012: "-", 0x2013: "-", 0x2014: "--", 0x2015: "--", 0x2020: "+", 0x2021: "+",
    0x2022: "*", 0x2024: ".", 0x2025: "..", 0x2026: "...", 0x2030: "0/00",
    0x2039: "'", 0x203A: "'", 0x203C: "!!", 0x2044: "/", 0x2028: " ", 0x2029: " ",
    0x20AC: "EUR ", 0x20B9: "INR ", 0x20BD: "RUB ", 0x2122: "(TM)", 0x2116: "No",
}
for cp in range(0x2000, 0x200B):
    OVERRIDE.setdefault(cp, " ")
for cp in range(0x200B, 0x2010):
    OVERRIDE.setdefault(cp, "")

CYR = {
    "А": "A", "Б": "B", "В": "V", "Г": "G", "Д": "D", "Е": "E", "Ё": "Yo", "Ж": "Zh",
    "З": "Z", "И": "I", "Й": "Y", "К": "K", "Л": "L", "М": "M", "Н": "N", "О": "O",
    "П": "P", "Р": "R", "С": "S", "Т": "T", "У": "U", "Ф": "F", "Х": "Kh", "Ц": "Ts",
    "Ч": "Ch", "Ш": "Sh", "Щ": "Shch", "Ъ": "", "Ы": "Y", "Ь": "", "Э": "E",
    "Ю": "Yu", "Я": "Ya", "Є": "Ye", "І": "I", "Ї": "Yi", "Ґ": "G", "Ў": "U",
    "Ђ": "Dj", "Ј": "J", "Љ": "Lj", "Њ": "Nj", "Ћ": "C", "Џ": "Dz",
}
for k, v in list(CYR.items()):
    CYR[k.lower()] = v.lower()
GRK = {
    "Α": "A", "Β": "B", "Γ": "G", "Δ": "D", "Ε": "E", "Ζ": "Z", "Η": "E", "Θ": "Th",
    "Ι": "I", "Κ": "K", "Λ": "L", "Μ": "M", "Ν": "N", "Ξ": "X", "Ο": "O", "Π": "P",
    "Ρ": "R", "Σ": "S", "Τ": "T", "Υ": "U", "Φ": "Ph", "Χ": "Ch", "Ψ": "Ps", "Ω": "O",
}
for k, v in list(GRK.items()):
    GRK[k.lower()] = v.lower()
GRK["ς"] = "s"


def fold(cp):
    if cp in OVERRIDE:
        return OVERRIDE[cp]
    s = unicodedata.normalize("NFKD", chr(cp)).replace("⁄", "/")
    s = "".join(c for c in s if unicodedata.category(c) != "Mn")
    out = []
    for c in s:
        if ord(c) < 128:
            out.append(c)
        elif c in CYR:
            out.append(CYR[c])
        elif c in GRK:
            out.append(GRK[c])
    r = "".join(out)
    return r if len(r) <= 6 else r[:6]


def main():
    blob = bytearray()
    index = {}
    starts = []
    idx = 0
    for lo, hi in RANGES:
        starts.append((lo, hi, idx))
        for cp in range(lo, hi):
            s = fold(cp)
            if s not in index:
                index[s] = len(blob)
                blob += s.encode("ascii") + b"\0"
            starts[-1] = starts[-1]
            idx += 1
    offs = []
    for lo, hi in RANGES:
        for cp in range(lo, hi):
            offs.append(index[fold(cp)])
    assert max(offs) < 65536 and len(offs) < 65536

    names = sorted(html.entities.name2codepoint.items())
    names.append(("apos", 0x27))
    nblob = bytearray()
    ncp = []
    for n, cp in names:
        nblob += n.encode("ascii") + b"\0"
        ncp.append(cp)

    cp1251 = [ord(bytes([b]).decode("cp1251", errors="replace")) if b >= 0x80 else b
              for b in range(0x80, 0x100)]
    cp1252 = []
    for b in range(0x80, 0xA0):
        try:
            cp1252.append(ord(bytes([b]).decode("cp1252")))
        except UnicodeDecodeError:
            cp1252.append(0x20)

    def carr(name, vals, per=12):
        lines = []
        for i in range(0, len(vals), per):
            lines.append("    " + ", ".join(str(v) for v in vals[i:i + per]) + ",")
        return "static const uint16_t %s[%d] = {\n%s\n};\n" % (name, len(vals), "\n".join(lines))

    def cstr(name, blob_bytes):
        # emit as a char array of numbers (avoids any string-literal escaping surprises)
        vals = list(blob_bytes)
        lines = []
        for i in range(0, len(vals), 20):
            lines.append("    " + ", ".join(str(v) for v in vals[i:i + 20]) + ",")
        return "static const char %s[%d] = {\n%s\n};\n" % (name, len(vals), "\n".join(lines))

    print("/* Generated by tools/gen_tables.py - do not edit. */")
    print("#ifndef RD_TABLES_H\n#define RD_TABLES_H\n#include <stdint.h>\n")
    print("#define RD_FOLD_NRANGES %d" % len(starts))
    print("static const uint32_t rd_fold_range[%d][3] = {  /* first, last+1, first index */" % len(starts))
    for lo, hi, i in starts:
        print("    {0x%04X, 0x%04X, %d}," % (lo, hi, i))
    print("};\n")
    print(carr("rd_fold_off", offs))
    print(cstr("rd_fold_str", blob))
    print("#define RD_ENT_COUNT %d" % len(names))
    print(cstr("rd_ent_names", nblob))
    print(carr("rd_ent_cp", ncp))
    print(carr("rd_cp1251_hi", cp1251))
    print(carr("rd_cp1252_hi", cp1252))
    print("#endif")


if __name__ == "__main__":
    main()
