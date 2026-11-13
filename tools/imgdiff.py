#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Screenshot and render-parity image comparison (chapter 14 section 14.10.2 and 14.4.15; WI-0.568).

    imgdiff.py --mode ui         a.png b.png [--diff out.png]
    imgdiff.py --mode hdr-parity a.png b.png [--diff out.png] [--p99-max 1.0] [--mean-max X]

Exit status: 0 = the images match, 1 = they differ beyond the tolerance, 2 = usage or input error.

``ui`` (GUI screenshots, 14.10.2): passes when the SSIM of the grey images (7x7 window) is at least 0.995 AND no 8x8
tile has a mean absolute difference above 12/255. The SSIM tolerates sub-pixel anti-aliasing noise; the tile rule
catches a small layout break that barely moves the global SSIM.

``hdr-parity`` (renderer against mpv, 14.4.15): both images are converted to linear light in nits (16-bit PNG: ST 2084
EOTF, BT.2020 primaries; 8-bit PNG: BT.1886 with 203 nits for white, BT.709 primaries unless ``--sdr-primaries
bt2020``), and compared per pixel with Delta E ITP (ITU-R BT.2124). It reports mean, p99 and max; it passes when
``p99 <= --p99-max`` (default 1.0) and, if given, ``mean <= --mean-max``.

The PNG reader and writer are built in (zlib + NumPy) because Pillow cannot decode 16-bit RGB PNGs. scikit-image is
needed for the ``ui`` SSIM only (tools/requirements-test.txt).
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

SSIM_MIN = 0.995  # 14.10.2
SSIM_WINDOW = 7
TILE = 8
TILE_MAX_DELTA = 12.0  # of 255
SDR_WHITE_NITS = 203.0
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

EXIT_OK = 0
EXIT_DIFFERENT = 1
EXIT_ERROR = 2


class ImageError(Exception):
    """An input that cannot be read or compared."""


# --------------------------------------------------------------------------------------------------------------
# PNG codec
# --------------------------------------------------------------------------------------------------------------


def _unfilter(raw: bytes, height: int, row_bytes: int, bpp: int) -> np.ndarray:
    """Undo the PNG scanline filters; returns a (height, row_bytes) uint8 array."""
    out = np.zeros((height, row_bytes), dtype=np.uint8)
    prev = np.zeros(row_bytes, dtype=np.int32)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        line = np.frombuffer(raw, dtype=np.uint8, count=row_bytes, offset=pos + 1).astype(np.int32)
        pos += row_bytes + 1
        if ftype == 0:
            cur = line
        elif ftype == 2:  # Up
            cur = (line + prev) & 0xFF
        elif ftype == 1:  # Sub: a running sum per byte lane
            cur = line.copy()
            for i in range(bpp):
                cur[i::bpp] = np.cumsum(line[i::bpp]) & 0xFF
        elif ftype == 3:  # Average: sequential dependency on the left neighbour
            cur = line.copy()
            for x in range(row_bytes):
                left = cur[x - bpp] if x >= bpp else 0
                cur[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            cur = line.copy()
            for x in range(row_bytes):
                a = cur[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[x] = (line[x] + pred) & 0xFF
        else:
            raise ImageError(f"unknown PNG filter type {ftype}")
        out[y] = cur
        prev = cur
    return out


def read_png(path: Path) -> tuple[np.ndarray, int]:
    """Read a non-interlaced PNG into an (H, W, C) array and its bit depth (8 or 16). C is 1, 3 or 4.

    Palette images are expanded to RGB. 16-bit samples are returned as uint16, 8-bit as uint8.
    """
    data = Path(path).read_bytes()
    if data[:8] != PNG_SIGNATURE:
        raise ImageError(f"{path}: not a PNG file")
    pos = 8
    ihdr = None
    palette = None
    idat = bytearray()
    while pos + 8 <= len(data):
        length, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            palette = np.frombuffer(body, dtype=np.uint8).reshape(-1, 3)
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    if ihdr is None:
        raise ImageError(f"{path}: no IHDR chunk")
    width, height, depth, color, _comp, _filt, interlace = ihdr
    if interlace != 0:
        raise ImageError(f"{path}: interlaced PNG is not supported")
    if depth not in (8, 16) and color != 3:
        raise ImageError(f"{path}: bit depth {depth} is not supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise ImageError(f"{path}: unknown PNG colour type {color}")
    bits = depth * channels
    row_bytes = (width * bits + 7) // 8
    raw = zlib.decompress(bytes(idat))
    if len(raw) != height * (row_bytes + 1):
        raise ImageError(f"{path}: truncated image data")
    lines = _unfilter(raw, height, row_bytes, max(1, bits // 8))
    if color == 3:
        if palette is None:
            raise ImageError(f"{path}: palette image without PLTE")
        if depth == 8:
            idx = lines
        else:
            per = 8 // depth
            shifts = np.arange(per - 1, -1, -1) * depth
            idx = ((lines[:, :, None] >> shifts) & ((1 << depth) - 1)).reshape(height, -1)[:, :width]
        return palette[idx], 8
    if depth == 8:
        pix = lines.reshape(height, width, channels)
        pix = pix[:, :, :]
    else:
        pix = lines.reshape(height, width * channels, 2)
        pix = ((pix[:, :, 0].astype(np.uint16) << 8) | pix[:, :, 1]).reshape(height, width, channels)
    if channels == 2:  # grey + alpha: drop alpha
        pix = pix[:, :, :1]
    return pix, depth


def write_png(path: Path, pix: np.ndarray) -> None:
    """Write an (H, W, 3) uint8 array as an RGB PNG (compression level 9, filter 0, deterministic)."""
    height, width, channels = pix.shape
    if channels != 3 or pix.dtype != np.uint8:
        raise ImageError("write_png takes an (H, W, 3) uint8 array")
    raw = b"".join(b"\x00" + pix[y].tobytes() for y in range(height))

    def chunk(ctype: bytes, body: bytes) -> bytes:
        crc = zlib.crc32(ctype + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + ctype + body + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    Path(path).write_bytes(
        PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    )


def _to_rgb8(pix: np.ndarray, depth: int) -> np.ndarray:
    if depth == 16:
        pix = (pix.astype(np.uint32) + 128) // 257  # exact 16 -> 8 bit rounding
        pix = pix.astype(np.uint8)
    if pix.shape[2] == 1:
        pix = np.repeat(pix, 3, axis=2)
    return pix[:, :, :3]


# --------------------------------------------------------------------------------------------------------------
# ui mode
# --------------------------------------------------------------------------------------------------------------


def _grey(rgb: np.ndarray) -> np.ndarray:
    return rgb[:, :, 0] * 0.299 + rgb[:, :, 1] * 0.587 + rgb[:, :, 2] * 0.114


def _tile_means(absdiff: np.ndarray) -> np.ndarray:
    """Mean |delta| over each 8x8 tile (edge tiles are smaller); absdiff is (H, W)."""
    h, w = absdiff.shape
    ph, pw = (-h) % TILE, (-w) % TILE
    padded = np.pad(absdiff, ((0, ph), (0, pw)))
    ones = np.pad(np.ones_like(absdiff), ((0, ph), (0, pw)))
    sums = padded.reshape((h + ph) // TILE, TILE, (w + pw) // TILE, TILE).sum(axis=(1, 3))
    counts = ones.reshape((h + ph) // TILE, TILE, (w + pw) // TILE, TILE).sum(axis=(1, 3))
    return sums / counts


def compare_ui(a: np.ndarray, b: np.ndarray) -> dict:
    try:
        from skimage.metrics import structural_similarity
    except ImportError as err:  # pragma: no cover - exercised only without the test requirements
        raise ImageError("scikit-image is required for --mode ui (pip install -r tools/requirements-test.txt)") from err
    ssim = float(structural_similarity(_grey(a), _grey(b), win_size=SSIM_WINDOW, data_range=255.0))
    absdiff = np.abs(a.astype(np.float64) - b.astype(np.float64)).mean(axis=2)
    tiles = _tile_means(absdiff)
    worst = float(tiles.max())
    ty, tx = np.unravel_index(int(tiles.argmax()), tiles.shape)
    bad = int((tiles > TILE_MAX_DELTA).sum())
    return {
        "mode": "ui",
        "ssim": ssim,
        "max_tile_delta": worst,
        "worst_tile": [int(tx) * TILE, int(ty) * TILE],
        "bad_tiles": bad,
        "pass": ssim >= SSIM_MIN and bad == 0,
    }


def diff_image(a: np.ndarray, b: np.ndarray) -> np.ndarray:
    """Side by side: a | b | amplified absolute difference (x4, clipped), separated by 4 px white bars."""
    delta = np.clip(np.abs(a.astype(np.int32) - b.astype(np.int32)) * 4, 0, 255).astype(np.uint8)
    bar = np.full((a.shape[0], 4, 3), 255, dtype=np.uint8)
    return np.concatenate([a, bar, b, bar, delta], axis=1)


# --------------------------------------------------------------------------------------------------------------
# hdr-parity mode
# --------------------------------------------------------------------------------------------------------------

# ST 2084 constants
_M1 = 2610.0 / 16384.0
_M2 = 2523.0 / 4096.0 * 128.0
_C1 = 3424.0 / 4096.0
_C2 = 2413.0 / 4096.0 * 32.0
_C3 = 2392.0 / 4096.0 * 32.0

_BT709_TO_BT2020 = np.array(
    [[0.627404, 0.329283, 0.043313], [0.069097, 0.919540, 0.011362], [0.016391, 0.088013, 0.895595]]
)
_BT2020_TO_LMS = np.array([[1688.0, 2146.0, 262.0], [683.0, 2951.0, 462.0], [99.0, 309.0, 3688.0]]) / 4096.0


def pq_eotf(v: np.ndarray) -> np.ndarray:
    """ST 2084 EOTF: non-linear [0, 1] -> nits."""
    p = np.power(np.clip(v, 0.0, 1.0), 1.0 / _M2)
    return 10000.0 * np.power(np.maximum(p - _C1, 0.0) / (_C2 - _C3 * p), 1.0 / _M1)


def pq_oetf(nits: np.ndarray) -> np.ndarray:
    """Inverse of pq_eotf: nits -> non-linear [0, 1]."""
    y = np.power(np.clip(nits, 0.0, 10000.0) / 10000.0, _M1)
    return np.power((_C1 + _C2 * y) / (1.0 + _C3 * y), _M2)


def to_linear_nits(pix: np.ndarray, depth: int, sdr_primaries: str) -> np.ndarray:
    """(H, W, C) PNG samples -> (H, W, 3) linear BT.2020 RGB in nits."""
    rgb = pix
    if rgb.shape[2] == 1:
        rgb = np.repeat(rgb, 3, axis=2)
    rgb = rgb[:, :, :3].astype(np.float64) / float((1 << depth) - 1)
    if depth == 16:
        return pq_eotf(rgb)
    nits = SDR_WHITE_NITS * np.power(rgb, 2.4)  # BT.1886 with a zero black level
    if sdr_primaries == "bt709":
        nits = nits @ _BT709_TO_BT2020.T
    return nits


def delta_e_itp(nits_a: np.ndarray, nits_b: np.ndarray) -> np.ndarray:
    """Per-pixel Delta E ITP (BT.2124) between two linear BT.2020 RGB images in nits."""

    def itp(nits: np.ndarray) -> np.ndarray:
        lms = np.clip(nits @ _BT2020_TO_LMS.T, 0.0, None)
        lp, mp, sp = np.moveaxis(pq_oetf(lms), -1, 0)
        i = 0.5 * lp + 0.5 * mp
        t = (6610.0 * lp - 13613.0 * mp + 7003.0 * sp) / 4096.0
        p = (17933.0 * lp - 17390.0 * mp - 543.0 * sp) / 4096.0
        return np.stack([i, t, p], axis=-1)

    d = itp(nits_a) - itp(nits_b)
    return 720.0 * np.sqrt(d[..., 0] ** 2 + (0.5 * d[..., 1]) ** 2 + d[..., 2] ** 2)


def compare_hdr(
    a: tuple[np.ndarray, int], b: tuple[np.ndarray, int], p99_max: float, mean_max: float | None, sdr_primaries: str
) -> tuple[dict, np.ndarray]:
    de = delta_e_itp(to_linear_nits(*a, sdr_primaries), to_linear_nits(*b, sdr_primaries))
    p99 = float(np.percentile(de, 99.0))
    mean = float(de.mean())
    ok = p99 <= p99_max and (mean_max is None or mean <= mean_max)
    return {
        "mode": "hdr-parity",
        "mean_delta_e_itp": mean,
        "p99_delta_e_itp": p99,
        "max_delta_e_itp": float(de.max()),
        "pass": ok,
    }, de


# --------------------------------------------------------------------------------------------------------------
# command line
# --------------------------------------------------------------------------------------------------------------


def _heat(de: np.ndarray) -> np.ndarray:
    """A grey heat map of Delta E ITP: 1.0 and above is white."""
    return np.repeat(np.clip(de * 255.0, 0, 255).astype(np.uint8)[:, :, None], 3, axis=2)


def run(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], prog="imgdiff.py")
    ap.add_argument("--mode", choices=("ui", "hdr-parity"), required=True)
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--diff", type=Path, help="write a side-by-side diff PNG here (always on a mismatch if given)")
    ap.add_argument("--p99-max", type=float, default=1.0, help="hdr-parity: p99 Delta E ITP limit (default 1.0)")
    ap.add_argument("--mean-max", type=float, default=None, help="hdr-parity: mean Delta E ITP limit (default none)")
    ap.add_argument(
        "--sdr-primaries",
        choices=("bt709", "bt2020"),
        default="bt709",
        help="hdr-parity: primaries of 8-bit inputs (default bt709)",
    )
    ap.add_argument("--json", action="store_true", help="print the result as one JSON object")
    args = ap.parse_args(argv)
    try:
        img_a, depth_a = read_png(args.a)
        img_b, depth_b = read_png(args.b)
        if img_a.shape[:2] != img_b.shape[:2]:
            raise ImageError(
                f"size mismatch: {args.a} is {img_a.shape[1]}x{img_a.shape[0]}, "
                f"{args.b} is {img_b.shape[1]}x{img_b.shape[0]}"
            )
        if args.mode == "ui":
            rgb_a, rgb_b = _to_rgb8(img_a, depth_a), _to_rgb8(img_b, depth_b)
            result = compare_ui(rgb_a, rgb_b)
            panel = diff_image(rgb_a, rgb_b)
        else:
            result, de = compare_hdr(
                (img_a, depth_a), (img_b, depth_b), args.p99_max, args.mean_max, args.sdr_primaries
            )
            panel = diff_image(_to_rgb8(img_a, depth_a), _to_rgb8(img_b, depth_b))
            panel[:, -img_a.shape[1] :] = _heat(de)
        if args.diff is not None and not result["pass"]:
            write_png(args.diff, panel)
            result["diff"] = str(args.diff)
    except (ImageError, OSError, zlib.error) as err:
        print(f"imgdiff: {err}", file=sys.stderr)
        return EXIT_ERROR
    if args.json:
        print(json.dumps(result, sort_keys=True))
    elif args.mode == "ui":
        print(
            f"ssim {result['ssim']:.5f} (min {SSIM_MIN}), max tile delta {result['max_tile_delta']:.2f}/255 "
            f"(max {TILE_MAX_DELTA:.0f}) at {result['worst_tile']}, bad tiles {result['bad_tiles']}: "
            f"{'PASS' if result['pass'] else 'FAIL'}"
        )
    else:
        print(
            f"mean dE_ITP {result['mean_delta_e_itp']:.4f}  p99 dE_ITP {result['p99_delta_e_itp']:.4f}  "
            f"max dE_ITP {result['max_delta_e_itp']:.4f}: {'PASS' if result['pass'] else 'FAIL'}"
        )
    return EXIT_OK if result["pass"] else EXIT_DIFFERENT


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
