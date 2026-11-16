#!/usr/bin/env python3
"""Generate the LVGL 9 bitmap font subsets used by the SoundRing UI.

Rasterises glyphs with Pillow and writes LVGL `lv_font_fmt_txt` C files where
every glyph is stored as PLAIN 4 bpp (high nibble first, each glyph starting on
a byte boundary).  The source font's `cmap` table is parsed directly, so the
coverage gate does not depend on the rasteriser guessing about missing glyphs.

The generated files are re-read from disk and decoded exactly like LVGL does at
runtime (`lv_font_fmt_txt.c`, stride == 0 -> rows packed continuously) and then
compared pixel by pixel with the Pillow rasterisation.  Any difference fails the
build.

Usage:
    python tools/gen_sr_font.py           # regenerate main/fonts/sr_font_*.c
    python tools/gen_sr_font.py --check   # fail if the committed files are stale
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
MAIN_DIR = ROOT / "main"
OUTPUT_DIR = MAIN_DIR / "fonts"
SOURCE_FONT = ROOT / "assets" / "fonts" / "NotoSansCJKsc-Regular.otf"
LICENSE_FILE = ROOT / "assets" / "fonts" / "NotoSansCJKsc-OFL.txt"
SOURCE_FONT_URL = "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf"

ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
# 必须显式加入子集的码位：源码里以转义形式出现，字符扫描看不到。
EXTRA_CODEPOINTS = (
    0x2026,  # … HORIZONTAL ELLIPSIS，sr_utf8_ellipsis() 的截断标记
)
BYTES_PER_LINE = 16
OFFSETS_PER_LINE = 12
MAX_SPARSE_SPAN = 0xFFFF


@dataclass(frozen=True)
class FontSpec:
    name: str
    px: int
    line_height: int
    includes_cjk: bool

    @property
    def path(self) -> Path:
        return OUTPUT_DIR / f"{self.name}.c"


SPECS = (
    FontSpec("sr_font_14", 14, 18, True),
    FontSpec("sr_font_20", 20, 26, True),
    FontSpec("sr_font_28", 28, 34, False),
)


@dataclass(frozen=True)
class Glyph:
    cp: int
    adv_w: int
    box_w: int
    box_h: int
    ofs_x: int
    ofs_y: int
    pixels: bytes
    bitmap: bytes


@dataclass(frozen=True)
class CMapEntry:
    range_start: int
    range_length: int
    glyph_id_start: int
    unicode_list: tuple[int, ...]
    list_length: int
    kind: str


@dataclass(frozen=True)
class ParsedFont:
    bitmap: bytes
    glyphs: tuple[tuple[int, int, int, int, int, int], ...]
    cmaps: tuple[CMapEntry, ...]


# --------------------------------------------------------------------------- #
# Source scanning / coverage
# --------------------------------------------------------------------------- #

def collect_targets() -> dict[int, set[str]]:
    """Map every codepoint used by main/ to the source files that use it."""
    targets: dict[int, set[str]] = {cp: {"<fixed ASCII range>"} for cp in range(ASCII_FIRST, ASCII_LAST + 1)}
    # 省略号由 sr_text.c 以 UTF-8 转义序列 "\xE2\x80\xA6" 输出，源码里没有字面量，
    # 光扫描字符永远收不到它；缺了它所有截断文本都会渲染成占位方块。
    for cp in EXTRA_CODEPOINTS:
        targets[cp] = {"<required by sr_utf8_ellipsis>"}
    # Common GB2312 level-1 characters cover many dynamic track titles and artists.
    # UTF-8 names remain unrestricted; unsupported glyphs retain LVGL placeholders.
    for lead in range(0xB0, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                ch = bytes((lead, trail)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            targets.setdefault(ord(ch), set()).add("<common song metadata>")
    for cp in range(0x3040, 0x3100):
        targets.setdefault(cp, set()).add("<Japanese kana>")
    # Additional Unicode codepoints extend the default metadata font coverage.
    metadata_codepoints = ROOT / "assets" / "fonts" / "metadata-codepoints.txt"
    if metadata_codepoints.is_file():
        for value in metadata_codepoints.read_text(encoding="ascii").split():
            targets.setdefault(int(value, 16), set()).add("<playlist metadata>")
    for path in sorted(list(MAIN_DIR.glob("*.c")) + list(MAIN_DIR.glob("*.h"))):
        if path.parent.name == "fonts":
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(ROOT).as_posix()
        for ch in text:
            cp = ord(ch)
            if cp > 0x7F:
                targets.setdefault(cp, set()).add(rel)
    return targets


def _sfnt_tables(data: bytes) -> dict[bytes, bytes]:
    count = struct.unpack(">H", data[4:6])[0]
    tables: dict[bytes, bytes] = {}
    for index in range(count):
        tag, _checksum, offset, length = struct.unpack(">4sIII", data[12 + 16 * index: 28 + 16 * index])
        tables[tag] = data[offset:offset + length]
    return tables


def read_font_cmap(path: Path) -> set[int]:
    """Return the set of codepoints the font actually has a glyph for."""
    data = path.read_bytes()
    cmap = _sfnt_tables(data)[b"cmap"]
    subtable_count = struct.unpack(">H", cmap[2:4])[0]

    format_12: int | None = None
    format_4: int | None = None
    for index in range(subtable_count):
        platform, encoding, offset = struct.unpack(">HHI", cmap[4 + 8 * index: 12 + 8 * index])
        fmt = struct.unpack(">H", cmap[offset:offset + 2])[0]
        if fmt == 12 and platform == 3:
            format_12 = offset
        elif fmt == 4 and platform == 3 and encoding == 1:
            format_4 = offset

    codepoints: set[int] = set()
    if format_12 is not None:
        group_count = struct.unpack(">I", cmap[format_12 + 12:format_12 + 16])[0]
        for index in range(group_count):
            start, end, _glyph = struct.unpack(">III", cmap[format_12 + 16 + 12 * index:format_12 + 28 + 12 * index])
            codepoints.update(range(start, end + 1))
        return codepoints

    if format_4 is None:
        raise SystemExit(f"{path.name}: no usable Unicode cmap subtable")

    seg_count = struct.unpack(">H", cmap[format_4 + 6:format_4 + 8])[0] // 2
    ends = struct.unpack(f">{seg_count}H", cmap[format_4 + 14:format_4 + 14 + 2 * seg_count])
    starts_at = format_4 + 16 + 2 * seg_count
    starts = struct.unpack(f">{seg_count}H", cmap[starts_at:starts_at + 2 * seg_count])
    deltas = struct.unpack(f">{seg_count}h", cmap[starts_at + 2 * seg_count:starts_at + 4 * seg_count])
    range_offsets_at = starts_at + 4 * seg_count
    range_offsets = struct.unpack(f">{seg_count}H", cmap[range_offsets_at:range_offsets_at + 2 * seg_count])
    for index in range(seg_count):
        for cp in range(starts[index], ends[index] + 1):
            if cp == 0xFFFF:
                continue
            if range_offsets[index] == 0:
                glyph_id = (cp + deltas[index]) & 0xFFFF
            else:
                at = range_offsets_at + 2 * index + range_offsets[index] + 2 * (cp - starts[index])
                if at + 2 > len(cmap):
                    continue
                glyph_id = struct.unpack(">H", cmap[at:at + 2])[0]
                if glyph_id:
                    glyph_id = (glyph_id + deltas[index]) & 0xFFFF
            if glyph_id:
                codepoints.add(cp)
    return codepoints


# --------------------------------------------------------------------------- #
# Rasterising / packing
# --------------------------------------------------------------------------- #

def _quantize_to_4bpp(value: int) -> int:
    return (value * 15 + 127) // 255


def pack_lvgl_bitmap(pixels: bytes) -> bytes:
    """Pack 8 bit coverage samples into LVGL's PLAIN 4 bpp stream."""
    packed = bytearray()
    high: int | None = None
    for value in pixels:
        nibble = _quantize_to_4bpp(value)
        if high is None:
            high = nibble
        else:
            packed.append((high << 4) | nibble)
            high = None
    if high is not None:
        packed.append(high << 4)
    return bytes(packed)


def render_glyph(font: ImageFont.FreeTypeFont, cp: int) -> Glyph:
    ch = chr(cp)
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    box_w = right - left
    box_h = bottom - top
    adv_w = int(round(font.getlength(ch) * 16))
    if box_w <= 0 or box_h <= 0:
        return Glyph(cp, adv_w, 0, 0, 0, 0, b"", b"")
    image = Image.new("L", (box_w, box_h), 0)
    ImageDraw.Draw(image).text((-left, -top), ch, font=font, fill=255, anchor="ls")
    pixels = image.tobytes()
    return Glyph(cp, adv_w, box_w, box_h, left, -bottom, pixels, pack_lvgl_bitmap(pixels))


def build_glyphs(font, codepoints: list[int]) -> list[Glyph]:
    return [render_glyph(font, cp) for cp in codepoints]


# --------------------------------------------------------------------------- #
# C emission
# --------------------------------------------------------------------------- #

def glyph_comment(cp: int) -> str:
    ch = chr(cp)
    if not ch.isprintable():
        return f"/* U+{cp:04X} */"
    shown = {'"': '\\"', "\\": "\\\\"}.get(ch, ch)
    return f'/* U+{cp:04X} "{shown}" */'


def _guard_name(spec: FontSpec) -> str:
    return spec.name.upper()


def _chunks(values, size):
    for start in range(0, len(values), size):
        yield values[start:start + size]


def emit_c(spec: FontSpec, glyphs: list[Glyph], base_line: int, cap_height: int, x_height: int) -> str:
    lines: list[str] = []
    add = lines.append
    guard = _guard_name(spec)
    codepoints = [glyph.cp for glyph in glyphs]
    sparse = [cp for cp in codepoints if cp > ASCII_LAST]
    ascii_count = ASCII_LAST - ASCII_FIRST + 1
    cmap_count = 2 if sparse else 1

    add("/*******************************************************************************")
    add(f" * {spec.name} - {spec.px} px LVGL 9 bitmap font subset")
    add(" *")
    add(" * Source font : Noto Sans SC (NotoSansCJKsc-Regular.otf)")
    add(" * License     : SIL Open Font License 1.1 (assets/fonts/NotoSansCJKsc-OFL.txt)")
    add(" * Generator   : python tools/gen_sr_font.py")
    add(" * Coverage    : ASCII 0x20-0x7E" + (" + every non-ASCII codepoint used by main/" if spec.includes_cjk else " only"))
    add(" *")
    add(" * Do not edit by hand.")
    add(" ******************************************************************************/")
    add("")
    add("#ifdef __has_include")
    add('    #if __has_include("lvgl.h")')
    add("        #ifndef LV_LVGL_H_INCLUDE_SIMPLE")
    add("            #define LV_LVGL_H_INCLUDE_SIMPLE")
    add("        #endif")
    add("    #endif")
    add("#endif")
    add("")
    add("#ifdef LV_LVGL_H_INCLUDE_SIMPLE")
    add('    #include "lvgl.h"')
    add("#else")
    add('    #include "../../lvgl.h"')
    add("#endif")
    add("")
    add("")
    add(f"#ifndef {guard}")
    add(f"    #define {guard} 1")
    add("#endif")
    add("")
    if spec.name == "sr_font_20":
        add("LV_FONT_DECLARE(sr_font_14);")
    add(f"#if {guard}")
    add("")
    add("/*-----------------")
    add(" *    BITMAPS")
    add(" *----------------*/")
    add("")
    add("/*Store the image of the glyphs*/")
    add("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    for glyph in glyphs:
        add(f"    {glyph_comment(glyph.cp)}")
        if glyph.bitmap:
            for chunk in _chunks(list(glyph.bitmap), BYTES_PER_LINE):
                add("    " + "".join(f"0x{byte:02x}, " for byte in chunk).rstrip())
    add("};")
    add("")
    add("/*---------------------")
    add(" *  GLYPH DESCRIPTION")
    add(" *---------------------*/")
    add("")
    add("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    add("    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,")
    index = 0
    for glyph in glyphs:
        add(
            "    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, .ofs_x = %d, .ofs_y = %d},"
            % (index, glyph.adv_w, glyph.box_w, glyph.box_h, glyph.ofs_x, glyph.ofs_y)
        )
        index += len(glyph.bitmap)
    add("};")
    add("")
    add("/*---------------------")
    add(" *  CHARACTER MAPPING")
    add(" *---------------------*/")
    add("")
    if sparse:
        add("static const uint16_t unicode_list_1[] = {")
        offsets = [cp - sparse[0] for cp in sparse]
        for chunk in _chunks(offsets, OFFSETS_PER_LINE):
            add("    " + "".join(f"0x{offset:04x}, " for offset in chunk).rstrip())
        add("};")
        add("")
    add("/*Collect the unicode lists and glyph_id offsets*/")
    add("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    add("    {")
    add(
        f"        .range_start = {ASCII_FIRST}, .range_length = {ascii_count}, .glyph_id_start = 1,"
    )
    add(
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,"
        " .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY"
    )
    add("    }" + ("," if sparse else ""))
    if sparse:
        add("    {")
        add(
            f"        .range_start = {sparse[0]}, .range_length = {sparse[-1] - sparse[0] + 1},"
            f" .glyph_id_start = {ascii_count + 1},"
        )
        add(
            "        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL,"
            f" .list_length = {len(sparse)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"
        )
        add("    }")
    add("};")
    add("")
    add("/*--------------------")
    add(" *  ALL CUSTOM DATA")
    add(" *--------------------*/")
    add("")
    add("#if LVGL_VERSION_MAJOR == 8")
    add("    /*Store all the custom data of the font*/")
    add("    static  lv_font_fmt_txt_glyph_cache_t cache;")
    add("#endif")
    add("")
    add("#if LVGL_VERSION_MAJOR >= 8")
    add("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    add("#else")
    add("static lv_font_fmt_txt_dsc_t font_dsc = {")
    add("#endif")
    add("    .glyph_bitmap = glyph_bitmap,")
    add("    .glyph_dsc = glyph_dsc,")
    add("    .cmaps = cmaps,")
    add("    .kern_dsc = NULL,")
    add("    .kern_scale = 16,")
    add(f"    .cmap_num = {cmap_count},")
    add("    .bpp = 4,")
    add("    .kern_classes = 0,")
    add("    .bitmap_format = 0,")
    add("#if LVGL_VERSION_MAJOR == 8")
    add("    .cache = &cache")
    add("#endif")
    add("};")
    add("")
    add("/*-----------------")
    add(" *  PUBLIC FONT")
    add(" *----------------*/")
    add("")
    add(f"const lv_font_t {spec.name} = {{")
    add("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    add("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    add(f"    .line_height = {spec.line_height},")
    add(f"    .base_line = {base_line},")
    add("#if LV_VERSION_CHECK(9, 6, 0) || LVGL_VERSION_MAJOR >= 10")
    add(f"    .cap_height = {cap_height},")
    add(f"    .x_height = {x_height},")
    add("#endif")
    add("    .subpx = LV_FONT_SUBPX_NONE,")
    add("    .underline_position = -1,")
    add("    .underline_thickness = 1,")
    add("#if LV_VERSION_CHECK(9, 3, 0)")
    add("    .static_bitmap = 1,")
    add("#endif")
    add("    .dsc = &font_dsc,")
    add("#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9")
    add("    .fallback = &sr_font_14," if spec.name == "sr_font_20" else "    .fallback = NULL,")
    add("#endif")
    add("    .user_data = NULL,")
    add("};")
    add("")
    add(f"#endif /*#if {guard}*/")
    add("")
    return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Re-reading the generated C file (independent of the emitter)
# --------------------------------------------------------------------------- #

BITMAP_RE = re.compile(r"glyph_bitmap\[\] = \{(.*?)\n\};", re.S)
GLYPHS_RE = re.compile(r"glyph_dsc\[\] = \{(.*?)\n\};", re.S)
CMAPS_RE = re.compile(r"lv_font_fmt_txt_cmap_t cmaps\[\] = \{(.*?)\n\};", re.S)
ENTRY_RE = re.compile(r"\{([^{}]*)\}")
FIELD_RE = re.compile(r"\.(\w+)\s*=\s*(-?\d+)")
HEX_BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})\b")


def parse_c(text: str) -> ParsedFont:
    bitmap_block = BITMAP_RE.search(text)
    glyph_block = GLYPHS_RE.search(text)
    cmaps_block = CMAPS_RE.search(text)
    if not (bitmap_block and glyph_block and cmaps_block):
        raise ValueError("generated file is missing glyph_bitmap / glyph_dsc / cmaps")

    bitmap_body = re.sub(r"/\*.*?\*/", "", bitmap_block.group(1), flags=re.S)
    bitmap = bytes(int(match, 16) for match in HEX_BYTE_RE.findall(bitmap_body))

    glyphs: list[tuple[int, int, int, int, int, int]] = []
    for body in ENTRY_RE.findall(glyph_block.group(1)):
        fields = {name: int(value) for name, value in FIELD_RE.findall(body)}
        glyphs.append((
            fields["bitmap_index"],
            fields["adv_w"],
            fields["box_w"],
            fields["box_h"],
            fields["ofs_x"],
            fields["ofs_y"],
        ))

    lists: dict[str, tuple[int, ...]] = {}
    for match in re.finditer(r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", text, re.S):
        lists[match.group(1)] = tuple(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{1,4})", match.group(2)))

    cmaps: list[CMapEntry] = []
    for body in ENTRY_RE.findall(cmaps_block.group(1)):
        fields = {name: int(value) for name, value in FIELD_RE.findall(body)}
        name_match = re.search(r"\.unicode_list = (unicode_list_\d+|NULL)", body)
        type_match = re.search(r"\.type = LV_FONT_FMT_TXT_CMAP_(\w+)", body)
        if not (name_match and type_match):
            raise ValueError(f"unparsable cmap entry: {body!r}")
        list_name = name_match.group(1)
        cmaps.append(CMapEntry(
            range_start=fields["range_start"],
            range_length=fields["range_length"],
            glyph_id_start=fields["glyph_id_start"],
            unicode_list=lists.get(list_name, ()),
            list_length=fields["list_length"],
            kind=type_match.group(1),
        ))
    return ParsedFont(bitmap=bitmap, glyphs=tuple(glyphs), cmaps=tuple(cmaps))


def decode_lvgl_bitmap(bitmap: bytes, box_w: int, box_h: int) -> bytearray:
    """Mirror lv_font_get_bitmap_fmt_txt() for PLAIN 4 bpp with stride == 0."""
    if box_w * box_h == 0:
        return bytearray()
    out = bytearray(box_w * box_h)
    pos = 0
    shift = 8
    current = 0
    for y in range(box_h):
        x = 0
        base = y * box_w
        if shift == 4:
            out[base + x] = current & 0xF
            x += 1
            shift = 8
        while x + 1 < box_w:
            current = bitmap[pos]
            pos += 1
            out[base + x] = current >> 4
            out[base + x + 1] = current & 0xF
            x += 2
        if x < box_w:
            current = bitmap[pos]
            pos += 1
            out[base + x] = current >> 4
            shift = 4
    return out


def cmap_lookup(cmaps: tuple[CMapEntry, ...], cp: int) -> int:
    """Mirror get_glyph_dsc_id()."""
    for cmap in cmaps:
        rcp = cp - cmap.range_start
        if rcp < 0 or rcp >= cmap.range_length:
            continue
        if cmap.kind == "FORMAT0_TINY":
            return cmap.glyph_id_start + rcp
        if cmap.kind == "SPARSE_TINY":
            try:
                offset = cmap.unicode_list.index(rcp)
            except ValueError:
                continue
            return cmap.glyph_id_start + offset
        raise ValueError(f"unsupported cmap type {cmap.kind}")
    return 0


def verify_font(spec: FontSpec, text: str, glyphs: list[Glyph], line_height: int, base_line: int) -> list[str]:
    problems: list[str] = []
    parsed = parse_c(text)

    expected_bitmap = b"".join(glyph.bitmap for glyph in glyphs)
    if parsed.bitmap != expected_bitmap:
        problems.append(f"glyph_bitmap differs from the rasterised data ({len(parsed.bitmap)} vs {len(expected_bitmap)} bytes)")
    if len(parsed.glyphs) != len(glyphs) + 1:
        problems.append(f"glyph_dsc holds {len(parsed.glyphs)} entries, expected {len(glyphs) + 1}")
    if parsed.glyphs and any(parsed.glyphs[0]):
        problems.append("glyph_dsc[0] is not the reserved all-zero entry")

    for index, glyph in enumerate(glyphs, start=1):
        if index >= len(parsed.glyphs):
            break
        bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y = parsed.glyphs[index]
        if (adv_w, box_w, box_h, ofs_x, ofs_y) != (glyph.adv_w, glyph.box_w, glyph.box_h, glyph.ofs_x, glyph.ofs_y):
            problems.append(f"U+{glyph.cp:04X}: glyph_dsc does not round-trip")
            continue
        expected_end = bitmap_index + len(glyph.bitmap)
        if expected_end > len(parsed.bitmap):
            problems.append(f"U+{glyph.cp:04X}: bitmap slice runs past the end of glyph_bitmap")
            continue
        decoded = decode_lvgl_bitmap(parsed.bitmap[bitmap_index:expected_end], box_w, box_h)
        expected = bytearray(_quantize_to_4bpp(value) for value in glyph.pixels)
        if decoded != expected:
            problems.append(f"U+{glyph.cp:04X}: decoded bitmap differs from the Pillow rasterisation")

    for glyph in glyphs:
        glyph_id = cmap_lookup(parsed.cmaps, glyph.cp)
        if glyph_id == 0:
            problems.append(f"U+{glyph.cp:04X}: not reachable through the cmap tables")
            continue
        if glyph_id >= len(parsed.glyphs):
            problems.append(f"U+{glyph.cp:04X}: cmap points at glyph id {glyph_id}, out of range")

    for glyph in glyphs:
        if glyph.ofs_y > 0 or glyph.box_h + glyph.ofs_y + base_line > line_height:
            problems.append(f"U+{glyph.cp:04X}: glyph box does not fit the {line_height}px line")
        if -glyph.ofs_y > base_line:
            problems.append(f"U+{glyph.cp:04X}: glyph descends {-glyph.ofs_y}px, base_line is {base_line}")

    return problems


# --------------------------------------------------------------------------- #
# Driver
# --------------------------------------------------------------------------- #

def ensure_source() -> None:
    if SOURCE_FONT.is_file():
        return
    raise SystemExit(
        "missing source font: {}\n"
        "download it with:\n"
        "  curl -L -o {} {}\n"
        "and keep the license next to it: {}".format(
            SOURCE_FONT.relative_to(ROOT).as_posix(),
            SOURCE_FONT.name,
            SOURCE_FONT_URL,
            LICENSE_FILE.relative_to(ROOT).as_posix(),
        )
    )


def build_spec(spec: FontSpec, codepoints: list[int]) -> tuple[list[Glyph], int, int, int]:
    font = ImageFont.truetype(str(SOURCE_FONT), spec.px)
    glyphs = build_glyphs(font, codepoints)
    # Tall accented capitals must fit existing fixed-height list rows. Rasterize
    # only those exceptional glyphs slightly smaller; keep every row at 18px.
    base = font.getmetrics()[1]
    if any(g.box_h + g.ofs_y > spec.line_height - base for g in glyphs):
        smaller = ImageFont.truetype(str(SOURCE_FONT), spec.px - 2)
        glyphs = [render_glyph(smaller, g.cp)
                  if g.box_h + g.ofs_y > spec.line_height - base else g for g in glyphs]
    ink_top = max((glyph.box_h + glyph.ofs_y for glyph in glyphs), default=0)
    ink_bottom = max((-glyph.ofs_y for glyph in glyphs), default=0)
    base_line = font.getmetrics()[1]
    if base_line < ink_bottom:
        base_line = ink_bottom
    if spec.line_height - base_line < ink_top:
        raise SystemExit(
            f"{spec.name}: {ink_top}px above the baseline does not fit a {spec.line_height}px line "
            f"with base_line {base_line}; pick a larger line height in SPECS"
        )
    cap_height = next((glyph.box_h for glyph in glyphs if glyph.cp == ord("H")), 0)
    x_height = next((glyph.box_h for glyph in glyphs if glyph.cp == ord("x")), 0)
    return glyphs, base_line, cap_height, x_height


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the SoundRing LVGL font subsets.")
    parser.add_argument("--check", action="store_true", help="do not write; fail if the generated files are stale")
    args = parser.parse_args(argv)

    ensure_source()
    if not LICENSE_FILE.is_file():
        raise SystemExit(f"missing font license: {LICENSE_FILE.relative_to(ROOT).as_posix()}")

    targets = collect_targets()
    coverage = read_font_cmap(SOURCE_FONT)
    targets = {cp: origins for cp, origins in targets.items()
               if cp in coverage or origins != {"<Japanese kana>"}}
    missing = sorted(cp for cp in targets if cp not in coverage)
    if missing:
        print(f"FAIL coverage: {len(missing)} codepoint(s) used by main/ have no glyph in Noto Sans SC", file=sys.stderr)
        for cp in missing[:20]:
            print(f"  U+{cp:04X} {chr(cp)!r} in {', '.join(sorted(targets[cp]))}", file=sys.stderr)
        return 1

    ascii_codepoints = list(range(ASCII_FIRST, ASCII_LAST + 1))
    full_codepoints = sorted(targets)
    sparse = [cp for cp in full_codepoints if cp > ASCII_LAST]
    if sparse and sparse[-1] - sparse[0] + 1 > MAX_SPARSE_SPAN:
        raise SystemExit(
            f"U+{sparse[0]:04X}..U+{sparse[-1]:04X} spans {sparse[-1] - sparse[0] + 1} slots, "
            f"a single LVGL sparse cmap can only cover {MAX_SPARSE_SPAN}"
        )

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    failures = 0
    for spec in SPECS:
        if spec.includes_cjk:
            if spec.px == 20:
                # Keep this bitmap within LVGL's compact 20-bit bitmap offset.
                codepoints = sorted(cp for cp in full_codepoints if cp < 128 or
                    any(origin != "<common song metadata>" for origin in targets[cp]) or
                    (chr(cp).encode("gb2312", errors="ignore")[:1] and
                     0xB0 <= chr(cp).encode("gb2312", errors="ignore")[0] < 0xD8))
            else:
                codepoints = full_codepoints
        else:
            # 纯 ASCII 字体也要带上必需码位（省略号），否则它的截断文本会显示占位方块。
            codepoints = sorted(set(ascii_codepoints) | set(EXTRA_CODEPOINTS))
        glyphs, base_line, cap_height, x_height = build_spec(spec, codepoints)
        text = emit_c(spec, glyphs, base_line, cap_height, x_height)
        rel = spec.path.relative_to(ROOT).as_posix()

        if args.check:
            if not spec.path.is_file():
                print(f"FAIL {rel}: missing; run python tools/gen_sr_font.py")
                failures += 1
                continue
            if spec.path.read_text(encoding="utf-8") != text:
                print(f"FAIL {rel}: stale; run python tools/gen_sr_font.py")
                failures += 1
                continue
            on_disk = text
        else:
            spec.path.write_text(text, encoding="utf-8", newline="\n")
            on_disk = spec.path.read_text(encoding="utf-8")

        problems = verify_font(spec, on_disk, glyphs, spec.line_height, base_line)
        size = len(on_disk.encode("utf-8"))
        if problems:
            failures += 1
            print(f"FAIL {rel}: self-check found {len(problems)} problem(s)")
            for problem in problems[:10]:
                print(f"  {problem}")
            continue
        print(
            f"PASS {rel}: {len(glyphs)} glyphs, {sum(len(glyph.bitmap) for glyph in glyphs)} bitmap bytes, "
            f"{size} file bytes, line_height {spec.line_height}, base_line {base_line}, "
            f"self-check ok ({len(codepoints)} codepoints)"
        )

    if failures:
        print(f"FAIL: {failures} font(s) did not pass", file=sys.stderr)
        return 1
    print(f"PASS: {len(SPECS)} fonts, {len(full_codepoints)} codepoints scanned from main/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
