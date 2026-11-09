// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Headerless raw YUV opening (chapter 08 §8.6.6, WI-1.229).

#include "container/rawyuv.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>
#include <system_error>

#include "container/tree_fb.hpp"
#include "ovt/color.h"

namespace ovt::container::raw {

namespace {

static_assert(sizeof(ovt_ColorDescription) == 44, "RawVideoFormat.color is the 44-byte ovt_ColorDescription image");

constexpr size_t kMaxNameChars = 255;                // file-name characters the heuristics read
constexpr uint32_t kMaxSamplesPerTable = 1U << 20U;  // samples one sample_table() call materialises
constexpr size_t kMaxListedCandidates = 16;

std::optional<uint64_t> parse_u64(std::string_view s) {
  uint64_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

std::optional<uint32_t> parse_u32(std::string_view s) {
  const auto v = parse_u64(s);
  if (!v || *v > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(*v);
}

std::optional<uint32_t> parse_dimension(std::string_view s) {
  const auto v = parse_u32(s);
  if (!v || *v == 0 || *v > kMaxDimension) {
    return std::nullopt;
  }
  return v;
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// Fills the derived fields; false when the geometry is out of range.
bool finalize(RawFormat& f) {
  f.frame_bytes = frame_bytes(f.pix, f.width, f.height);
  return f.frame_bytes != 0;
}

std::optional<std::pair<uint32_t, uint32_t>> reduce_ratio(uint64_t num, uint64_t den) {
  if (num == 0 || den == 0) {
    return std::nullopt;
  }
  const uint64_t g = std::gcd(num, den);
  num /= g;
  den /= g;
  if (num > UINT32_MAX || den > UINT32_MAX) {
    return std::nullopt;
  }
  return std::pair<uint32_t, uint32_t>{static_cast<uint32_t>(num), static_cast<uint32_t>(den)};
}

std::string_view last_component(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------------------------------------- file names

struct NameHints {
  std::optional<std::pair<uint32_t, uint32_t>> geometry;
  std::vector<PixFmt> formats;  // explicit pix_fmt tokens, in order, without duplicates
  std::optional<uint8_t> depth;
  std::optional<uint8_t> chroma;  // 420 / 422 / 444 as ovt chroma_format 1 / 2 / 3
  std::optional<std::pair<uint32_t, uint32_t>> fps;
};

// `(\d{2,5})[xX](\d{2,5})`, both runs maximal.
std::optional<std::pair<uint32_t, uint32_t>> find_geometry(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    if (!is_digit(s[i])) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && is_digit(s[j])) {
      ++j;
    }
    const size_t w_len = j - i;
    if (w_len >= 2 && w_len <= 5 && j < s.size() && (s[j] == 'x' || s[j] == 'X')) {
      size_t k = j + 1;
      while (k < s.size() && is_digit(s[k])) {
        ++k;
      }
      const size_t h_len = k - (j + 1);
      if (h_len >= 2 && h_len <= 5) {
        const auto w = parse_dimension(s.substr(i, w_len));
        const auto h = parse_dimension(s.substr(j + 1, h_len));
        if (w && h) {
          return std::pair<uint32_t, uint32_t>{*w, *h};
        }
      }
    }
    i = j;
  }
  return std::nullopt;
}

// `(\d{1,3}(?:\.\d+)?)fps`, else `_(\d{2,3})p\b` (the `_NNfps` form is the first pattern). The first is preferred;
// values above 240 are not rates.
std::optional<std::pair<uint32_t, uint32_t>> find_fps(std::string_view s) {
  for (size_t pos = s.find("fps"); pos != std::string_view::npos; pos = s.find("fps", pos + 1)) {
    size_t b = pos;
    while (b > 0 && (is_digit(s[b - 1]) || s[b - 1] == '.')) {
      --b;
    }
    const std::string_view num = s.substr(b, pos - b);
    if (const auto r = parse_fps(num); r && num.find('/') == std::string_view::npos && num.size() <= 12 &&
                                       static_cast<double>(r->first) / r->second <= 240.0) {
      return r;
    }
  }
  for (size_t pos = s.find('_'); pos != std::string_view::npos; pos = s.find('_', pos + 1)) {
    size_t e = pos + 1;
    while (e < s.size() && is_digit(s[e])) {
      ++e;
    }
    const size_t n = e - (pos + 1);
    if (n < 2 || n > 3 || e >= s.size() || s[e] != 'p') {
      continue;
    }
    const size_t after = e + 1;  // \b: the p must end the word
    if (after < s.size() && std::isalnum(static_cast<unsigned char>(s[after])) != 0) {
      continue;
    }
    const auto v = parse_u32(s.substr(pos + 1, n));
    if (v && *v >= 1 && *v <= 240) {
      return std::pair<uint32_t, uint32_t>{*v, 1};
    }
  }
  return std::nullopt;
}

// "420p10" / "422p" / "444p12le" without the leading "yuv".
std::optional<PixFmt> short_alias(std::string_view tok) {
  if (tok.size() < 4 || tok[3] != 'p' || !is_digit(tok[0])) {
    return std::nullopt;
  }
  const std::string_view rest = tok.substr(4);
  std::string full = "yuv" + std::string(tok.substr(0, 4));
  if (rest.empty()) {
    return parse_pix_fmt(full);
  }
  if (!rest.ends_with("le") && !rest.ends_with("be")) {
    full += std::string(rest) + "le";  // "420p10" means little-endian
  } else {
    full += std::string(rest);
  }
  return parse_pix_fmt(full);
}

NameHints hints_of(std::string_view display_name) {
  NameHints h;
  std::string name(last_component(display_name).substr(0, kMaxNameChars));
  for (char& c : name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  h.geometry = find_geometry(name);
  h.fps = find_fps(name);
  std::string token;
  const auto flush = [&] {
    if (token.empty()) {
      return;
    }
    std::optional<PixFmt> p = parse_pix_fmt(token);
    if (!p) {
      p = short_alias(token);
    }
    if (p) {
      const bool seen =
          std::any_of(h.formats.begin(), h.formats.end(), [&](const PixFmt& q) { return q.name == p->name; });
      if (!seen) {
        h.formats.push_back(std::move(*p));
      }
    } else if (token == "8bit") {
      h.depth = 8;
    } else if (token == "10bit") {
      h.depth = 10;
    } else if (token == "12bit") {
      h.depth = 12;
    } else if (token == "420") {
      h.chroma = 1;
    } else if (token == "422") {
      h.chroma = 2;
    } else if (token == "444") {
      h.chroma = 3;
    }
    token.clear();
  };
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      token.push_back(c);
    } else {
      flush();
    }
  }
  flush();
  return h;
}

constexpr std::array<std::pair<uint32_t, uint32_t>, 16> kCommonSizes = {{
    {3840, 2160},
    {2560, 1440},
    {1920, 1080},
    {1920, 1088},
    {1280, 720},
    {1024, 768},
    {960, 540},
    {832, 480},
    {720, 576},
    {720, 480},
    {640, 480},
    {640, 360},
    {416, 240},
    {352, 288},
    {320, 240},
    {176, 144},
}};

constexpr std::array<std::string_view, 9> kCommonFormats = {
    "yuv420p", "yuv420p10le", "yuv422p", "yuv422p10le", "yuv444p", "yuv444p10le", "nv12", "p010le", "gray"};

int chroma_rank(const PixFmt& p) {
  switch (p.chroma_format) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 3:
      return 2;
    default:
      return 3;
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------ strings

std::optional<std::pair<uint32_t, uint32_t>> parse_fps(std::string_view text) {
  if (const size_t slash = text.find('/'); slash != std::string_view::npos) {
    const auto n = parse_u32(text.substr(0, slash));
    const auto d = parse_u32(text.substr(slash + 1));
    if (!n || !d || *n == 0 || *d == 0) {
      return std::nullopt;
    }
    return reduce_ratio(*n, *d);
  }
  // Decimal: digits ["." digits], at most 15 digits in all.
  const size_t dot = text.find('.');
  const std::string_view int_part = text.substr(0, dot);
  const std::string_view frac_part = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (int_part.empty() || (dot != std::string_view::npos && frac_part.empty()) ||
      int_part.size() + frac_part.size() > 15) {
    return std::nullopt;
  }
  const bool digits_only = std::all_of(int_part.begin(), int_part.end(), is_digit) &&
                           std::all_of(frac_part.begin(), frac_part.end(), is_digit);
  if (!digits_only) {
    return std::nullopt;
  }
  uint64_t den = 1;
  for (size_t i = 0; i < frac_part.size(); ++i) {
    den *= 10;
  }
  const auto whole = parse_u64(int_part);
  const auto frac = frac_part.empty() ? std::optional<uint64_t>(0) : parse_u64(frac_part);
  if (!whole || !frac || *whole > UINT32_MAX) {
    return std::nullopt;
  }
  const uint64_t num = *whole * den + *frac;
  // NTSC rates: 23.976, 29.97, 59.94 ... are written as decimals of x·1000/1001.
  const double value = static_cast<double>(num) / static_cast<double>(den);
  const double x = std::round(value * 1.001);
  if (x >= 1.0 && std::fabs(value - x * 1000.0 / 1001.0) < 0.0006 && frac_part.size() >= 2) {
    return reduce_ratio(static_cast<uint64_t>(x) * 1000, 1001);
  }
  return reduce_ratio(num, den);
}

std::optional<RawFormat> parse_format_string(std::string_view text) {
  // "WxH" ":" pix_fmt [":" fps]
  const size_t c1 = text.find(':');
  if (c1 == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view geometry = text.substr(0, c1);
  const std::string_view rest = text.substr(c1 + 1);
  const size_t c2 = rest.find(':');
  const std::string_view fmt_text = rest.substr(0, c2);
  const size_t x = geometry.find_first_of("xX");
  if (x == std::string_view::npos) {
    return std::nullopt;
  }
  RawFormat f;
  const auto w = parse_dimension(geometry.substr(0, x));
  const auto h = parse_dimension(geometry.substr(x + 1));
  auto pix = parse_pix_fmt(fmt_text);
  if (!w || !h || !pix) {
    return std::nullopt;
  }
  f.width = *w;
  f.height = *h;
  f.pix = std::move(*pix);
  if (c2 != std::string_view::npos) {
    const auto fps = parse_fps(rest.substr(c2 + 1));
    if (!fps) {
      return std::nullopt;
    }
    f.fps_num = fps->first;
    f.fps_den = fps->second;
  }
  if (!finalize(f)) {
    return std::nullopt;
  }
  return f;
}

// ------------------------------------------------------------------------------------------------ sidecar

namespace {

std::optional<uint64_t> json_unsigned(const nlohmann::json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number_unsigned()) {
    return std::nullopt;
  }
  return it->get<uint64_t>();
}

// The `fps` value: "num/den", a decimal string or a number.
std::optional<std::pair<uint32_t, uint32_t>> json_fps(const nlohmann::json& v) {
  if (v.is_string()) {
    return parse_fps(v.get<std::string>());
  }
  if (v.is_number_unsigned()) {
    return reduce_ratio(v.get<uint64_t>(), 1);
  }
  if (v.is_number_float()) {
    const double d = v.get<double>();
    if (std::isfinite(d) && d > 0.0 && d < 1e6) {
      std::array<char, 40> buf{};
      (void)std::snprintf(buf.data(), buf.size(), "%.6f", d);
      return parse_fps(buf.data());
    }
  }
  return std::nullopt;
}

// The `color` object; false when a member has the wrong type or range.
bool json_color(const nlohmann::json& obj, RawColor& c) {
  if (!obj.is_object()) {
    return false;
  }
  c.present = true;
  for (const auto& [key, dst] : std::array<std::pair<const char*, uint8_t*>, 3>{
           {{"primaries", &c.primaries}, {"transfer", &c.transfer}, {"matrix", &c.matrix}}}) {
    if (obj.contains(key)) {
      const auto v = json_unsigned(obj, key);
      if (!v || *v > 255) {
        return false;
      }
      *dst = static_cast<uint8_t>(*v);
    }
  }
  if (const auto fr = obj.find("full_range"); fr != obj.end()) {
    if (!fr->is_boolean()) {
      return false;
    }
    c.full_range = fr->get<bool>();
  }
  return true;
}

}  // namespace

std::optional<RawFormat> parse_sidecar(std::string_view json) {
  if (json.size() > kMaxSidecarBytes) {
    return std::nullopt;
  }
  const auto doc = nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (!doc.is_object()) {
    return std::nullopt;
  }
  RawFormat f;
  const auto w = json_unsigned(doc, "width");
  const auto h = json_unsigned(doc, "height");
  const auto pix_it = doc.find("pix_fmt");
  if (!w || !h || *w == 0 || *h == 0 || *w > kMaxDimension || *h > kMaxDimension || pix_it == doc.end() ||
      !pix_it->is_string()) {
    return std::nullopt;
  }
  auto pix = parse_pix_fmt(pix_it->get<std::string>());
  if (!pix) {
    return std::nullopt;
  }
  f.width = static_cast<uint32_t>(*w);
  f.height = static_cast<uint32_t>(*h);
  f.pix = std::move(*pix);
  if (const auto it = doc.find("fps"); it != doc.end()) {
    const auto fps = json_fps(*it);
    if (!fps) {
      return std::nullopt;
    }
    f.fps_num = fps->first;
    f.fps_den = fps->second;
  }
  if (doc.contains("header_bytes")) {
    const auto hb = json_unsigned(doc, "header_bytes");
    if (!hb) {
      return std::nullopt;
    }
    f.header_bytes = *hb;
  }
  if (const auto it = doc.find("color"); it != doc.end() && !json_color(*it, f.color)) {
    return std::nullopt;
  }
  if (!finalize(f)) {
    return std::nullopt;
  }
  return f;
}

std::string sidecar_json(const RawFormat& fmt) {
  nlohmann::ordered_json j;
  j["width"] = fmt.width;
  j["height"] = fmt.height;
  j["pix_fmt"] = fmt.pix.name;
  j["fps"] = std::to_string(fmt.fps_num) + "/" + std::to_string(fmt.fps_den);
  j["header_bytes"] = fmt.header_bytes;
  if (fmt.color.present) {
    j["color"] = {{"primaries", fmt.color.primaries},
                  {"transfer", fmt.color.transfer},
                  {"matrix", fmt.color.matrix},
                  {"full_range", fmt.color.full_range}};
  }
  return j.dump(2) + "\n";
}

std::string sidecar_path(std::string_view media_path) {
  if (media_path.empty() || media_path.starts_with("fd:") || media_path.find("://") != std::string_view::npos) {
    return {};
  }
  return std::string(media_path) + ".ovt-raw.json";
}

bool write_sidecar(std::string_view media_path, const RawFormat& fmt) {
  const std::string path = sidecar_path(media_path);
  if (path.empty()) {
    return false;
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    out << sidecar_json(fmt);
    out.flush();
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);  // atomic replace, so a reader never sees half a sidecar
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

std::optional<RawFormat> read_sidecar(std::string_view media_path) {
  const std::string path = sidecar_path(media_path);
  if (path.empty()) {
    return std::nullopt;
  }
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > kMaxSidecarBytes) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string text(static_cast<size_t>(size), '\0');
  in.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (in.gcount() != static_cast<std::streamsize>(text.size())) {
    return std::nullopt;
  }
  return parse_sidecar(text);
}

// -------------------------------------------------------------------------------------------- heuristics

uint64_t frame_count_of(const RawFormat& fmt, uint64_t file_size) noexcept {
  if (fmt.frame_bytes == 0 || file_size <= fmt.header_bytes) {
    return 0;
  }
  return (file_size - fmt.header_bytes) / fmt.frame_bytes;
}

namespace {

struct Ranked {
  Candidate cand;
  size_t order;
};

// Rank (§8.6.6 step 4): file-name geometry first (all or none here), then frame_count ≤ 100 000 with the larger
// count first, then 4:2:0 < 4:2:2 < 4:4:4, 8-bit before deeper, then generation order.
// SPEC-ASSUMPTION(#366): "larger frame_count ≤ 100 000" is read literally; counts above the bound rank after.
bool ranks_before(const Ranked& a, const Ranked& b) {
  const bool a_big = a.cand.frame_count > kMaxRankedFrameCount;
  const bool b_big = b.cand.frame_count > kMaxRankedFrameCount;
  if (a_big != b_big) {
    return b_big;
  }
  if (a.cand.frame_count != b.cand.frame_count) {
    return a.cand.frame_count > b.cand.frame_count;
  }
  if (chroma_rank(a.cand.format.pix) != chroma_rank(b.cand.format.pix)) {
    return chroma_rank(a.cand.format.pix) < chroma_rank(b.cand.format.pix);
  }
  if ((a.cand.format.pix.bit_depth > 8) != (b.cand.format.pix.bit_depth > 8)) {
    return b.cand.format.pix.bit_depth > 8;
  }
  return a.order < b.order;
}

// The pix_fmts to try: the ones named in the file name, else the common set narrowed by the bit-depth and chroma
// hints.
std::vector<PixFmt> formats_to_try(const NameHints& hints) {
  std::vector<PixFmt> formats = hints.formats;
  if (!formats.empty()) {
    return formats;
  }
  for (const std::string_view name : kCommonFormats) {
    auto p = parse_pix_fmt(name);
    if (p && (!hints.depth || p->bit_depth == *hints.depth) && (!hints.chroma || p->chroma_format == *hints.chroma)) {
      formats.push_back(std::move(*p));
    }
  }
  return formats;
}

}  // namespace

std::vector<Candidate> candidates(std::string_view display_name, uint64_t file_size, uint32_t fps_num, uint32_t fps_den,
                                  uint64_t header_bytes) {
  const NameHints hints = hints_of(display_name);
  if (hints.fps) {
    fps_num = hints.fps->first;
    fps_den = hints.fps->second;
  }

  std::vector<std::pair<uint32_t, uint32_t>> sizes;
  if (hints.geometry) {
    sizes.push_back(*hints.geometry);
  } else {
    sizes.assign(kCommonSizes.begin(), kCommonSizes.end());
  }
  const std::vector<PixFmt> formats = formats_to_try(hints);

  std::vector<Ranked> found;
  for (const auto& [w, h] : sizes) {
    for (const PixFmt& p : formats) {
      RawFormat f;
      f.width = w;
      f.height = h;
      f.pix = p;
      f.fps_num = fps_num;
      f.fps_den = fps_den;
      f.header_bytes = header_bytes;
      if (!finalize(f)) {
        continue;
      }
      const uint64_t count = frame_count_of(f, file_size);
      if (count < 1 || (file_size - header_bytes) % f.frame_bytes != 0) {
        continue;
      }
      found.push_back({Candidate{std::move(f), count, hints.geometry.has_value()}, found.size()});
    }
  }
  std::stable_sort(found.begin(), found.end(), ranks_before);
  std::vector<Candidate> out;
  out.reserve(found.size());
  for (Ranked& r : found) {
    out.push_back(std::move(r.cand));
  }
  return out;
}

Resolution resolve(const io::RandomAccessFile& file, std::string_view explicit_format, const Overrides& overrides) {
  Resolution r;
  const std::string name = file.info().display_name;
  const auto apply = [&](RawFormat& f) {
    if (overrides.header_bytes) {
      f.header_bytes = *overrides.header_bytes;
    }
    if (overrides.fps) {
      f.fps_num = overrides.fps->first;
      f.fps_den = overrides.fps->second;
    }
  };

  if (auto side = read_sidecar(name)) {  // 1. sidecar; an invalid one is ignored
    apply(*side);
    r.source = Source::Sidecar;
    r.format = std::move(side);
    return r;
  }
  if (!explicit_format.empty()) {  // 2. explicit string
    auto f = parse_format_string(explicit_format);
    if (!f) {
      r.status = OVT_E_INVALID_ARG;
      return r;
    }
    apply(*f);
    r.source = Source::Explicit;
    r.format = std::move(f);
    return r;
  }
  // 3.–4. heuristics
  uint32_t fps_num = 25;
  uint32_t fps_den = 1;
  if (overrides.fps) {
    fps_num = overrides.fps->first;
    fps_den = overrides.fps->second;
  }
  r.candidates = candidates(name, file.size(), fps_num, fps_den, overrides.header_bytes.value_or(0));
  if (overrides.fps) {  // the override beats a rate read from the file name
    for (Candidate& c : r.candidates) {
      c.format.fps_num = fps_num;
      c.format.fps_den = fps_den;
    }
  }
  if (r.candidates.size() == 1) {
    r.source = Source::Heuristic;
    r.format = r.candidates.front().format;
    return r;
  }
  std::string list;
  for (size_t i = 0; i < std::min(r.candidates.size(), kMaxListedCandidates); ++i) {
    const RawFormat& f = r.candidates[i].format;
    list += (i == 0 ? "" : ", ") + std::to_string(f.width) + "x" + std::to_string(f.height) + ":" + f.pix.name;
  }
  r.diagnostics.push_back({DiagCode::CNT_RAW_0002,
                           "raw format undetermined; `" + std::to_string(r.candidates.size()) + "` candidates (`" +
                               list + "`); supply `--raw-format` or a sidecar",
                           0});
  r.status = OVT_E_INVALID_ARG;
  return r;
}

// ------------------------------------------------------------------------------------------------- reader

namespace {

class RawReader final : public Reader {
 public:
  RawReader(uint64_t file_size, RawFormat fmt) : file_size_(file_size), f_(std::move(fmt)) {
    count_ = frame_count_of(f_, file_size_);
    const uint64_t used = f_.header_bytes < file_size_ ? count_ * f_.frame_bytes + f_.header_bytes : file_size_;
    const uint64_t tail = file_size_ - std::min(used, file_size_);
    if (tail > 0) {
      diags_.push_back({DiagCode::CNT_RAW_0001,
                        "`" + std::to_string(tail) + "` trailing bytes do not form a whole frame (ignored)", used});
    }
  }

  [[nodiscard]] int track_count() const noexcept override { return 1; }
  [[nodiscard]] std::vector<uint8_t> tree() const override { return build_tree(); }
  [[nodiscard]] std::vector<uint8_t> raw_format() const override { return build_raw_format(); }
  ovt_status expand(uint64_t box_offset) override { return box_offset == 0 ? OVT_OK : OVT_E_INVALID_ARG; }

  // At most kMaxSamplesPerTable samples per call (count 0 = as many as that); page with `first`.
  [[nodiscard]] std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const override {
    if (track_index != 0 || first > count_) {
      return {};
    }
    const uint64_t left = count_ - first;
    const uint64_t want = count == 0 ? left : std::min<uint64_t>(count, left);
    return build_sample_table(first, static_cast<size_t>(std::min<uint64_t>(want, kMaxSamplesPerTable)));
  }

 private:
  uint64_t file_size_;
  RawFormat f_;
  uint64_t count_ = 0;
  std::vector<Diagnostic> diags_;

  static std::vector<uint8_t> finish(treefb::Fbb& fbb) {
    const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
    return {bytes.begin(), bytes.end()};
  }

  [[nodiscard]] std::vector<uint8_t> build_sample_table(uint32_t first, size_t n) const {
    treefb::Fbb fbb;
    // SPEC-ASSUMPTION(#366): §8.6.6 says `dts = i`, `timescale = fps_num`, `duration = fps_den`; with that timescale
    // a frame lasts fps_den ticks, so dts = i * fps_den (as in the Y4M reader).
    std::vector<int64_t> dts(n);
    std::vector<uint64_t> offset(n);
    for (size_t k = 0; k < n; ++k) {
      const uint64_t i = first + k;
      dts[k] = static_cast<int64_t>(i * f_.fps_den);
      offset[k] = f_.header_bytes + i * f_.frame_bytes;
    }
    const std::vector<int32_t> cts_offset(n, 0);
    const std::vector<uint32_t> duration(n, f_.fps_den);
    const std::vector<uint32_t> size(n, static_cast<uint32_t>(std::min<uint64_t>(f_.frame_bytes, UINT32_MAX)));
    const std::vector<uint8_t> sync(n, 1);
    const std::vector<uint16_t> desc(n, 1);
    const std::vector<uint32_t> chunk(n, 0);
    const std::vector<int32_t> fragment(n, -1);
    const auto table = fb::CreateSampleTable(fbb, 1, f_.fps_num, n, fbb.CreateVector(dts), fbb.CreateVector(cts_offset),
                                             fbb.CreateVector(duration), fbb.CreateVector(offset),
                                             fbb.CreateVector(size), fbb.CreateVector(sync), 0, fbb.CreateVector(desc),
                                             fbb.CreateVector(chunk), fbb.CreateVector(fragment));
    fbb.Finish(table);
    return finish(fbb);
  }

  [[nodiscard]] std::vector<uint8_t> build_raw_format() const {
    treefb::Fbb fbb;
    ovt_ColorDescription color{};
    color.struct_size = sizeof(ovt_ColorDescription);
    color.primaries = f_.color.primaries;
    color.transfer = f_.color.transfer;
    color.matrix = f_.color.matrix;
    color.full_range = f_.color.full_range ? 1 : 0;
    color.chroma_siting = 0;
    color.chroma_format = f_.pix.chroma_format;
    color.bit_depth = f_.pix.bit_depth;
    std::array<uint8_t, sizeof(ovt_ColorDescription)> image{};
    std::memcpy(image.data(), &color, image.size());

    const auto pix_fmt = fbb.CreateString(f_.pix.name);
    const auto color_vec = fbb.CreateVector(image.data(), image.size());
    fb::RawVideoFormatBuilder b(fbb);
    b.add_width(f_.width);
    b.add_height(f_.height);
    b.add_pix_fmt(pix_fmt);
    b.add_fps_num(f_.fps_num);
    b.add_fps_den(f_.fps_den);
    b.add_chroma_format(f_.pix.chroma_format);
    b.add_bit_depth(f_.pix.bit_depth);
    b.add_little_endian(f_.pix.little_endian);
    b.add_planar(f_.pix.planar);
    b.add_interleaved_uv(f_.pix.interleaved_uv);
    b.add_frame_bytes(f_.frame_bytes);
    b.add_frame_count(count_);
    b.add_header_bytes(f_.header_bytes);
    b.add_color(color_vec);
    fbb.Finish(b.Finish());
    return finish(fbb);
  }

  [[nodiscard]] std::vector<uint8_t> build_tree() const {
    treefb::Fbb fbb;
    const auto uint_f = [&](std::string_view name, uint64_t v) {
      return fb::CreateField(fbb, fbb.CreateString(name.data(), name.size()), fb::SyntaxValueKind::UInt, v);
    };
    const auto text_f = [&](std::string_view name, const std::string& v) {
      return fb::CreateField(fbb, fbb.CreateString(name.data(), name.size()), fb::SyntaxValueKind::String, 0, 0,
                             fbb.CreateString(v));
    };
    // The raw format lives outside the file (sidecar, command line), so the fields carry no bit span.
    std::vector<flatbuffers::Offset<fb::Field>> fields;
    fields.push_back(uint_f("width", f_.width));
    fields.push_back(uint_f("height", f_.height));
    fields.push_back(text_f("pix_fmt", f_.pix.name));
    fields.push_back(text_f("frame_rate", std::to_string(f_.fps_num) + "/" + std::to_string(f_.fps_den)));
    fields.push_back(uint_f("header_bytes", f_.header_bytes));
    fields.push_back(uint_f("frame_bytes", f_.frame_bytes));
    fields.push_back(uint_f("frame_count", count_));

    const fb::ByteSpan root_span(0, file_size_);
    const auto root = fb::CreateBoxNode(fbb, fbb.CreateString("RAW "), 0, &root_span,
                                        static_cast<uint8_t>(std::min<uint64_t>(f_.header_bytes, 255)), -1, -1,
                                        fbb.CreateVector(fields));
    std::vector<flatbuffers::Offset<fb::Diagnostic>> tree_diags;
    tree_diags.reserve(diags_.size());
    for (const Diagnostic& d : diags_) {
      tree_diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
    }

    const uint64_t dur = count_ * f_.fps_den;
    const double dur_s = f_.fps_num == 0 ? -1.0 : static_cast<double>(dur) / f_.fps_num;
    const auto handler_fourcc = fbb.CreateString("vide");
    fb::TrackInfoBuilder tb(fbb);
    tb.add_index(0);
    tb.add_track_id(1);
    tb.add_handler(fb::HandlerType::Video);
    tb.add_handler_fourcc(handler_fourcc);
    tb.add_codec(fb::Codec::Unknown);
    tb.add_timescale(f_.fps_num);
    tb.add_duration(dur);
    tb.add_duration_seconds(dur_s);
    tb.add_enabled(true);
    tb.add_in_movie(true);
    tb.add_width(f_.width);
    tb.add_height(f_.height);
    tb.add_coded_width(f_.width);
    tb.add_coded_height(f_.height);
    tb.add_sample_count(count_);
    const auto track = tb.Finish();
    fbb.Finish(fb::CreateContainerTree(fbb, fbb.CreateString("raw"), fbb.CreateVector(&root, 1),
                                       fbb.CreateVector(&track, 1), fbb.CreateVector(tree_diags)));
    return finish(fbb);
  }
};

}  // namespace

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, const RawFormat& fmt) {
  RawFormat f = fmt;
  if (f.frame_bytes == 0) {
    finalize(f);
  }
  return std::make_unique<RawReader>(file.size(), std::move(f));
}

}  // namespace ovt::container::raw
