# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The `io` module of libovt-core (chapter 0 §0.3, chapter 7 §7.1): ovt::io::RandomAccessFile and its mmap
# and pread backends (WI-1.100). Sources go into ovt_core_objects, which ovt_core absorbs.

target_sources(ovt_core_objects PRIVATE random_access_file.cpp mmap_file.cpp pread_file.cpp)
