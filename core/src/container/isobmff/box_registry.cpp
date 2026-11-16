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

// Is `parent` one of the comma-separated fourccs of a row's allowed_parents?
bool lists_parent(std::string_view allowed, std::string_view parent) {
  while (!allowed.empty()) {
    const size_t comma = allowed.find(',');
    if (allowed.substr(0, comma) == parent) {
      return true;
    }
    allowed = comma == std::string_view::npos ? std::string_view{} : allowed.substr(comma + 1);
  }
  return false;
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

// Binary search in a generated name table (sorted by the code bytes).
std::string_view find_name(std::span<const RegistryName> table, std::string_view code) noexcept {
  const auto it =
      std::ranges::lower_bound(table, code, {}, [](const RegistryName& n) { return std::string_view(n.code); });
  return (it != table.end() && std::string_view(it->code) == code) ? std::string_view(it->description)
                                                                   : std::string_view{};
}

}  // namespace

bool context_matches(std::string_view pattern, const std::vector<FourCC>& ancestors) {
  return match_from(split_pattern(pattern), 0, ancestors, 0);
}

unsigned pattern_specificity(std::string_view pattern) noexcept {
  unsigned n = 0;
  while (!pattern.empty()) {
    const size_t len = std::min(pattern.find('/'), pattern.size());  // substr() would be potentially throwing
    const std::string_view seg(pattern.data(), len);
    n += (seg.empty() || seg == "*") ? 0U : 1U;
    pattern.remove_prefix(std::min(len + 1, pattern.size()));
  }
  return n;
}

BoxRegistry::BoxRegistry(std::span<const RegistryEntry> base_entries) {
  for (const RegistryEntry& e : base_entries) {
    entries_.push_back(e);
    std::array<uint8_t, 16> uuid{};
    if (parse_uuid(e.fourcc, uuid)) {
      by_uuid_.emplace_back(uuid, &entries_.back());
    } else {
      by_fourcc_[e.fourcc].push_back(&entries_.back());
    }
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
  const RegistryEntry* base = nullptr;
  if (is_uuid) {  // the generated row of the same usertype carries the metadata
    const auto it = std::ranges::find_if(by_uuid_, [&](const auto& kv) { return kv.first == uuid; });
    base = it == by_uuid_.end() ? nullptr : it->second;
  } else if (const auto rows = by_fourcc_.find(key); rows != by_fourcc_.end() && !is_parent_wild) {
    base = base_row_for(rows->second, *d);
  }
  if (base != nullptr) {
    e = *base;
    e.context_pattern = d->context_pattern;
    e.decoder = d;
  }
  entries_.push_back(e);
  const RegistryEntry* p = &entries_.back();
  // SPEC-ASSUMPTION(#574): a "*" decoder also decorates every more specific table row of its fourcc, so a generated
  // context row (higher specificity, no decoder) cannot hide it; each decorated copy keeps the row's pattern and
  // metadata.
  if (!is_uuid && !is_parent_wild && std::string_view(d->context_pattern) == "*") {
    if (const auto rows = by_fourcc_.find(key); rows != by_fourcc_.end()) {
      const std::vector<const RegistryEntry*> table_rows = rows->second;  // the loop appends to rows->second
      for (const RegistryEntry* row : table_rows) {
        if (row->decoder == nullptr && std::string_view(row->context_pattern) != "*") {
          RegistryEntry copy = *row;
          copy.decoder = d;
          entries_.push_back(copy);
          rows->second.push_back(&entries_.back());
        }
      }
    }
  }
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
  const RegistryEntry* listed = nullptr;  // the most specific table row of this fourcc that names the parent
  if (auto it = by_fourcc_.find(fourcc); it != by_fourcc_.end()) {
    consider(it->second, true);
    if (!ctx.ancestors.empty()) {
      const std::string_view parent = as_view(ctx.ancestors.back());
      for (const RegistryEntry* e : it->second) {
        if (e->decoder == nullptr && lists_parent(e->allowed_parents, parent) &&
            context_matches(e->context_pattern, ctx.ancestors) &&
            (listed == nullptr ||
             pattern_specificity(e->context_pattern) > pattern_specificity(listed->context_pattern))) {
          listed = e;
        }
      }
    }
  }
  if (!ctx.ancestors.empty()) {
    if (auto it = by_parent_.find(as_view(ctx.ancestors.back())); it != by_parent_.end()) {
      const RegistryEntry* before = best;
      consider(it->second, false);
      // SPEC-ASSUMPTION(#592): a "<parent>/*" decoder that wins over a table row listing that parent keeps the row's
      // kind and metadata (an `hvc1` under `stsd` stays a ContainerWithPrefix FullBox-less sample entry).
      if (best != before && best->decoder != nullptr && listed != nullptr) {
        best = merged(*listed, *best);
      }
    }
  }
  return best;
}

const RegistryEntry* BoxRegistry::merged(const RegistryEntry& row, const RegistryEntry& wildcard) const {
  const auto key = std::make_pair(&row, wildcard.decoder);
  if (const auto it = merged_.find(key); it != merged_.end()) {
    return it->second;
  }
  RegistryEntry e = row;
  e.context_pattern = wildcard.context_pattern;
  e.decoder = wildcard.decoder;
  entries_.push_back(e);
  merged_.emplace(key, &entries_.back());
  return &entries_.back();
}

const RegistryEntry* BoxRegistry::lookup_uuid(const std::array<uint8_t, 16>& usertype) const {
  const std::scoped_lock lock(mutex_);
  join_pending();
  const RegistryEntry* best = nullptr;  // a decoder row wins over the generated row of the same usertype
  for (const auto& [id, e] : by_uuid_) {
    if (id == usertype && (best == nullptr || (best->decoder == nullptr && e->decoder != nullptr))) {
      best = e;
    }
  }
  return best;
}

BoxRegistry& global_registry() {
  static BoxRegistry registry(generated_entries());
  return registry;
}

std::span<const RegistryEntry> generated_entries() noexcept {
  return {&kEntries[0], kEntryCount};
}
std::span<const RegistryName> generated_brands() noexcept {
  return {&kBrands[0], BrandsCount};
}
std::span<const RegistryName> generated_handlers() noexcept {
  return {&kHandlers[0], HandlersCount};
}
std::span<const RegistryName> generated_item_types() noexcept {
  return {&kItemTypes[0], ItemTypesCount};
}
std::span<const RegistryName> generated_schemes() noexcept {
  return {&kSchemes[0], SchemesCount};
}
std::string_view brand_description(std::string_view code) noexcept {
  return find_name(generated_brands(), code);
}
std::string_view handler_description(std::string_view code) noexcept {
  return find_name(generated_handlers(), code);
}
std::string_view item_type_description(std::string_view code) noexcept {
  return find_name(generated_item_types(), code);
}
std::string_view scheme_description(std::string_view code) noexcept {
  return find_name(generated_schemes(), code);
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
