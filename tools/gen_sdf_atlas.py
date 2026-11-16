#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the SDF glyph atlas render/fonts/dejavu_sans_mono_sdf.h (chapter 10 §10.5.1, WI-2.433).

    gen_sdf_atlas.py [--font DejaVuSansMono.ttf] [-o render/fonts/dejavu_sans_mono_sdf.h]
    gen_sdf_atlas.py --check [--font ...] [-o ...]

The atlas holds the 95 printable ASCII glyphs (U+0020..U+007E) followed by → ↑ ↓ ← − · (§10.5.1), rasterised from
DejaVu Sans Mono at 48 px with Pillow and turned into a signed distance field with an 8 px spread by the exact
Euclidean distance transform below (NumPy only). Every glyph gets the same cell: the font's advance by its line height
(ascent + descent), plus the spread on every side, with the baseline at `spread + ascent`. Cells are laid out row by
row in a 1024 px wide R8 texture as tall as the glyphs need (not 1024 rows: spec question #648), so the shader
finds glyph i at (i % columns, i / columns) without a metrics table.

Byte encoding: 127.5 at the outline, larger inside; byte = round(127.5 - d * 127.5 / spread) clamped to 0..255,
where d is the distance to the outline in source pixels (negative inside). The shader inverts it with
d = (0.5 - v) * 2 * spread for the normalised texel v.

Determinism: the output depends only on the font file and the Pillow/FreeType rasteriser; the header records the
font's SHA-256 and the Pillow and FreeType versions, and two runs with the same inputs write identical bytes. --check
exits 1 when the checked-in header differs from what this script generates, 2 when Pillow or the font is missing.
The font is not checked in: DejaVu Sans Mono 2.37 ships with every Linux distribution (fonts-dejavu-core).
"""

from __future__ import annotations

import argparse
import hashlib
import math
import sys
from pathlib import Path

import numpy as np

FONT_PX = 48
SPREAD = 8
ATLAS_WIDTH = 1024
EXTRA_GLYPHS = "→↑↓←−·"  # → ↑ ↓ ← − ·
GLYPHS = "".join(chr(c) for c in range(0x20, 0x7F)) + EXTRA_GLYPHS
DEFAULT_FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
DEFAULT_OUT = Path(__file__).resolve().parent.parent / "render" / "fonts" / "dejavu_sans_mono_sdf.h"
INF = 1e20


def edt_1d(f: np.ndarray) -> np.ndarray:
    """Squared distance transform of one sampled function (Felzenszwalb and Huttenlocher, 2012)."""
    n = len(f)
    d = np.zeros(n)
    v = np.zeros(n, dtype=np.int64)
    z = np.zeros(n + 1)
    k = 0
    z[0] = -INF
    z[1] = INF
    for q in range(1, n):
        s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k])
        while s <= z[k]:
            k -= 1
            s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2 * q - 2 * v[k])
        k += 1
        v[k] = q
        z[k] = s
        z[k + 1] = INF
    k = 0
    for q in range(n):
        while z[k + 1] < q:
            k += 1
        d[q] = (q - v[k]) * (q - v[k]) + f[v[k]]
    return d


def edt_2d(grid: np.ndarray) -> np.ndarray:
    """Squared Euclidean distance transform: columns first, then rows."""
    out = grid.astype(np.float64)
    for x in range(out.shape[1]):
        out[:, x] = edt_1d(out[:, x])
    for y in range(out.shape[0]):
        out[y, :] = edt_1d(out[y, :])
    return out


def sdf_bytes(coverage: np.ndarray, spread: int = SPREAD) -> np.ndarray:
    """Coverage (0..255) -> SDF bytes. Partly covered pixels move the outline by their coverage (as in TinySDF)."""
    a = coverage.astype(np.float64) / 255.0
    # Sources of each transform get 0; a partly covered pixel sits (its coverage - 0.5) px inside the outline.
    to_inside = np.where(a >= 1.0, 0.0, np.where(a <= 0.0, INF, np.square(np.maximum(0.5 - a, 0.0))))
    to_outside = np.where(a <= 0.0, 0.0, np.where(a >= 1.0, INF, np.square(np.maximum(a - 0.5, 0.0))))
    dist = np.sqrt(edt_2d(to_inside)) - np.sqrt(edt_2d(to_outside))  # > 0 outside the glyph
    value = np.floor(127.5 - dist * 127.5 / spread + 0.5)
    return np.clip(value, 0, 255).astype(np.uint8)


def layout(advance: int, line_height: int) -> tuple[int, int, int, int, int]:
    cell_w = advance + 2 * SPREAD
    cell_h = line_height + 2 * SPREAD
    columns = ATLAS_WIDTH // cell_w
    rows = math.ceil(len(GLYPHS) / columns)
    height = (rows * cell_h + 3) // 4 * 4
    return cell_w, cell_h, columns, rows, height


def build(font_path: Path) -> tuple[np.ndarray, dict[str, object]]:
    try:
        import PIL
        from PIL import Image, ImageDraw, ImageFont, features
    except ImportError:
        print("gen_sdf_atlas.py: Pillow is required (pip install pillow)", file=sys.stderr)
        sys.exit(2)
    font = ImageFont.truetype(str(font_path), FONT_PX)
    ascent, descent = font.getmetrics()
    advance = round(font.getlength("0"))
    for ch in GLYPHS:
        if round(font.getlength(ch)) != advance:
            raise SystemExit(f"gen_sdf_atlas.py: {ch!r} is not {advance} px wide; the font must be monospaced")
    cell_w, cell_h, columns, _rows, height = layout(advance, ascent + descent)
    atlas = np.zeros((height, ATLAS_WIDTH), dtype=np.uint8)
    for i, ch in enumerate(GLYPHS):
        img = Image.new("L", (cell_w, cell_h), 0)
        ImageDraw.Draw(img).text((SPREAD, SPREAD + ascent), ch, font=font, fill=255, anchor="ls")
        cell = sdf_bytes(np.asarray(img, dtype=np.uint8))
        x0 = (i % columns) * cell_w
        y0 = (i // columns) * cell_h
        atlas[y0 : y0 + cell_h, x0 : x0 + cell_w] = cell
    meta: dict[str, object] = {
        "sha256": hashlib.sha256(font_path.read_bytes()).hexdigest(),
        "pillow": PIL.__version__,
        "freetype": features.version("freetype2") or "unknown",
        "ascent": ascent,
        "descent": descent,
        "advance": advance,
        "cell_w": cell_w,
        "cell_h": cell_h,
        "columns": columns,
        "height": height,
    }
    return atlas, meta


def header(atlas: np.ndarray, meta: dict[str, object]) -> str:
    guard = "OVT_RENDER_FONTS_DEJAVU_SANS_MONO_SDF_H"
    flat = atlas.reshape(-1).tolist()
    rows = [",".join(str(b) for b in flat[i : i + 48]) + "," for i in range(0, len(flat), 48)]
    extra = ", ".join(f"0x{ord(c):04X}" for c in EXTRA_GLYPHS)
    # REUSE-IgnoreStart
    return (
        "// SPDX-FileCopyrightText: 2003 Bitstream, Inc.\n"
        "// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors\n"
        "// SPDX-License-Identifier: Bitstream-Vera\n"
        "//\n"
        "// Signed-distance-field glyph atlas of DejaVu Sans Mono (chapter 10 §10.5.1), R8, row-major. Generated by\n"
        "// tools/gen_sdf_atlas.py; do not edit. The glyph outlines are Bitstream Vera Sans Mono (Bitstream Vera\n"
        "// licence, render/fonts/LICENSE.DejaVu); the DejaVu changes are in the public domain.\n"
        f"// Font SHA-256 {meta['sha256']}; Pillow {meta['pillow']}, FreeType {meta['freetype']}.\n"
        "\n"
        f"#ifndef {guard}\n#define {guard}\n\n#include <array>\n#include <cstdint>\n\n"
        "namespace ovt::render::fonts {\n\n"
        "// clang-format off\n"
        f"inline constexpr int kSdfFontPx = {FONT_PX};       // source raster size (px per em)\n"
        f"inline constexpr int kSdfSpread = {SPREAD};         // distance range on each side of the outline, source px\n"
        f"inline constexpr int kSdfAdvance = {meta['advance']};       // monospaced advance, source px\n"
        f"inline constexpr int kSdfAscent = {meta['ascent']};\n"
        f"inline constexpr int kSdfDescent = {meta['descent']};\n"
        f"inline constexpr int kSdfCellW = {meta['cell_w']};         // advance + 2 * spread\n"
        f"inline constexpr int kSdfCellH = {meta['cell_h']};         // ascent + descent + 2 * spread\n"
        f"inline constexpr int kSdfColumns = {meta['columns']};       // glyph i lives in cell (i % columns, i / columns)\n"
        f"inline constexpr int kSdfAtlasWidth = {ATLAS_WIDTH};\n"
        f"inline constexpr int kSdfAtlasHeight = {meta['height']};\n"
        f"inline constexpr int kSdfGlyphCount = {len(GLYPHS)};    // U+0020..U+007E, then kSdfExtraGlyphs\n"
        f"inline constexpr std::array<char32_t, {len(EXTRA_GLYPHS)}> kSdfExtraGlyphs = {{{extra}}};\n\n"
        f"inline constexpr std::array<uint8_t, {len(flat)}> kSdfAtlas = {{\n" + "\n".join(rows) + "\n};\n"
        "// clang-format on\n\n"
        "}  // namespace ovt::render::fonts\n\n"
        f"#endif  // {guard}\n"
    )
    # REUSE-IgnoreEnd


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    ap.add_argument("--font", type=Path, default=DEFAULT_FONT, help="DejaVu Sans Mono TrueType file")
    ap.add_argument("-o", "--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true", help="compare with the checked-in header instead of writing")
    args = ap.parse_args(argv)
    if not args.font.is_file():
        print(f"gen_sdf_atlas.py: font {args.font} not found", file=sys.stderr)
        return 2
    text = header(*build(args.font))
    if args.check:
        if not args.out.is_file() or args.out.read_text(encoding="utf-8") != text:
            print(f"gen_sdf_atlas.py: {args.out} is out of date", file=sys.stderr)
            return 1
        return 0
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
