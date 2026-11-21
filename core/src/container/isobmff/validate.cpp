// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/isobmff/validate.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/box_registry.h"

namespace ovt::container::isobmff {
namespace {

using fb::BoxNodeT;

struct Required {
  std::string_view parent;
  std::string_view child;  // "stsz|stz2": any one of the alternatives satisfies the rule
};

// §8.5 CNT-1011 list. `mfhd` in `moof` and `tfhd` in `traf` are raised by the walk (fragment_boxes.cpp).
constexpr std::array kRequired = {
    Required{"moov", "mvhd"},      Required{"trak", "tkhd"}, Required{"trak", "mdia"}, Required{"mdia", "mdhd"},
    Required{"mdia", "hdlr"},      Required{"mdia", "minf"}, Required{"minf", "stbl"}, Required{"minf", "dinf"},
    Required{"stbl", "stsd"},      Required{"stbl", "stts"}, Required{"stbl", "stsc"}, Required{"stbl", "stsz|stz2"},
    Required{"stbl", "stco|co64"},
};

// §8.5 CNT-1012 list: boxes of which a parent holds at most one. `stco` and `co64` are separate here: both present is
// CNT-2008 from the sample-table decoder.
constexpr std::array<std::string_view, 12> kSingle = {"mvhd", "tkhd", "mdhd", "hdlr", "stsd", "stts",
                                                      "stsz", "stco", "co64", "tfhd", "tfdt", "mehd"};

constexpr std::array<std::string_view, 7> kWalkErrors = {"CNT-0102", "CNT-0103", "CNT-0106", "CNT-0107",
                                                         "CNT-0108", "CNT-0110", "CNT-0111"};

bool has_walk_error(const BoxNodeT& n) {
  return std::ranges::any_of(n.diagnostics, [](const auto& d) {
    return std::ranges::find(kWalkErrors, std::string_view(d->code)) != kWalkErrors.end();
  });
}

bool has_diag(const BoxNodeT& n, std::string_view code, std::string_view message) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code && d->message == message; });
}

// Adds a finding unless the same one is already there; true when added.
bool add(BoxNodeT& n, DiagCode code, std::string message) {
  const DiagInfo& info = diag(code);
  if (has_diag(n, info.code, message)) {
    return false;
  }
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = static_cast<fb::Severity>(info.severity);
  d->code = std::string(info.code);
  d->message = std::move(message);
  d->span = std::make_unique<fb::BitSpan>(0U, static_cast<uint32_t>(n.header_length) * 8U);
  d->spec_ref = std::string(info.spec_ref);
  n.diagnostics.push_back(std::move(d));
  return true;
}

bool has_child(const BoxNodeT& n, std::string_view alternatives) {
  while (!alternatives.empty()) {
    const size_t bar = alternatives.find('|');
    const std::string_view one = alternatives.substr(0, bar);
    if (std::ranges::any_of(n.children, [&](const auto& c) { return c && c->fourcc == one; })) {
      return true;
    }
    alternatives = bar == std::string_view::npos ? std::string_view{} : alternatives.substr(bar + 1);
  }
  return false;
}

// The registry's `allowed_parents` of every row of a fourcc (§8.2.10); a fourcc with a row that allows any parent
// (empty list) or no row at all is never reported.
const std::map<std::string, std::vector<std::string_view>, std::less<>>& parent_table() {
  static const auto table = [] {
    std::map<std::string, std::vector<std::string_view>, std::less<>> t;
    for (const RegistryEntry& e : generated_entries()) {
      t[e.fourcc].emplace_back(e.allowed_parents);
    }
    return t;
  }();
  return table;
}

bool list_has(std::string_view list, std::string_view item) {
  while (!list.empty()) {
    const size_t comma = list.find(',');
    if (list.substr(0, comma) == item) {
      return true;
    }
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
  }
  return false;
}

bool parent_allowed(std::string_view fourcc, std::string_view parent) {
  const auto it = parent_table().find(fourcc);
  if (it == parent_table().end()) {
    return true;
  }
  return std::ranges::any_of(it->second, [&](std::string_view list) { return list.empty() || list_has(list, parent); });
}

// Checks the children of `n` (named `parent`; "file" for the top level) and recurses. Returns the number of findings
// added below and including this level, and sets `damaged` when the subtree has a walk error.
// NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxBoxDepth, which the walk enforces.
size_t visit(BoxNodeT* self, BoxNodes& children, std::string_view parent, bool& damaged) {
  size_t added = 0;
  bool below_damaged = false;
  std::map<std::string_view, unsigned> seen;
  for (auto& c : children) {
    if (!c) {
      continue;
    }
    bool child_damaged = false;
    added += visit(c.get(), c->children, c->fourcc, child_damaged);
    below_damaged = below_damaged || child_damaged || has_walk_error(*c);
    if (c->fourcc != "uuid" && !parent_allowed(c->fourcc, parent)) {
      added += add(*c, DiagCode::CNT_1013, c->fourcc + " is not allowed in " + std::string(parent)) ? 1U : 0U;
    }
    if (std::ranges::find(kSingle, std::string_view(c->fourcc)) != kSingle.end() ||
        (parent == "file" && c->fourcc == "ftyp")) {
      if (++seen[c->fourcc] == 2) {
        added += add(*c, DiagCode::CNT_1012, "duplicate " + c->fourcc + " in " + std::string(parent)) ? 1U : 0U;
      }
    }
  }
  damaged = below_damaged || (self != nullptr && has_walk_error(*self));
  if (self != nullptr && !damaged) {
    for (const Required& r : kRequired) {
      if (r.parent == parent && !has_child(*self, r.child)) {
        added += add(*self, DiagCode::CNT_1011,
                     "required box " + std::string(r.child) + " missing in " + std::string(parent))
                     ? 1U
                     : 0U;
      }
    }
  }
  return added;
}

}  // namespace

size_t validate(BoxNodes& top_level, ValidationLevel level) {
  static_cast<void>(level);  // Quick, Standard and Deep run the same rules until WI-1.224b adds the sample-level ones
  bool damaged = false;
  return visit(nullptr, top_level, "file", damaged);
}

}  // namespace ovt::container::isobmff
