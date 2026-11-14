# WI-4.211 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, system FFmpeg 6.1.1 (libavformat 60).
Date: 2026-11-14. Spec version: 1.0-rc24. Spec question: #429.

## Test 1: OP1a XDCAM file probes as `mxf` with one MPEG-2 video track and readable packets

Automated: `mxf placeholder: OP1a XDCAM file probes as mxf with one MPEG-2 video track and readable packets`
(`[container][mxf][needs-corpus][fast]`, `core/tests/container/mxf_placeholder_test.cpp`). It skips when corpus
`syn_mxf_op1a_xdcam` is absent.

The spec's `tests/corpora/mxf/op1a_xdcam.mxf` has no source (#429), so the file is synthetic recipe 77
(`syn_mxf_op1a_xdcam`, alias `op1a_xdcam.mxf`): 12 frames of 1920x1080 MPEG-2 4:2:2 at 50 Mb/s CBR, interlaced,
GOP 12, plus one PCM s24le 48 kHz track, OP1a, `+bitexact`. Its hash is recorded when the oracle image (pinned
FFmpeg n7.1.1) runs `generate.py --update-manifest`; until then the entry is `unverified`.

Local run with the recipe's command on the distribution FFmpeg 6.1.1 (two runs give identical bytes):

```console
$ ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=25,format=yuv422p -f lavfi -i sine=frequency=1000:sample_rate=48000 \
    -frames:v 12 -t 0.48 -map 0:v -map 1:a -c:v mpeg2video -threads 1 -pix_fmt yuv422p -profile:v 0 -level:v 2 \
    -b:v 50M -minrate 50M -maxrate 50M -bufsize 17825792 -rc_init_occupancy 17825792 -g 12 -bf 2 \
    -flags +ildct+ilme -top 1 -c:a pcm_s24le -fflags +bitexact -flags:v +bitexact -flags:a +bitexact \
    -f mxf syn_mxf_op1a_xdcam.mxf          # run twice
57bdab0a571ebfd61dc03639ba227467d5f11cc7d57db8f2055d44c8c0675cbb  out1.mxf
57bdab0a571ebfd61dc03639ba227467d5f11cc7d57db8f2055d44c8c0675cbb  out2.mxf   (3 092 025 bytes)

$ ffprobe -v error -show_entries stream=index,codec_name,codec_type,width,height,time_base,duration out1.mxf
index=0 codec_name=mpeg2video codec_type=video width=1920 height=1080 time_base=1/25 duration=0.480000
index=1 codec_name=pcm_s24le  codec_type=audio time_base=1/48000 duration=0.480000

$ OVT_CORPORA_DIR=<dir holding syn_mxf_op1a_xdcam/> build/linux-x64-debug/core/tests/container/ovt_tests_container "[mxf]"
All tests passed (77 assertions in 3 test cases)
```

Result: `ovt_container_open` → `format == "mxf"`, one root node with the placeholder description spanning the
whole file, two `TrackInfo` (one video with `codec == MPEG2`, 1920x1080; one audio), the same tree bytes on a
second open, and 12 non-empty video packets through `Demuxer` (Role B), the first a keyframe.
