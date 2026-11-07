# Known differences between the H.264/HEVC header oracles

`tools/goldens/h26x_selfcheck.py` (WI-0.567, chapter 14 §14.4.1 and §14.4.2) compares every `<id>.trace_headers.json`
(FFmpeg CBS) with its `<id>.gstparse.json` (GStreamer codecparsers) member by member. A value that the two oracles
name identically and report differently is a disagreement; it must be listed here, or `gen_h26x.py` fails.

CBS and GStreamer name some elements differently; such a difference goes into a name map
(`tests/oracles/normalize/cbs_name_map.json`, `tools/gen_gst_fields.py` `RENAMES`), never into OVT's spec names.
This file lists only value disagreements that remain after that.

Row format: `| <codec>.<structure>.<member, subscripts removed> | reason | optional corpus id pattern |`.
`fnmatch` patterns are allowed in the first and third columns.

| Member | Reason | Corpus ids |
|---|---|---|
