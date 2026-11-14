// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ISOBMFF box decoder registry (chapter 08 §8.2.9, WI-1.202).

#include "container/isobmff/box_registry.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace ovt::container::isobmff {

namespace {

std::vector<std::string_view> split_pattern(std::string_view pattern) {
  std::vector<std::string_view> out;
  size_t pos = 0;
  while (true) {
    const size_t slash = pattern.find('/', pos);
    out.push_back(pattern.substr(pos, slash == std::string_view::npos ? slash : slash - pos));
    if (slash == std::string_view::npos) {
      return out;
    }
    pos = slash + 1;
  }
}

std::string_view as_view(const FourCC& cc) {
  return {cc.data(), cc.size()};
}

// Segments p[pi..) against ancestors a[ai..). A "*" segment takes zero or more ancestors. Depth <= 32 ancestors.
// NOLINTNEXTLINE(misc-no-recursion)
bool match_from(const std::vector<std::string_view>& p, size_t pi, const std::vector<FourCC>& a, size_t ai) {
  if (pi == p.size()) {
    return ai == a.size();
  }
  if (p[pi] == "*") {
    for (size_t take = 0; ai + take <= a.size(); ++take) {
      if (match_from(p, pi + 1, a, ai + take)) {
        return true;
      }
    }
    return false;
  }
  return ai < a.size() && as_view(a[ai]) == p[pi] && match_from(p, pi + 1, a, ai + 1);
}

// "uuid:8974dbce-7be7-4c51-84f9-7148f9882554" -> 16 bytes.
bool parse_uuid(std::string_view s, std::array<uint8_t, 16>& out) {
  if (!s.starts_with("uuid:")) {
    return false;
  }
  std::string hex;
  std::ranges::copy_if(s.substr(5), std::back_inserter(hex), [](char c) { return c != '-'; });
  if (hex.size() != 2 * out.size()) {
    return false;
  }
  for (size_t i = 0; i < out.size(); ++i) {
    const std::string_view pair = std::string_view(hex).substr(2 * i, 2);
    unsigned v = 0;
    const auto [end, ec] = std::from_chars(pair.begin(), pair.end(), v, 16);
    if (ec != std::errc{} || end != pair.end()) {
      return false;
    }
    out[i] = static_cast<uint8_t>(v);
  }
  return true;
}

unsigned score(const RegistryEntry& e, bool exact_fourcc) {
  return pattern_specificity(e.context_pattern) * 4U + (e.decoder != nullptr ? 2U : 0U) + (exact_fourcc ? 1U : 0U);
}

constexpr uint8_t kV01 = 0x03;

// clang-format off
#define OVT_ENTRY(CC, PAT, KIND, FULL, VMASK, FMASK, TRAIL, REF, DESC, PARENTS) \
  RegistryEntry{CC, PAT, BoxKind::KIND, FULL, VMASK, FMASK, TRAIL, REF, DESC, PARENTS, nullptr}
constexpr auto kBase = std::to_array<RegistryEntry>({
    OVT_ENTRY("moov", "*", Container, false, 0, 0, false, "ISO/IEC 14496-12:2022 §8.2.1", "Movie container", ""),
    OVT_ENTRY("tkhd", "*", Leaf, true, kV01, 0x7, false, "ISO/IEC 14496-12:2022 §8.3.2", "Track header, overall information about the track", "trak"),
    OVT_ENTRY("hdlr", "*/mdia", Leaf, true, 0x01, 0, false, "ISO/IEC 14496-12:2022 §8.4.3", "Handler reference for a track", "mdia"),
    OVT_ENTRY("hdlr", "*/meta", Leaf, true, 0x01, 0, false, "ISO/IEC 14496-12:2022 §8.4.3", "Handler reference for metadata", "meta"),
    OVT_ENTRY("stsd", "*", ContainerWithPrefix, true, 0x01, 0, false, "ISO/IEC 14496-12:2022 §8.5.2", "Sample descriptions", "stbl"),
    OVT_ENTRY("colr", "*/stsd/*", Leaf, false, 0, 0, false, "ISO/IEC 14496-12:2022 §12.1.5", "Colour information", ""),
    OVT_ENTRY("colr", "*/ipco", Leaf, false, 0, 0, false, "ISO/IEC 23008-12 §6.5.5", "Colour information (item property)", "ipco"),
    OVT_ENTRY("meta", "*", ContainerWithPrefix, true, 0x01, 0, false, "ISO/IEC 14496-12:2022 §8.11.1", "Metadata container", ""),
});
#undef OVT_ENTRY
// clang-format on

}  // namespace

bool context_matches(std::string_view pattern, const std::vector<FourCC>& ancestors) {
  return match_from(split_pattern(pattern), 0, ancestors, 0);
}

unsigned pattern_specificity(std::string_view pattern) noexcept {
  unsigned n = 0;
  while (!pattern.empty()) {
    const size_t slash = pattern.find('/');
    const std::string_view seg = pattern.substr(0, slash);
    n += (seg.empty() || seg == "*") ? 0U : 1U;
    pattern.remove_prefix(slash == std::string_view::npos ? pattern.size() : slash + 1);
  }
  return n;
}

BoxRegistry::BoxRegistry(std::span<const RegistryEntry> base_entries) {
  for (const RegistryEntry& e : base_entries) {
    entries_.push_back(e);
    by_fourcc_[e.fourcc].push_back(&entries_.back());
  }
}

bool BoxRegistry::add(const BoxDecoder* d) {
  if (d == nullptr || d->fourcc == nullptr || d->context_pattern == nullptr || d->decode == nullptr) {
    return false;
  }
  std::array<uint8_t, 16> scratch{};
  if (std::string_view(d->fourcc).starts_with("uuid:") && !parse_uuid(d->fourcc, scratch)) {
    return false;
  }
  const std::scoped_lock lock(mutex_);
  decoders_.push_back(d);
  return true;
}

namespace {

// The table row a decoder attaches to: the same pattern, else the "*" row, else the first undecorated row of the
// fourcc. nullptr when the table has none.
const RegistryEntry* base_row_for(const std::vector<const RegistryEntry*>& rows, const BoxDecoder& d) {
  const RegistryEntry* base = nullptr;
  for (const RegistryEntry* c : rows) {
    if (c->decoder != nullptr) {
      continue;
    }
    if (std::string_view(c->context_pattern) == d.context_pattern) {
      return c;
    }
    if (base == nullptr || std::string_view(c->context_pattern) == "*") {
      base = c;
    }
  }
  return base;
}

}  // namespace

void BoxRegistry::join_decoder(const BoxDecoder* d) const {
  const std::string_view cc = d->fourcc;
  std::array<uint8_t, 16> uuid{};
  const bool is_uuid = parse_uuid(cc, uuid);
  const bool is_parent_wild = !is_uuid && cc.size() > 2 && cc.ends_with("/*");
  const std::string key = is_uuid ? "uuid" : std::string(cc);
  RegistryEntry e{d->fourcc, d->context_pattern, BoxKind::Leaf, false, 0, 0, true, "", "", "", d};
  const auto rows = by_fourcc_.find(key);
  if (const RegistryEntry* base =
          (rows == by_fourcc_.end() || is_parent_wild) ? nullptr : base_row_for(rows->second, *d);
      base != nullptr) {
    e = *base;
    e.context_pattern = d->context_pattern;
    e.decoder = d;
  }
  entries_.push_back(e);
  const RegistryEntry* p = &entries_.back();
  if (is_uuid) {
    by_uuid_.emplace_back(uuid, p);
  } else if (is_parent_wild) {
    by_parent_[std::string(cc.substr(0, cc.size() - 2))].push_back(p);
  } else {
    by_fourcc_[key].push_back(p);
  }
}

void BoxRegistry::join_pending() const {
  for (; joined_ < decoders_.size(); ++joined_) {
    join_decoder(decoders_[joined_]);
  }
}

const RegistryEntry* BoxRegistry::lookup(std::string_view fourcc, const BoxContext& ctx) const {
  const std::scoped_lock lock(mutex_);
  join_pending();
  const RegistryEntry* best = nullptr;
  unsigned best_score = 0;
  const auto consider = [&](const std::vector<const RegistryEntry*>& rows, bool exact) {
    for (const RegistryEntry* e : rows) {
      if (!context_matches(e->context_pattern, ctx.ancestors)) {
        continue;
      }
      const unsigned s = score(*e, exact);
      if (best == nullptr || s > best_score) {
        best = e;
        best_score = s;
      }
    }
  };
  if (auto it = by_fourcc_.find(fourcc); it != by_fourcc_.end()) {
    consider(it->second, true);
  }
  if (!ctx.ancestors.empty()) {
    if (auto it = by_parent_.find(as_view(ctx.ancestors.back())); it != by_parent_.end()) {
      consider(it->second, false);
    }
  }
  return best;
}

const RegistryEntry* BoxRegistry::lookup_uuid(const std::array<uint8_t, 16>& usertype) const {
  const std::scoped_lock lock(mutex_);
  join_pending();
  const auto it = std::ranges::find_if(by_uuid_, [&](const auto& kv) { return kv.first == usertype; });
  return it == by_uuid_.end() ? nullptr : it->second;
}

BoxRegistry& global_registry() {
  static BoxRegistry registry(generated_entries());
  return registry;
}

std::span<const RegistryEntry> generated_entries() noexcept {
  return kBase;
}

const RegistryEntry* lookup(std::string_view fourcc, const BoxContext& ctx) {
  return global_registry().lookup(fourcc, ctx);
}

const RegistryEntry* lookup_uuid(const std::array<uint8_t, 16>& usertype) {
  return global_registry().lookup_uuid(usertype);
}

bool register_decoder(const BoxDecoder* d) {
  return global_registry().add(d);
}

DecodeOutcome run_decoder(const BoxDecoder& d, BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  try {
    return {d.decode(ctx, br, fb), false};
  } catch (const ParseError& e) {
    fb.report_truncated(e);
    return {0, true};
  }
}

}  // namespace ovt::container::isobmff
