#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""HDR/SDR test pattern generator (chapter 10 section 10.9.2; WI-1.413).

Writes, for each of five patterns, into ``tests/corpora/patterns/`` (or ``--out``):

* ``<name>.y4m``           the exact 10-bit 4:2:0 limited-range source (not committed: large, regenerated),
* ``<name>.mp4``           lossless HEVC Main10 encode of it (committed, hashed in the corpora manifest),
* ``<name>.expected.json`` expected measurement values computed here from the formulas (never typed in).

and registers the ``syn_pat_<name>`` entries (source ``syn``, tag ``patterns``, SHA-256 of the mp4) in
``tests/corpora/manifest.json`` (chapter 14 section 14.3.3 recipe 73; there is no separate pattern manifest).

Usage: ``gen_hdr_patterns.py [--out DIR] [--manifest FILE] [--no-manifest] [--check] [--only NAME]``

Determinism (section 0.7.5): the Y4M files depend only on NumPy integer arithmetic and ``float64`` formulas
evaluated per pattern value; the mp4 is encoded single-threaded with bit-exact muxing flags. ``--check``
regenerates into a temporary directory and compares every file with the committed one, exit 1 on a difference.
The encoder identity is pinned in ``encoder.json`` next to the mp4 files; a different x265 version exits 5
without touching anything (its bitstream, hence the mp4 hash, may differ).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
WIDTH, HEIGHT, FRAMES = 1920, 1080, 3
FPS = "24/1"
EXIT_ENCODER_MISMATCH = 5

# ST 2084 constants (section 10.7.2)
M1, M2 = 0.1593017578125, 78.84375
C1, C2, C3 = 0.8359375, 18.8515625, 18.6875
# BT.2100 HLG constants (Table 5)
HLG_A, HLG_B = 0.17883277, 0.28466892
HLG_C = 0.55991073
HLG_LW = 1000.0  # nominal display peak used by every OVT HLG test (section 10.7)
HLG_GAMMA = 1.2

# Table 10.7-1 band edges in nits (lower end inclusive)
BAND_EDGES = [1.0, 100.0, 203.0, 1000.0, 4000.0]

PQ_NITS = [1, 100, 203, 1000, 4000, 10000]
# Section 10.9.2 and 10.4.4: limited-range 10-bit codes of the six patches
PQ_LIMITED_CODES = [195, 509, 573, 723, 855, 940]
PQ_FULL_CODES = [153, 520, 594, 769, 923, 1023]


# --------------------------------------------------------------------------------------------------------
# Transfer functions
# --------------------------------------------------------------------------------------------------------
def pq_oetf(nits: np.ndarray | float) -> np.ndarray:
    """ST 2084 inverse EOTF: absolute luminance in nits to non-linear E' in [0, 1]."""
    y = np.clip(np.asarray(nits, dtype=np.float64) / 10000.0, 0.0, 1.0)
    p = np.power(y, M1)
    return np.power((C1 + C2 * p) / (1.0 + C3 * p), M2)


def pq_eotf(e: np.ndarray | float) -> np.ndarray:
    """ST 2084 EOTF: non-linear E' in [0, 1] to nits."""
    ep = np.power(np.clip(np.asarray(e, dtype=np.float64), 0.0, 1.0), 1.0 / M2)
    y = np.power(np.maximum(ep - C1, 0.0) / (C2 - C3 * ep), 1.0 / M1)
    return y * 10000.0


def hlg_inverse_oetf(e: np.ndarray | float) -> np.ndarray:
    """BT.2100 HLG inverse OETF: signal E' to normalised scene light in [0, 1]."""
    e = np.clip(np.asarray(e, dtype=np.float64), 0.0, 1.0)
    low = e * e / 3.0
    high = (np.exp((e - HLG_C) / HLG_A) + HLG_B) / 12.0
    return np.where(e <= 0.5, low, high)


def hlg_display_nits_grey(e: np.ndarray | float) -> np.ndarray:
    """Display light of a neutral HLG signal at Lw = 1000 nits: ``Lw * E**1.2`` (BT.2100 OOTF)."""
    return HLG_LW * np.power(hlg_inverse_oetf(e), HLG_GAMMA)


def srgb_eotf(e: np.ndarray) -> np.ndarray:
    """IEC 61966-2-1 decode to linear light (relative, 1.0 = reference white)."""
    e = np.asarray(e, dtype=np.float64)
    return np.where(e <= 0.04045, e / 12.92, np.power((np.abs(e) + 0.055) / 1.055, 2.4))


def srgb_oetf(v: np.ndarray | float) -> np.ndarray:
    v = np.asarray(v, dtype=np.float64)
    return np.where(
        v <= 0.0031308, 12.92 * v, 1.055 * np.power(np.maximum(v, 0.0), 1 / 2.4) - 0.055
    )


# --------------------------------------------------------------------------------------------------------
# Colour encoding (10-bit limited range, H.273 matrices)
# --------------------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Matrix:
    name: str  # ffmpeg -colorspace name
    kr: float
    kb: float


BT709 = Matrix("bt709", 0.2126, 0.0722)
BT2020 = Matrix("bt2020nc", 0.2627, 0.0593)


def luma_code(e: np.ndarray | float) -> np.ndarray:
    """Limited-range 10-bit luma code of a non-linear value: ``round(876 * E' + 64)``."""
    return np.floor(876.0 * np.asarray(e, dtype=np.float64) + 64.0 + 0.5).astype(
        np.int64
    )


def full_luma_code(e: np.ndarray | float) -> np.ndarray:
    return np.floor(1023.0 * np.asarray(e, dtype=np.float64) + 0.5).astype(np.int64)


def rgb_to_ycbcr10(rgb: np.ndarray, m: Matrix) -> np.ndarray:
    """Non-linear R'G'B' in [0, 1] (last axis 3) to 10-bit limited-range (Y', Cb, Cr) integer codes."""
    rgb = np.asarray(rgb, dtype=np.float64)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = m.kr * r + (1.0 - m.kr - m.kb) * g + m.kb * b
    cb = (b - y) / (2.0 * (1.0 - m.kb))
    cr = (r - y) / (2.0 * (1.0 - m.kr))
    out = np.stack([876.0 * y + 64.0, 896.0 * cb + 512.0, 896.0 * cr + 512.0], axis=-1)
    return np.floor(out + 0.5).astype(np.int64)


def ycbcr10_to_rgb(ycc: np.ndarray, m: Matrix) -> np.ndarray:
    ycc = np.asarray(ycc, dtype=np.float64)
    y = (ycc[..., 0] - 64.0) / 876.0
    cb = (ycc[..., 1] - 512.0) / 896.0
    cr = (ycc[..., 2] - 512.0) / 896.0
    r = y + 2.0 * (1.0 - m.kr) * cr
    b = y + 2.0 * (1.0 - m.kb) * cb
    g = (y - m.kr * r - m.kb * b) / (1.0 - m.kr - m.kb)
    return np.stack([r, g, b], axis=-1)


# --------------------------------------------------------------------------------------------------------
# Frame model: three full-resolution-luma planes + 4:2:0 chroma, built from regions
# --------------------------------------------------------------------------------------------------------
class Canvas:
    """A 1920x1080 10-bit 4:2:0 picture. Chroma is set per chroma sample, so patterns with chroma detail finer
    than a region (the siting checker) are exact; flat regions must be aligned to even coordinates."""

    def __init__(self) -> None:
        self.y = np.full((HEIGHT, WIDTH), 64, dtype=np.uint16)
        self.cb = np.full((HEIGHT // 2, WIDTH // 2), 512, dtype=np.uint16)
        self.cr = np.full((HEIGHT // 2, WIDTH // 2), 512, dtype=np.uint16)

    def fill(
        self, x0: int, x1: int, y0: int, y1: int, code: tuple[int, int, int]
    ) -> None:
        assert x0 % 2 == 0 and x1 % 2 == 0 and y0 % 2 == 0 and y1 % 2 == 0, (
            "regions must be 4:2:0 aligned"
        )
        self.y[y0:y1, x0:x1] = code[0]
        self.cb[y0 // 2 : y1 // 2, x0 // 2 : x1 // 2] = code[1]
        self.cr[y0 // 2 : y1 // 2, x0 // 2 : x1 // 2] = code[2]

    def frame_bytes(self) -> bytes:
        return b"".join(p.astype("<u2").tobytes() for p in (self.y, self.cb, self.cr))


def y4m_bytes(canvas: Canvas) -> bytes:
    header = (
        f"YUV4MPEG2 W{WIDTH} H{HEIGHT} F{FPS} Ip A1:1 C420p10 XYSCSS=420P10\n".encode()
    )
    frame = b"FRAME\n" + canvas.frame_bytes()
    return header + frame * FRAMES


# --------------------------------------------------------------------------------------------------------
# Patterns. Each returns (canvas, expected dict, colour tags for the encoder).
# --------------------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Tags:
    primaries: str
    trc: str
    matrix: str


def _band_index(nits: float) -> int:
    return sum(1 for edge in BAND_EDGES if nits >= edge)


def _band_fractions(nits_and_area: list[tuple[float, int]]) -> list[float]:
    total = sum(a for _, a in nits_and_area)
    acc = [0] * 6
    for nits, area in nits_and_area:
        acc[_band_index(nits)] += area
    return [c / total for c in acc]


def _grey_nits_from_codes(
    codes: np.ndarray, eotf: Callable[[np.ndarray], np.ndarray]
) -> np.ndarray:
    """Luminance in nits that a decoder reconstructs from limited-range 10-bit codes (grey: R'=G'=B'=Y')."""
    return eotf(np.clip((np.asarray(codes, dtype=np.float64) - 64.0) / 876.0, 0.0, 1.0))


def pattern_pq_patches() -> tuple[Canvas, dict, Tags]:
    c = Canvas()
    n = len(PQ_NITS)
    w = WIDTH // n  # 320: even, so chroma is exact
    codes = [int(luma_code(pq_oetf(v))) for v in PQ_NITS]
    full = [int(full_luma_code(pq_oetf(v))) for v in PQ_NITS]
    assert codes == PQ_LIMITED_CODES, (
        f"PQ limited codes {codes} differ from the spec list {PQ_LIMITED_CODES}"
    )
    assert full == PQ_FULL_CODES, (
        f"PQ full-range codes {full} differ from the spec list {PQ_FULL_CODES}"
    )
    for i, code in enumerate(codes):
        c.fill(i * w, (i + 1) * w, 0, HEIGHT, (code, 512, 512))
    area = w * HEIGHT
    decoded = [float(_grey_nits_from_codes(code, pq_eotf)) for code in codes]
    exp = {
        "pattern": "pq_patches_1000",
        "corpus_id": "syn_pat_pq_patches_1000",
        "width": WIDTH,
        "height": HEIGHT,
        "frames": FRAMES,
        "transfer": "PQ (ST 2084)",
        "primaries": "BT.2020",
        "range": "limited",
        "bit_depth": 10,
        "patch_columns": [[i * w, (i + 1) * w] for i in range(n)],
        "patch_area_samples": area,
        "nits": [float(v) for v in PQ_NITS],
        "codes_limited": codes,
        "codes_full": full,
        "decoded_nits_from_limited_codes": decoded,
        "max_cll_nits": float(max(PQ_NITS)),
        "fall_nits": float(sum(PQ_NITS) / n),
        "fall_definition": "area-weighted mean of max(R,G,B) over the nominal patch nits (equal areas)",
        "band_edges_nits": BAND_EDGES,
        "band_fractions": _band_fractions([(float(v), area) for v in PQ_NITS]),
        "band_fractions_basis": "nominal patch nits; Table 10.7-1 bands, lower edge inclusive",
        "band_fractions_from_decoded_codes": _band_fractions(
            [(v, area) for v in decoded]
        ),
        "out_of_gamut_709_samples": 0,
        "saturated_area_samples": 0,
    }
    return c, exp, Tags("bt2020", "smpte2084", BT2020.name)


def pattern_hlg_patches() -> tuple[Canvas, dict, Tags]:
    """Top half: HLG 75 % white (left) and 100 % white (right); bottom half: 1920-step grey ramp."""
    c = Canvas()
    half_h = HEIGHT // 2 // 2 * 2  # 540
    sig = [0.75, 1.0]
    codes = [int(luma_code(s)) for s in sig]
    for i, code in enumerate(codes):
        c.fill(i * WIDTH // 2, (i + 1) * WIDTH // 2, 0, half_h, (code, 512, 512))
    ramp_e = np.arange(WIDTH, dtype=np.float64) / (WIDTH - 1)
    ramp_codes = luma_code(ramp_e)
    c.y[half_h:, :] = ramp_codes[None, :].astype(np.uint16)  # chroma stays 512
    c.cb[half_h // 2 :, :] = 512
    c.cr[half_h // 2 :, :] = 512
    nits = [float(hlg_display_nits_grey(s)) for s in sig]
    area = (WIDTH // 2) * half_h
    ramp_nits = _grey_nits_from_codes(ramp_codes, hlg_display_nits_grey)
    ramp_rows = HEIGHT - half_h
    total = WIDTH * HEIGHT
    fall_decoded = (
        sum(_grey_nits_from_codes(code, hlg_display_nits_grey) * area for code in codes)
        + float(ramp_nits.sum()) * ramp_rows
    ) / total
    exp = {
        "pattern": "hlg_patches",
        "corpus_id": "syn_pat_hlg_patches",
        "width": WIDTH,
        "height": HEIGHT,
        "frames": FRAMES,
        "transfer": "HLG (BT.2100)",
        "primaries": "BT.2020",
        "range": "limited",
        "bit_depth": 10,
        "display_peak_nits": HLG_LW,
        "system_gamma": HLG_GAMMA,
        "patch_signals": sig,
        "patch_codes_limited": codes,
        "patch_rects": [[0, WIDTH // 2, 0, half_h], [WIDTH // 2, WIDTH, 0, half_h]],
        "patch_nits": nits,
        "patch_nits_note": "BT.2100 OOTF at Lw = 1000: 75 % signal is 203 nits (BT.2408 nominal), 100 % is 1000",
        "ramp_rect": [0, WIDTH, half_h, HEIGHT],
        "ramp_min_nits": float(ramp_nits.min()),
        "ramp_max_nits": float(ramp_nits.max()),
        "max_cll_nits": float(max(nits)),
        "fall_nits": float(fall_decoded),
        "fall_definition": "mean over all pixels of max(R,G,B) display nits reconstructed from the limited codes",
        "band_edges_nits": BAND_EDGES,
        "band_fractions": _band_fractions(
            [(v, area) for v in nits] + [(float(v), ramp_rows) for v in ramp_nits]
        ),
        "band_fractions_basis": "display nits reconstructed from the codes (Lw = 1000); ramp columns weigh ramp_rows",
        "out_of_gamut_709_samples": 0,
        "saturated_area_samples": 0,
    }
    return c, exp, Tags("bt2020", "arib-std-b67", BT2020.name)


def pattern_sdr709_ramp() -> tuple[Canvas, dict, Tags]:
    """BT.709 gamma SDR: a horizontal ramp from code 64 (black) to 940 (white), 4:2:0, neutral chroma."""
    c = Canvas()
    e = np.arange(WIDTH, dtype=np.float64) / (WIDTH - 1)
    codes = luma_code(e)
    assert codes[0] == 64 and codes[-1] == 940
    c.y[:, :] = codes[None, :].astype(np.uint16)
    exp = {
        "pattern": "sdr709_ramp",
        "corpus_id": "syn_pat_sdr709_ramp",
        "width": WIDTH,
        "height": HEIGHT,
        "frames": FRAMES,
        "transfer": "BT.709 (SDR, gamma 2.4 display)",
        "primaries": "BT.709",
        "range": "limited",
        "bit_depth": 10,
        "ramp_first_code": int(codes[0]),
        "ramp_last_code": int(codes[-1]),
        "ramp_codes_sha256": hashlib.sha256(codes.astype("<u2").tobytes()).hexdigest(),
        "ramp_rule": "code(x) = round(876 * x / 1919 + 64), identical on every row; Cb = Cr = 512",
        "out_of_gamut_709_samples": 0,
        "saturated_area_samples": 0,
    }
    return c, exp, Tags("bt709", "bt709", BT709.name)


# Display P3 (D65) and BT.709 to XYZ (D65), used for the out-of-gamut classification
_P3_TO_XYZ = np.array(
    [
        [0.4865709, 0.2656677, 0.1982173],
        [0.2289746, 0.6917385, 0.0792869],
        [0.0, 0.0451134, 1.0439444],
    ]
)
_709_TO_XYZ = np.array(
    [
        [0.4123908, 0.3575843, 0.1804808],
        [0.2126390, 0.7151687, 0.0721923],
        [0.0193308, 0.1191948, 0.9505322],
    ]
)
_P3_TO_709 = np.linalg.inv(_709_TO_XYZ) @ _P3_TO_XYZ
GAMUT_TOLERANCE = 0.002  # linear light; absorbs 10-bit rounding of the codes


def pattern_p3_saturation() -> tuple[Canvas, dict, Tags]:
    """Eight 240-column stripes in Display P3 primaries (sRGB transfer, BT.709 matrix, limited range)."""
    c = Canvas()
    stripes = [
        ("red", (1, 0, 0)),
        ("green", (0, 1, 0)),
        ("blue", (0, 0, 1)),
        ("cyan", (0, 1, 1)),
        ("magenta", (1, 0, 1)),
        ("yellow", (1, 1, 0)),
        ("grey50", (0.5, 0.5, 0.5)),
        ("white", (1, 1, 1)),
    ]
    w = WIDTH // len(stripes)  # 240
    entries = []
    oog_total = 0
    for i, (name, rgb) in enumerate(stripes):
        code = rgb_to_ycbcr10(np.array(rgb, dtype=np.float64), BT709)
        c.fill(
            i * w, (i + 1) * w, 0, HEIGHT, (int(code[0]), int(code[1]), int(code[2]))
        )
        decoded = ycbcr10_to_rgb(code.astype(np.float64), BT709)
        lin_p3 = srgb_eotf(np.clip(decoded, 0.0, 1.0))
        lin_709 = _P3_TO_709 @ lin_p3
        outside = bool(
            np.any(lin_709 < -GAMUT_TOLERANCE)
            or np.any(lin_709 > 1.0 + GAMUT_TOLERANCE)
        )
        samples = w * HEIGHT if outside else 0
        oog_total += samples
        entries.append(
            {
                "name": name,
                "columns": [i * w, (i + 1) * w],
                "p3_rgb": list(rgb),
                "codes_ycbcr_limited": [int(v) for v in code],
                "linear_709_rgb": [round(float(v), 6) for v in lin_709],
                "outside_709": outside,
                "out_of_gamut_709_samples": samples,
            }
        )
    exp = {
        "pattern": "p3_saturation",
        "corpus_id": "syn_pat_p3_saturation",
        "width": WIDTH,
        "height": HEIGHT,
        "frames": FRAMES,
        "transfer": "IEC 61966-2-1 (sRGB)",
        "primaries": "Display P3 (D65)",
        "matrix": "BT.709",
        "range": "limited",
        "bit_depth": 10,
        "stripes": entries,
        "gamut_tolerance_linear": GAMUT_TOLERANCE,
        "out_of_gamut_709_samples": oog_total,
        "saturated_area_samples": oog_total,
        "saturated_area_definition": "pixels of the stripes whose linear BT.709 components fall outside [0, 1]",
    }
    return c, exp, Tags("smpte432", "iec61966-2-1", BT709.name)


def pattern_siting_checker() -> tuple[Canvas, dict, Tags]:
    """Flat luma with a 4:2:0 chroma checkerboard of one chroma sample (a 2x2 luma block) per cell."""
    c = Canvas()
    c.y[:, :] = 502
    hi, lo = 768, 256
    yy, xx = np.indices(c.cb.shape)
    odd = ((yy + xx) & 1).astype(bool)
    c.cb[:, :] = np.where(odd, hi, lo).astype(np.uint16)
    c.cr[:, :] = np.where(odd, lo, hi).astype(np.uint16)
    exp = {
        "pattern": "siting_checker",
        "corpus_id": "syn_pat_siting_checker",
        "width": WIDTH,
        "height": HEIGHT,
        "frames": FRAMES,
        "transfer": "BT.709",
        "primaries": "BT.709",
        "range": "limited",
        "bit_depth": 10,
        "luma_code": 502,
        "chroma_rule": "chroma sample (cx, cy): odd = (cx + cy) & 1; Cb = 768 if odd else 256; Cr = 256 if odd else 768",
        "chroma_values": {"high": hi, "low": lo},
        "chroma_planes_sha256": hashlib.sha256(
            c.cb.astype("<u2").tobytes() + c.cr.astype("<u2").tobytes()
        ).hexdigest(),
        "out_of_gamut_709_samples": 0,
        "saturated_area_samples": 0,
    }
    return c, exp, Tags("bt709", "bt709", BT709.name)


PATTERNS: dict[str, Callable[[], tuple[Canvas, dict, Tags]]] = {
    "pq_patches_1000": pattern_pq_patches,
    "hlg_patches": pattern_hlg_patches,
    "sdr709_ramp": pattern_sdr709_ramp,
    "p3_saturation": pattern_p3_saturation,
    "siting_checker": pattern_siting_checker,
}


# --------------------------------------------------------------------------------------------------------
# Encoding
# --------------------------------------------------------------------------------------------------------
class EncoderMismatch(Exception):
    pass


def _ffmpeg() -> str:
    exe = shutil.which("ffmpeg")
    if exe is None:
        sys.exit("gen_hdr_patterns.py: ffmpeg (with libx265) is required on PATH")
    return exe


X265_VERSION_RE = re.compile(r"x265 \[info\]: HEVC encoder version ([^\s]+)")


def encode_mp4(y4m: Path, mp4: Path, tags: Tags) -> str:
    """Lossless HEVC Main10 via ffmpeg's libx265 (the x265 library behind ``x265 --lossless``). Returns the
    x265 version string printed by the encoder."""
    cmd = [
        _ffmpeg(),
        "-hide_banner",
        "-nostdin",
        "-y",
        "-loglevel",
        "info",
        "-threads",
        "1",
        "-i",
        str(y4m),
        "-c:v",
        "libx265",
        "-pix_fmt",
        "yuv420p10le",
        "-tag:v",
        "hvc1",
        "-x265-params",
        "lossless=1:frame-threads=1:pools=none:log-level=info:keyint=3:repeat-headers=0",
        "-color_range",
        "tv",
        "-colorspace",
        tags.matrix,
        "-color_primaries",
        tags.primaries,
        "-color_trc",
        tags.trc,
        "-fflags",
        "+bitexact",
        "-flags:v",
        "+bitexact",
        "-map_metadata",
        "-1",
        "-an",
        str(mp4),
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if res.returncode != 0:
        raise RuntimeError(f"ffmpeg failed ({res.returncode}):\n{res.stderr[-2000:]}")
    m = X265_VERSION_RE.search(res.stderr)
    return m.group(1) if m else "unknown"


def decode_raw(mp4: Path) -> bytes:
    cmd = [
        _ffmpeg(),
        "-hide_banner",
        "-nostdin",
        "-loglevel",
        "error",
        "-i",
        str(mp4),
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p10le",
        "-",
    ]
    res = subprocess.run(cmd, capture_output=True, check=True)
    return res.stdout


def y4m_payload(data: bytes) -> bytes:
    """Concatenated raw planes of a Y4M produced by y4m_bytes (strips the stream header and FRAME markers)."""
    header_end = data.index(b"\n") + 1
    frame_size = WIDTH * HEIGHT * 3  # 2 bytes * 1.5 samples per pixel
    out = bytearray()
    pos = header_end
    while pos < len(data):
        assert data[pos : pos + 6] == b"FRAME\n"
        pos += 6
        out += data[pos : pos + frame_size]
        pos += frame_size
    return bytes(out)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dump_json(obj: object) -> str:
    return json.dumps(obj, indent=2, sort_keys=True) + "\n"


def generate(out: Path, names: list[str], encoder_pin: dict | None) -> dict[str, str]:
    """Write y4m, mp4 and expected.json for ``names`` into ``out``; returns {name: x265 version}."""
    out.mkdir(parents=True, exist_ok=True)
    versions: dict[str, str] = {}
    for name in names:
        canvas, expected, tags = PATTERNS[name]()
        y4m = out / f"{name}.y4m"
        mp4 = out / f"{name}.mp4"
        data = y4m_bytes(canvas)
        y4m.write_bytes(data)
        version = encode_mp4(y4m, mp4, tags)
        if encoder_pin is not None and version != encoder_pin["x265"]:
            mp4.unlink(missing_ok=True)
            raise EncoderMismatch(
                f"x265 {version} differs from the pinned {encoder_pin['x265']}"
            )
        versions[name] = version
        if decode_raw(mp4) != y4m_payload(
            data
        ):  # step 5 pitfall: the lossless round trip must be exact
            raise RuntimeError(
                f"{name}: decoded mp4 differs from the generated Y4M (not lossless)"
            )
        (out / f"{name}.expected.json").write_text(
            dump_json(expected), encoding="utf-8"
        )
    return versions


# --------------------------------------------------------------------------------------------------------
# Manifest
# --------------------------------------------------------------------------------------------------------
def manifest_entry(name: str, mp4: Path) -> dict:
    return {
        "id": f"syn_pat_{name}",
        "source": "syn",
        "licence": "Synthetic test pattern generated by tools/gen_hdr_patterns.py (BSD-2-Clause, OVT contributors)",
        "licence_spdx": "BSD-2-Clause",
        "redistributable": True,
        "sha256": sha256_file(mp4),
        "size_bytes": mp4.stat().st_size,
        "codecs": ["h265"],
        "containers": ["mp4"],
        "tests": ["render", "measure"],
        "tags": ["patterns", "fast"],
        "aliases": [f"{name}.mp4"],
    }


def update_manifest(manifest: Path, out: Path, names: list[str]) -> None:
    doc = json.loads(manifest.read_text(encoding="utf-8"))
    entries = doc["entries"]
    for name in names:
        new = manifest_entry(name, out / f"{name}.mp4")
        for i, e in enumerate(entries):
            if e["id"] == new["id"]:
                entries[i] = new
                break
        else:
            entries.append(new)
    manifest.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, default=REPO / "tests/corpora/patterns")
    ap.add_argument(
        "--manifest", type=Path, default=REPO / "tests/corpora/manifest.json"
    )
    ap.add_argument(
        "--no-manifest", action="store_true", help="do not touch the corpora manifest"
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="regenerate to a temp dir and compare with --out",
    )
    ap.add_argument(
        "--only",
        action="append",
        choices=sorted(PATTERNS),
        help="restrict to one pattern",
    )
    args = ap.parse_args(argv)
    names = args.only or list(PATTERNS)
    pin_path = args.out / "encoder.json"
    pin = (
        json.loads(pin_path.read_text(encoding="utf-8")) if pin_path.exists() else None
    )

    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            try:
                generate(Path(tmp), names, pin)
            except EncoderMismatch as exc:
                print(f"gen_hdr_patterns.py: {exc}", file=sys.stderr)
                return EXIT_ENCODER_MISMATCH
            bad = []
            for name in names:
                for suffix in (".mp4", ".expected.json"):
                    a, b = Path(tmp) / f"{name}{suffix}", args.out / f"{name}{suffix}"
                    if not b.exists() or a.read_bytes() != b.read_bytes():
                        bad.append(b.name)
            if not args.no_manifest and args.manifest.exists():
                doc = {
                    e["id"]: e
                    for e in json.loads(args.manifest.read_text(encoding="utf-8"))[
                        "entries"
                    ]
                }
                for name in names:
                    if doc.get(f"syn_pat_{name}") != manifest_entry(
                        name, Path(tmp) / f"{name}.mp4"
                    ):
                        bad.append(f"manifest:syn_pat_{name}")
            if bad:
                print(
                    "gen_hdr_patterns.py: differs: " + ", ".join(bad), file=sys.stderr
                )
                return 1
        print("gen_hdr_patterns.py: all patterns match")
        return 0

    try:
        versions = generate(args.out, names, pin)
    except EncoderMismatch as exc:
        print(f"gen_hdr_patterns.py: {exc}", file=sys.stderr)
        return EXIT_ENCODER_MISMATCH
    if pin is None:
        (args.out / "encoder.json").write_text(
            dump_json(
                {
                    "x265": min(versions.values()),
                    "driver": "ffmpeg libx265, lossless=1",
                }
            ),
            encoding="utf-8",
        )
    if not args.no_manifest:
        update_manifest(args.manifest, args.out, names)
    for name in names:
        print(f"{name}: {(args.out / (name + '.mp4')).stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
