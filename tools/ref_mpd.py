#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reference expansion of the SegmentTemplate parts of a static DASH MPD (chapter 8 WI-4.208).

An independent implementation (Python standard library only: xml.etree, fractions, urllib) of the template and
timeline arithmetic of ISO/IEC 23009-1 §5.3.9.4 / §5.3.9.6, used to produce and check the expected segment lists
of core/tests/data/dash/*.expected.json. It covers SegmentTemplate with @duration or SegmentTimeline, BaseURL
chains, the Period start/duration rules and the template identifiers; SegmentList and SegmentBase are covered by
hand-computed expectations in core/tests/container/mpd_test.cpp.

SPEC-ASSUMPTION(#349): WI-4.208 names python-mpegdash as the reference; it only parses the MPD and does not
expand templates, so this script does the expansion with the stdlib instead.

Usage: tools/ref_mpd.py <manifest.mpd> [--url <manifest url>]   (JSON on stdout)
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from fractions import Fraction
from urllib.parse import urljoin

NS = "urn:mpeg:dash:schema:mpd:2011"
_DUR = re.compile(r"^P(?:(\d+)Y)?(?:(\d+)M)?(?:(\d+)D)?(?:T(?:(\d+)H)?(?:(\d+)M)?(?:(\d+(?:\.\d+)?)S)?)?$")


def local(tag):
    return tag.rsplit("}", 1)[-1]


def kids(node, name):
    return [c for c in node if local(c.tag) == name]


def kid(node, name):
    found = kids(node, name)
    return found[0] if found else None


def duration_ms(text):
    m = _DUR.match(text.strip())
    if not m:
        raise ValueError("bad duration " + text)
    y, mo, d, h, mi, s = m.groups()
    seconds = (Fraction(y or 0) * 365 + Fraction(mo or 0) * 30 + Fraction(d or 0)) * 86400
    seconds += Fraction(h or 0) * 3600 + Fraction(mi or 0) * 60 + Fraction(s or 0)
    return round(seconds * 1000)


def expand(template, rep_id, number, bandwidth, time):
    def sub(m):
        name, fmt = m.group(1), m.group(2)
        if name == "":
            return "$"
        if name == "RepresentationID":
            return rep_id
        value = {"Number": number, "Bandwidth": bandwidth, "Time": time}[name]
        width = int(fmt[2:-1]) if fmt else 1
        return str(value).zfill(width)

    return re.sub(r"\$(|RepresentationID|Number|Bandwidth|Time)(%0\d+d)?\$", sub, template)


def ceil_div(a, b):
    return -((-a) // b)


def timeline(tl, start_number, end_media):
    entries = []
    items = kids(tl, "S")
    cursor, number = 0, start_number
    for i, s in enumerate(items):
        d = int(s.get("d"))
        if s.get("t") is not None:
            cursor = int(s.get("t"))
        if s.get("n") is not None:
            number = int(s.get("n"))
        r = int(s.get("r", "0"))
        if r < 0:
            nxt = items[i + 1].get("t") if i + 1 < len(items) else None
            end = int(nxt) if nxt is not None else end_media
            count = max(ceil_div(end - cursor, d), 0)
        else:
            count = r + 1
        for _ in range(count):
            entries.append((cursor, d, number))
            cursor += d
            number += 1
    return entries


def expand_mpd(path, url=""):
    root = ET.parse(path).getroot()
    base0 = url
    if (b := kid(root, "BaseURL")) is not None:
        base0 = urljoin(url, b.text.strip())
    total = duration_ms(root.get("mediaPresentationDuration")) if root.get("mediaPresentationDuration") else None
    periods = kids(root, "Period")
    starts, durs = [], []
    for i, p in enumerate(periods):
        start = duration_ms(p.get("start")) if p.get("start") else (starts[i - 1] + durs[i - 1] if i else 0)
        starts.append(start)
        durs.append(duration_ms(p.get("duration")) if p.get("duration") else None)
        if i and durs[i - 1] is None:
            durs[i - 1] = start - starts[i - 1]
    if periods and durs[-1] is None and total is not None:
        durs[-1] = total - starts[-1]

    out = {"periods": []}
    for p, start, dur in zip(periods, starts, durs):
        pbase = base0
        if (b := kid(p, "BaseURL")) is not None:
            pbase = urljoin(pbase, b.text.strip())
        po = {"id": p.get("id", ""), "start_ms": start, "duration_ms": dur, "adaptation_sets": []}
        for a in kids(p, "AdaptationSet"):
            abase = pbase
            if (b := kid(a, "BaseURL")) is not None:
                abase = urljoin(abase, b.text.strip())
            ao = {"id": a.get("id", ""), "representations": []}
            for r in kids(a, "Representation"):
                rbase = abase
                if (b := kid(r, "BaseURL")) is not None:
                    rbase = urljoin(rbase, b.text.strip())
                rep_id, bw = r.get("id", ""), int(r.get("bandwidth", "0"))
                st = kid(r, "SegmentTemplate")
                if st is None:
                    st = kid(a, "SegmentTemplate")
                if st is None:
                    st = kid(p, "SegmentTemplate")
                segs, timescale = [], 1
                if st is not None:
                    timescale = int(st.get("timescale", "1"))
                    pto = int(st.get("presentationTimeOffset", "0"))
                    start_number = int(st.get("startNumber", "1"))
                    if st.get("initialization"):
                        segs.append(
                            {
                                "url": urljoin(rbase, expand(st.get("initialization"), rep_id, 0, bw, 0)),
                                "number": -1,
                                "start_time": -1,
                                "duration": -1,
                                "init": True,
                            }
                        )
                    tl = kid(st, "SegmentTimeline")
                    if tl is not None:
                        entries = timeline(tl, start_number, pto + dur * timescale // 1000)
                    else:
                        d = int(st.get("duration"))
                        count = ceil_div(dur * timescale, d * 1000)
                        entries = [(pto + k * d, d, start_number + k) for k in range(count)]
                    for t, d, n in entries:
                        segs.append(
                            {
                                "url": urljoin(rbase, expand(st.get("media"), rep_id, n, bw, t)),
                                "number": n,
                                "start_time": t,
                                "duration": d,
                                "init": False,
                            }
                        )
                ao["representations"].append({"id": rep_id, "timescale": timescale, "segments": segs})
            po["adaptation_sets"].append(ao)
        out["periods"].append(po)
    return out


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("mpd")
    ap.add_argument("--url", default="")
    args = ap.parse_args(argv)
    json.dump(expand_mpd(args.mpd, args.url), sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
