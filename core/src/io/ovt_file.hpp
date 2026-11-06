// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The definition behind the opaque ovt_File handle (chapter 12 §12.3.6). WI-1.200 needs it for
// ovt_container_open(); the public ovt_file_open*() functions and ovt/file.h come later.
// SPEC-ASSUMPTION(#138): ovt_File wraps one io::RandomAccessFile.

#ifndef OVT_IO_OVT_FILE_HPP
#define OVT_IO_OVT_FILE_HPP

#include <memory>
#include <utility>

#include "ovt/cxx/io/random_access_file.hpp"

struct ovt_File {
  std::unique_ptr<ovt::io::RandomAccessFile> file;
  explicit ovt_File(std::unique_ptr<ovt::io::RandomAccessFile> f) : file(std::move(f)) {}
};

#endif  // OVT_IO_OVT_FILE_HPP
