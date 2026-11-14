#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reference parse of an HLS master or media playlist (chapter 8 WI-4.209).

Uses the `m3u8` PyPI package (pip install m3u8==6.0.0) for the line and attribute parsing, then does the
arithmetic the package leaves to the player: absolute URIs, byte-range offsets that continue from the previous
segment (RFC 8216 §4.3.2.2), running start times in microseconds, media sequence numbers and the init segments of
#EXT-X-MAP. It produces the expected JSON of core/tests/data/hls/*.expected.json that core/tests/container/
m3u8_test.cpp compares against.

Usage: tools/ref_m3u8.py <playlist.m3u8> --url <playlist url>   (JSON on stdout)
"""

import argparse
import json
import sys
from urllib.parse import urljoin

import m3u8


def micros(seconds):
    return round(float(seconds) * 1_000_000)


def parse_range(text):
    """'len[@off]' -> (len, off or None)."""
    length, _, offset = text.partition("@")
    return int(length), int(offset) if offset else None


def variant_json(playlist, is_iframe, base):
    info = playlist.iframe_stream_info if is_iframe else playlist.stream_info
    resolution = info.resolution or (0, 0)
    return {
        "uri": urljoin(base, playlist.uri),
        "is_iframe": is_iframe,
        "bandwidth": info.bandwidth or 0,
        "average_bandwidth": getattr(info, "average_bandwidth", None) or 0,
        "codecs": info.codecs or "",
        "width": resolution[0],
        "height": resolution[1],
        "frame_rate": float(info.frame_rate) if getattr(info, "frame_rate", None) else 0.0,
        "video_range": getattr(info, "video_range", None) or "",
    }


def master_json(playlist, base):
    variants = [variant_json(p, False, base) for p in playlist.playlists]
    variants += [variant_json(p, True, base) for p in playlist.iframe_playlists]
    renditions = [
        {
            "type": m.type,
            "group_id": m.group_id,
            "name": m.name,
            "language": m.language or "",
            "uri": urljoin(base, m.uri) if m.uri else "",
            "default": m.default == "YES",
            "autoselect": m.autoselect == "YES",
        }
        for m in playlist.media
    ]
    return {"kind": "master", "variants": variants, "renditions": renditions}


def media_json(playlist, base):
    segments = []
    time_us = 0
    number = playlist.media_sequence or 0
    last_url, last_end = None, 0
    init_key = None
    for seg in playlist.segments:
        init = seg.init_section
        if init is not None:
            key = (init.uri, init.byterange)
            if key != init_key:
                init_key = key
                length, offset = parse_range(init.byterange) if init.byterange else (0, 0)
                segments.append(
                    {
                        "url": urljoin(base, init.uri),
                        "offset": offset or 0,
                        "length": length,
                        "start_time": -1,
                        "duration": -1,
                        "number": -1,
                        "is_init": True,
                    }
                )
        url = urljoin(base, seg.uri)
        offset = length = 0
        if seg.byterange:
            length, off = parse_range(seg.byterange)
            if off is None:
                off = last_end if last_url == url else 0
            offset = off
            last_url, last_end = url, off + length
        else:
            last_url = None
        duration = micros(seg.duration)
        segments.append(
            {
                "url": url,
                "offset": offset,
                "length": length,
                "start_time": time_us,
                "duration": duration,
                "number": number,
                "is_init": False,
            }
        )
        time_us += duration
        number += 1
    return {
        "kind": "media",
        "media_sequence": playlist.media_sequence or 0,
        "target_duration": playlist.target_duration or 0,
        "end_list": bool(playlist.is_endlist),
        "segments": segments,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("playlist")
    ap.add_argument("--url", default="")
    args = ap.parse_args()
    with open(args.playlist, encoding="utf-8") as f:
        text = f.read()
    playlist = m3u8.loads(text)
    base = args.url
    result = master_json(playlist, base) if playlist.is_variant else media_json(playlist, base)
    json.dump(result, sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
