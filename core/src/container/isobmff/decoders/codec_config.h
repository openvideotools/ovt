// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Group C part 1 (chapter 08 §8.3.3, WI-1.207): the codec configuration children of a sample entry as the
// `CodecPrivate` list of `TrackInfo` (§8.1.3 `codec_private`; WI-1.217 builds TrackInfo from it).

#ifndef OVT_CONTAINER_ISOBMFF_DECODERS_CODEC_CONFIG_H
#define OVT_CONTAINER_ISOBMFF_DECODERS_CODEC_CONFIG_H

#include <string_view>
#include <vector>

#include "ovt/cxx/io/random_access_file.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

// `avcC`, `hvcC`, `av1C`, `vpcC`, `esds`, `dOps`, `dvcC`, `dvvC`, `dvwC` (the §6.4 `CodecPrivate.fourcc` list).
[[nodiscard]] bool is_codec_config_fourcc(std::string_view fourcc) noexcept;

// CodecPrivate{fourcc, bytes = box payload without the box header, box_offset} for every configuration child of
// `sample_entry`, in child order. For a FullBox (`esds`, `vpcC`) `bytes` starts with version/flags. Throws
// io::IoError only.
[[nodiscard]] std::vector<fb::CodecPrivateT> codec_private_of(const io::RandomAccessFile& file,
                                                              const fb::BoxNodeT& sample_entry);

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_DECODERS_CODEC_CONFIG_H
