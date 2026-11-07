# Follow-up #230: licence file staging for libplacebo and libvmaf

Command: `cmake -P` with a copy of the `cmake_parse_arguments(PARSE_ARGV 1 arg
"NO_DOWNLOAD" "URL;SHA256;LICENSE" "LICENSE_FILES")` call of `ovt_tp_declare()`.

- `LICENSE_FILES LICENSE` gives `arg_LICENSE_FILES=[]` and
  `arg_KEYWORDS_MISSING_VALUES=[LICENSE;LICENSE_FILES]` (the new guard fails on this).
- `LICENSE_FILES ./LICENSE` gives `arg_LICENSE_FILES=[./LICENSE]`, no missing values.

Date: 2026-11-07. Machine: cloud container, Ubuntu 24.04 x86_64, CMake 3.28.3.
