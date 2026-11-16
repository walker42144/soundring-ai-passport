#!/usr/bin/env python3
"""Structural checks for the generated LVGL font subsets in main/fonts/.

The test parses the C files on its own (nothing is imported from the generator)
and validates them against the layout LVGL assumes at runtime:

* `glyph_dsc[0]` is the reserved all-zero entry, glyph ids start at 1
* `bitmap_index` is monotonic, glyph slices never overlap and cover the array
* every glyph slice is exactly `ceil(box_w * box_h / 2)` bytes (PLAIN 4 bpp)
* every codepoint reachable through the cmap tables resolves to a glyph
* every required runtime codepoint (the U+2026 ellipsis) is present in every font
* every glyph is reachable, so no orphan bitmaps are left behind
* `line_height` / `base_line` fit the em size and host every glyph box
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "main" / "fonts"

# name -> (px size, allow non-ASCII glyphs)
FONTS = {
    "sr_font_14": (14, True),
    "sr_font_20": (20, True),
    "sr_font_28": (28, False),
}
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E

# 运行时一定会被绘制的字符：sr_utf8_ellipsis() 的截断标记。
# 它在 sr_text.c 里是转义序列，源码扫描收不到，必须由生成器显式加入并在测试里守住。
REQUIRED_CODEPOINTS = {
    0x2026: "horizontal ellipsis (sr_utf8_ellipsis)",
}

BITMAP_RE = re.compile(r"glyph_bitmap\[\] = \{(.*?)\n\};", re.S)
GLYPHS_RE = re.compile(r"glyph_dsc\[\] = \{(.*?)\n\};", re.S)
CMAPS_RE = re.compile(r"lv_font_fmt_txt_cmap_t cmaps\[\] = \{(.*?)\n\};", re.S)
LISTS_RE = re.compile(r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", re.S)
ENTRY_RE = re.compile(r"\{([^{}]*)\}")
FIELD_RE = re.compile(r"\.(\w+)\s*=\s*(-?\d+)")
HEX_BYTE_RE = re.compile(r"0x([0-9a-fA-F]{2})\b")


@dataclass(frozen=True)
class Glyph:
    bitmap_index: int
    adv_w: int
    box_w: int
    box_h: int
    ofs_x: int
    ofs_y: int

    @property
    def bitmap_bytes(self) -> int:
        return (self.box_w * self.box_h + 1) // 2


@dataclass(frozen=True)
class CMap:
    range_start: int
    range_length: int
    glyph_id_start: int
    unicode_list: tuple[int, ...]
    list_length: int
    kind: str


def parse(path: Path) -> tuple[bytes, list[Glyph], list[CMap], str]:
    text = path.read_text(encoding="utf-8")
    bitmap_match = BITMAP_RE.search(text)
    glyph_match = GLYPHS_RE.search(text)
    cmap_match = CMAPS_RE.search(text)
    if not (bitmap_match and glyph_match and cmap_match):
        raise AssertionError("missing glyph_bitmap / glyph_dsc / cmaps array")

    body = re.sub(r"/\*.*?\*/", "", bitmap_match.group(1), flags=re.S)
    bitmap = bytes(int(value, 16) for value in HEX_BYTE_RE.findall(body))

    glyphs = []
    for entry in ENTRY_RE.findall(glyph_match.group(1)):
        fields = {name: int(value) for name, value in FIELD_RE.findall(entry)}
        glyphs.append(Glyph(**{key: fields[key] for key in Glyph.__dataclass_fields__}))

    lists = {name: tuple(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{1,4})", values))
             for name, values in LISTS_RE.findall(text)}

    cmaps = []
    for entry in ENTRY_RE.findall(cmap_match.group(1)):
        fields = {name: int(value) for name, value in FIELD_RE.findall(entry)}
        list_name = re.search(r"\.unicode_list = (unicode_list_\d+|NULL)", entry).group(1)
        kind = re.search(r"\.type = LV_FONT_FMT_TXT_CMAP_(\w+)", entry).group(1)
        cmaps.append(CMap(fields["range_start"], fields["range_length"], fields["glyph_id_start"],
                          lists.get(list_name, ()), fields["list_length"], kind))
    return bitmap, glyphs, cmaps, text


def sourceline(text: str) -> str:
    match = re.search(r"\* Source font : (.+)", text)
    return match.group(1).strip() if match else ""


def lookup(cmaps: list[CMap], cp: int) -> int:
    for cmap in cmaps:
        rcp = cp - cmap.range_start
        if rcp < 0 or rcp >= cmap.range_length:
            continue
        if cmap.kind == "FORMAT0_TINY":
            return cmap.glyph_id_start + rcp
        if cmap.kind == "SPARSE_TINY":
            if rcp in cmap.unicode_list:
                return cmap.glyph_id_start + cmap.unicode_list.index(rcp)
    return 0


def check_font(name: str, px: int, allow_cjk: bool) -> list[str]:
    problems: list[str] = []
    path = FONT_DIR / f"{name}.c"
    if not path.is_file():
        return [f"{path.relative_to(ROOT).as_posix()}: missing"]

    bitmap, glyphs, cmaps, text = parse(path)

    if "Noto Sans SC" not in sourceline(text):
        problems.append("header comment does not name the source font")
    if "SIL Open Font License" not in text:
        problems.append("header comment does not name the license")
    if ".static_bitmap = 1" not in text:
        problems.append("lv_font_t lacks .static_bitmap = 1")
    for callback in ("lv_font_get_glyph_dsc_fmt_txt", "lv_font_get_bitmap_fmt_txt"):
        if f".get_glyph_dsc = {callback}" not in text and f".get_glyph_bitmap = {callback}" not in text:
            problems.append(f"lv_font_t lacks {callback}")
    if f"const lv_font_t {name} =" not in text:
        problems.append(f"public symbol {name} is not defined")
    for setting in (".bpp = 4,", ".bitmap_format = 0,", ".kern_dsc = NULL,"):
        if setting not in text:
            problems.append(f"font_dsc lacks {setting}")

    if not glyphs:
        return problems + ["glyph_dsc is empty"]
    if any((glyphs[0].bitmap_index, glyphs[0].adv_w, glyphs[0].box_w, glyphs[0].box_h,
            glyphs[0].ofs_x, glyphs[0].ofs_y)):
        problems.append("glyph_dsc[0] is not the reserved all-zero entry")

    line_height = int(re.search(r"\.line_height = (\d+)", text).group(1))
    base_line = int(re.search(r"\.base_line = (\d+)", text).group(1))
    if line_height < px:
        problems.append(f"line_height {line_height} is smaller than the {px}px em size")
    if not 0 < base_line < line_height:
        problems.append(f"base_line {base_line} is not inside a {line_height}px line")

    used = 0
    for gid in range(1, len(glyphs)):
        glyph = glyphs[gid]
        if glyph.bitmap_index < used:
            problems.append(f"glyph {gid}: bitmap_index {glyph.bitmap_index} overlaps the previous glyph")
        if glyph.adv_w >= 1 << 12:
            problems.append(f"glyph {gid}: adv_w {glyph.adv_w} does not fit the 12 bit field")
        if glyph.bitmap_index >= 1 << 20:
            problems.append(f"glyph {gid}: bitmap_index {glyph.bitmap_index} does not fit the 20 bit field")
        end = glyph.bitmap_index + glyph.bitmap_bytes
        if end > len(bitmap):
            problems.append(f"glyph {gid}: bitmap slice {glyph.bitmap_index}..{end} exceeds {len(bitmap)} bytes")
        elif gid + 1 < len(glyphs) and glyphs[gid + 1].bitmap_index != end:
            problems.append(
                f"glyph {gid}: {glyph.box_w}x{glyph.box_h} needs {glyph.bitmap_bytes} bytes but the next glyph "
                f"starts at {glyphs[gid + 1].bitmap_index} instead of {end}"
            )
        if glyph.ofs_y > 0:
            problems.append(f"glyph {gid}: positive ofs_y {glyph.ofs_y} pushes the box below the baseline")
        if -glyph.ofs_y > base_line:
            problems.append(f"glyph {gid}: descends {-glyph.ofs_y}px past base_line {base_line}")
        if glyph.box_h + glyph.ofs_y > line_height - base_line:
            problems.append(f"glyph {gid}: rises {glyph.box_h + glyph.ofs_y}px above base_line, "
                            f"only {line_height - base_line}px available")
        used = end

    if glyphs[-1].bitmap_index + glyphs[-1].bitmap_bytes != len(bitmap):
        problems.append(f"glyph_bitmap has {len(bitmap)} bytes, glyphs consume "
                        f"{glyphs[-1].bitmap_index + glyphs[-1].bitmap_bytes}")

    reachable: set[int] = set()
    ascii_cmap = next((c for c in cmaps if c.kind == "FORMAT0_TINY"), None)
    if ascii_cmap is None:
        problems.append("no FORMAT0_TINY cmap for the ASCII range")
    else:
        if ascii_cmap.range_start != ASCII_FIRST or ascii_cmap.range_length != ASCII_LAST - ASCII_FIRST + 1:
            problems.append("FORMAT0_TINY cmap does not cover exactly 0x20-0x7E")
        if ascii_cmap.glyph_id_start != 1:
            problems.append(f"FORMAT0_TINY glyph_id_start is {ascii_cmap.glyph_id_start}, expected 1")
        for cp in range(ASCII_FIRST, ASCII_LAST + 1):
            gid = lookup(cmaps, cp)
            if not 1 <= gid < len(glyphs):
                problems.append(f"U+{cp:04X}: ASCII cmap resolves to glyph id {gid}")
            else:
                reachable.add(gid)

    sparse = [cmap for cmap in cmaps if cmap.kind == "SPARSE_TINY"]
    if not allow_cjk:
        # ASCII 字体允许携带必需码位（省略号），但不许夹带其它非 ASCII 字形。
        for cmap in sparse:
            unexpected = [off for off in cmap.unicode_list
                          if cmap.range_start + off not in REQUIRED_CODEPOINTS]
            if unexpected:
                problems.append("ASCII-only font ships non-required codepoints in a sparse cmap")
    for cmap in sparse:
        if len(cmap.unicode_list) != cmap.list_length:
            problems.append(f"sparse cmap list_length {cmap.list_length} != {len(cmap.unicode_list)} entries")
        if list(cmap.unicode_list) != sorted(set(cmap.unicode_list)):
            problems.append("sparse cmap unicode_list is not strictly ascending")
        for offset in cmap.unicode_list:
            if offset >= cmap.range_length:
                problems.append(f"sparse cmap offset {offset} is outside range_length {cmap.range_length}")
            if offset > 0xFFFF:
                problems.append(f"sparse cmap offset {offset} does not fit uint16_t")
            cp = cmap.range_start + offset
            gid = lookup(cmaps, cp)
            if not 1 <= gid < len(glyphs):
                problems.append(f"U+{cp:04X}: sparse cmap resolves to glyph id {gid}")
            else:
                reachable.add(gid)
    if allow_cjk and not sparse:
        problems.append("font is declared to carry CJK but has no sparse cmap")

    # 运行时必然绘制的码位必须真的在 cmap 里，否则 LVGL 会画占位方块。
    for cp, label in REQUIRED_CODEPOINTS.items():
        gid = lookup(cmaps, cp)
        if not 1 <= gid < len(glyphs):
            problems.append(f"U+{cp:04X} ({label}) is missing from the cmap")
        else:
            reachable.add(gid)

    orphans = set(range(1, len(glyphs))) - reachable
    if orphans:
        problems.append(f"{len(orphans)} glyph(s) are not reachable through any cmap, e.g. id {min(orphans)}")

    return problems


def main() -> int:
    failures = 0
    for name, (px, allow_cjk) in FONTS.items():
        problems = check_font(name, px, allow_cjk)
        if problems:
            failures += 1
            print(f"FAIL {name}")
            for problem in problems[:10]:
                print(f"  {problem}")
        else:
            path = FONT_DIR / f"{name}.c"
            bitmap, glyphs, cmaps, _ = parse(path)
            print(f"PASS {name}: {len(glyphs) - 1} glyphs, {len(bitmap)} bitmap bytes, "
                  f"{path.stat().st_size} file bytes, {len(cmaps)} cmap table(s)")
    if failures:
        print(f"FAIL: {failures} font(s)")
        return 1
    print(f"PASS: {len(FONTS)} fonts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
