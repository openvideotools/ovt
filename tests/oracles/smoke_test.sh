#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Smoke test of the oracle image (chapter 14 §14.4, WI-0.554 acceptance test 2): build
# syn_h265_amp_sao_on.265 with the recipe of §14.3.3 (plus small H.264/AV1/MP4/MKV/TS wrappers of the same
# source for the oracles that need them), run every third-party oracle command of §14.4 and require exit
# status 0 and non-empty output from each.
#
#   smoke_test.sh [OUT_DIR]      (default: a fresh temporary directory)

set -uo pipefail

out="${1:-$(mktemp -d)}"
mkdir -p "${out}"
cd "${out}" || exit 1
failures=0

# check <name> <output-file> <command...>: run the command (stdout to the file unless the command writes it)
check() {
  local name="$1" file="$2"
  shift 2
  if "$@" >"${name}.stdout" 2>"${name}.stderr"; then
    [[ "${file}" == - ]] && file="${name}.stdout"
    if [[ -s "${file}" ]]; then
      printf 'ok    %-22s %s (%s bytes)\n' "${name}" "${file}" "$(stat -c %s "${file}")"
      return 0
    fi
    printf 'FAIL  %-22s empty output %s\n' "${name}" "${file}"
  else
    printf 'FAIL  %-22s exit %s\n' "${name}" "$?"
    tail -n 20 "${name}.stderr" | sed 's/^/      /'
  fi
  failures=$((failures + 1))
}

ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }

# --- inputs (§14.3.3 source A and recipe 8) --------------------------------------------------------------
check src_720p_8bit src_720p_8bit.y4m ff -f lavfi -i "testsrc2=size=1280x720:rate=30,format=yuv420p" -frames:v 60 \
  src_720p_8bit.y4m
check x265 syn_h265_amp_sao_on.265 x265 --input src_720p_8bit.y4m --preset slower --amp --sao --rect --keyint 30 \
  --no-open-gop --bframes 4 --ref 4 --output syn_h265_amp_sao_on.265
check x264 syn_h264.264 x264 --preset fast --keyint 30 --frames 10 --output syn_h264.264 src_720p_8bit.y4m
check aomenc syn_av1.ivf aomenc --codec=av1 --cpu-used=8 --limit=5 --end-usage=q --cq-level=40 --ivf -o syn_av1.ivf \
  src_720p_8bit.y4m
check mp4_wrap syn_h265.mp4 ff -i syn_h265_amp_sao_on.265 -c copy -movflags +faststart syn_h265.mp4
check mkvmerge syn_h265.mkv mkvmerge -q -o syn_h265.mkv syn_h265.mp4
check ts_wrap syn_h265.ts ff -i syn_h265.mp4 -c copy -f mpegts syn_h265.ts
check dist_y4m dist.y4m ff -i syn_h265_amp_sao_on.265 -pix_fmt yuv420p dist.y4m

# --- §14.4.1 FFmpeg trace_headers, §14.4.6 and §14.4.7-14.4.9 container oracles --------------------------
check trace_headers trace_headers.txt sh -c 'ffmpeg -hide_banner -loglevel trace -i syn_h265_amp_sao_on.265 \
  -map 0:v:0 -c:v copy -bsf:v trace_headers -f null - 2> trace_headers.txt'
check trace_headers_mp4 - sh -c 'ffmpeg -hide_banner -loglevel trace -i syn_h265.mp4 -map 0:v:0 -c:v copy \
  -bsf:v hevc_mp4toannexb,trace_headers -f null - 2>&1 | grep -m1 nal_unit_type'
check mp4box_diso - MP4Box -diso -std syn_h265.mp4
check mp4box_dts - MP4Box -dts -std syn_h265.mp4
check mp4box_dnal - MP4Box -dnal 1 -std syn_h265.mp4
check mp4ff_info - mp4ff-info -l all:1 syn_h265.mp4
check mediainfo_trace - mediainfo --Details=1 --Output=XML syn_h265.mp4
check mediainfo_json - mediainfo --Output=JSON syn_h265.mp4
check mp4boxjs - node -e '
  const MP4Box = require("mp4box"); const buf = require("fs").readFileSync(process.argv[1]);
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength); ab.fileStart = 0;
  const f = MP4Box.createFile(); f.appendBuffer(ab); f.flush();
  console.log(JSON.stringify(f.boxes.map(b => ({type: b.type, start: b.start, size: b.size}))));' syn_h265.mp4
check mp4dump - mp4dump syn_h265.mp4
check mp4fragment syn_h265_frag.mp4 mp4fragment syn_h265.mp4 syn_h265_frag.mp4
check mkvinfo - mkvinfo syn_h265.mkv
check tsanalyze - tsanalyze syn_h265.ts
check tstables - tstables syn_h265.ts
check tsp tsp.stderr tsp -I file syn_h265.ts -P count -O drop # count reports on stderr
check gst_codecparsers - pkg-config --cflags --libs gstreamer-codecparsers-1.0
check gst_parse_h264 - gst_parse_oracle --codec h264 syn_h264.264
check gst_parse_h265 - gst_parse_oracle --codec h265 syn_h265_amp_sao_on.265
check gst_parse_av1 - gst_parse_oracle --codec av1 --ivf syn_av1.ivf

# --- §14.4.3-14.4.5 block-level oracles --------------------------------------------------------------------
check ldecod trace_dec.txt ldecod -p InputFile=syn_h264.264 -p OutputFile=ldecod.yuv -p RefOffset=0 -p POCScale=2 \
  -p WriteUV=1 -p SilentDecode=1
check TAppDecoder hm.yuv TAppDecoder -b syn_h265_amp_sao_on.265 -o hm.yuv
check TAppDecoder_trace TraceDec.txt TAppDecoder_trace -b syn_h265_amp_sao_on.265 -o /dev/null \
  --SEIDecodedPictureHash=1
# WI-0.563 acceptance 2: an MBAFF stream (x264 --interlaced) decoded by the TRACE build names mb_field_decoding_flag.
check x264_mbaff syn_h264_mbaff.264 x264 --preset fast --interlaced --frames 5 --output syn_h264_mbaff.264 \
  src_720p_8bit.y4m
check ldecod_mbaff - sh -c 'ldecod -p InputFile=syn_h264_mbaff.264 -p OutputFile=ldecod_mbaff.yuv -p RefOffset=0 \
  -p POCScale=2 -p WriteUV=1 -p SilentDecode=1 && grep -m1 mb_field_decoding_flag trace_dec.txt'
check ffmpeg_mvs - ffmpeg_mvs syn_h264.264
check ffmpeg_mvs_qp ffmpeg_qp.csv ffmpeg_mvs --qp ffmpeg_qp.csv syn_h264.264
check hm_cu_dump - hm_cu_dump -b syn_h265_amp_sao_on.265
# The encoder configs of the {jm_cfg} and {hm_cfg} recipes (tests/corpora/synthetic/generate.py).
check jm_cfg - ls "${OVT_ORACLES_PREFIX:-/opt/ovt-oracles}/share/jm/cfg/encoder_main.cfg"
check hm_cfg - ls "${OVT_ORACLES_PREFIX:-/opt/ovt-oracles}/share/hm/cfg/encoder_randomaccess_main.cfg" \
  "${OVT_ORACLES_PREFIX:-/opt/ovt-oracles}/share/hm/cfg/encoder_lowdelay_main.cfg"
check inspect - inspect -A -a syn_av1.ivf
check dav1d dav1d.y4m dav1d -q -i syn_av1.ivf -o dav1d.y4m

# --- every tool, pin and FFmpeg component the synthetic corpus recipes use (recipe_tools.json, WI-0.553) --------
check recipe_tools - oracle-recipe-tools --check

# --- §14.4.10-14.4.15 metric and render oracles ------------------------------------------------------------
check ffpsnr ffpsnr.txt ff -i dist.y4m -i src_720p_8bit.y4m -lavfi "[0:v][1:v]psnr=stats_file=ffpsnr.txt" -f null -
check ffvmaf ffvmaf.json ff -i dist.y4m -i src_720p_8bit.y4m -lavfi \
  "[0:v][1:v]libvmaf=model=version=vmaf_v0.6.1:feature=name=psnr|name=float_ssim:log_path=ffvmaf.json:log_fmt=json:n_threads=4" \
  -f null -
check vmaf vmaf.json vmaf --reference src_720p_8bit.y4m --distorted dist.y4m --model version=vmaf_v0.6.1 \
  --feature psnr --feature float_ssim --feature float_ms_ssim --feature psnr_hvs --feature ciede --feature cambi \
  --threads 4 --json --output vmaf.json
# SPEC-ASSUMPTION(#49): x264 turns --ssim/--psnr off when --qp 0 makes the encode lossless (encoder.c),
# so §14.4.12's `--qp 0` sanity command never prints SSIM; --qp 20 does.
check x264_cli_ssim - sh -c 'x264 --ssim --psnr --qp 20 --frames 5 -o /dev/null src_720p_8bit.y4m 2>&1 | grep "SSIM Mean Y"'
check dump_psnrhvs - dump_psnrhvs -y src_720p_8bit.y4m dist.y4m
check dump_ciede2000 - dump_ciede2000 src_720p_8bit.y4m dist.y4m
check bjontegaard - python3 -c 'import bjontegaard as bd
print(bd.bd_rate([100, 200, 400, 800], [30, 33, 36, 39], [90, 180, 360, 720], [30, 33, 36, 39], method="pchip"))'
run_uvq() {  # run_uvq <model_version>: the reference CLI on CPU, one thread, all statistics as JSON
  OMP_NUM_THREADS=1 python3 "${OVT_ORACLES_PREFIX}/uvq/uvq_inference.py" "${out}/syn_h265.mp4" \
    --model_version "$1" --device cpu --threads 1 --output_all_stats
}
check uvq_1p5 - run_uvq 1.5
check uvq_1p0 - run_uvq 1.0
check mpv - mpv --no-config --vo=null --ao=null --frames=5 --msg-level=all=v syn_h265.mp4
check oracle_versions - oracle-versions

echo "smoke test: ${failures} failure(s); outputs in ${out}"
[[ "${failures}" -eq 0 ]]
