# WI-3.314 acceptance record

Date: 2026-11-07. Machine: cloud container, Ubuntu 24.04, 4 vCPU Intel Xeon @ 2.10 GHz, 15 GiB RAM, Python 3.13,
FFmpeg 6.1.1 (Ubuntu package). Venv: `tools/uvq_export/requirements.txt` (torch 2.8.0 from PyPI, onnxruntime
1.20.1). google/uvq at `c204ecc04f093e90c941cb126c7ac3d5beca0414`.

```sh
python -m venv venv && venv/bin/pip install -r tools/uvq_export/requirements.txt
python3 -I tools/fetch_corpora.py --cache-dir corpora --ids ytugc_clip01,...,ytugc_clip20
venv/bin/python tools/uvq_export/export_uvq.py export   --work work
venv/bin/python tools/uvq_export/export_uvq.py validate --work work --clips corpora   # about 70 min
venv/bin/python tools/uvq_export/export_uvq.py manifest --work work
```

## Test 1: |Δ MOS| ≤ 0.01 on ytugc_clip01 … ytugc_clip20

`validate` ran the reference CLI (`uvq_inference.py --output_all_stats`) and ONNX Runtime CPU on the exported
graphs. The ORT side was fed by the reference's own `utils/video_reader.py`. Result: 0 failing cells, recorded per
clip in `tools/uvq_export/report.json`.

| version | max abs Δ MOS fp32 | max abs Δ MOS fp16 | other checks |
|---|---|---|---|
| 1.5 | 1.2e-6 | 0.0014 | per-frame scores, max Δ in the report |
| 1.0 | 6.5e-6 | 0.0041 | all seven combinations; per-chunk top-1 content label identical to the torch ContentNet on all 20 clips |

Automated pin of the committed record: `tools/tests/test_uvq_export.py::test_report_passes_on_twenty_clips_and_matches_the_manifest`.

## Test 2: `onnx.checker.check_model` on each exported file

`export` calls `onnx.checker.check_model(path, full_check=True)` on all 14 files (7 graphs, fp32 and fp16). No error
was raised (an error aborts the step). A second export gave byte-identical files, and their SHA-256 values are
those in `core/share/models/uvq/manifest.json`.

## Test 3: every table row of docs/dev/uvq_preprocessing.md carries a source file:line

Automated: `tools/tests/test_uvq_export.py::test_committed_document_cites_a_source_line_in_every_row`. With the
clone, `python3 -I tools/uvq_export/check_doc.py --uvq-dir work/uvq` also confirmed that every cited line exists
(exit 0).
