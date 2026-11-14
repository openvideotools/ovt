// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The DASH MPD parser and segment URL expansion (chapter 08 §8.10, WI-4.208). See mpd.hpp.

#include "container/virtual_file/mpd.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/tree_fb.hpp"

namespace ovt::container::mpd {
namespace {

// A representation never expands past this many segments (a hostile `r="1000000000"` must not exhaust memory).
// manifest-wide: the budget is shared by all representations, so a hostile MPD with many representations cannot
// allocate more than this in total.
constexpr size_t kMaxSegments = 1'000'000;

// base + k * step without signed overflow; false when the result does not fit.
bool checked_madd(int64_t base, int64_t k, int64_t step, int64_t& out) {
  int64_t product = 0;
  return !__builtin_mul_overflow(k, step, &product) && !__builtin_add_overflow(base, product, &out);
}

using Chain = std::vector<pugi::xml_node>;  // most specific element first

std::string_view local_name(const char* qualified) {
  const std::string_view name(qualified);
  const size_t colon = name.rfind(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::vector<pugi::xml_node> children(const pugi::xml_node& parent, std::string_view name) {
  std::vector<pugi::xml_node> out;
  for (pugi::xml_node c = parent.first_child(); !c.empty(); c = c.next_sibling()) {
    if (c.type() == pugi::node_element && local_name(c.name()) == name) {
      out.push_back(c);
    }
  }
  return out;
}

pugi::xml_node first_child(const pugi::xml_node& parent, std::string_view name) {
  for (pugi::xml_node c = parent.first_child(); !c.empty(); c = c.next_sibling()) {
    if (c.type() == pugi::node_element && local_name(c.name()) == name) {
      return c;
    }
  }
  return {};
}

std::string trim(std::string_view s) {
  const auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && is_ws(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && is_ws(s.back())) {
    s.remove_suffix(1);
  }
  return std::string(s);
}

// Attribute value by exact name; nullptr when absent.
const char* attr(const pugi::xml_node& n, const char* name) {
  const pugi::xml_attribute a = n.attribute(name);
  return a.empty() ? nullptr : a.value();
}

// Attribute whose name ends in `:suffix` (namespace-prefix tolerant: `cenc:default_KID`, `xlink:href`).
const char* prefixed_attr(const pugi::xml_node& n, std::string_view suffix) {
  for (pugi::xml_attribute a = n.first_attribute(); !a.empty(); a = a.next_attribute()) {
    const std::string_view name(a.name());
    const size_t colon = name.find(':');
    if (colon != std::string_view::npos && name.substr(colon + 1) == suffix) {
      return a.value();
    }
  }
  return nullptr;
}

const char* chain_attr(const Chain& chain, const char* name) {
  for (const pugi::xml_node& n : chain) {
    if (const char* v = attr(n, name)) {
      return v;
    }
  }
  return nullptr;
}

pugi::xml_node chain_child(const Chain& chain, std::string_view name) {
  for (const pugi::xml_node& n : chain) {
    if (pugi::xml_node c = first_child(n, name)) {
      return c;
    }
  }
  return {};
}

// Whole-string number parse; false on any trailing or missing character.
template <typename T>
bool from_chars_exact(std::string_view s, T& value) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): std::from_chars takes a pointer pair
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  return ec == std::errc() && ptr == s.data() + s.size() &&
         !s.empty();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

// Unsigned decimal "123" or "12.345" without std::from_chars(double), which Apple's libc++ marks unavailable
// before macOS 26 (and which would be locale-proof anyway). Digits past the 9th fractional one are dropped.
std::optional<long double> parse_decimal(std::string_view s) {
  const size_t dot = s.find('.');
  const std::string_view whole = s.substr(0, dot);
  const std::string_view frac = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
  uint64_t integer = 0;
  if (!whole.empty() && !from_chars_exact(whole, integer)) {
    return std::nullopt;
  }
  if (whole.empty() && frac.empty()) {
    return std::nullopt;
  }
  auto value = static_cast<long double>(integer);
  long double place = 0.1L;
  for (size_t i = 0; i < frac.size(); ++i) {
    if (frac[i] < '0' || frac[i] > '9') {
      return std::nullopt;
    }
    if (i < 9) {
      value += static_cast<long double>(frac[i] - '0') * place;
      place /= 10.0L;
    }
  }
  return value;
}

std::optional<uint64_t> parse_uint(std::string_view s) {
  const std::string t = trim(s);
  uint64_t v = 0;
  return from_chars_exact(t, v) ? std::optional<uint64_t>(v) : std::nullopt;
}

std::optional<int64_t> parse_int(std::string_view s) {
  const std::string t = trim(s);
  int64_t v = 0;
  return from_chars_exact(t, v) ? std::optional<int64_t>(v) : std::nullopt;
}

// Seconds in one `unit` of an xs:duration; 0 for an invalid unit. Years are 365 days and months 30 days.
double duration_unit_seconds(bool in_time, char unit) {
  constexpr double kDay = 86400.0;
  if (!in_time) {
    return unit == 'Y' ? 365.0 * kDay : unit == 'M' ? 30.0 * kDay : unit == 'D' ? kDay : 0.0;
  }
  return unit == 'H' ? 3600.0 : unit == 'M' ? 60.0 : unit == 'S' ? 1.0 : 0.0;
}

// xs:duration (PnYnMnDTnHnMnS) to milliseconds.
std::optional<int64_t> parse_duration_ms(std::string_view text) {
  std::string_view s = text;
  const std::string trimmed = trim(text);
  s = trimmed;
  const bool negative = s.starts_with('-');
  if (negative) {
    s.remove_prefix(1);
  }
  if (!s.starts_with('P')) {
    return std::nullopt;
  }
  s.remove_prefix(1);
  bool in_time = false;
  bool any = false;
  double total_s = 0.0;
  while (!s.empty()) {
    if (s.front() == 'T') {
      in_time = true;
      s.remove_prefix(1);
      continue;
    }
    const size_t digits = s.find_first_not_of("0123456789.");
    if (digits == 0 || digits == std::string_view::npos) {
      return std::nullopt;
    }
    const double scale = duration_unit_seconds(in_time, s[digits]);
    const auto value = parse_decimal(s.substr(0, digits));
    if (scale == 0.0 || !value) {
      return std::nullopt;
    }
    total_s += static_cast<double>(*value) * scale;
    any = true;
    s.remove_prefix(digits + 1);
  }
  if (!any || total_s > 1.0e12) {
    return std::nullopt;
  }
  const auto ms = static_cast<int64_t>(std::llround(total_s * 1000.0));
  return negative ? -ms : ms;
}

// "first-last" inclusive byte range; nullopt when malformed.
std::optional<ByteRange> parse_range(std::string_view text) {
  const std::string s = trim(text);
  const size_t dash = s.find('-');
  if (dash == std::string::npos) {
    return std::nullopt;
  }
  const auto first = parse_uint(std::string_view(s).substr(0, dash));
  const auto last = parse_uint(std::string_view(s).substr(dash + 1));
  // last = UINT64_MAX is rejected so that offset + length never wraps (it would equal 2^64).
  if (!first || !last || *last < *first || *last == std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  return ByteRange{*first, *last - *first + 1};
}

// ---------------------------------------------------------------------------------------------------------
// RFC 3986 reference resolution

struct UriParts {
  std::string scheme;
  std::string authority;
  std::string path;
  std::string query;
  bool has_scheme = false;
  bool has_authority = false;
  bool has_query = false;
};

UriParts split_uri(std::string_view u) {
  UriParts p;
  // scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
  size_t i = 0;
  if (!u.empty() && std::isalpha(static_cast<unsigned char>(u[0])) != 0) {
    i = 1;
    while (i < u.size() &&
           (std::isalnum(static_cast<unsigned char>(u[i])) != 0 || u[i] == '+' || u[i] == '-' || u[i] == '.')) {
      ++i;
    }
    if (i < u.size() && u[i] == ':') {
      p.scheme = std::string(u.substr(0, i));
      p.has_scheme = true;
      u.remove_prefix(i + 1);
    }
  }
  const size_t frag = u.find('#');
  if (frag != std::string_view::npos) {
    u = u.substr(0, frag);  // fragments are not sent to the server
  }
  if (u.starts_with("//")) {
    u.remove_prefix(2);
    const size_t end = u.find_first_of("/?");
    p.authority = std::string(u.substr(0, end));
    p.has_authority = true;
    u = end == std::string_view::npos ? std::string_view() : u.substr(end);
  }
  const size_t q = u.find('?');
  if (q != std::string_view::npos) {
    p.query = std::string(u.substr(q + 1));
    p.has_query = true;
    u = u.substr(0, q);
  }
  p.path = std::string(u);
  return p;
}

std::string remove_dot_segments(std::string_view path) {
  // A dot segment is either at the start of the path or right after a '/': a path with neither is returned as is
  // (the common case, taken once per expanded segment).
  if (!path.starts_with('.') && path.find("/.") == std::string_view::npos) {
    return std::string(path);
  }
  std::string out;
  while (!path.empty()) {
    if (path.starts_with("../")) {
      path.remove_prefix(3);
    } else if (path.starts_with("./") || path.starts_with("/./")) {
      path.remove_prefix(2);
    } else if (path == "/.") {
      path = "/";
    } else if (path.starts_with("/../") || path == "/..") {
      path = path == "/.." ? std::string_view("/") : path.substr(3);
      const size_t slash = out.rfind('/');
      out.erase(slash == std::string::npos ? 0 : slash);
    } else if (path == "." || path == "..") {
      path = {};
    } else {
      const size_t start = path.front() == '/' ? 1 : 0;
      const size_t next = path.find('/', start);
      const size_t len = next == std::string_view::npos ? path.size() : next;
      out.append(path.substr(0, len));
      path.remove_prefix(len);
    }
  }
  return out;
}

std::string merge_paths(const UriParts& base, std::string_view ref_path) {
  if (base.has_authority && base.path.empty()) {
    return "/" + std::string(ref_path);
  }
  const size_t slash = base.path.rfind('/');
  return (slash == std::string::npos ? std::string() : base.path.substr(0, slash + 1)) + std::string(ref_path);
}

std::string recompose(const UriParts& p) {
  std::string out;
  if (p.has_scheme) {
    out += p.scheme + ":";
  }
  if (p.has_authority) {
    out += "//" + p.authority;
  }
  out += p.path;
  if (p.has_query) {
    out += "?" + p.query;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------
// Template identifiers

void append_padded(std::string& out, int64_t value, size_t width) {
  const bool negative = value < 0;
  const uint64_t magnitude = negative ? (~static_cast<uint64_t>(value) + 1U) : static_cast<uint64_t>(value);
  const std::string digits = std::to_string(magnitude);
  if (negative) {
    out += '-';
  }
  if (digits.size() < width) {
    out.append(width - digits.size(), '0');
  }
  out += digits;
}

// Width of a `%0Nd` format tag; nullopt when `fmt` is not one. An empty tag means width 1.
std::optional<size_t> parse_format_tag(std::string_view fmt) {
  if (fmt.empty()) {
    return size_t{1};
  }
  if (fmt.size() < 3 || fmt[0] != '%' || fmt[1] != '0' || fmt.back() != 'd') {
    return std::nullopt;
  }
  const auto width = parse_uint(fmt.substr(2, fmt.size() - 3));
  if (!width || *width > 64) {
    return std::nullopt;
  }
  return static_cast<size_t>(*width);
}

}  // namespace

namespace {

// RFC 3986 §5.2.2 against an already split base (`base_empty` when there is no base at all). The segment loops split
// the representation's base once instead of once per segment.
std::string resolve_against(const UriParts& b, bool base_empty, std::string_view ref) {
  const UriParts r = split_uri(ref);
  if (r.has_scheme || base_empty) {
    UriParts t = r;
    t.path = remove_dot_segments(r.path);
    return recompose(t);
  }
  UriParts t;
  t.scheme = b.scheme;
  t.has_scheme = b.has_scheme;
  if (r.has_authority) {
    t.authority = r.authority;
    t.has_authority = true;
    t.path = remove_dot_segments(r.path);
    t.query = r.query;
    t.has_query = r.has_query;
  } else {
    t.authority = b.authority;
    t.has_authority = b.has_authority;
    if (r.path.empty()) {
      t.path = b.path;
      t.query = r.has_query ? r.query : b.query;
      t.has_query = r.has_query || b.has_query;
    } else {
      t.path = remove_dot_segments(r.path.front() == '/' ? r.path : merge_paths(b, r.path));
      t.query = r.query;
      t.has_query = r.has_query;
    }
  }
  return recompose(t);
}

}  // namespace

std::string resolve_url(std::string_view base, std::string_view ref) {
  return resolve_against(split_uri(base), base.empty(), ref);
}

namespace {

// Appends the expansion of `tmpl` to `out`; false (with `out` partly written) when the template is invalid.
bool expand_template_into(std::string& out, std::string_view tmpl, const TemplateValues& values) {
  size_t i = 0;
  while (i < tmpl.size()) {
    const size_t dollar = tmpl.find('$', i);
    if (dollar == std::string_view::npos) {
      out.append(tmpl.substr(i));
      break;
    }
    out.append(tmpl.substr(i, dollar - i));
    const size_t close = tmpl.find('$', dollar + 1);
    if (close == std::string_view::npos) {
      return false;
    }
    const std::string_view token = tmpl.substr(dollar + 1, close - dollar - 1);
    i = close + 1;
    if (token.empty()) {
      out += '$';
      continue;
    }
    const size_t pct = token.find('%');
    const std::string_view name = token.substr(0, pct);
    const std::string_view fmt = pct == std::string_view::npos ? std::string_view() : token.substr(pct);
    if (name == "RepresentationID") {
      if (!fmt.empty()) {
        return false;  // no format tag allowed (23009-1 Table 16)
      }
      out.append(values.representation_id);
      continue;
    }
    int64_t value = 0;
    if (name == "Number") {
      value = values.number;
    } else if (name == "Bandwidth") {
      value = values.bandwidth;
    } else if (name == "Time") {
      value = values.time;
    } else {
      return false;
    }
    const auto width = parse_format_tag(fmt);
    if (!width) {
      return false;
    }
    append_padded(out, value, *width);
  }
  return true;
}

}  // namespace

std::optional<std::string> expand_template(std::string_view tmpl, const TemplateValues& values) {
  std::string out;
  if (!expand_template_into(out, tmpl, values)) {
    return std::nullopt;
  }
  return out;
}

namespace {

struct Context {
  const Options& options;
  Manifest& manifest;
  size_t segments_left = kMaxSegments;  // manifest-wide segment budget
};

// Records `n` segments as produced; the budget saturates at 0 (init segments are not capped).
void spend(Context& ctx, size_t n) {
  ctx.segments_left -= std::min(n, ctx.segments_left);
}

void diag(Context& ctx, DiagCode code, std::string message, const pugi::xml_node& at = {}) {
  const std::ptrdiff_t off = at.empty() ? -1 : at.offset_debug();
  ctx.manifest.diagnostics.push_back({code, std::move(message), off > 0 ? static_cast<uint64_t>(off) : 0});
}

std::string base_url_of(const pugi::xml_node& element, const std::string& parent) {
  const pugi::xml_node b = first_child(element, "BaseURL");
  if (!b) {
    return parent;
  }
  const std::string text = trim(b.text().get());
  return text.empty() ? parent : resolve_url(parent, text);
}

struct SegmentInfo {
  Chain template_chain;
  Chain list_chain;
  Chain base_chain;
};

// Most specific level that carries any segment-information element decides the kind; every level supplies
// attributes the more specific ones lack (23009-1 §5.3.9.1).
SegmentInfo collect_segment_info(const std::array<pugi::xml_node, 3>& levels /* rep, set, period */) {
  SegmentInfo info;
  enum class Kind : uint8_t { None, Template, List, Base } kind = Kind::None;
  for (const pugi::xml_node& level : levels) {
    if (!first_child(level, "SegmentTemplate").empty()) {
      kind = Kind::Template;
    } else if (!first_child(level, "SegmentList").empty()) {
      kind = Kind::List;
    } else if (!first_child(level, "SegmentBase").empty()) {
      kind = Kind::Base;
    }
    if (kind != Kind::None) {
      break;
    }
  }
  for (const pugi::xml_node& level : levels) {
    if (kind == Kind::Template) {
      if (auto n = first_child(level, "SegmentTemplate")) {
        info.template_chain.push_back(n);
      }
    } else if (kind == Kind::List) {
      if (auto n = first_child(level, "SegmentList")) {
        info.list_chain.push_back(n);
      }
    } else if (kind == Kind::Base) {
      if (auto n = first_child(level, "SegmentBase")) {
        info.base_chain.push_back(n);
      }
    }
  }
  return info;
}

struct TimelineEntry {
  int64_t time = 0;
  int64_t duration = 0;
  int64_t number = 0;
};

int64_t saturate(long double v) {
  constexpr long double kMax = 9.0e18L;
  return static_cast<int64_t>(std::clamp(v, -kMax, kMax));
}

struct Timing {
  uint32_t timescale = 1;
  int64_t presentation_time_offset = 0;
  int64_t start_number = 1;
  int64_t period_duration_ms = -1;
};

Timing read_timing(Context& ctx, const Chain& chain, int64_t period_duration_ms) {
  Timing t;
  t.period_duration_ms = period_duration_ms;
  if (const char* v = chain_attr(chain, "timescale")) {
    const auto ts = parse_uint(v);
    if (ts && *ts > 0 && *ts <= std::numeric_limits<uint32_t>::max()) {
      t.timescale = static_cast<uint32_t>(*ts);
    } else {
      diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid timescale `") + v + "`; using 1", chain.front());
    }
  }
  if (const char* v = chain_attr(chain, "presentationTimeOffset")) {
    if (const auto p = parse_int(v)) {
      t.presentation_time_offset = *p;
    } else {
      diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid presentationTimeOffset `") + v + "`; using 0",
           chain.front());
    }
  }
  if (const char* v = chain_attr(chain, "startNumber")) {
    if (const auto n = parse_int(v)) {
      t.start_number = *n;
    } else {
      diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid startNumber `") + v + "`; using 1", chain.front());
    }
  }
  return t;
}

// ceil(a / b) for a >= 0, b > 0, computed in long double (exact for the magnitudes real manifests have; a hostile
// huge value saturates and is then cut by kMaxSegments).
int64_t ceil_div(long double a, long double b) {
  return saturate(std::ceil(a / b));
}

// Period end on the media timeline of the representation, or nullopt when the period duration is unknown.
std::optional<int64_t> period_end_media(const Timing& t) {
  if (t.period_duration_ms < 0) {
    return std::nullopt;
  }
  int64_t end = 0;
  const int64_t span = saturate(
      std::floor(static_cast<long double>(t.period_duration_ms) * static_cast<long double>(t.timescale) / 1000.0L));
  return __builtin_add_overflow(t.presentation_time_offset, span, &end) ? std::numeric_limits<int64_t>::max() : end;
}

std::optional<int64_t> int_attr(const pugi::xml_node& n, const char* name) {
  const char* v = attr(n, name);
  return v == nullptr ? std::nullopt : parse_int(v);
}

// Number of segments an S element stands for. `r >= 0` repeats r more times; `r < 0` runs to the start of the
// next S (when it has a `t`) or to the period end.
int64_t repeat_count(Context& ctx, const pugi::xml_node& s, const pugi::xml_node& next, int64_t cursor, int64_t d,
                     const Timing& t) {
  const int64_t repeat = int_attr(s, "r").value_or(0);
  if (repeat >= 0) {
    const auto cap = static_cast<int64_t>(ctx.segments_left);
    return repeat >= cap ? cap + 1 : repeat + 1;
  }
  std::optional<int64_t> end;
  if (!next.empty()) {
    end = int_attr(next, "t");
  }
  if (!end) {
    end = period_end_media(t);
  }
  if (!end) {
    diag(ctx, DiagCode::CNT_DASH_0004, "SegmentTimeline `r=-1` with no period duration; one segment kept", s);
    return 1;
  }
  return *end > cursor
             ? ceil_div(static_cast<long double>(*end) - static_cast<long double>(cursor), static_cast<long double>(d))
             : 0;
}

// SegmentTimeline -> one entry per segment (S@r expanded, r = -1 filled to the next S@t or the period end), at most
// `limit` of them. `limit` is the remaining segment budget, or less when the caller needs fewer entries; reaching it
// is reported as a truncation only when `report_limit` is set.
std::vector<TimelineEntry> expand_timeline(Context& ctx, const pugi::xml_node& timeline, const Timing& t, size_t limit,
                                           bool report_limit) {
  std::vector<TimelineEntry> out;
  const std::vector<pugi::xml_node> items = children(timeline, "S");
  int64_t cursor = 0;
  int64_t number = t.start_number;
  for (size_t i = 0; i < items.size(); ++i) {
    const pugi::xml_node& s = items[i];
    const int64_t d = int_attr(s, "d").value_or(0);
    if (d <= 0) {
      diag(ctx, DiagCode::CNT_DASH_0007, "SegmentTimeline S without a positive `d`; skipped", s);
      continue;
    }
    cursor = int_attr(s, "t").value_or(cursor);
    number = int_attr(s, "n").value_or(number);
    const int64_t count = repeat_count(ctx, s, i + 1 < items.size() ? items[i + 1] : pugi::xml_node(), cursor, d, t);
    for (int64_t k = 0; k < count; ++k) {
      const bool overflow =
          cursor > std::numeric_limits<int64_t>::max() - d || number == std::numeric_limits<int64_t>::max();
      if (out.size() >= limit || overflow) {
        if (overflow || report_limit) {
          diag(ctx, DiagCode::CNT_DASH_0004, "segment list truncated (segment budget exhausted or 64-bit range)", s);
        }
        return out;
      }
      out.push_back({cursor, d, number});
      cursor += d;
      ++number;
    }
  }
  return out;
}

// The representation's base URL, split once for all of its segments. `dir_prefix` is the resolution of any plain
// relative reference without its own path: scheme, authority and the base directory with its dot segments removed.
// A plain reference (no scheme, authority, query or fragment; a relative path without dot segments) resolves to
// dir_prefix + reference, which is what RFC 3986 §5.2 gives, without re-splitting and re-composing per segment.
struct SplitBase {
  explicit SplitBase(const std::string& base) : parts(split_uri(base)), empty(base.empty()) {
    if (!empty) {
      UriParts dir = parts;
      dir.path = remove_dot_segments(merge_paths(parts, ""));
      dir.query.clear();
      dir.has_query = false;
      dir_prefix = recompose(dir);
    }
  }
  UriParts parts;
  bool empty;
  std::string dir_prefix;
};

bool plain_relative(std::string_view ref) {
  if (ref.empty() || ref.front() == '/' || ref.front() == '.' || ref.find_first_of("?#") != std::string_view::npos ||
      ref.find("/.") != std::string_view::npos) {
    return false;
  }
  const size_t colon = ref.find(':');
  return colon == std::string_view::npos || ref.find('/') < colon;  // a ':' before any '/' may end a scheme
}

std::string resolve_segment(const SplitBase& base, std::string_view ref) {
  if (!base.empty && plain_relative(ref)) {
    std::string out;
    out.reserve(base.dir_prefix.size() + ref.size());
    out.append(base.dir_prefix).append(ref);
    return out;
  }
  return resolve_against(base.parts, base.empty, ref);
}

// Whether a template expands at all. Validity depends only on the identifiers and format tags, never on the values,
// so a media template is checked (and an invalid one reported) once per representation, not once per segment.
bool template_valid(std::string_view tmpl) {
  return expand_template(tmpl, TemplateValues{}).has_value();
}

std::string segment_url(Context& ctx, const SplitBase& base, std::string_view tmpl, const TemplateValues& values,
                        const pugi::xml_node& at) {
  if (!base.empty) {
    // Expand straight behind the base directory; keep it when the expansion is a plain relative reference.
    std::string out;
    out.reserve(base.dir_prefix.size() + tmpl.size() + 24);  // room for a 20-digit identifier: one allocation
    out.append(base.dir_prefix);
    if (expand_template_into(out, tmpl, values)) {
      const std::string_view ref = std::string_view(out).substr(base.dir_prefix.size());
      return plain_relative(ref) ? out : resolve_against(base.parts, base.empty, ref);
    }
  }
  const auto expanded = expand_template(tmpl, values);
  if (!expanded) {
    diag(ctx, DiagCode::CNT_DASH_0005, "invalid template `" + std::string(tmpl) + "`; used as written", at);
    return resolve_segment(base, tmpl);
  }
  return resolve_segment(base, *expanded);
}

// Reads an optional byte-range attribute into `out`; a malformed one is reported and leaves `out` alone.
void read_range_attr(Context& ctx, const pugi::xml_node& n, const char* name, ByteRange& out) {
  const char* range = attr(n, name);
  if (range == nullptr) {
    return;
  }
  if (const auto r = parse_range(range)) {
    out = *r;
  } else {
    diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid ") + name + " `" + range + "`", n);
  }
}

Segment init_from_element(Context& ctx, const pugi::xml_node& init, const std::string& rep_base) {
  Segment seg;
  seg.is_init = true;
  const char* src = attr(init, "sourceURL");
  seg.url = src != nullptr ? resolve_url(rep_base, trim(src)) : rep_base;
  if (const char* range = attr(init, "range")) {
    if (const auto r = parse_range(range)) {
      seg.byte_range = *r;
    } else {
      diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid Initialization range `") + range + "`", init);
    }
  }
  return seg;
}

void expand_template_segments(Context& ctx, Representation& rep, const SegmentInfo& info, const std::string& rep_base,
                              int64_t period_duration_ms) {
  const Chain& chain = info.template_chain;
  const Timing t = read_timing(ctx, chain, period_duration_ms);
  rep.timescale = t.timescale;
  TemplateValues values{rep.id, 0, rep.bandwidth, 0};
  const SplitBase base(rep_base);

  if (const char* init = chain_attr(chain, "initialization")) {
    Segment seg;
    seg.is_init = true;
    seg.url = segment_url(ctx, base, init, values, chain.front());
    rep.segments.push_back(std::move(seg));
  } else if (const pugi::xml_node init_el = chain_child(chain, "Initialization")) {
    rep.segments.push_back(init_from_element(ctx, init_el, rep_base));
  }

  const char* media = chain_attr(chain, "media");
  if (media == nullptr) {
    diag(ctx, DiagCode::CNT_DASH_0006, "SegmentTemplate without `media`; no media segments", chain.front());
    return;
  }

  std::vector<TimelineEntry> entries;
  if (const pugi::xml_node timeline = chain_child(chain, "SegmentTimeline")) {
    entries = expand_timeline(ctx, timeline, t, ctx.segments_left, true);
  } else {
    const auto d = parse_int(chain_attr(chain, "duration") != nullptr ? chain_attr(chain, "duration") : "");
    if (!d || *d <= 0) {
      diag(ctx, DiagCode::CNT_DASH_0006, "SegmentTemplate without SegmentTimeline or a positive `duration`",
           chain.front());
      return;
    }
    if (t.period_duration_ms < 0) {
      diag(ctx, DiagCode::CNT_DASH_0004, "SegmentTemplate `duration` with no period duration; no media segments",
           chain.front());
      return;
    }
    int64_t count = ceil_div(static_cast<long double>(t.period_duration_ms) * static_cast<long double>(t.timescale),
                             static_cast<long double>(*d) * 1000.0L);
    if (static_cast<size_t>(std::max<int64_t>(count, 0)) > ctx.segments_left) {
      diag(ctx, DiagCode::CNT_DASH_0004, "segment list truncated (segment budget exhausted)", chain.front());
      count = static_cast<int64_t>(ctx.segments_left);
    }
    for (int64_t k = 0; k < count; ++k) {
      TimelineEntry e{0, *d, 0};
      if (!checked_madd(t.presentation_time_offset, k, *d, e.time) || !checked_madd(t.start_number, k, 1, e.number)) {
        diag(ctx, DiagCode::CNT_DASH_0004, "segment time or number leaves the 64-bit range; list truncated",
             chain.front());
        break;
      }
      entries.push_back(e);
    }
  }

  spend(ctx, entries.size());
  rep.segments.reserve(rep.segments.size() + entries.size());
  // An invalid media template is used as written for every segment: report it once and resolve it once.
  std::optional<std::string> as_written;
  if (!entries.empty() && !template_valid(media)) {
    as_written = segment_url(ctx, base, media, values, chain.front());
  }
  for (const TimelineEntry& e : entries) {
    values.number = e.number;
    values.time = e.time;
    Segment seg;
    seg.url = as_written ? *as_written : segment_url(ctx, base, media, values, chain.front());
    seg.start_time = e.time;
    seg.duration = e.duration;
    seg.number = e.number;
    rep.segments.push_back(std::move(seg));
  }
}

void expand_list_segments(Context& ctx, Representation& rep, const SegmentInfo& info, const std::string& rep_base,
                          int64_t period_duration_ms) {
  const Chain& chain = info.list_chain;
  const Timing t = read_timing(ctx, chain, period_duration_ms);
  rep.timescale = t.timescale;
  if (const pugi::xml_node init = chain_child(chain, "Initialization")) {
    rep.segments.push_back(init_from_element(ctx, init, rep_base));
  }
  std::vector<pugi::xml_node> urls;
  for (const pugi::xml_node& level : chain) {
    urls = children(level, "SegmentURL");
    if (!urls.empty()) {
      break;
    }
  }
  if (urls.size() > ctx.segments_left) {
    diag(ctx, DiagCode::CNT_DASH_0004, "SegmentList truncated (segment budget exhausted)", chain.front());
    urls.resize(ctx.segments_left);
  }
  std::vector<TimelineEntry> entries;
  if (const pugi::xml_node timeline = chain_child(chain, "SegmentTimeline")) {
    // Only the first urls.size() entries are used; one more shows a longer timeline. Expanding the whole timeline
    // would cost up to the full segment budget per representation without spending any of it, so a short MPD with
    // many representations under one SegmentList could build millions of entries per representation.
    entries = expand_timeline(ctx, timeline, t, urls.size() + 1, false);
    if (entries.size() != urls.size()) {
      const bool longer = entries.size() > urls.size();
      diag(ctx, DiagCode::CNT_DASH_0008,
           "SegmentList has " + std::to_string(urls.size()) + " SegmentURL elements but its SegmentTimeline has " +
               (longer ? "more" : std::to_string(entries.size())) + " segments",
           chain.front());
      entries.resize(std::min(entries.size(), urls.size()));
    }
  }
  const auto d = parse_int(chain_attr(chain, "duration") != nullptr ? chain_attr(chain, "duration") : "");
  spend(ctx, urls.size());
  for (size_t k = 0; k < urls.size(); ++k) {
    const pugi::xml_node u = urls[k];
    Segment seg;
    const char* media = attr(u, "media");
    seg.url = media != nullptr ? resolve_url(rep_base, trim(media)) : rep_base;
    read_range_attr(ctx, u, "mediaRange", seg.byte_range);
    if (k < entries.size()) {
      seg.start_time = entries[k].time;
      seg.duration = entries[k].duration;
      seg.number = entries[k].number;
    } else if (entries.empty()) {
      int64_t number = 0;
      if (!checked_madd(t.start_number, static_cast<int64_t>(k), 1, number)) {
        number = std::numeric_limits<int64_t>::max();
      }
      seg.number = number;
      int64_t time = 0;
      if (d && *d > 0 && checked_madd(t.presentation_time_offset, static_cast<int64_t>(k), *d, time)) {
        seg.start_time = time;
        seg.duration = *d;
      }
    }
    rep.segments.push_back(std::move(seg));
  }
}

// The sidx comes from untrusted bytes: every 64-bit sum is checked and an overflow truncates the list.
void expand_sidx_segments(Context& ctx, Representation& rep, const std::string& rep_base, const ByteRange& index_range,
                          const SidxIndex& sidx, const pugi::xml_node& at) {
  uint64_t offset = 0;
  uint64_t time = sidx.earliest_presentation_time;
  bool overflow = __builtin_add_overflow(index_range.offset, index_range.length, &offset) ||
                  __builtin_add_overflow(offset, sidx.first_offset, &offset);
  int64_t number = 1;
  for (const SidxIndex::Reference& ref : sidx.references) {
    if (overflow || time > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      diag(ctx, DiagCode::CNT_DASH_0004, "sidx offsets or times overflow 64 bits; segment list truncated", at);
      break;
    }
    if (ctx.segments_left == 0) {
      diag(ctx, DiagCode::CNT_DASH_0004, "segment list truncated (segment budget exhausted)", at);
      break;
    }
    spend(ctx, 1);
    Segment seg;
    seg.url = rep_base;
    seg.byte_range = {offset, ref.referenced_size};
    seg.start_time = static_cast<int64_t>(time);
    seg.duration = ref.subsegment_duration;
    seg.number = number++;
    rep.segments.push_back(std::move(seg));
    overflow = __builtin_add_overflow(offset, static_cast<uint64_t>(ref.referenced_size), &offset) ||
               __builtin_add_overflow(time, static_cast<uint64_t>(ref.subsegment_duration), &time);
  }
}

void expand_base_segments(Context& ctx, Representation& rep, const SegmentInfo& info, const std::string& rep_base,
                          int64_t period_duration_ms) {
  const Chain& chain = info.base_chain;
  const Timing t = read_timing(ctx, chain, period_duration_ms);
  rep.timescale = t.timescale;
  if (const pugi::xml_node init = chain_child(chain, "Initialization")) {
    rep.segments.push_back(init_from_element(ctx, init, rep_base));
  }
  std::optional<ByteRange> index_range;
  if (const char* v = chain_attr(chain, "indexRange")) {
    index_range = parse_range(v);
    if (!index_range) {
      diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid indexRange `") + v + "`", chain.front());
    }
  }
  if (index_range && ctx.options.sidx_resolver) {
    if (const auto sidx = ctx.options.sidx_resolver(rep_base, *index_range); sidx && sidx->timescale > 0) {
      rep.timescale = sidx->timescale;
      expand_sidx_segments(ctx, rep, rep_base, *index_range, *sidx, chain.front());
      return;
    }
  }
  if (index_range) {
    diag(ctx, DiagCode::CNT_DASH_0003,
         "sidx at bytes " + std::to_string(index_range->offset) + "+" + std::to_string(index_range->length) + " of `" +
             rep_base + "` not resolved; the representation is one segment",
         chain.front());
  }
  Segment seg;
  seg.url = rep_base;
  seg.number = t.start_number;
  int64_t span = 0;
  if (const auto end = period_end_media(t); end && !__builtin_sub_overflow(*end, t.presentation_time_offset, &span)) {
    seg.start_time = t.presentation_time_offset;
    seg.duration = span;
  }
  spend(ctx, 1);
  rep.segments.push_back(std::move(seg));
}

std::optional<std::vector<uint8_t>> parse_kid(std::string_view text) {
  std::vector<uint8_t> out;
  int high = -1;
  for (const char c : text) {
    if (c == '-' || c == '{' || c == '}') {
      continue;
    }
    int v = 0;
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else {
      return std::nullopt;
    }
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<uint8_t>((high << 4) | v));
      high = -1;
    }
  }
  if (high >= 0 || out.size() != 16) {
    return std::nullopt;
  }
  return out;
}

void add_unique(std::vector<Encryption>& list, Encryption e) {
  if (std::find(list.begin(), list.end(), e) == list.end()) {
    list.push_back(std::move(e));
  }
}

void collect_protection(Context& ctx, const pugi::xml_node& element, std::vector<Encryption>& out) {
  for (const pugi::xml_node& cp : children(element, "ContentProtection")) {
    const char* scheme = attr(cp, "schemeIdUri");
    if (scheme == nullptr) {
      continue;
    }
    Encryption e;
    std::string lowered(scheme);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const char* value = attr(cp, "value");
    e.method =
        lowered == "urn:mpeg:dash:mp4protection:2011" && value != nullptr ? std::string(value) : std::string(scheme);
    if (const char* kid = prefixed_attr(cp, "default_KID")) {
      if (auto bytes = parse_kid(kid)) {
        e.default_kid = std::move(*bytes);
      } else {
        diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid default_KID `") + kid + "`", cp);
      }
    }
    if (const pugi::xml_node la = first_child(cp, "Laurl")) {
      e.key_uri = trim(la.text().get());
    }
    add_unique(out, std::move(e));
  }
}

uint32_t uint_attr(const char* v) {
  if (v == nullptr) {
    return 0;
  }
  const auto n = parse_uint(v);
  return n && *n <= std::numeric_limits<uint32_t>::max() ? static_cast<uint32_t>(*n) : 0;
}

std::string inherited(const pugi::xml_node& rep, const pugi::xml_node& set, const char* name) {
  if (const char* v = attr(rep, name)) {
    return v;
  }
  if (const char* v = attr(set, name)) {
    return v;
  }
  return {};
}

bool has_any_base_url(const pugi::xml_node& root, const pugi::xml_node& period, const pugi::xml_node& set,
                      const pugi::xml_node& rep) {
  return std::ranges::any_of(std::array{root, period, set, rep},
                             [](const pugi::xml_node& n) { return !first_child(n, "BaseURL").empty(); });
}

Representation build_representation(Context& ctx, const pugi::xml_node& root, const pugi::xml_node& period,
                                    const pugi::xml_node& set, const pugi::xml_node& rep_el,
                                    const std::string& set_base, int64_t period_duration_ms) {
  Representation rep;
  rep.id = attr(rep_el, "id") != nullptr ? attr(rep_el, "id") : "";
  rep.bandwidth = uint_attr(attr(rep_el, "bandwidth"));
  rep.codecs = inherited(rep_el, set, "codecs");
  rep.mime_type = inherited(rep_el, set, "mimeType");
  rep.width = uint_attr(inherited(rep_el, set, "width").c_str());
  rep.height = uint_attr(inherited(rep_el, set, "height").c_str());
  rep.frame_rate = inherited(rep_el, set, "frameRate");
  collect_protection(ctx, set, rep.encryption);
  collect_protection(ctx, rep_el, rep.encryption);
  for (const Encryption& e : rep.encryption) {
    add_unique(ctx.manifest.encryption, e);
  }

  const std::string rep_base = base_url_of(rep_el, set_base);
  const SegmentInfo info = collect_segment_info({rep_el, set, period});
  if (!info.template_chain.empty()) {
    expand_template_segments(ctx, rep, info, rep_base, period_duration_ms);
  } else if (!info.list_chain.empty()) {
    expand_list_segments(ctx, rep, info, rep_base, period_duration_ms);
  } else if (!info.base_chain.empty()) {
    expand_base_segments(ctx, rep, info, rep_base, period_duration_ms);
  } else if (has_any_base_url(root, period, set, rep_el)) {
    Segment seg;  // a lone BaseURL names the whole media resource
    seg.url = rep_base;
    rep.segments.push_back(std::move(seg));
  } else {
    diag(ctx, DiagCode::CNT_DASH_0006, "Representation `" + rep.id + "` has no segment information", rep_el);
  }
  return rep;
}

AdaptationSet build_adaptation_set(Context& ctx, const pugi::xml_node& root, const pugi::xml_node& period,
                                   const pugi::xml_node& set, const std::string& period_base,
                                   int64_t period_duration_ms) {
  AdaptationSet out;
  out.id = attr(set, "id") != nullptr ? attr(set, "id") : "";
  out.lang = attr(set, "lang") != nullptr ? attr(set, "lang") : "";
  out.content_type = attr(set, "contentType") != nullptr ? attr(set, "contentType") : "";
  if (prefixed_attr(set, "href") != nullptr) {
    diag(ctx, DiagCode::CNT_DASH_0002, "xlink on AdaptationSet `" + out.id + "` ignored", set);
  }
  const std::string set_base = base_url_of(set, period_base);
  for (const pugi::xml_node& rep_el : children(set, "Representation")) {
    out.representations.push_back(build_representation(ctx, root, period, set, rep_el, set_base, period_duration_ms));
  }
  if (out.content_type.empty()) {
    std::string mime = attr(set, "mimeType") != nullptr ? attr(set, "mimeType") : "";
    if (mime.empty() && !out.representations.empty()) {
      mime = out.representations.front().mime_type;
    }
    out.content_type = mime.substr(0, mime.find('/'));
  }
  return out;
}

}  // namespace

namespace {

pugi::xml_node find_mpd_root(const pugi::xml_document& doc) {
  for (pugi::xml_node c = doc.first_child(); !c.empty(); c = c.next_sibling()) {
    if (c.type() == pugi::node_element && local_name(c.name()) == "MPD") {
      return c;
    }
  }
  return {};
}

std::optional<int64_t> duration_attr(Context& ctx, const pugi::xml_node& n, const char* name) {
  const char* v = attr(n, name);
  if (v == nullptr) {
    return std::nullopt;
  }
  const auto ms = parse_duration_ms(v);
  if (!ms) {
    diag(ctx, DiagCode::CNT_DASH_0007, std::string("invalid ") + name + " `" + v + "`", n);
  }
  return ms;
}

struct PeriodTimes {
  std::vector<int64_t> start_ms;
  std::vector<int64_t> duration_ms;
};

// Period start/duration: explicit attribute, else derived from neighbours (23009-1 §5.3.2.1).
PeriodTimes compute_period_times(Context& ctx, const std::vector<pugi::xml_node>& periods,
                                 std::optional<int64_t> presentation_ms) {
  PeriodTimes t{std::vector<int64_t>(periods.size(), -1), std::vector<int64_t>(periods.size(), -1)};
  for (size_t i = 0; i < periods.size(); ++i) {
    t.start_ms[i] = duration_attr(ctx, periods[i], "start").value_or(-1);
    if (t.start_ms[i] < 0) {
      if (i == 0) {
        t.start_ms[i] = ctx.manifest.dynamic ? -1 : 0;
      } else if (t.start_ms[i - 1] >= 0 && t.duration_ms[i - 1] >= 0) {
        t.start_ms[i] = t.start_ms[i - 1] + t.duration_ms[i - 1];
      }
    }
    t.duration_ms[i] = duration_attr(ctx, periods[i], "duration").value_or(-1);
    // The duration of the previous period is the gap to this start when it was not given.
    if (i > 0 && t.duration_ms[i - 1] < 0 && t.start_ms[i] >= 0 && t.start_ms[i - 1] >= 0) {
      t.duration_ms[i - 1] = t.start_ms[i] - t.start_ms[i - 1];
    }
  }
  if (!periods.empty() && t.duration_ms.back() < 0 && presentation_ms && t.start_ms.back() >= 0) {
    t.duration_ms.back() = std::max<int64_t>(*presentation_ms - t.start_ms.back(), 0);
  }
  return t;
}

}  // namespace

ParseResult parse(std::string_view xml, const Options& options) {
  ParseResult result;
  result.manifest.url = options.manifest_url;
  Context ctx{options, result.manifest};

  pugi::xml_document doc;  // pugixml does not process DTDs or external entities
  const pugi::xml_parse_result loaded = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default);
  const pugi::xml_node root = loaded ? find_mpd_root(doc) : pugi::xml_node();
  if (root.empty()) {
    diag(ctx, DiagCode::CNT_DASH_0009,
         loaded ? std::string("no MPD root element") : std::string("XML error: ") + loaded.description());
    return result;
  }
  result.ok = true;

  const char* type = attr(root, "type");
  result.manifest.dynamic = type != nullptr && std::string_view(type) == "dynamic";
  if (result.manifest.dynamic) {
    diag(ctx, DiagCode::CNT_DASH_0001, "dynamic MPD: expanded as written, live edge not tracked", root);
  }
  const std::string mpd_base = base_url_of(root, options.manifest_url);
  const std::vector<pugi::xml_node> periods = children(root, "Period");
  const PeriodTimes times = compute_period_times(ctx, periods, duration_attr(ctx, root, "mediaPresentationDuration"));

  for (size_t i = 0; i < periods.size(); ++i) {
    Period p;
    p.id = attr(periods[i], "id") != nullptr ? attr(periods[i], "id") : "";
    p.start_ms = times.start_ms[i];
    p.duration_ms = times.duration_ms[i];
    if (prefixed_attr(periods[i], "href") != nullptr) {
      diag(ctx, DiagCode::CNT_DASH_0002, "xlink on Period `" + p.id + "` ignored", periods[i]);
    }
    const std::string period_base = base_url_of(periods[i], mpd_base);
    for (const pugi::xml_node& set : children(periods[i], "AdaptationSet")) {
      p.adaptation_sets.push_back(build_adaptation_set(ctx, root, periods[i], set, period_base, p.duration_ms));
    }
    result.manifest.periods.push_back(std::move(p));
  }
  return result;
}

flatbuffers::Offset<fb::Manifest> write_manifest(flatbuffers::FlatBufferBuilder& fbb, const Manifest& manifest) {
  const auto write_enc = [&fbb](const std::vector<Encryption>& list) {
    std::vector<flatbuffers::Offset<fb::ManifestEncryption>> out;
    out.reserve(list.size());
    for (const Encryption& e : list) {
      out.push_back(fb::CreateManifestEncryption(fbb, fbb.CreateString(e.method), fbb.CreateString(e.key_uri), 0,
                                                 e.default_kid.empty() ? 0 : fbb.CreateVector(e.default_kid)));
    }
    return fbb.CreateVector(out);
  };

  std::vector<flatbuffers::Offset<fb::ManifestPeriod>> periods;
  for (const Period& p : manifest.periods) {
    std::vector<flatbuffers::Offset<fb::ManifestAdaptationSet>> sets;
    for (const AdaptationSet& s : p.adaptation_sets) {
      std::vector<flatbuffers::Offset<fb::ManifestRepresentation>> reps;
      for (const Representation& r : s.representations) {
        std::vector<flatbuffers::Offset<fb::ManifestSegment>> segs;
        segs.reserve(r.segments.size());
        for (const Segment& g : r.segments) {
          const fb::ByteSpan range(g.byte_range.offset, g.byte_range.length);
          segs.push_back(fb::CreateManifestSegment(fbb, fbb.CreateString(g.url), &range, g.start_time, g.duration,
                                                   g.number, g.is_init));
        }
        const auto enc = write_enc(r.encryption);
        reps.push_back(fb::CreateManifestRepresentation(
            fbb, fbb.CreateString(r.id), r.bandwidth, fbb.CreateString(r.codecs), fbb.CreateString(r.mime_type),
            r.width, r.height, fbb.CreateString(r.frame_rate), 0, r.timescale, fbb.CreateVector(segs), enc));
      }
      sets.push_back(fb::CreateManifestAdaptationSet(fbb, fbb.CreateString(s.id), fbb.CreateString(s.content_type),
                                                     fbb.CreateString(s.lang), fbb.CreateVector(reps)));
    }
    periods.push_back(
        fb::CreateManifestPeriod(fbb, fbb.CreateString(p.id), p.start_ms, p.duration_ms, fbb.CreateVector(sets)));
  }
  std::vector<flatbuffers::Offset<fb::Diagnostic>> diags;
  diags.reserve(manifest.diagnostics.size());
  for (const Diagnostic& d : manifest.diagnostics) {
    diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
  }
  return fb::CreateManifest(fbb, fbb.CreateString("dash-mpd"), fbb.CreateString(manifest.url),
                            fbb.CreateVector(periods), write_enc(manifest.encryption), fbb.CreateVector(diags));
}

}  // namespace ovt::container::mpd
