// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group C part 2 (chapter 08 §8.3.3 rows 9-17, WI-1.208): the colour, HDR, Dolby Vision and
// miscellaneous sample-entry children `btrt`, `pasp`, `clap`, `colr`, `mdcv`, `clli`, `cclv`, `dvcC`/`dvvC`/`dvwC`,
// `fiel` and `srat`. The same syntax is used under a sample entry (`*/stsd/*`) and as a HEIF item property
// (`*/ipco`), so every decoder registers with the pattern "*", which also decorates the generated `*/ipco` rows
// (box_registry.cpp). Only `srat` is a FullBox; the walker has read its version/flags already (§8.2.9).
//
// Derived text goes into `Field.svalue` (SPEC-ASSUMPTION(#415), field_builder.h); text with no source field of its
// own is a `<name>.derived` String sibling (SPEC-ASSUMPTION(#567)). H.273 names come from the syntax module's shared
// table (syntax/common/h273_names.h), so `colr`, the VUI and the AV1 color_config print the same names.
// Dolby Vision: field layout from the public "Dolby Vision Streams Within the ISO Base Media File Format" §2.2.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt_container_generated.h"
#include "syntax/common/h273_names.h"

namespace ovt::container::isobmff {

namespace {

// ---- shared helpers (same conventions as decoders/movie.cpp and decoders/sample_entries.cpp) -------------------

// Stamps `spec_ref` on every top-level field a decoder adds, also when the decoder throws (CNT-1002).
class SpecRefScope {
 public:
  SpecRefScope(FieldBuilder& fb, std::string_view ref) : fb_(fb), first_(fb.node().fields.size()), ref_(ref) {}
  SpecRefScope(const SpecRefScope&) = delete;
  SpecRefScope& operator=(const SpecRefScope&) = delete;
  SpecRefScope(SpecRefScope&&) = delete;
  SpecRefScope& operator=(SpecRefScope&&) = delete;
  ~SpecRefScope() {
    auto& fields = fb_.node().fields;
    for (size_t i = first_; i < fields.size(); ++i) {
      if (fields[i] && fields[i]->spec_ref.empty()) {
        fields[i]->spec_ref = std::string(ref_);
      }
    }
  }

 private:
  FieldBuilder& fb_;
  size_t first_;
  std::string_view ref_;
};

constexpr std::string_view kRefBtrt = "ISO/IEC 14496-12:2022 §8.5.2.2";
constexpr std::string_view kRefPaspClap = "ISO/IEC 14496-12:2022 §12.1.4";
constexpr std::string_view kRefColr = "ISO/IEC 14496-12:2022 §12.1.5";
constexpr std::string_view kRefNclc = "QuickTime File Format, Color Parameter Atoms";
constexpr std::string_view kRefHdr = "ISO/IEC 14496-12:2022 §12.1.6";
constexpr std::string_view kRefDv = "DV-ISOBMFF §2.2 (DOVIDecoderConfigurationRecord)";
constexpr std::string_view kRefFiel = "QuickTime File Format, Video Sample Description Extensions";
constexpr std::string_view kRefSrat = "ISO/IEC 14496-12:2022 §12.2.4";

// SPEC-ASSUMPTION(#567): a String sibling `<name>.derived` with an empty span right after the previous field.
void derived_sibling(FieldBuilder& fb, std::string_view name, std::string_view text) {
  auto& fields = fb.node().fields;
  uint64_t at = static_cast<uint64_t>(fb.node().header_length) * 8U;
  if (!fields.empty() && fields.back() && fields.back()->span) {
    at = fields.back()->span->bit_offset() + fields.back()->span->bit_length();
  }
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(name) + ".derived";
  f->kind = fb::SyntaxValueKind::String;
  f->svalue = std::string(text);
  f->span = std::make_unique<fb::BitSpan>(at, 0);
  fields.push_back(std::move(f));
}

// CNT-1005: bytes after the last field of a box whose layout has no trailing data.
void check_trailing(FieldBuilder& fb) {
  if (const uint64_t extra = fb.reader().bits_left() / 8U; extra > 0) {
    fb.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in " + fb.node().fourcc);
  }
}

// `scaled / 10^places` as decimal text without rounding: trailing zeros dropped, at least `min_places` digits kept
// ("1000.0", "0.0001", "0.265").
std::string decimal(int64_t scaled, unsigned places, unsigned min_places = 1) {
  const bool negative = scaled < 0;
  const uint64_t mag = negative ? 0U - static_cast<uint64_t>(scaled) : static_cast<uint64_t>(scaled);
  uint64_t div = 1;
  for (unsigned i = 0; i < places; ++i) {
    div *= 10U;
  }
  std::string frac = std::to_string(mag % div);
  frac.insert(0, places - frac.size(), '0');
  while (frac.size() > min_places && frac.back() == '0') {
    frac.pop_back();
  }
  const std::string out = (negative ? "-" : "") + std::to_string(mag / div);
  return frac.empty() ? out : out + "." + frac;
}

// Chromaticity in 0.00002 units (mdcv, cclv): raw × 2 in 10^-5.
std::string chromaticity(int64_t raw) {
  return decimal(raw * 2, 5);
}

std::string h273_or_reserved(std::string_view name) {
  return name.empty() ? std::string("reserved") : std::string(name);
}

// The width/height fields of the enclosing visual sample entry, when the box sits under one.
std::optional<std::pair<uint64_t, uint64_t>> picture_size(const BoxContext& ctx) {
  if (ctx.parent == nullptr) {
    return std::nullopt;
  }
  std::optional<uint64_t> w;
  std::optional<uint64_t> h;
  for (const auto& f : ctx.parent->fields) {
    if (f && f->name == "width") {
      w = f->uvalue;
    } else if (f && f->name == "height") {
      h = f->uvalue;
    }
  }
  if (!w || !h || *w == 0 || *h == 0) {
    return std::nullopt;
  }
  return std::make_pair(*w, *h);
}

std::string ratio(uint64_t a, uint64_t b) {
  const uint64_t g = std::gcd(a, b);
  return g == 0 ? std::string("0:0") : std::to_string(a / g) + ":" + std::to_string(b / g);
}

// ---- btrt (14496-12 §8.5.2.2) ---------------------------------------------------------------------------------

uint64_t decode_btrt(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefBtrt);
  fb.derived(std::to_string(fb.u32("bufferSizeDB")) + " bytes");
  fb.derived(decimal(fb.u32("maxBitrate"), 3) + " kbit/s");
  fb.derived(decimal(fb.u32("avgBitrate"), 3) + " kbit/s");
  check_trailing(fb);
  return 0;
}

// ---- pasp, clap (14496-12 §12.1.4) ----------------------------------------------------------------------------

uint64_t decode_pasp(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefPaspClap);
  const uint32_t h = fb.u32("hSpacing");
  const uint32_t v = fb.u32("vSpacing");
  if (h == 0 || v == 0) {
    derived_sibling(fb, "pixel_aspect_ratio", "invalid (zero spacing)");
  } else {
    derived_sibling(fb, "pixel_aspect_ratio", ratio(h, v));
    if (const auto size = picture_size(ctx)) {
      const uint64_t dw = size->first * h;
      const uint64_t dh = size->second * v;
      const auto r = static_cast<double>(dw) / static_cast<double>(dh);
      const auto r1000 = static_cast<int64_t>(std::llround(r * 1000.0));
      derived_sibling(fb, "display_aspect_ratio", ratio(dw, dh) + " (" + decimal(r1000, 3, 3) + ")");
    }
  }
  check_trailing(fb);
  return 0;
}

struct Fraction {
  int64_t n;
  uint32_t d;
  [[nodiscard]] double value() const { return static_cast<double>(n) / static_cast<double>(d); }
};

bool integral(double v) {
  return std::fabs(v - std::round(v)) < 1e-9;
}

std::string number(double v) {
  return integral(v) ? std::to_string(std::llround(v)) : decimal(std::llround(v * 1000.0), 3);
}

uint64_t decode_clap(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefPaspClap);
  std::array<uint32_t, 8> raw{};
  constexpr std::array<std::string_view, 8> kNames = {"cleanApertureWidthN",
                                                      "cleanApertureWidthD",
                                                      "cleanApertureHeightN",
                                                      "cleanApertureHeightD",
                                                      "horizOffN",
                                                      "horizOffD",
                                                      "vertOffN",
                                                      "vertOffD"};
  for (size_t i = 0; i < raw.size(); ++i) {
    raw.at(i) = fb.u32(kNames.at(i));
  }
  check_trailing(fb);
  if (raw[1] == 0 || raw[3] == 0 || raw[5] == 0 || raw[7] == 0) {
    fb.diag(Severity::Warning, "CNT-1023", "clap has a zero denominator");
    derived_sibling(fb, "clean_aperture", "invalid (zero denominator)");
    return 0;
  }
  // SPEC-ASSUMPTION(#678): the offsets are read as the u32 of §8.3.3; the rectangle treats them as two's complement
  // (23008-12 and common writers encode negative offsets that way).
  const Fraction w{raw[0], raw[1]};
  const Fraction h{raw[2], raw[3]};
  const Fraction ho{static_cast<int32_t>(raw[4]), raw[5]};
  const Fraction vo{static_cast<int32_t>(raw[6]), raw[7]};
  bool ok = integral(w.value()) && integral(h.value());
  std::string text = number(w.value()) + "x" + number(h.value());
  if (const auto size = picture_size(ctx)) {
    // left = horizOff + (width - cleanApertureWidth) / 2 (§12.1.4.3: the offsets move the centre).
    const double left = ho.value() + ((static_cast<double>(size->first) - w.value()) / 2.0);
    const double top = vo.value() + ((static_cast<double>(size->second) - h.value()) / 2.0);
    ok = ok && integral(left) && integral(top) && left >= 0.0 && top >= 0.0 &&
         left + w.value() <= static_cast<double>(size->first) && top + h.value() <= static_cast<double>(size->second);
    text += " at (" + number(left) + ", " + number(top) + ")";
  } else {
    text += ", centre offset (" + number(ho.value()) + ", " + number(vo.value()) + ")";
  }
  derived_sibling(fb, "clean_aperture", text);
  if (!ok) {
    fb.diag(Severity::Warning, "CNT-1023", "clap aperture " + text + " is non-integral or outside the picture");
  }
  return 0;
}

// ---- colr (14496-12 §12.1.5; QTFF Color Parameter Atoms) -------------------------------------------------------

uint32_t be32(std::span<const uint8_t> b, size_t at) {
  return (uint32_t{b[at]} << 24U) | (uint32_t{b[at + 1]} << 16U) | (uint32_t{b[at + 2]} << 8U) | b[at + 3];
}

std::string four(std::span<const uint8_t> b, size_t at) {
  std::string s;
  for (size_t i = 0; i < 4; ++i) {
    const auto c = static_cast<char>(b[at + i]);
    s.push_back(c >= 0x20 && c < 0x7F ? c : '.');
  }
  while (!s.empty() && s.back() == ' ') {
    s.pop_back();
  }
  return s;
}

// The text of one ICC `desc` tag: v2 textDescriptionType ('desc') or v4 multiLocalizedUnicodeType ('mluc', first
// record, UTF-16BE). At most 64 characters, printable ASCII (others become '?'). `tag` holds at least 16 bytes.
std::string desc_tag_text(std::span<const uint8_t> tag) {
  constexpr size_t kMaxChars = 64;
  std::string out;
  const auto put = [&](uint32_t c) {
    if (c != 0 && out.size() < kMaxChars) {
      out.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
    }
  };
  if (four(tag, 0) == "desc") {
    const uint64_t n = std::min<uint64_t>(be32(tag, 8), tag.size() - 12);
    for (uint64_t k = 0; k < n && tag[12 + k] != 0; ++k) {
      put(tag[12 + k]);
    }
  } else if (four(tag, 0) == "mluc" && be32(tag, 8) > 0 && tag.size() >= 28) {
    const uint64_t n = be32(tag, 20);
    const uint64_t at = be32(tag, 24);
    for (uint64_t k = 0; k + 1 < n && at + k + 1 < tag.size(); k += 2) {
      put((uint32_t{tag[at + k]} << 8U) | tag[at + k + 1]);
    }
  }
  return out;
}

// The description of an ICC profile from its tag table (ICC.1:2022 §7.3); empty when absent or malformed.
std::string icc_description(std::span<const uint8_t> icc) {
  if (icc.size() < 132) {
    return {};
  }
  const uint32_t count = be32(icc, 128);
  for (uint64_t i = 0; i < count && 132 + (12 * (i + 1)) <= icc.size(); ++i) {
    const size_t e = 132 + (12 * i);
    if (four(icc, e) != "desc") {
      continue;
    }
    const uint64_t off = be32(icc, e + 4);
    const uint64_t len = be32(icc, e + 8);
    if (off + len > icc.size() || len < 16) {
      return {};
    }
    return desc_tag_text(icc.subspan(off, len));
  }
  return {};
}

// "ICC v4.3, mntr RGB, 3144 bytes, "Display P3"": the 128-byte header (ICC.1:2022 §7.2) and the description tag.
std::string icc_preview(std::span<const uint8_t> icc) {
  if (icc.size() < 128) {
    return "ICC profile too short (" + std::to_string(icc.size()) + " bytes)";
  }
  const uint32_t declared = be32(icc, 0);
  std::string s = "ICC v" + std::to_string(icc[8]) + "." + std::to_string(icc[9] >> 4U) + ", " + four(icc, 12) + " " +
                  four(icc, 16) + ", profile size " + std::to_string(declared) + " bytes";
  if (declared != icc.size()) {
    s += " (box holds " + std::to_string(icc.size()) + ")";
  }
  if (const std::string d = icc_description(icc); !d.empty()) {
    s += ", \"" + d + "\"";
  }
  return s;
}

void colour_triplet(FieldBuilder& fb) {
  fb.enum_name(h273_or_reserved(ovt::syntax::h273_colour_primaries_name(fb.u16("colour_primaries"))));
  fb.enum_name(h273_or_reserved(ovt::syntax::h273_transfer_characteristics_name(fb.u16("transfer_characteristics"))));
  fb.enum_name(h273_or_reserved(ovt::syntax::h273_matrix_coefficients_name(fb.u16("matrix_coefficients"))));
}

uint64_t decode_colr(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefColr);
  const auto type = fb.fourcc("colour_type");
  const std::string_view t(type.data(), type.size());
  if (t == "nclx") {
    colour_triplet(fb);
    fb.enum_name(fb.u("full_range_flag", 1) != 0 ? "full range" : "limited range");
    fb.reserved(7);
    check_trailing(fb);
  } else if (t == "nclc") {
    {
      const SpecRefScope qt(fb, kRefNclc);  // stamps the three fields before the outer scope does
      colour_triplet(fb);
    }
    if (!ctx.quicktime) {
      fb.diag(Severity::Warning, "CNT-5002", "QuickTime nclc colr in a file that is not QuickTime");
    }
    check_trailing(fb);
  } else if (t == "rICC" || t == "prof") {
    const auto icc = fb.bytes("ICC_profile", br.bits_left() / 8U);
    fb.derived(icc_preview(icc));
  } else {
    fb.bytes("data", br.bits_left() / 8U);
    fb.derived("unknown colour_type");
  }
  return 0;
}

// ---- mdcv, clli, cclv (14496-12 §12.1.6) ----------------------------------------------------------------------

struct XY {
  double x;
  double y;
};

struct Gamut {
  std::string_view name;
  std::array<XY, 3> rgb;  // red, green, blue
};

// BT.709 / BT.2020 (H.273 Table 2 rows 1 and 9) and the P3 primaries shared by rows 11 and 12.
constexpr std::array<Gamut, 3> kGamuts = {{
    {"BT.709", {{{0.640, 0.330}, {0.300, 0.600}, {0.150, 0.060}}}},
    {"P3", {{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}}}},
    {"BT.2020", {{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}}},
}};
constexpr std::array<std::string_view, 3> kColourNames = {"red", "green", "blue"};
constexpr XY kD65{0.3127, 0.3290};
constexpr XY kDciWhite{0.314, 0.351};

bool near(XY a, XY b) {
  constexpr double kTolerance = 0.002;
  return std::fabs(a.x - b.x) <= kTolerance && std::fabs(a.y - b.y) <= kTolerance;
}

// The colour of each measured primary when all three match the three distinct primaries of `g`.
bool match_gamut(const Gamut& g, const std::array<XY, 3>& p, std::array<std::string_view, 3>& colour) {
  std::array<std::string_view, 3> names{};
  for (size_t i = 0; i < 3; ++i) {
    const auto* const it = std::ranges::find_if(g.rgb, [&](XY q) { return near(p.at(i), q); });
    if (it == g.rgb.end()) {
      return false;
    }
    names.at(i) = kColourNames.at(static_cast<size_t>(it - g.rgb.begin()));
  }
  if (names[0] == names[1] || names[1] == names[2] || names[0] == names[2]) {
    return false;
  }
  colour = names;
  return true;
}

// Names each primary by its chromaticity, never by its position in the box (§8.3.3 note). Returns the gamut name
// ("P3-D65", "BT.2020") or "custom".
std::string classify_primaries(const std::array<XY, 3>& p, XY white, std::array<std::string_view, 3>& colour) {
  for (const Gamut& g : kGamuts) {
    if (!match_gamut(g, p, colour)) {
      continue;
    }
    const std::string name(g.name);
    if (near(white, kD65)) {
      return name == "P3" ? "P3-D65" : name;
    }
    if (name == "P3" && near(white, kDciWhite)) {
      return "P3-DCI";
    }
    return name + " (custom white)";
  }
  // No known gamut: green has the largest y, red the largest x of the other two.
  size_t g = 0;
  for (size_t i = 1; i < 3; ++i) {
    g = p.at(i).y > p.at(g).y ? i : g;
  }
  size_t r = (g + 1) % 3;
  if (const size_t other = (g + 2) % 3; p.at(other).x > p.at(r).x) {
    r = other;
  }
  colour = {"blue", "blue", "blue"};
  colour.at(g) = "green";
  colour.at(r) = "red";
  return "custom";
}

uint64_t decode_mdcv(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefHdr);
  std::array<XY, 3> p{};
  fb.array("display_primaries", 3);
  for (uint32_t i = 0; i < 3; ++i) {
    fb.array_element("display_primaries", i);
    const uint16_t x = fb.u16("display_primaries_x");
    fb.derived(chromaticity(x));
    const uint16_t y = fb.u16("display_primaries_y");
    fb.derived(chromaticity(y));
    p.at(i) = {x * 0.00002, y * 0.00002};
    fb.end();
  }
  fb.end();
  auto& primaries = *fb.node().fields.back();
  const uint16_t wx = fb.u16("white_point_x");
  fb.derived(chromaticity(wx));
  const uint16_t wy = fb.u16("white_point_y");
  fb.derived(chromaticity(wy));
  const uint32_t max = fb.u32("max_display_mastering_luminance");
  fb.derived(decimal(max, 4) + " cd/m²");
  const uint32_t min = fb.u32("min_display_mastering_luminance");
  fb.derived(decimal(min, 4) + " cd/m²");
  std::array<std::string_view, 3> colour{};
  const std::string gamut = classify_primaries(p, {wx * 0.00002, wy * 0.00002}, colour);
  primaries.svalue = gamut;
  for (size_t i = 0; i < 3 && i < primaries.children.size(); ++i) {
    primaries.children.at(i)->svalue = std::string(colour.at(i));
  }
  derived_sibling(fb, "mastering_display", gamut + " " + decimal(max, 4, 0) + "/" + decimal(min, 4, 0));
  check_trailing(fb);
  return 0;
}

uint64_t decode_clli(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefHdr);
  fb.derived(std::to_string(fb.u16("max_content_light_level")) + " cd/m²");
  fb.derived(std::to_string(fb.u16("max_pic_average_light_level")) + " cd/m²");
  check_trailing(fb);
  return 0;
}

// Field names and order of the H.265 content_colour_volume SEI (D.2.40) after a 2-bit reserved field; luminance in
// 0.0000001 cd/m² (not the 0.0001 of mdcv).
uint64_t decode_cclv(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefHdr);
  fb.reserved(2);
  fb.u("ccv_cancel_flag", 1);
  fb.u("ccv_persistence_flag", 1);
  const bool primaries = fb.u("ccv_primaries_present_flag", 1) != 0;
  const bool min = fb.u("ccv_min_luminance_value_present_flag", 1) != 0;
  const bool max = fb.u("ccv_max_luminance_value_present_flag", 1) != 0;
  const bool avg = fb.u("ccv_avg_luminance_value_present_flag", 1) != 0;
  if (primaries) {
    fb.array("ccv_primaries", 3);
    for (uint32_t i = 0; i < 3; ++i) {
      fb.array_element("ccv_primaries", i);
      fb.derived(chromaticity(fb.i32("ccv_primaries_x")));
      fb.derived(chromaticity(fb.i32("ccv_primaries_y")));
      fb.end();
    }
    fb.end();
  }
  const auto luminance = [&](bool present, std::string_view name) {
    if (present) {
      fb.derived(decimal(fb.u32(name), 7) + " cd/m²");
    }
  };
  luminance(min, "ccv_min_luminance_value");
  luminance(max, "ccv_max_luminance_value");
  luminance(avg, "ccv_avg_luminance_value");
  check_trailing(fb);
  return 0;
}

// ---- dvcC / dvvC / dvwC (DOVIDecoderConfigurationRecord) ------------------------------------------------------

std::string_view dv_compatibility_name(uint64_t id) {
  switch (id) {
    case 0:
      return "none";
    case 1:
      return "HDR10";
    case 2:
      return "SDR";
    case 4:
      return "HLG";
    case 6:
      return "Blu-ray HDR10";
    default:
      return "reserved";
  }
}

uint64_t decode_dovi(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefDv);
  fb.u8("dv_version_major");
  fb.u8("dv_version_minor");
  const uint64_t profile = fb.u("dv_profile", 7);
  const size_t profile_index = fb.node().fields.size() - 1;
  fb.u("dv_level", 6);
  fb.u("rpu_present_flag", 1);
  fb.u("el_present_flag", 1);
  fb.u("bl_present_flag", 1);
  const uint64_t compat = fb.u("dv_bl_signal_compatibility_id", 4);
  const std::string_view compat_name = dv_compatibility_name(compat);
  fb.enum_name(compat_name);
  std::string text = "Profile " + std::to_string(profile);
  if (compat != 0) {
    text += "." + std::to_string(compat) + " (" + std::string(compat_name) + " compatible)";
  }
  fb.node().fields.at(profile_index)->svalue = text;
  const std::string_view type = fb.node().fourcc;
  if ((type == "dvcC" && profile >= 8) || (type == "dvvC" && profile <= 7)) {
    fb.diag(Severity::Warning, "CNT-5020",
            std::string(type) + " carries profile " + std::to_string(profile) +
                "; dvcC is for profiles up to 7 and dvvC for profiles 8 and above");
  }
  fb.reserved(28);
  fb.reserved(128);
  check_trailing(fb);
  return 0;
}

// ---- fiel (QTFF), srat (14496-12 §12.2.4) ---------------------------------------------------------------------

uint64_t decode_fiel(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefFiel);
  constexpr std::array<std::string_view, 3> kFields = {"reserved", "progressive", "interlaced"};
  const uint8_t fields = fb.u8("fields");
  fb.enum_name(fields < kFields.size() ? kFields.at(fields) : "reserved");
  std::string_view detail;
  switch (fb.u8("detail")) {
    case 0:
      detail = "unknown";
      break;
    case 1:
      detail = "separated fields, top first";
      break;
    case 6:
      detail = "separated fields, bottom first";
      break;
    case 9:
      detail = "interleaved fields, top first (top displayed first)";
      break;
    case 14:
      detail = "interleaved fields, bottom first (bottom displayed first)";
      break;
    default:
      detail = "reserved";
  }
  fb.enum_name(detail);
  check_trailing(fb);
  return 0;
}

uint64_t decode_srat(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefSrat);
  fb.derived(std::to_string(fb.u32("sampling_rate")) + " Hz");
  check_trailing(fb);
  return 0;
}

}  // namespace

OVT_BOX_DECODER("btrt", "*", &decode_btrt)
OVT_BOX_DECODER("pasp", "*", &decode_pasp)
OVT_BOX_DECODER("clap", "*", &decode_clap)
OVT_BOX_DECODER("colr", "*", &decode_colr)
OVT_BOX_DECODER("mdcv", "*", &decode_mdcv)
OVT_BOX_DECODER("clli", "*", &decode_clli)
OVT_BOX_DECODER("cclv", "*", &decode_cclv)
OVT_BOX_DECODER("dvcC", "*", &decode_dovi)
OVT_BOX_DECODER("dvvC", "*", &decode_dovi)
OVT_BOX_DECODER("dvwC", "*", &decode_dovi)
OVT_BOX_DECODER("fiel", "*", &decode_fiel)
OVT_BOX_DECODER("srat", "*", &decode_srat)

}  // namespace ovt::container::isobmff
