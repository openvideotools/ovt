// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The definition behind the opaque ovt_File handle (chapter 12 §12.3.6), shared by api/file_api.cpp
// (WI-1.101) and the container module (WI-1.200).

#ifndef OVT_IO_OVT_FILE_HPP
#define OVT_IO_OVT_FILE_HPP

#include <memory>

#include "ovt/cxx/io/random_access_file.hpp"

struct ovt_File {
  std::unique_ptr<ovt::io::RandomAccessFile> file;
};

#endif  // OVT_IO_OVT_FILE_HPP
