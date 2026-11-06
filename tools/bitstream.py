#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shared byte-level helpers of the §14.3.3 fixture mutators (chapter 14; WI-0.553).

Annex B NAL units (H.264 / H.265), emulation prevention, an Exp-Golomb bit reader and writer, AV1 ``leb128``
and IVF framing. Pure Python and deterministic; not a parser of record (the core's parsers are), only enough
to rewrite synthetic test streams.
"""

from __future__ import annotations

import re
import struct
from dataclasses import dataclass

START = re.compile(b"\x00\x00\x01")


@dataclass
class Nal:
    prefix: bytes  # the start code as found (3 or 4 bytes), kept so untouched units round-trip
    data: bytes  # header + EBSP payload


def split_annexb(data: bytes) -> list[Nal]:
    """Split an Annex B byte stream into NAL units; ``join_annexb`` restores the input byte-exactly."""
    starts = [m.start() for m in START.finditer(data)]
    if not starts:
        raise ValueError("no Annex B start code")
    bounds = []
    for i, pos in enumerate(starts):
        floor = starts[i - 1] + 3 if i else 0
        begin = pos
        while begin > floor and data[begin - 1] == 0:
            begin -= 1  # zero_byte / leading_zero_8bits / trailing_zero_8bits go with the next start code
        bounds.append((begin, pos + 3))
    bounds[0] = (0, bounds[0][1])
    ends = [b for b, _ in bounds[1:]] + [len(data)]
    return [Nal(data[b:p], data[p:e]) for (b, p), e in zip(bounds, ends)]


def join_annexb(nals: list[Nal]) -> bytes:
    return b"".join(n.prefix + n.data for n in nals)


def h264_type(nal: bytes) -> int:
    return nal[0] & 0x1F


def h265_type(nal: bytes) -> int:
    return (nal[0] >> 1) & 0x3F


def unescape(ebsp: bytes) -> bytes:
    return re.sub(b"\x00\x00\x03(?=[\x00-\x03])", b"\x00\x00", ebsp)


def escape(rbsp: bytes) -> bytes:
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


class BitReader:
    def __init__(self, data: bytes, pos: int = 0) -> None:
        self.data = data
        self.pos = pos  # in bits

    def u(self, n: int) -> int:
        v = 0
        for _ in range(n):
            if self.pos >= 8 * len(self.data):
                raise ValueError("read past the end of the payload")
            v = (v << 1) | ((self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return v

    def ue(self) -> int:
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
        return (1 << zeros) - 1 + self.u(zeros)

    def se(self) -> int:
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)

    def uvlc(self) -> int:  # AV1 §4.10.3
        zeros = 0
        while self.u(1) == 0:
            zeros += 1
        return 0xFFFFFFFF if zeros >= 32 else (1 << zeros) - 1 + self.u(zeros)

    def bits(self, start: int, end: int) -> str:
        """The bits [start, end) as a '0'/'1' string."""
        return "".join(str((self.data[i >> 3] >> (7 - (i & 7))) & 1) for i in range(start, end))


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[str] = []

    def u(self, n: int, v: int) -> None:
        self.bits.append(format(v, f"0{n}b") if n else "")

    def ue(self, v: int) -> None:
        code = format(v + 1, "b")
        self.bits.append("0" * (len(code) - 1) + code)

    def se(self, v: int) -> None:
        self.ue(2 * v - 1 if v > 0 else -2 * v)

    def raw(self, bits: str) -> None:
        self.bits.append(bits)

    def tobytes(self) -> bytes:
        s = "".join(self.bits)
        if len(s) % 8:
            raise ValueError("bit string is not byte aligned")
        return int(s, 2).to_bytes(len(s) // 8, "big") if s else b""


def payload_bits(rbsp: bytes) -> int:
    """Bit length of an RBSP without its ``rbsp_trailing_bits`` (position of the final 1 bit)."""
    for i in range(len(rbsp) - 1, -1, -1):
        if rbsp[i]:
            return 8 * i + 7 - (rbsp[i] & -rbsp[i]).bit_length() + 1
    raise ValueError("RBSP has no stop bit")


def with_trailing_bits(bits: str) -> bytes:
    bits += "1"
    bits += "0" * (-len(bits) % 8)
    return int(bits, 2).to_bytes(len(bits) // 8, "big")


def read_leb128(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    for i in range(8):
        b = data[pos + i]
        value |= (b & 0x7F) << (7 * i)
        if not b & 0x80:
            return value, pos + i + 1
    raise ValueError("leb128 longer than 8 bytes")


def leb128(value: int) -> bytes:
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        out.append(b | (0x80 if value else 0))
        if not value:
            return bytes(out)


@dataclass
class Obu:
    header: bytes  # obu_header (+ extension byte), with obu_has_size_field as found
    payload: bytes

    @property
    def type(self) -> int:
        return (self.header[0] >> 3) & 0xF

    def encode(self) -> bytes:
        if self.header[0] & 0x02:
            return self.header + leb128(len(self.payload)) + self.payload
        return self.header + self.payload


def split_obus(data: bytes) -> list[Obu]:
    obus, pos = [], 0
    while pos < len(data):
        hlen = 2 if data[pos] & 0x04 else 1
        header = data[pos : pos + hlen]
        pos += hlen
        if header[0] & 0x02:
            size, pos = read_leb128(data, pos)
        else:
            size = len(data) - pos
        obus.append(Obu(header, data[pos : pos + size]))
        pos += size
    return obus


def read_ivf(data: bytes) -> tuple[bytes, list[tuple[int, bytes]]]:
    """(32-byte file header, [(pts, temporal unit)])."""
    if data[:4] != b"DKIF":
        raise ValueError("not an IVF file")
    hlen = struct.unpack_from("<H", data, 6)[0]
    frames, pos = [], hlen
    while pos + 12 <= len(data):
        size, pts = struct.unpack_from("<IQ", data, pos)
        frames.append((pts, data[pos + 12 : pos + 12 + size]))
        pos += 12 + size
    return data[:hlen], frames


def write_ivf(header: bytes, frames: list[tuple[int, bytes]]) -> bytes:
    return header + b"".join(struct.pack("<IQ", len(tu), pts) + tu for pts, tu in frames)
