# UVQ preprocessing and exported graphs

This document is the WI-3.314 deliverable of chapter 9 §9.6.4. It records how the pinned reference CLI turns a
video file into model inputs, and the input and output tensors of the ONNX graphs that
`tools/uvq_export/export_uvq.py` writes. The UVQ host plugin (WI-3.315) MUST reproduce exactly what is written
here. Every row of every table carries the `source file:line` it was read from. `tools/uvq_export/check_doc.py`
enforces that, and with `--uvq-dir` it also checks that each cited line exists.

* **Pin:** `google/uvq` at `c204ecc04f093e90c941cb126c7ac3d5beca0414` (`UVQ_COMMIT`, the same revision as the
  oracle image, `tests/oracles/Dockerfile`). This revision is Google's PyTorch implementation of UVQ 1.5 and
  UVQ 1.0. The owner chose it on #47, so the converter is `torch.onnx.export`, not tf2onnx.
* **Reference CLI:** `python uvq_inference.py <file> --model_version {1.5|1.0} --output_all_stats`.
* **Paths** in the `source` column are relative to the root of the clone. `OVT:` marks a fact taken from the
  OVT export wrapper in `tools/uvq_export/export_uvq.py` instead of the upstream code.

Spec deviations found while reading the source are tracked in spec question #246. The main ones: UVQ 1.5 has
no CompressionNet, no labels and a single output; the resize filter is bicubic, not bilinear; UVQ 1.0 always
samples at 5 fps.

## 1. Model input table (§9.6.4)

One row per graph input. Frame tensors are float32, NCHW, RGB in that channel order. Values are
`v = byte * 2 / 255 - 1`, so the range is [-1, 1], where `byte` is the 8-bit `rgb24` sample that FFmpeg
produced (section 2). "Frames per chunk" is the number of sampled frames that one call of the graph consumes.

| model version | input tensor name | shape | dtype | channel order | value range/normalisation | frames per chunk | resize filter | source file:line |
|---|---|---|---|---|---|---|---|---|
| 1.5 | `uvq15_content:rgb` | [1, 3, 1080, 1920] | float32 | RGB, NCHW | `byte*2/255-1`, [-1, 1] | 1 (every sampled frame is scored on its own) | FFmpeg `scale=w=1920:h=1080:flags=bicubic` (stretch, no letterbox), then in-graph bilinear to 256x256 (`align_corners=False`, ONNX `Resize` linear, half_pixel) | utils/video_reader.py:173, utils/video_reader.py:283, uvq1p5_pytorch/utils/contentnet.py:134 |
| 1.5 | `uvq15_distortion:rgb` | [1, 3, 1080, 1920] | float32 | RGB, NCHW | `byte*2/255-1`, [-1, 1] | 1 | same FFmpeg scale; the graph tiles the frame into 3x3 patches of 360x640 | utils/video_reader.py:173, uvq1p5_pytorch/utils/distortionnet.py:47 |
| 1.5 | `uvq15_aggregation:content` | [1, 8, 8, 128] | float32 | NHWC features | output `features` of `uvq15_content` | 1 | none | uvq1p5_pytorch/utils/contentnet.py:168, uvq1p5_pytorch/utils/aggregationnet.py:230 |
| 1.5 | `uvq15_aggregation:distortion` | [1, 24, 24, 128] | float32 | NHWC features | output `features` of `uvq15_distortion` | 1 | none | uvq1p5_pytorch/utils/distortionnet.py:382, uvq1p5_pytorch/utils/aggregationnet.py:231 |
| 1.0 | `uvq10_content:rgb` | [1, 3, 496, 496] | float32 | RGB, NCHW | `(byte/255-0.5)*2`, [-1, 1] | 1 (frame 0 of the 5-frame chunk) | FFmpeg letterbox to 1280x720 (bicubic, `force_original_aspect_ratio=1`, black pad), then FFmpeg `scale=496:496:flags=bilinear` on the `rgb24` frame | utils/video_reader.py:70, utils/video_reader.py:74, utils/video_reader.py:148, uvq_pytorch/utils/contentnet.py:181 |
| 1.0 | `uvq10_compression:rgb` | [1, 3, 5, 720, 1280] | float32 | RGB, NCDHW (C, frame, H, W) | `(byte/255-0.5)*2`, [-1, 1] | 5 (all frames of the chunk) | FFmpeg `scale=w=1280:h=720:flags=bicubic:force_original_aspect_ratio=1`, `pad=1280:720:(ow-iw)/2:(oh-ih)/2`; the graph tiles 4x4 patches of 5x180x320 | utils/video_reader.py:70, utils/video_reader.py:72, uvq_pytorch/utils/compressionnet.py:36, uvq_pytorch/utils/compressionnet.py:188 |
| 1.0 | `uvq10_distortion:rgb` | [1, 3, 720, 1280] | float32 | RGB, NCHW | `(byte/255-0.5)*2`, [-1, 1] | 1 (frame 0 of the chunk) | same letterbox as CompressionNet; the graph tiles 2x2 patches of 360x640 | utils/video_reader.py:70, uvq_pytorch/utils/distortionnet.py:41 |
| 1.0 | `uvq10_aggregation:compression`, `:content`, `:distortion` | [1, 16, 16, 100] each | float32 | NHWC features | the `features` outputs of the three graphs above | 1 chunk | none | uvq_pytorch/utils/aggregationnet.py:106, uvq_pytorch/utils/aggregationnet.py:30 |

## 2. Decoding, sampling and chunking (the host's side)

The reference runs FFmpeg and reads `rgb24` bytes from it. The host replaces FFmpeg with OVT's decoder and
zimg, so each FFmpeg behaviour that changes the bytes is listed here.

| step | UVQ 1.5 | UVQ 1.0 | source file:line |
|---|---|---|---|
| Video length `L` (seconds) | `ceil(format duration)` from ffprobe; with `--fps -1`, `ceil(nb_frames / fps)` | `ceil(format duration)` | uvq_inference.py:138, uvq_inference.py:150, utils/probe.py:133 |
| Sampling rate | option `fps`, default 1 (`--fps`); `-1` means `ceil(r_frame_rate)` | fixed 5 fps; `--fps` is ignored | uvq_inference.py:293, utils/probe.py:118, utils/video_reader.py:41 |
| Frame selection | FFmpeg filter `fps=<fps>:start_time=0:round=up`, then `-fps_mode passthrough`. Sampled frame `k` is the frame that the fps filter emits for output time `k/fps` with timestamps rounded up | FFmpeg output option `-r 5` (constant-rate frame duplication or dropping at the muxer, nearest timestamp) | utils/video_reader.py:172, utils/video_reader.py:78 |
| Expected frame count | `L * fps`. Extra decoded frames are dropped, and missing ones are padded with black frames (every value -1.0) | `L * 5`. The byte buffer is truncated or zero-padded, so missing frames are black (-1.0) | utils/video_reader.py:235, utils/video_reader.py:319, utils/video_reader.py:120 |
| Rotation | FFmpeg autorotation applies the display matrix (default `-autorotate`). `--transpose` adds `transpose=2` (90° counter-clockwise). Batch mode (`.txt` input) sets it when ffprobe's coded width < height | same autorotation; `--transpose` adds `transpose=1` (90° clockwise) | utils/video_reader.py:170, utils/video_reader.py:62, uvq_inference.py:60 |
| Colour conversion | one swscale context: `scale=...:flags=bicubic,format=rgb24`. The input matrix and range come from the frame's tags (`in_color_matrix=auto`, BT.601 when untagged), and the output is full-range 8-bit RGB | same, via `format=rgb24` after the letterbox | utils/video_reader.py:173, utils/video_reader.py:73 |
| Resize kernel | swscale `SWS_BICUBIC` with default parameters (B = 0, C = 0.6) to 1920x1080, ignoring aspect ratio | swscale bicubic to fit inside 1280x720, then centred black padding; ContentNet input is a second bilinear swscale pass from the padded `rgb24` to 496x496 | utils/video_reader.py:173, utils/video_reader.py:71, utils/video_reader.py:74 |
| Normalisation | `v = byte * (2/255) - 1` in float32 | `v = (byte/255 - 0.5) * 2` in float64, cast to float32 at the model | utils/video_reader.py:283, utils/video_reader.py:284, utils/video_reader.py:134 |
| Chunk | one sampled frame (`unsqueeze(1)` makes F = 1); ContentNet and DistortionNet both use frame 0 | one second = 5 sampled frames; ContentNet and DistortionNet use frame 0, CompressionNet uses all 5 | uvq1p5_pytorch/utils/uvq1p5.py:151, uvq1p5_pytorch/utils/contentnet.py:185, uvq_pytorch/utils/uvq1p0.py:94 |
| Sequence score | mean of the per-frame scores, including padded frames | per combination: mean over chunks of each fold's score, then mean over the 5 folds (`uvq10_aggregation` already averages the folds, and the two means commute) | uvq1p5_pytorch/utils/uvq1p5.py:156, uvq_pytorch/utils/aggregationnet.py:77, uvq_pytorch/utils/aggregationnet.py:114 |
| Frame index reported | `round(k * ceil(r_frame_rate) / fps)` | n/a (the CLI prints sequence scores only) | uvq1p5_pytorch/utils/uvq1p5.py:161 |

Notes for WI-3.315:

* swscale resizes the Y'CbCr planes and converts to RGB in its output stage. The 8-bit rounding of `rgb24`
  happens before normalisation. A host that converts to float RGB first and resizes afterwards (the order
  sketched in §9.6.4 steps 3 and 4) gets slightly different bytes. Whether that stays inside the 0.01 MOS
  tolerance is for the `metrics_uvq_parity` test to show. The validation in `tools/uvq_export/report.json` used
  the reference FFmpeg chain itself, so it isolates the export error from the host's preprocessing error.
* The black padding of the last second is part of the score. A 20.3 s clip is scored over 21 seconds, and the
  21st sampled frame is black when the decoder runs out.

## 3. Graph outputs

| graph | output | shape | meaning | source file:line |
|---|---|---|---|---|
| `uvq15_content` | `features` | [1, 8, 8, 128] | ContentNet features, NHWC | uvq1p5_pytorch/utils/contentnet.py:168 |
| `uvq15_distortion` | `features` | [1, 24, 24, 128] | DistortionNet features, 3x3 patch grid reassembled | uvq1p5_pytorch/utils/distortionnet.py:375 |
| `uvq15_aggregation` | `mos` | [1, 1] | per-frame MOS, `tanh(x)*2+3`, range [1, 5] | uvq1p5_pytorch/utils/aggregationnet.py:170 |
| `uvq10_content` | `features`, `label_probs` | [1, 16, 16, 100], [1, 3862] | features; sigmoid probabilities of the YouTube-8M classes (`labels_yt8m.txt` line i+1 = class i; the 56 classes without a name in `contentnet_labels.csv` carry their Knowledge Graph id `/m/...` instead) | uvq_pytorch/utils/contentnet.py:91, uvq_pytorch/utils/contentnet.py:38 |
| `uvq10_compression` | `features`, `compression_level` | [1, 16, 16, 100], [1, 4, 4, 1] | features; per-patch sigmoid compression level of the 4x4 grid (the §9.6.5 `compression/grid4` map) | uvq_pytorch/utils/compressionnet.py:92, OVT: tools/uvq_export/export_uvq.py:229 |
| `uvq10_distortion` | `features`, `distortion_probs` | [1, 16, 16, 100], [1, 2, 2, 26] | features; probabilities of 26 distortion classes per patch of the 2x2 grid (the §9.6.5 `distortion/grid4` map after repetition) | uvq_pytorch/utils/distortionnet.py:244, uvq_pytorch/utils/distortionnet.py:50 |
| `uvq10_aggregation` | `scores` | [1, 7] | the seven README combinations, mean over folds 0 to 4, in the order `compression, content, distortion, compression_content, compression_distortion, content_distortion, compression_content_distortion` (the last one is the MOS) | uvq_pytorch/utils/aggregationnet.py:134, uvq_pytorch/utils/aggregationnet.py:135 |

## 4. Export settings

| setting | value | source file:line |
|---|---|---|
| Converter | `torch.onnx.export(dynamo=False)`, TorchScript exporter of torch 2.8.0, opset 17, constant folding on | OVT: tools/uvq_export/export_uvq.py:288 |
| Shapes | every input fixed, batch 1 (no dynamic axes) | OVT: tools/uvq_export/export_uvq.py:66 |
| Custom symbolic | `aten::to_dense` mapped to identity (dense input; used by `PermuteLayerNHWC`) | uvq1p5_pytorch/utils/custom_nn_layers.py:266, uvq_pytorch/utils/custom_nn_layers.py:233 |
| fp16 variants | `onnxconverter_common.float16.convert_float_to_float16(keep_io_types=True)`; inputs and outputs stay float32 | OVT: tools/uvq_export/export_uvq.py:303 |
| Determinism | two exports with the pinned venv give byte-identical files; the manifest hashes depend on that | OVT: tools/uvq_export/export_uvq.py:324 |
