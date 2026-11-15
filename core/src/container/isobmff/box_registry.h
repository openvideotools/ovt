// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF box decoder registry (chapter 08 §8.2.9, WI-1.202). The entry table is hand-coded for now
// (box_registry.cpp); WI-1.203 replaces it with the generated box_registry.gen.cpp.

#ifndef OVT_CONTAINER_ISOBMFF_BOX_REGISTRY_H
#define OVT_CONTAINER_ISOBMFF_BOX_REGISTRY_H

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/isobmff/box_context.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

enum class BoxKind : uint8_t { Container, ContainerWithPrefix, Leaf, Opaque };

struct BoxDecoder {
  const char*
      fourcc;  // "tkhd"; "uuid:8974dbce-7be7-4c51-84f9-7148f9882554" for uuid boxes; "ilst/*" for every child of ilst
  const char* context_pattern;  // glob over the ancestor chain, innermost last: "*/stsd/*", "*/ipco", "*" (any)
  // Decodes payload fields. For ContainerWithPrefix the decoder returns the byte offset (from the payload begin)
  // where the children start; otherwise 0.
  uint64_t (*decode)(BoxContext& ctx, BitReader& br, FieldBuilder& fb);
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): field order is the §8.2.9 layout the generator emits.
struct RegistryEntry {  // generated (§8.2.10) + the hand decoder resolved at lookup
  const char* fourcc;
  const char* context_pattern;  // "*" when the registry has no container info
  BoxKind kind;
  bool is_fullbox;
  uint8_t known_versions_mask;  // bit v set = version v known (bits 0..7)
  uint32_t known_flag_mask;
  bool has_trailing_data_ok;
  const char* spec_ref;         // "ISO/IEC 14496-12:2022 §8.3.2"
  const char* description;      // MP4RA description
  const char* allowed_parents;  // comma list from the conformance JSON, e.g. "trak"; "" = unknown
  const BoxDecoder* decoder;    // nullptr = generic shell only
};

// Does `pattern` (segments separated by '/'; "*" matches zero or more ancestors) match the ancestor chain?
[[nodiscard]] bool context_matches(std::string_view pattern, const std::vector<FourCC>& ancestors);
// Number of literal (non-"*") segments: the specificity ordering of §8.2.9 (more literal segments win).
[[nodiscard]] unsigned pattern_specificity(std::string_view pattern) noexcept;

// The entry table joined lazily with the registered decoders. lookup()/register_decoder() use global_registry().
class BoxRegistry {
 public:
  explicit BoxRegistry(std::span<const RegistryEntry> base_entries = {});
  BoxRegistry(const BoxRegistry&) = delete;
  BoxRegistry& operator=(const BoxRegistry&) = delete;

  // Appends a decoder (static storage duration). Safe before main(). False for a malformed decoder.
  bool add(const BoxDecoder* d);
  // Exact fourcc with the most specific matching pattern, or a "<parent>/*" decoder; nullptr when nothing matches.
  [[nodiscard]] const RegistryEntry* lookup(std::string_view fourcc, const BoxContext& ctx) const;
  [[nodiscard]] const RegistryEntry* lookup_uuid(const std::array<uint8_t, 16>& usertype) const;

 private:
  void join_decoder(const BoxDecoder* d) const;  // adds one decoder to the index; caller holds mutex_
  void join_pending() const;  // folds decoders added since the last call into the entry index; caller holds mutex_

  mutable std::mutex mutex_;
  mutable std::deque<RegistryEntry> entries_;                                                // stable addresses
  mutable std::map<std::string, std::vector<const RegistryEntry*>, std::less<>> by_fourcc_;  // exact fourccs
  mutable std::map<std::string, std::vector<const RegistryEntry*>, std::less<>> by_parent_;  // "<parent>/*" decoders
  mutable std::vector<std::pair<std::array<uint8_t, 16>, const RegistryEntry*>> by_uuid_;
  std::vector<const BoxDecoder*> decoders_;
  mutable size_t joined_ = 0;
};

[[nodiscard]] BoxRegistry& global_registry();

// The hand-coded entry table (WI-1.203 replaces it with the generated one).
[[nodiscard]] std::span<const RegistryEntry> generated_entries() noexcept;

const RegistryEntry* lookup(std::string_view fourcc, const BoxContext& ctx);
const RegistryEntry* lookup_uuid(const std::array<uint8_t, 16>& usertype);
bool register_decoder(const BoxDecoder* d);  // appends to the global registry; lookup() joins lazily

struct DecodeOutcome {
  uint64_t children_offset = 0;  // ContainerWithPrefix: payload offset of the first child
  bool truncated = false;        // the decoder threw ParseError; CNT-1002 is on the node
};
// The walker's call of a decoder: a ParseError is caught and becomes CNT-1002 ("truncated <box>: <message>"); fields
// recorded before the throw are kept.
DecodeOutcome run_decoder(const BoxDecoder& d, BoxContext& ctx, BitReader& br, FieldBuilder& fb);

}  // namespace ovt::container::isobmff

#define OVT_BOX_DECODER_CONCAT_(a, b) a##b
#define OVT_BOX_DECODER_CONCAT(a, b) OVT_BOX_DECODER_CONCAT_(a, b)
// Hand decoders register themselves with this macro (one per translation unit per line; static-init-order independent).
#define OVT_BOX_DECODER(FOURCC, PATTERN, FN)                                                                        \
  static const ::ovt::container::isobmff::BoxDecoder OVT_BOX_DECODER_CONCAT(k_dec_, __LINE__){FOURCC, PATTERN, FN}; \
  [[maybe_unused]] static const bool OVT_BOX_DECODER_CONCAT(k_reg_, __LINE__) =                                     \
      ::ovt::container::isobmff::register_decoder(&OVT_BOX_DECODER_CONCAT(k_dec_, __LINE__));

#endif  // OVT_CONTAINER_ISOBMFF_BOX_REGISTRY_H
