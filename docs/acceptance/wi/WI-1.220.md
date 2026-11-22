# WI-1.220 acceptance record

Spec version 1.0.0-rc46. Command for the corpus sections (needs the corpus cache, `OVT_REQUIRE_CORPORA=1`):

```sh
OVT_REQUIRE_CORPORA=1 ctest --preset linux-x64-debug -R "edit list: the corpus files of the acceptance tests"
```

| AT | Automated test | Corpus run (command output, date, machine) |
|---|---|---|
| 1 | `edit list: a B-frame delay removed by elst is one normal segment (x264 default)` (synthetic, fast); corpus section `syn_h264_weightedpred_ltr` | pending: Mac Mini run, to be recorded here |
| 2 | `edit list: a QuickTime empty edit delays the media by its duration` (synthetic, fast); corpus section `syn_mp4_elst` (entry `syn_mp4_edit_list_frag_cenc`) | pending: Mac Mini run, to be recorded here |
| 3 | `elst ↔ tfdt: CNT-3010 when media_time matches neither T0+C0 nor C0` (direct `FragmentBuild`) | pending WI-1.224b: the hand-broken `syn_cmaf_bad_elst_cmfv` file joins the `isobmff-broken` set there |
