// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The invalidation rules I1–I7 of chapter 06 §6.7.3 (WI-0.092 step 1): one check function per rule, each
// returning the InvalidationReason it detects or None. The functions only read; Cache and Entry delete what
// they reject. Internal header.

#ifndef OVT_CORE_CACHE_INVALIDATE_HPP
#define OVT_CORE_CACHE_INVALIDATE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include "ovt/cxx/cache/cache.hpp"
#include "ovt/cxx/cache/meta.hpp"
#include "ovt_model_generated.h"

namespace ovt::cache {

/// I2 on the raw meta.json text: CacheFormat when the text is not a JSON object, its `cache_format` is not 1,
/// or the document does not parse as a §6.7.2 record. On None, @p out (if given) receives the record.
[[nodiscard]] InvalidationReason check_cache_format(std::span<const std::uint8_t> meta_json, Meta* out = nullptr);

/// I1: SourceHash when meta.source_hash differs from @p dir_name. (Recomputing the hash from the file is the
/// stream opener's job, §6.2.8; a different hash simply names a different entry.)
[[nodiscard]] InvalidationReason check_source_hash(const Meta& meta, std::string_view dir_name) noexcept;

/// I3: SchemaMajor when any schemas.*.major differs from the compiled-in major (ovt::model::k*SchemaMajor).
[[nodiscard]] InvalidationReason check_schema_majors(const Meta& meta) noexcept;

/// I4 on the bytes of index.fb (size-prefixed, "OVTI"): Index when it does not verify, its schema_version is
/// not the model major, its codec differs from meta.codec, or (with @p codec) from the stream's codec.
[[nodiscard]] InvalidationReason check_index(std::span<const std::uint8_t> index_fb, const Meta& meta,
                                             std::optional<fb::Codec> codec);

/// I5 on one decompressed frame file (size-prefixed "OVTA"): Frame when it does not verify, its
/// frame.id.decode_index differs from @p decode_index, or (with @p plugin) its plugin id or version differs.
[[nodiscard]] InvalidationReason check_frame(std::span<const std::uint8_t> frame_fb, std::uint32_t decode_index,
                                             const std::optional<SelectedPlugin>& plugin);

/// I6: Plane when @p frame has no PlaneRef of @p stage / @p plane, or @p plane_bytes differs from that
/// PlaneRef's height * width * sample_bytes.
[[nodiscard]] InvalidationReason check_plane(const fb::FrameAnalysis& frame, fb::PlaneStage stage, std::uint8_t plane,
                                             std::size_t plane_bytes);

/// I7: OvtVersion when meta.ovt_version differs from the running OVT_VERSION_FULL. Never acted upon.
[[nodiscard]] InvalidationReason check_ovt_version(const Meta& meta) noexcept;

/// I1–I4 and I7 in order on the existing entry directory @p dir (named @p hex): the first failing rule other
/// than I7, else None. I7 and every failure are logged at info level.
[[nodiscard]] InvalidationReason check_entry_dir(const std::filesystem::path& dir, std::string_view hex,
                                                 std::optional<fb::Codec> codec);

}  // namespace ovt::cache

#endif  // OVT_CORE_CACHE_INVALIDATE_HPP
