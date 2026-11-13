# YUView statistics CSV samples (WI-0.089)

These are hand-written importer inputs for `read_yuview_csv` (chapter 06 §6.8.2). They follow the "Statistics
Extension Syntax" grammar described in appendix F.5, and no YUView file was copied (clean-room rule, #233
item 4). They're licensed CC0-1.0 (see the `.license` files).

| File | What it covers |
|---|---|
| `foreign_types.csv` | Type names YUView's own decoders use (spaces, non-OVT names), a `v1.22` version line, a vector type with `scaleFactor 4`, and two POCs |
| `ovt_mixed_poc.csv` | CRLF line endings, rows out of order, OVT vector, map and range types, and three POCs (only POC 2 is complete) |
| `malformed.csv` | Wrong field counts, a non-numeric field, a zero width, an out-of-range value, an undeclared type and a bad type id |

`test_yuview_csv` asserts the grid and the diagnostics the importer produces for each file.
