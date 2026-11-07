#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write a WebM whose audio SimpleBlocks use Xiph lacing (§14.3.3 recipe 71; WI-0.553).

    make_mkv_lacing.py --lacing xiph --frames-per-block N --input in.webm out.webm

mkvmerge and FFmpeg choose lacing themselves, so this pure-Python EBML writer re-muxes the Opus packets of the
first audio track of ``in.webm`` (made by the recipe with FFmpeg's libopus from ``sine``) into one
``SimpleBlock`` per N packets, laced with Xiph lacing (Matroska §10.3; flags bits 1-2 = 01). A trimmed last
packet goes alone into a BlockGroup with its ``DiscardPadding``, since end trimming applies to a whole block.
``OpusHead``, ``CodecDelay``, ``SeekPreRoll``, sample rate and channel count are copied; one Cluster per 32
blocks. No date, UID or app version is written, so the bytes depend only on the packets. Decoding the output
gives the same PCM as the input. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# Element ids (Matroska / WebM)
EBML, DOCTYPE, DOCTYPE_VERSION, DOCTYPE_READ = 0x1A45DFA3, 0x4282, 0x4287, 0x4285
SEGMENT, INFO, TIMESCALE, MUXING_APP, WRITING_APP, DURATION = 0x18538067, 0x1549A966, 0x2AD7B1, 0x4D80, 0x5741, 0x4489
TRACKS, TRACK_ENTRY, TRACK_NUMBER, TRACK_UID, TRACK_TYPE, CODEC_ID = 0x1654AE6B, 0xAE, 0xD7, 0x73C5, 0x83, 0x86
CODEC_PRIVATE, CODEC_DELAY, SEEK_PRE_ROLL, AUDIO, SAMPLING, CHANNELS = 0x63A2, 0x56AA, 0x56BB, 0xE1, 0xB5, 0x9F
CLUSTER, TIMECODE, SIMPLE_BLOCK, BLOCK_GROUP, BLOCK = 0x1F43B675, 0xE7, 0xA3, 0xA0, 0xA1
DISCARD_PADDING = 0x75A2
MASTERS = {SEGMENT, INFO, TRACKS, TRACK_ENTRY, AUDIO, CLUSTER, BLOCK_GROUP}
TIMECODE_SCALE = 1_000_000  # ns per tick


# --- reader ---------------------------------------------------------------------------------------------------


def read_vint(data: bytes, pos: int, keep_marker: bool) -> tuple[int, int]:
    first = data[pos]
    length = 9 - first.bit_length()
    if length > 8:
        raise ValueError(f"bad EBML vint at {pos}")
    value = first if keep_marker else first & (0xFF >> length)
    for b in data[pos + 1 : pos + length]:
        value = value << 8 | b
    return value, pos + length


def elements(data: bytes, start: int, end: int):
    pos = start
    while pos < end:
        eid, pos = read_vint(data, pos, True)
        size_at = pos
        size, pos = read_vint(data, pos, False)
        unknown = size == (1 << (7 * (pos - size_at))) - 1
        stop = end if unknown else min(pos + size, end)
        yield eid, pos, stop
        pos = stop


def read_opus(data: bytes) -> tuple[dict, list[bytes]]:
    track: dict = {}
    packets: list[bytes] = []

    def walk(start: int, end: int) -> None:
        for eid, a, b in elements(data, start, end):
            if eid in MASTERS:
                walk(a, b)
            elif eid in (CODEC_ID, CODEC_PRIVATE, CODEC_DELAY, SEEK_PRE_ROLL, SAMPLING, CHANNELS, TRACK_NUMBER):
                track.setdefault(eid, data[a:b])
            elif eid == DISCARD_PADDING:
                track[DISCARD_PADDING] = data[a:b]  # of the last packet that has one
            elif eid in (SIMPLE_BLOCK, BLOCK):
                number, p = read_vint(data, a, False)
                if number != 1 or (data[p + 2] >> 1) & 3:
                    raise ValueError("input must have one track and unlaced blocks")
                packets.append(data[p + 3 : b])

    walk(0, len(data))
    if track.get(CODEC_ID) != b"A_OPUS" or not packets:
        raise ValueError("no Opus track in the input")
    return track, packets


# --- writer ---------------------------------------------------------------------------------------------------


def vint_id(eid: int) -> bytes:
    return eid.to_bytes((eid.bit_length() + 7) // 8, "big")


def vint_size(size: int) -> bytes:
    length = 1
    while size >= (1 << (7 * length)) - 1:
        length += 1
    return (size | 1 << (7 * length)).to_bytes(length, "big")


def el(eid: int, *body: bytes) -> bytes:
    payload = b"".join(body)
    return vint_id(eid) + vint_size(len(payload)) + payload


def uint(eid: int, value: int) -> bytes:
    return el(eid, value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big"))


def xiph_block(timecode: int, frames: list[bytes]) -> bytes:
    lace = bytes([len(frames) - 1])
    for f in frames[:-1]:
        lace += b"\xff" * (len(f) // 255) + bytes([len(f) % 255])
    return el(SIMPLE_BLOCK, b"\x81", struct.pack(">hB", timecode, 0x80 | 0x02), lace, *frames)


def trimmed_block(timecode: int, frame: bytes, discard: bytes) -> bytes:
    """The stream's last packet alone in a BlockGroup with DiscardPadding (end trimming applies per block)."""
    return el(BLOCK_GROUP, el(BLOCK, b"\x81", struct.pack(">hB", timecode, 0), frame), el(DISCARD_PADDING, discard))


def packet_ms(packet: bytes) -> int:
    """Duration of an Opus packet from its TOC byte (RFC 6716 §3.1), which must be whole milliseconds."""
    config, code = packet[0] >> 3, packet[0] & 3
    frame = (
        [10, 20, 40, 60][config % 4]
        if config < 12
        else [10, 20][config % 2]
        if config < 16
        else [2.5, 5, 10, 20][config % 4]
    )
    count = 1 if code == 0 else 2 if code < 3 else packet[1] & 0x3F
    if count == 0 or (frame * count) % 1:
        raise ValueError("Opus packets must last whole milliseconds")
    return int(frame * count)


def write_webm(track: dict, packets: list[bytes], per_block: int) -> bytes:
    durations = {packet_ms(p) for p in packets}
    if len(durations) != 1:
        raise ValueError("Opus packets of different durations")
    ms = durations.pop()
    discard = track.get(DISCARD_PADDING)
    laced = packets[:-1] if discard else packets
    blocks = [laced[i : i + per_block] for i in range(0, len(laced), per_block)]
    clusters = []
    for c in range(0, len(blocks), 32):
        start = c * per_block * ms
        body = [uint(TIMECODE, start)]
        body += [xiph_block((c + k) * per_block * ms - start, b) for k, b in enumerate(blocks[c : c + 32])]
        if discard and c + 32 >= len(blocks):
            body.append(trimmed_block(len(laced) * ms - start, packets[-1], discard))
        clusters.append(el(CLUSTER, *body))
    audio = el(AUDIO, el(SAMPLING, track[SAMPLING]), el(CHANNELS, track[CHANNELS]))
    entry = [uint(TRACK_NUMBER, 1), uint(TRACK_UID, 1), uint(TRACK_TYPE, 2), el(CODEC_ID, b"A_OPUS")]
    entry += [el(CODEC_PRIVATE, track[CODEC_PRIVATE])]
    entry += [el(eid, track[eid]) for eid in (CODEC_DELAY, SEEK_PRE_ROLL) if eid in track]
    info = el(
        INFO,
        uint(TIMESCALE, TIMECODE_SCALE),
        el(MUXING_APP, b"ovt make_mkv_lacing"),
        el(WRITING_APP, b"ovt make_mkv_lacing"),
        el(DURATION, struct.pack(">d", float(len(packets) * ms))),
    )
    header = el(
        EBML,
        uint(0x4286, 1),
        uint(0x42F7, 1),
        uint(0x42F2, 4),
        uint(0x42F3, 8),
        el(DOCTYPE, b"webm"),
        uint(DOCTYPE_VERSION, 4),
        uint(DOCTYPE_READ, 2),
    )
    return header + el(SEGMENT, info, el(TRACKS, el(TRACK_ENTRY, audio, *entry)), *clusters)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--lacing", choices=("xiph",), required=True)
    ap.add_argument("--frames-per-block", type=int, required=True)
    ap.add_argument("--input", type=Path, required=True, help="WebM with one unlaced Opus track")
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    if not 2 <= args.frames_per_block <= 256:
        ap.error("--frames-per-block must be 2..256")
    try:
        track, packets = read_opus(args.input.read_bytes())
        args.output.write_bytes(write_webm(track, packets, args.frames_per_block))
    except (ValueError, IndexError, KeyError, OSError) as e:
        print(f"make_mkv_lacing: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
