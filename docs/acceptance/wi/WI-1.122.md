# WI-1.122 acceptance record

Spec version: 1.0-rc41. The ITU conformance site and the JM repository are not reachable from the cloud container,
so the JM oracle ran on the Mac Mini and the corpus tests run in CI (fast streams) and on the Mini (slow streams).

## The JM 19.0 reference lists (oracle for acceptance test 1)

- **Date:** 2026-11-17 05:27 UTC
- **Machine:** Mac Mini runner, Apple M4 (arm64), macOS 15.8.2, Apple clang 17.0.0
- **JM:** JM-19.0, commit `ef2362d159339d583fc18cea2de4d4bed5a1417c`, the pin of `tests/oracles/build_jm.sh`. The
  committed oracle build is untouched; this is a separate scratch build.

JM's `TRACE` build does not print the reference lists (contrary to the WI's step 6), and its own `PRINTREFLIST`
blocks print only for MVC profiles. `tests/oracles/patches/jm-reflist-print.patch` adds one print to `init_slice()`
in `ldecod/src/image.c`, after `init_lists()` and `reorder_lists()`, so it prints the final lists after
modification. The print covers the first slice of each primary coded picture of the base view. Each list is
truncated to `num_ref_idx_lX_active`, and its entries are the referenced picture's `poc`: the frame POC in a frame
list, and the field POC in a field list.

```sh
git init jm && cd jm
git fetch --depth 1 https://vcgit.hhi.fraunhofer.de/jvet/JM.git ef2362d159339d583fc18cea2de4d4bed5a1417c
git checkout FETCH_HEAD
git apply <ovt>/tests/oracles/patches/jm-reflist-print.patch
make -C ldecod -j8 CC="clang -fcommon -w"
# for each of the 38 jvt_* corpus ids:
OVT_REFLIST_OUT=<id>.txt bin/ldecod.exe -i <corpus cache>/<id>/<stream> -o /dev/null
gzip -9 -n -c <id>.txt > tests/golden/h264/<id>.jm-reflist.txt.gz
```

All 38 streams decode with exit code 0, and a second run gave byte-identical files. The line counts match JM's
per-frame log, with a field pair giving two lines. MVCNV_1 decodes its base view only (JM's default
`DecodeAllLayers=0`), and SVCBST_1 its AVC base layer. No first-slice list contains a non-existing frame or "no
reference picture". The only warning is cvmp_mot_frm_L31_B's "zero_byte shall exist" (a byte-stream nit), and that
stream decodes normally.

## Test 1: declared_refs equals JM's RefPicList0 ∪ RefPicList1 of the first slice

Automated: `h264_reflist_vs_jm: fast JVT streams` (17 streams, labels `fast`, `needs-corpus`) and
`h264_reflist_vs_jm: slow JVT streams` (the other 21, label `slow`), in `core/tests/index/test_index_h264_dpb.cpp`.
Each case runs the H.264 dispatch of the builder over the stream and compares every picture with the golden, in
decode order. The structure, the POC, and both lists entry by entry as POCs must match, which is stricter than the
union. `declared_refs` must also equal the frame stores those lists name, in order and without duplicates. The lists
come from the frame records the builder keeps privately (§7.4.4 "RefSlot names"), so a field reference to a
frame-coded picture compares by its field POC.

## Test 2: MR6_BT_B … MR9_BT_B stay within max_num_ref_frames

Automated: `h264_dpb: MR6_BT_B and MR9_BT_B stay within max_num_ref_frames` (`fast`, `needs-corpus`). The corpus
has MR6_BT_B and MR9_BT_B of that range. For every frame, `dpb_after.size()` is at most the largest
`max_num_ref_frames` of the stream's SPSs, and the index has no `SYN-H264-0105` or `SYN-H264-0106`. The WI text says
"SYN-H264-0105/0006"; 0006 is not a registered code, so this is read as 0106.

## Test 3: MR1_BT_A infers non-existing frames

Automated: `h264_dpb: MR1_BT_A infers non-existing frames` (`fast`, `needs-corpus`): the index has at least one
`SYN-H264-0102`.
