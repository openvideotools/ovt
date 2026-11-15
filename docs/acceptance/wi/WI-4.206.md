# WI-4.206 acceptance record (parts a and b)

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, ffmpeg/ffprobe 6.1.1 (distribution, with libx264).
Date: 2026-11-15. Spec question: #569.

## Test 1: `ts_h264_dvb` PES count and PTS/DTS against `ffprobe -show_packets`

Automated: `corpus: ts_h264_dvb PES count and timestamps` (`[container][mpegts][pes][needs-corpus][fast]`).
Despite its id, corpus `ts_h264_dvb` (`input/test-012.ts`, 373 556 bytes) carries packets only on PID 0x42C
(DVB teletext); the video PID of its PMT has none.

```console
$ ffprobe -v error -show_packets -of csv=p=0 -show_entries packet=pts,dts,size,pos test-012.ts | head -2
N/A,N/A,323,0,MPEGTS Stream ID,189
N/A,N/A,323,564,MPEGTS Stream ID,189
$ ffprobe ... | awk -F, '{s+=$4; n++} END{print n, s}'
916 170894444
```

OVT: 916 PES on PID 0x42C, every one 323 payload bytes, sum of PES start-packet offsets 170 894 444 (equal to
ffprobe's `pos` list). ffprobe prints no PTS because libavformat discards teletext PTS when the program has no PCR
(this excerpt has none), so the PTS are compared with an independent decode of the PES headers instead:

```console
$ python3 - <<'PY'   # PTS of every PES start on PID 0x42C
d = open('test-012.ts', 'rb').read(); pts = []
for i in range(len(d) // 188):
    p = d[i*188:(i+1)*188]
    if (((p[1] & 0x1f) << 8) | p[2]) == 0x42c and p[1] & 0x40:
        b = p[13:18]
        pts.append(((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | b[4] >> 1)
print(len(pts), pts[0], pts[-1], sum(pts))
PY
916 3856608233 3859902233 3534161793428
```

OVT gives the same first, last and summed PTS; no PES carries a DTS.

Second comparison with B-frames and DTS: recipe 33 regenerated locally (`syn_ts_clean.ts` and
`syn_ts_cc_errors.ts`; the bytes differ from the manifest hashes because of the ffmpeg version). For both files
OVT's 120 PES of PID 0x100 equal `ffprobe -show_packets -select_streams v` in `pts`, `pos` and `size` for every
packet, and in `dts` for every PES that carries one; the 24 PES without a DTS have `dts = -1` in OVT where ffprobe
fills in the PTS (the SampleTable uses the PTS then, as ffprobe does). The dump came from a throw-away test case
that printed `File::pes`; it is not committed.

### Automated comparison with B-frames (part b)

`corpus: ts_test_025 PTS/DTS per PES equal the ffprobe golden` (`[needs-corpus]`; `ts_test_025` is tagged `slow`,
so it runs where the slow corpora are fetched) compares every PES of tsduck-test `input/test-025.ts` (MPEG-2 video
with B-frames, MPEG-1 audio; 198 PES) with the committed golden `tests/golden/mpegts/ts_test_025.ffprobe-packets.json`:

```console
$ ffprobe -v error -show_packets -of json -show_entries packet=stream_index,pts,dts,pos test-025.ts
```

reduced to `{pid, pts, dts, pos}` per packet in file order (stream 0 = PID 0x1000, 1 = 0x1001). OVT matches all 198
(PID, PTS, DTS, position) tuples, 25 of them with DTS ≠ PTS. Sizes are not compared: ffprobe's MPEG-2 video parser
moves a few bytes between pictures (61 video packets differ by 1 to 3 bytes from the PES payload size).
`ts_test_026` (H.264) was not usable as a golden because ffprobe's parser splits its PES into more packets (145 vs 51).

## Test 2: a 1 s PCR gap gives `CNT-TS-2003`

Automated: `timing: PCR repetition, discontinuity and accuracy checks` section "a 1 s gap of missing PCRs is a
repetition error" (`[container][mpegts][timing][fast]`, synthetic stream: one `CNT-TS-2003` "1020 ms", one
`CNT-TS-2004`), and `corpus: syn_ts_cc_errors has a 1 s PCR gap` (`[needs-corpus]`). On the regenerated recipe 33
files OVT reports:

| File | CNT-TS-1006 | CNT-TS-2003 | CNT-TS-2004 |
|---|---|---|---|
| `syn_ts_clean.ts` | 0 | 39 (every 100 ms PCR interval of ffmpeg's VBR muxer) | 0 |
| `syn_ts_cc_errors.ts` | 7 | 29, one of them the 1 100 ms gap | 1 (the gap) |
