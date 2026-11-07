#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the H.264/HEVC oracle goldens (chapter 14 §14.4.1-14.4.4, WI-0.567).

    python -I tools/goldens/gen_h26x.py --manifest tests/corpora/manifest.json --out tests/golden/ [--only ID[,ID...]]
        [--corpora-dir DIR] [--oracles trace_headers,gstparse,jm,hm,mvs] [--generate-syn] [--jobs N]
        [--work-dir DIR] [--keep-going] [--list]

Runs inside the oracle image (chapter 14 §14.2.6; ``ffmpeg``, ``gst_parse_oracle``, ``ldecod``, ``hm_cu_dump``
and ``ffmpeg_mvs`` on ``PATH``, each overridable with ``$OVT_ORACLE_<NAME>``). For every ``fast`` H.264/HEVC
manifest entry (plus ``syn_h264_epb_dense``) it writes, below ``--out``:

    h264/<id>.trace_headers.json.gz  h264/<id>.gstparse.json.gz  h264/<id>.jm-trace.json.gz  h264/<id>.ffmpeg-mvs.json.gz
    h265/<id>.trace_headers.json.gz  h265/<id>.gstparse.json.gz  h265/<id>.hm-grid.json.gz
    epb/syn_h264_epb_dense.json   h26x-index.json

Every oracle reads the same Annex B elementary stream: the corpus file itself, or the video track copied out of
its container with ``-bsf:v h264_mp4toannexb|hevc_mp4toannexb``. The stream is cut before the picture that would
exceed the per-oracle picture budget (``HEADER_PICTURES`` for the header oracles, ``BLOCK_PICTURES`` for the block
oracles) so that no golden passes ``--max-bytes``; a golden that is still too large is retried with half the
pictures and finally skipped, with the reason in ``h26x-index.json``. Step 5 (``h26x_selfcheck.py``) compares the
two header oracles on every stream. Exit status: 0 all done, 1 a step failed (unless listed in ``h26x_skips.json``
or ``--keep-going``), 2 usage, 4 a tool or corpus is missing.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
NORM = REPO / "tests" / "oracles" / "normalize"
SYNTH = REPO / "tests" / "corpora" / "synthetic" / "generate.py"
sys.path.insert(0, str(HERE))
import h26x_selfcheck  # noqa: E402

ALL_ORACLES = ("trace_headers", "gstparse", "jm", "hm", "mvs")
HEADER_PICTURES = 6
BLOCK_PICTURES = 2
MAX_BYTES = 900_000  # header goldens; CLAUDE.md forbids goldens over 2 MB, and a PR carries 100+ of them
MAX_BLOCK_BYTES = {"jm": 1_900_000, "mvs": 1_000_000, "hm": 1_000_000}  # one 1080p picture of JM output is 1.2 MB
CONTAINER_PACKETS = 16  # packets copied out of a container: enough for HEADER_PICTURES whatever the GOP
EPB_ID = "syn_h264_epb_dense"
SKIP_CONTAINERS = {
    "heif": "an image item, not a video track",
    "hls": "a playlist, not a stream",
}
SKIP_IDS = {
    r"cenc": "encrypted samples: the slice data is not decodable and the headers repeat the clear ones",
    r"hev1_clg1_(header|segment)$": "an initialisation or media segment on its own: no sample description or no samples",
}
PROBE = ["-probesize", "50M", "-analyzeduration", "50M"]  # TS and Matroska need more than the default to know the size
EXIT_OK, EXIT_STEP, EXIT_USAGE, EXIT_MISSING = 0, 1, 2, 4

CODEC_INFO = {
    "h264": {"bsf": "h264_mp4toannexb", "fmt": "h264", "ext": "264", "area": "h264"},
    "h265": {"bsf": "hevc_mp4toannexb", "fmt": "hevc", "ext": "265", "area": "h265"},
}
# NAL unit types that start a new access unit before its first slice (H.264 6-9, 14, 15; HEVC 32-35, 39, 41-44, 48-55)
AU_PREFIX = {
    "h264": {6, 7, 8, 9, 14, 15},
    "h265": {32, 33, 34, 35, 39, 41, 42, 43, 44, *range(48, 56)},
}


class StepError(Exception):
    """One oracle step of one entry failed."""


class NoStream(StepError):
    """The corpus file has no stream of the manifest's codec (the manifest names the file's family, not a track)."""


# ------------------------------------------------------------------------------------------------ selection


def entry_codec(entry: dict[str, Any]) -> str | None:
    for codec in entry.get("codecs") or []:
        if codec in CODEC_INFO:
            return codec
    m = re.match(r"syn_(h264|h265)", entry["id"])
    return m.group(1) if m else None


def select(manifest: dict[str, Any], only: list[str] | None) -> tuple[list[dict[str, Any]], dict[str, str]]:
    """The entries to process and the ones skipped (id -> reason)."""
    chosen, skipped = [], {}
    for entry in manifest["entries"]:
        if only is not None:
            if entry["id"] not in only:
                continue
        elif "fast" not in entry.get("tags", []) and entry["id"] != EPB_ID:
            continue
        if entry_codec(entry) is None:
            continue
        reason = next((r for pat, r in SKIP_IDS.items() if re.search(pat, entry["id"])), None)
        if entry.get("unverified") and entry.get("source") != "syn":
            reason = reason or "no recorded hash: tools/fetch_corpora.py cannot fetch it"
        for container in entry.get("containers") or []:
            reason = reason or SKIP_CONTAINERS.get(container)
        if reason and only is None:
            skipped[entry["id"]] = reason
        else:
            chosen.append(entry)
    if only is not None:
        missing = set(only) - {e["id"] for e in chosen} - set(skipped)
        if missing:
            raise StepError(f"not H.264/HEVC manifest entries: {', '.join(sorted(missing))}")
    return chosen, skipped


def oracles_for(entry: dict[str, Any], codec: str, wanted: set[str]) -> list[str]:
    out = ["trace_headers", "gstparse"]
    syn = entry["id"].startswith("syn_")
    if codec == "h264" and (entry.get("source") == "jvt" or syn):
        out += ["jm", "mvs"]
    if codec == "h265" and (entry.get("source") == "jctvc" or syn):
        out += ["hm"]
    return [o for o in out if o in wanted]


def corpus_file(corpora: Path, cid: str) -> Path:
    try:
        record = json.loads((corpora / cid / ".ok").read_text(encoding="utf-8"))
        return corpora / cid / record["primary"]
    except (OSError, ValueError, KeyError) as exc:
        raise StepError(f"corpus {cid} is not in {corpora} (tools/fetch_corpora.py --ids {cid})") from exc


# ------------------------------------------------------------------------------------------------ tools


def tool(name: str) -> str:
    env = os.environ.get("OVT_ORACLE_" + name.upper())
    found = env or shutil.which(name)
    if not found:
        raise FileNotFoundError(name)
    return found


def run(
    cmd: list[str],
    cwd: Path,
    what: str,
    stdout: Any = subprocess.DEVNULL,
    stderr: Any = subprocess.PIPE,
) -> str:
    res = subprocess.run(  # noqa: S603
        cmd,
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=stdout,
        stderr=stderr,
        text=True,
        errors="replace",
        check=False,
        env={**os.environ, "SOURCE_DATE_EPOCH": "0", "TZ": "UTC"},
    )
    if res.returncode != 0:
        tail = (res.stderr or "").strip().splitlines()[-3:] if isinstance(res.stderr, str) else []
        # addresses in the messages differ from run to run; the index must not
        message = re.sub(r"0x[0-9a-f]{6,}", "0x…", " | ".join(tail))
        raise StepError(f"{what} exited {res.returncode}: {message}")
    return res.stderr if isinstance(res.stderr, str) else ""


def normalise(script: str, input_file: Path, args: list[str], cwd: Path) -> str:
    cmd = [sys.executable, "-I", str(NORM / script), str(input_file), *args]
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False)  # noqa: S603
    if res.returncode != 0:
        raise StepError(f"{script} exited {res.returncode}: {res.stderr.strip()[-400:]}")
    return res.stdout


# ------------------------------------------------------------------------------------------------ streams


def sniff(head: bytes) -> str:
    if head[4:8] == b"ftyp":
        return "isobmff"
    if head[:4] == b"\x1a\x45\xdf\xa3":
        return "matroska"
    if len(head) > 188 and head[0] == 0x47 and head[188] == 0x47:
        return "mpegts"
    if re.match(rb"(\x00)*\x01", head[:8]):
        return "annexb"
    raise StepError("unrecognised container (not Annex B, ISOBMFF, Matroska or MPEG-TS)")


def video_stream(src: Path, codec: str, work: Path) -> int:
    """Index of the first stream of ``src`` that is ``codec`` (Matroska and MP4 test files carry other codecs too)."""
    name = {"h264": "h264", "h265": "hevc"}[codec]
    out = subprocess.run(  # noqa: S603
        [tool("ffprobe"), "-v", "error", *PROBE, "-select_streams", "v", "-show_entries", "stream=index,codec_name",
         "-of", "csv=p=0", str(src)],
        cwd=work, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )  # fmt: skip
    for line in out.stdout.splitlines():
        index, _, codec_name = line.partition(",")
        if codec_name.strip() == name and index.strip().isdigit():
            return int(index)
    raise NoStream(f"no {name} video stream in {src.name}")


def to_annexb(src: Path, codec: str, work: Path) -> Path:
    """The Annex B elementary stream of ``src``: itself, or its H.26x video track copied out of the container."""
    with src.open("rb") as f:
        kind = sniff(f.read(512))
    if kind == "annexb":
        return src
    info = CODEC_INFO[codec]
    es = work / f"es.{info['ext']}"
    index = video_stream(src, codec, work)
    run(
        [tool("ffmpeg"), "-hide_banner", "-v", "error", "-y", *PROBE, "-i", str(src), "-map", f"0:{index}",
         "-c:v", "copy", "-frames:v", str(CONTAINER_PACKETS), "-bsf:v", info["bsf"], "-f", info["fmt"], str(es)],
        work, "ffmpeg (container to Annex B)",
    )  # fmt: skip
    return es


def nal_units(data: bytes) -> list[tuple[int, int, int]]:
    """``(start of the start code, first byte of the NAL unit, end)`` of every NAL unit of an Annex B stream."""
    starts = [m.start() for m in re.finditer(rb"\x00\x00\x01", data)]
    begins = [pos - 1 if pos > 0 and data[pos - 1] == 0 else pos for pos in starts]
    return [
        (begin, pos + 3, begins[n + 1] if n + 1 < len(begins) else len(data))
        for n, (begin, pos) in enumerate(zip(begins, starts))
    ]


def first_slice_of_picture(data: bytes, codec: str, nal_start: int) -> bool | None:
    """True for the first slice of a picture, False for another slice, None when the NAL unit is not a slice."""
    if nal_start >= len(data):
        return None
    if codec == "h264":
        if data[nal_start] & 0x1F not in (1, 2, 3, 4, 5) or nal_start + 1 >= len(data):
            return None
        return bool(data[nal_start + 1] & 0x80)  # first_mb_in_slice == 0 is ue(v) '1'
    nal_type = (data[nal_start] >> 1) & 0x3F
    if nal_type > 21 or 10 <= nal_type <= 15 or nal_start + 2 >= len(data):
        return None
    return bool(data[nal_start + 2] & 0x80)  # first_slice_segment_in_pic_flag


def nal_type(data: bytes, codec: str, nal_start: int) -> int:
    return data[nal_start] & 0x1F if codec == "h264" else (data[nal_start] >> 1) & 0x3F


def truncate(data: bytes, codec: str, pictures: int) -> tuple[bytes, int]:
    """Cut an Annex B stream before the access unit of picture ``pictures + 1``; returns (bytes, pictures kept)."""
    units = nal_units(data)
    count = 0
    for i, (_begin, nal, _end) in enumerate(units):
        if first_slice_of_picture(data, codec, nal) is not True:
            continue
        count += 1
        if count == pictures + 1:
            j = i
            while j > 0 and nal_type(data, codec, units[j - 1][1]) in AU_PREFIX[codec]:
                j -= 1
            return data[: units[j][0]], pictures
    return data, count


# ------------------------------------------------------------------------------------------------ oracles


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Entry:
    """The state of one manifest entry while its goldens are produced."""

    def __init__(self, entry: dict[str, Any], codec: str, es: Path, work: Path) -> None:
        self.entry, self.codec, self.es, self.work = entry, codec, es, work
        self.data = es.read_bytes()

    def stream(self, pictures: int) -> tuple[Path, int]:
        cut, kept = truncate(self.data, self.codec, pictures)
        path = self.work / f"cut{pictures}.{CODEC_INFO[self.codec]['ext']}"
        path.write_bytes(cut)
        return path, kept

    def trace_headers(self, es: Path, extra: list[str]) -> str:
        info = CODEC_INFO[self.codec]
        log = run(
            [tool("ffmpeg"), "-hide_banner", "-loglevel", "trace", "-f", info["fmt"], "-i", str(es), "-map", "0:v:0",
             "-c:v", "copy", "-bsf:v", "trace_headers", "-f", info["fmt"], "-y", os.devnull],
            self.work, "ffmpeg trace_headers",
        )  # fmt: skip
        path = self.work / "trace_headers.txt"
        path.write_text(log, encoding="utf-8", errors="replace")
        return normalise("trace_headers.py", path, ["--codec", self.codec, *extra], self.work)

    def gstparse(self, es: Path) -> str:
        path = self.work / "gstparse.jsonl"
        with path.open("w", encoding="utf-8") as out:
            # exit status 1 means "at least one unit failed to parse"; the units are still printed and the
            # self-check reports the failed ones
            res = subprocess.run(  # noqa: S603
                [tool("gst_parse_oracle"), "--codec", self.codec, "--annexb", str(es)],
                stdout=out, stderr=subprocess.PIPE, stdin=subprocess.DEVNULL, text=True, check=False,
            )  # fmt: skip
        if res.returncode not in (0, 1):
            raise StepError(f"gst_parse_oracle exited {res.returncode}: {res.stderr.strip()[-300:]}")
        return normalise("gstparse.py", path, ["--codec", self.codec], self.work)

    def jm(self, es: Path) -> str:
        run(
            [tool("ldecod"), "-p", f"InputFile={es}", "-p", "OutputFile=ldecod.yuv", "-p", "RefOffset=0", "-p",
             "POCScale=2", "-p", "WriteUV=1", "-p", "SilentDecode=1"],
            self.work, "ldecod", stdout=subprocess.DEVNULL,
        )  # fmt: skip
        trace = self.work / "trace_dec.txt"
        if not trace.is_file():
            raise StepError("ldecod wrote no trace_dec.txt (is the TRACE build first on PATH?)")
        return normalise("jm_trace.py", trace, [], self.work)

    def mvs(self, es: Path) -> str:
        csv_path = self.work / "mvs.csv"
        with csv_path.open("w", encoding="utf-8") as out:
            run([tool("ffmpeg_mvs"), str(es)], self.work, "ffmpeg_mvs", stdout=out)
        return normalise("ffmpeg_mvs.py", csv_path, [], self.work)

    def hm(self, es: Path) -> str:
        dump = self.work / "hm.json"
        with dump.open("w", encoding="utf-8") as out:
            run([tool("hm_cu_dump"), "-b", str(es)], self.work, "hm_cu_dump", stdout=out)
        return normalise("hm_grid.py", dump, [], self.work)


GOLDEN_SUFFIX = {
    "trace_headers": "trace_headers",
    "gstparse": "gstparse",
    "jm": "jm-trace",
    "mvs": "ffmpeg-mvs",
    "hm": "hm-grid",
}
PICTURE_BUDGET = {"trace_headers": HEADER_PICTURES, "gstparse": HEADER_PICTURES, "jm": BLOCK_PICTURES,
                  "mvs": BLOCK_PICTURES, "hm": BLOCK_PICTURES}  # fmt: skip


class TooLarge(StepError):
    """Even one picture's golden exceeds the size limit: not a failure, the golden is left out."""


def produce(state: Entry, oracle: str, max_bytes: int) -> tuple[str, int]:
    """Run one oracle on the longest stream prefix whose golden fits ``max_bytes``; returns (text, pictures)."""
    pictures = PICTURE_BUDGET[oracle]
    max_bytes = max(max_bytes, MAX_BLOCK_BYTES.get(oracle, 0))
    while True:
        es, kept = state.stream(pictures)
        text = state.trace_headers(es, []) if oracle == "trace_headers" else getattr(state, oracle)(es)
        if len(text.encode()) <= max_bytes:
            return text, kept
        if pictures == 1:
            raise TooLarge(f"the golden of one picture is {len(text)} bytes (limit {max_bytes})")
        pictures = max(1, pictures // 2)


def process(
    entry: dict[str, Any],
    args: argparse.Namespace,
    skips: dict[str, str],
    work_root: Path,
    corpora: Path,
) -> dict[str, Any]:
    cid, codec = entry["id"], entry_codec(entry)
    assert codec is not None
    work = work_root / cid
    work.mkdir(parents=True, exist_ok=True)
    record: dict[str, Any] = {
        "codec": codec,
        "goldens": {},
        "skipped": {},
        "failed": {},
    }
    src = corpus_file(corpora, cid)
    record["input_sha256"] = sha256_file(src)
    state = Entry(entry, codec, to_annexb(src, codec, work), work)
    area = args.out / CODEC_INFO[codec]["area"]
    area.mkdir(parents=True, exist_ok=True)
    texts: dict[str, str] = {}
    for oracle in oracles_for(entry, codec, set(args.oracles)):
        key = f"{cid}:{oracle}"
        try:
            text, pictures = produce(state, oracle, args.max_bytes)
        except TooLarge as exc:
            record["skipped"][oracle] = str(exc)
            continue
        except StepError as exc:
            if key in skips:
                record["skipped"][oracle] = skips[key]
            else:
                record["failed"][oracle] = str(exc)
            continue
        texts[oracle] = text
        name = f"{cid}.{GOLDEN_SUFFIX[oracle]}.json.gz"
        write_gz(area / name, text)
        record["goldens"][oracle] = {
            "file": f"{CODEC_INFO[codec]['area']}/{name}",
            "pictures": pictures,
        }
    if cid == EPB_ID and "trace_headers" in args.oracles:
        es, _ = state.stream(HEADER_PICTURES)
        epb = state.trace_headers(es, ["--epb-offsets", str(es)])
        (args.out / "epb").mkdir(parents=True, exist_ok=True)
        (args.out / "epb" / f"{cid}.json").write_text(epb, encoding="utf-8")
        record["goldens"]["epb"] = {
            "file": f"epb/{cid}.json",
            "pictures": HEADER_PICTURES,
        }
    if "trace_headers" in texts and "gstparse" in texts:
        record["selfcheck"] = self_check(codec, cid, texts, args.known)
    shutil.rmtree(work, ignore_errors=True)
    return record


def write_gz(path: Path, text: str) -> None:
    """Goldens are stored gzip-compressed (the repository carries them for ever); no name or time in the header."""
    with path.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as f:
        f.write(text.encode("utf-8"))


def self_check(codec: str, cid: str, texts: dict[str, str], known: Path) -> dict[str, Any]:
    trace = json.loads(texts["trace_headers"])
    gst = json.loads(texts["gstparse"])
    report = h26x_selfcheck.check(codec, trace, gst, cid, h26x_selfcheck.load_known(known))
    return {
        k: report[k]
        for k in (
            "compared",
            "documented",
            "ambiguous",
            "gst_only",
            "undocumented",
            "problems",
        )
    }


# ------------------------------------------------------------------------------------------------ main


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=REPO / "tests" / "corpora" / "manifest.json")
    p.add_argument("--out", type=Path, default=REPO / "tests" / "golden")
    p.add_argument("--only", help="comma-separated corpus ids (any H.264/HEVC entry, fast or not)")
    p.add_argument(
        "--corpora-dir",
        type=Path,
        default=Path(os.environ.get("OVT_CORPORA_DIR", "corpora")),
    )
    p.add_argument(
        "--oracles",
        default=",".join(ALL_ORACLES),
        help="subset of " + ",".join(ALL_ORACLES),
    )
    p.add_argument(
        "--generate-syn",
        action="store_true",
        help="run tests/corpora/synthetic/generate.py for missing syn_ ids",
    )
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--work-dir", type=Path)
    p.add_argument("--max-bytes", type=int, default=MAX_BYTES)
    p.add_argument("--known", type=Path, help="KNOWN_DIFFS.md (default: <out>/KNOWN_DIFFS.md)")
    p.add_argument("--skips", type=Path, help="h26x_skips.json (default: next to this script)")
    p.add_argument(
        "--keep-going",
        action="store_true",
        help="exit 0 even when a step failed (CI report mode)",
    )
    p.add_argument("--list", action="store_true", help="print the plan and exit")
    args = p.parse_args(argv)
    args.oracles = [o for o in args.oracles.split(",") if o]
    if set(args.oracles) - set(ALL_ORACLES):
        p.error("unknown oracle: " + ",".join(sorted(set(args.oracles) - set(ALL_ORACLES))))
    args.only = [i for i in args.only.split(",") if i] if args.only else None
    args.out = args.out.resolve()
    args.corpora_dir = args.corpora_dir.resolve()
    args.known = args.known or args.out / "KNOWN_DIFFS.md"
    args.skips = args.skips or HERE / "h26x_skips.json"
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    try:
        entries, skipped = select(manifest, args.only)
    except StepError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    skips = json.loads(args.skips.read_text(encoding="utf-8")) if args.skips.is_file() else {}
    if args.list:
        for e in entries:
            codec = entry_codec(e)
            print(e["id"], codec, " ".join(oracles_for(e, codec or "", set(args.oracles))))
        for cid, reason in skipped.items():
            print(f"{cid} skipped: {reason}")
        return EXIT_OK
    if args.generate_syn:
        wanted = [
            e["id"]
            for e in entries
            if e["id"].startswith("syn_") and not (args.corpora_dir / e["id"] / ".ok").is_file()
        ]
        if wanted:
            run([sys.executable, "-I", str(SYNTH), "--only", ",".join(wanted), "--out", str(args.corpora_dir)],
                REPO, "generate.py", stdout=sys.stderr)  # fmt: skip
    needed = {"trace_headers": ["ffmpeg"], "gstparse": ["gst_parse_oracle"], "jm": ["ldecod"], "hm": ["hm_cu_dump"],
              "mvs": ["ffmpeg_mvs"]}  # fmt: skip
    missing = sorted({t for o in args.oracles for t in needed[o] + ["ffmpeg", "ffprobe"] if not _have(t)})
    if missing:
        print(
            f"error: not on PATH (run this in the oracle image): {', '.join(missing)}",
            file=sys.stderr,
        )
        return EXIT_MISSING
    args.out.mkdir(parents=True, exist_ok=True)
    work_root = args.work_dir or Path(tempfile.mkdtemp(prefix="gen_h26x_"))
    results: dict[str, dict[str, Any]] = {}

    def one(entry: dict[str, Any]) -> tuple[str, dict[str, Any]]:
        try:
            return entry["id"], process(entry, args, skips, work_root, args.corpora_dir)
        except (StepError, OSError) as exc:
            reason = skips.get(f"{entry['id']}:entry") or (str(exc) if isinstance(exc, NoStream) else None)
            if reason:
                return entry["id"], {"failed": {}, "goldens": {}, "skipped": {"entry": reason}}
            return entry["id"], {"failed": {"entry": str(exc)}, "goldens": {}, "skipped": {}}

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for cid, record in pool.map(one, entries):
            results[cid] = record
            status = "FAILED " + ",".join(record["failed"]) if record["failed"] else "ok"
            print(f"{cid}: {status} ({len(record['goldens'])} goldens)", file=sys.stderr)
    index_path = args.out / "h26x-index.json"
    index: dict[str, Any] = {}
    if args.only and index_path.is_file():
        index = json.loads(index_path.read_text(encoding="utf-8")).get("entries", {})
    index.update({cid: r for cid, r in sorted(results.items())})
    doc = {"skipped_entries": skipped, "entries": dict(sorted(index.items()))}
    index_path.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    bad = {cid: r for cid, r in results.items() if r["failed"] or (r.get("selfcheck") or {}).get("problems")}
    for cid, r in bad.items():
        for oracle, why in r["failed"].items():
            print(f"FAILED {cid} {oracle}: {why}", file=sys.stderr)
        for line in (r.get("selfcheck") or {}).get("problems", []):
            print(f"DISAGREEMENT {cid}: {line}", file=sys.stderr)
    if args.work_dir is None:
        shutil.rmtree(work_root, ignore_errors=True)
    return EXIT_OK if args.keep_going or not bad else EXIT_STEP


def _have(name: str) -> bool:
    try:
        tool(name)
        return True
    except FileNotFoundError:
        return False


if __name__ == "__main__":
    sys.exit(main())
