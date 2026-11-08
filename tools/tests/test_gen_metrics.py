# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/goldens/gen_metrics.py with stand-ins for the oracle tools (chapter 14 §14.4.10-14.4.13; WI-0.565).

The real tools only exist in the oracle image; here ``ffmpeg``, ``vmaf``, ``x264``, ``dump_psnrhvs`` and
``dump_ciede2000`` are small Python scripts that write what the real ones print, so the plumbing (trimming, command
lines, normalisers, gzip, index, checks) runs for real. ``x264_ssim`` is the real program, built with CMake.
"""

from __future__ import annotations

import gzip
import json
import random
import shutil
import stat
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "goldens"))
sys.path.insert(0, str(REPO / "tests" / "oracles" / "normalize"))

import canon  # noqa: E402
import gen_metrics as gen  # noqa: E402

W, H, FRAMES = 64, 48, 5

STUBS = {
    "ffmpeg": """
        import re, shutil, sys
        a = sys.argv[1:]
        inputs = [a[i + 1] for i, x in enumerate(a) if x == "-i"]
        graph = a[a.index("-lavfi") + 1] if "-lavfi" in a else ""
        if "psnr=stats_file=" in graph:
            path = re.search(r"stats_file=(\\S+)", graph).group(1)
            open(path, "w").write("n:1 mse_avg:1.00 mse_y:1.00 mse_u:1.00 mse_v:1.00 psnr_avg:48.13 psnr_y:48.13 psnr_u:48.13 psnr_v:48.13\\n"
                                  "n:2 mse_avg:2.00 mse_y:2.00 mse_u:2.00 mse_v:2.00 psnr_avg:45.12 psnr_y:45.12 psnr_u:45.12 psnr_v:45.12\\n")
        elif "libvmaf=" in graph:
            assert "feature='name=psnr|name=float_ssim'" in graph and "model='version=vmaf_v0.6.1'" in graph, graph
            path = re.search(r"log_path=(\\S+?):log_fmt=json", graph).group(1)
            open(path, "w").write('{"version":"3","fps":1.0,"frames":[{"frameNum":0,"metrics":{"vmaf":90.0,"psnr_y":40.0}}],'
                                  '"pooled_metrics":{"vmaf":{"min":90.0,"max":90.0,"mean":90.0,"harmonic_mean":90.0}}}')
        elif inputs[0].endswith(".264"):
            shutil.copy(open(inputs[0]).read().strip(), a[-1])
        else:
            n = int(a[a.index("-frames:v") + 1])
            data = open(inputs[0], "rb").read()
            head, _, rest = data.partition(b"\\n")
            w, h = (int(re.search(rb" W(\\d+)", head).group(1)), int(re.search(rb" H(\\d+)", head).group(1)))
            size = w * h * 3 // 2 + 6
            open(a[-1], "wb").write(head + b"\\n" + rest[: n * size])
    """,
    "vmaf": """
        import json, sys
        a = sys.argv[1:]
        model = a[a.index("--model") + 1]
        feats = [a[i + 1] for i, x in enumerate(a) if x == "--feature"]
        assert feats == ["psnr", "float_ssim", "float_ms_ssim", "psnr_hvs", "ciede", "cambi"], feats
        score = 76.668905 if model == "version=vmaf_v0.6.1" else 80.0
        step = 5 if "--subsample" in a else 1
        frames = [{"frameNum": i, "metrics": {"vmaf": score, "psnr_y": 40.0}} for i in range(0, 5, step)]
        pooled = {"vmaf": {"min": score, "max": score, "mean": score, "harmonic_mean": score}}
        json.dump({"version": "x", "fps": 1.0, "frames": frames, "pooled_metrics": pooled}, open(a[a.index("--output") + 1], "w"))
    """,
    "x264": """
        import sys
        a = sys.argv[1:]
        open(a[a.index("--output") + 1], "w").write(a[-1])
        sys.stderr.write("x264 [info]: SSIM Mean Y:1.0000000 (inf)\\n")
    """,
    "dump_psnrhvs": """
        print("00000000: 40.5\\n00000001: 41.5\\nTotal: 41.0")
    """,
    "dump_ciede2000": """
        print("Opening a file\\n00000000: 1.2500\\n00000001: 1.5000\\nTotal: 1.3750")
    """,
}


@pytest.fixture
def stubs(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for name, body in STUBS.items():
        path = bin_dir / name
        path.write_text(f"#!{sys.executable}\n" + textwrap.dedent(body), encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        monkeypatch.setenv("OVT_ORACLE_" + name.upper(), str(path))
    return bin_dir


def y4m(path: Path, seed: int) -> None:
    rng = random.Random(seed)
    with path.open("wb") as f:
        f.write(f"YUV4MPEG2 W{W} H{H} F25:1 Ip A0:0 C420jpeg\n".encode())
        for _ in range(FRAMES):
            f.write(b"FRAME\n" + bytes(rng.randrange(256) for _ in range(W * H * 3 // 2)))


@pytest.fixture
def corpora(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    root = tmp_path / "corpora"
    for seed, cid in enumerate(("ref_corpus", "dist_corpus")):
        (root / cid).mkdir(parents=True)
        y4m(root / cid / "x.y4m", seed)
        (root / cid / ".ok").write_text(json.dumps({"primary": "x.y4m"}), encoding="utf-8")
    pair = {
        "ref": ("ref_corpus", None),
        "dist": ("dist_corpus", None),
        "frames": 3,
        "daala": True,
    }
    monkeypatch.setattr(gen, "PAIRS", {gen.NETFLIX_PAIR: pair, "tenbit": {**pair, "daala": False}})
    return root


def test_plan_lists_every_pair(capsys: pytest.CaptureFixture[str]) -> None:
    assert gen.main(["--list"]) == 0
    out = capsys.readouterr().out
    assert "netflix_src01_576x324 netflix_src01_hrc00_576x324 48" in out
    assert "syn_metrics_pq_1080p syn_metrics_pq_1080p_pair 60" in out


@pytest.mark.skipif(
    not (shutil.which("cmake") and shutil.which("cc")),
    reason="needs cmake and a C compiler",
)
def test_all_oracles_run_and_the_output_is_reproducible(stubs: Path, corpora: Path, tmp_path: Path) -> None:
    rd = tmp_path / "rd"
    rd.mkdir()
    shutil.copy(REPO / "tests" / "golden" / "metrics" / "rd_points" / "jvet4_overlap.json", rd)
    outs = []
    for n in (1, 2):
        out = tmp_path / f"out{n}"
        args = ["--out", str(out), "--corpora-dir", str(corpora), "--rd-points", str(rd), "--work-dir", str(tmp_path / f"w{n}"),
                "--oracles", "ffpsnr,ffvmaf,vmaf,x264ssim,daala"]  # fmt: skip
        assert gen.main(args) == 0
        outs.append(out)
    index = json.loads((outs[0] / "metrics-index.json").read_text(encoding="utf-8"))
    pair = index["pairs"][gen.NETFLIX_PAIR]
    assert pair["failed"] == {} and pair["frames"] == 3
    assert pair["checks"]["x264_log"]["frames"] == 3
    assert abs(pair["checks"]["netflix_vmaf_mean"]["got"] - gen.NETFLIX_VMAF) < 1e-4
    expected = {
        "ffpsnr",
        "ffvmaf",
        "x264ssim",
        "psnrhvs",
        "ciede",
        "vmaf_v0.6.1-sub5.vmaf",
    } | {f"{m}.vmaf" for m in gen.VMAF_MODELS}
    assert set(pair["goldens"]) == expected
    for name in pair["goldens"].values():
        doc = json.loads(gzip.decompress((outs[0] / name).read_bytes()))
        assert canon.validate(doc, "scores") == [], name
    x264 = json.loads(gzip.decompress((outs[0] / pair["goldens"]["x264ssim"]).read_bytes()))
    assert [sorted(f) for f in x264["frames"]] == [["i", "ssim_x264_u", "ssim_x264_v", "ssim_x264_y"]] * 3
    # a 10-bit style pair, Daala allowed to fail: none of the stubs fails, so it is a plain success
    assert index["pairs"]["tenbit"]["failed"] == {}
    for rel in sorted(p.relative_to(outs[0]) for p in outs[0].rglob("*") if p.is_file()):
        assert (outs[0] / rel).read_bytes() == (outs[1] / rel).read_bytes(), rel


@pytest.mark.skipif(
    not (shutil.which("cmake") and shutil.which("cc")),
    reason="needs cmake and a C compiler",
)
def test_a_wrong_netflix_score_fails_the_run(
    stubs: Path, corpora: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(gen, "NETFLIX_VMAF", 70.0)
    args = [
        "--out",
        str(tmp_path / "o"),
        "--corpora-dir",
        str(corpora),
        "--work-dir",
        str(tmp_path / "w"),
        "--oracles",
        "vmaf",
    ]
    assert gen.main(args) == 1
    assert gen.main([*args, "--keep-going"]) == 0
    index = json.loads((tmp_path / "o" / "metrics-index.json").read_text(encoding="utf-8"))
    assert "check:netflix_vmaf_mean" in index["pairs"][gen.NETFLIX_PAIR]["failed"]


def test_a_missing_tool_is_exit_4(corpora: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("PATH", str(tmp_path))
    for name in STUBS:
        monkeypatch.delenv("OVT_ORACLE_" + name.upper(), raising=False)
    assert gen.main(["--out", str(tmp_path / "o"), "--corpora-dir", str(corpora)]) == gen.EXIT_MISSING


def test_bd_goldens_are_written_for_every_set(tmp_path: Path) -> None:
    pytest.importorskip("bjontegaard")
    out = tmp_path / "out"
    out.mkdir()
    files = gen.bd_goldens(out, REPO / "tests" / "golden" / "metrics" / "rd_points", tmp_path)
    assert sorted(files) == [
        "aom6_overlap",
        "aom6_partial",
        "jvet4_overlap",
        "jvet4_partial",
    ]
    four = json.loads((out / files["jvet4_overlap"]).read_text(encoding="utf-8"))
    six = json.loads((out / files["aom6_overlap"]).read_text(encoding="utf-8"))
    assert (
        "cubic" in four["pooled"]["bd_rate"] and "cubic" not in six["pooled"]["bd_rate"]
    )  # cubic only on 4-point sets


def test_y4m_info_counts_frames(tmp_path: Path) -> None:
    y4m(tmp_path / "a.y4m", 1)
    assert gen.y4m_info(tmp_path / "a.y4m") == {
        "width": W,
        "height": H,
        "bit_depth": 8,
        "frames": FRAMES,
    }


def test_a_golden_over_the_cap_is_refused(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(gen, "MAX_GOLDEN_BYTES", 100)
    with pytest.raises(gen.StepError):
        gen.write_gz(
            tmp_path / "x.json.gz",
            json.dumps([rng.random() for rng in [random.Random(1)] for _ in range(500)]),
        )
    assert not (tmp_path / "x.json.gz").exists()
    assert (
        subprocess.run(
            [
                sys.executable,
                "-I",
                str(REPO / "tools" / "goldens" / "gen_metrics.py"),
                "--list",
                "--only",
                "nope",
            ],
            capture_output=True,
        ).returncode
        == 2
    )
