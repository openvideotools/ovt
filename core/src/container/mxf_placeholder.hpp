// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The MXF placeholder reader (chapter 08 §8.6.7, WI-4.211). OVT has no own KLV reader yet (the decision is
// deferred, docs/adr/ADR-CNT-MXF.md): an MXF file opens through libavformat (Role B) and its ContainerTree holds
// one root node that says so, plus one TrackInfo per libavformat stream.

#ifndef OVT_CONTAINER_MXF_PLACEHOLDER_HPP
#define OVT_CONTAINER_MXF_PLACEHOLDER_HPP

#include <memory>
#include <string_view>

#include "container/container.hpp"
#include "ovt/cxx/io/random_access_file.hpp"

namespace ovt::container::mxf {

// The description of the single root node (the Interface line of WI-4.211).
inline constexpr std::string_view kPlaceholderDescription = "MXF structure view not implemented (phase 4+)";

// Opens `file` with the libavformat "mxf" demuxer and builds the placeholder tree. nullptr when libavformat
// rejects the file; *why (if given) then holds the demuxer's status (OVT_E_CORRUPT, OVT_E_UNSUPPORTED, OVT_E_IO
// or OVT_E_NOMEM). `file` must outlive the call only: the demuxer is closed before this returns.
[[nodiscard]] std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why = nullptr);

}  // namespace ovt::container::mxf

#endif  // OVT_CONTAINER_MXF_PLACEHOLDER_HPP
