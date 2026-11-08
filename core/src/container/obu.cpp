// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Raw AV1 OBU streams (chapter 08 §8.6.4, WI-1.227).

#include "container/obu.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

#include "container/diagnostics.h"

namespace ovt::container {
namespace {

constexpr size_t kWindowBytes = 64U * 1024U;
constexpr uint64_t kDetectWindow = 64U * 1024U;

// Random access over a file or a span, with a small read cache for the walkers.
class Source {
 public:
  Source() = default;
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  virtual ~Source() = default;
  [[nodiscard]] virtual uint64_t size() const = 0;
  virtual size_t read(uint64_t offset, size_t len, uint8_t* dst) const = 0;
};

class FileSource final : public Source {
 public:
  explicit FileSource(const io::RandomAccessFile& f) : f_(f) {}
  [[nodiscard]] uint64_t size() const override { return f_.size(); }
  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override { return f_.read(offset, len, dst); }

 private:
  const io::RandomAccessFile& f_;
};

class SpanSource final : public Source {
 public:
  SpanSource(std::span<const uint8_t> d, uint64_t base) : d_(d), base_(base) {}
  [[nodiscard]] uint64_t size() const override { return base_ + d_.size(); }
  size_t read(uint64_t offset, size_t len, uint8_t* dst) const override {
    if (offset < base_ || offset - base_ >= d_.size()) {
      return 0;
    }
    const size_t n = std::min<uint64_t>(len, d_.size() - (offset - base_));
    std::copy_n(d_.begin() + static_cast<std::ptrdiff_t>(offset - base_), n, dst);
    return n;
  }

 private:
  std::span<const uint8_t> d_;
  uint64_t base_;
};

class Window {
 public:
  explicit Window(const Source& s) : src_(s), buf_(kWindowBytes) {}
  // Up to `len` bytes at `offset` (fewer near the end of the source, none at/after it).
  std::span<const uint8_t> at(uint64_t offset, size_t len) {
    const uint64_t size = src_.size();
    if (offset >= size) {
      return {};
    }
    len = static_cast<size_t>(std::min<uint64_t>(len, size - offset));
    if (offset < base_ || offset + len > base_ + len_) {
      len_ = src_.read(offset, std::min<uint64_t>(buf_.size(), size - offset), buf_.data());
      base_ = offset;
    }
    if (offset < base_ || offset >= base_ + len_) {
      return {};
    }
    return {buf_.data() + (offset - base_), std::min<size_t>(len, len_ - (offset - base_))};
  }

 private:
  const Source& src_;
  std::vector<uint8_t> buf_;
  uint64_t base_ = 0;
  size_t len_ = 0;
};

void report(const FindingSink& sink, DiagCode code, uint64_t offset, std::string message) {
  if (sink) {
    sink(Finding{code, offset, std::move(message)});
  }
}

bool known_obu_type(unsigned type) {
  return (type >= 1 && type <= 8) || type == 15;
}

// ---- detection ---------------------------------------------------------------------------------------------------

struct Section5Attempt {
  bool pass = false;
  bool missing_size_field = false;
};

// Up to three OBUs, each with a size field; the first is a temporal delimiter or a sequence header.
Section5Attempt attempt_section5(std::span<const uint8_t> d, uint64_t file_size) {
  Section5Attempt r;
  uint64_t pos = 0;
  int checked = 0;
  for (int n = 0; n < 3 && pos < d.size(); ++n) {
    ++checked;
    const ObuHeader h = parse_obu_header(d.subspan(pos, std::min<size_t>(2, d.size() - pos)));
    if (!h.valid) {
      return r;
    }
    if (n == 0 && h.obu_type != kObuSequenceHeader && h.obu_type != kObuTemporalDelimiter) {
      return r;
    }
    if (!h.has_size_field) {
      r.missing_size_field = n == 0;
      return r;
    }
    const uint64_t size_at = pos + h.header_bytes;
    if (size_at >= d.size()) {
      r.pass = true;  // the window ends inside this OBU, which passed so far
      return r;
    }
    const Leb128 leb = read_leb128(d.subspan(size_at));
    if (leb.status == LebStatus::TooLong) {
      return r;
    }
    if (leb.status == LebStatus::Truncated) {
      r.pass = true;
      return r;
    }
    const uint64_t end = size_at + leb.bytes + leb.value;
    if (leb.value >= (uint64_t{1} << 32U) || end > file_size) {
      return r;
    }
    if (end >= d.size()) {
      r.pass = true;
      return r;
    }
    pos = end;
  }
  r.pass = checked > 0;
  return r;
}

// temporal_unit_size / frame_unit_size / obu_length nesting, then the OBU header; the second temporal unit is
// checked when it lies inside the window.
bool attempt_annexb(std::span<const uint8_t> d, uint64_t file_size) {
  uint64_t pos = 0;
  for (int units = 0; units < 2 && pos < d.size(); ++units) {
    const auto stop = [&] { return units > 0; };  // ran out of window after a unit that passed
    const Leb128 tus = read_leb128(d.subspan(pos));
    if (tus.status == LebStatus::TooLong) {
      return false;
    }
    if (tus.status == LebStatus::Truncated) {
      return stop();
    }
    const uint64_t body = pos + tus.bytes;
    if (tus.value < 1 || body > file_size || tus.value > file_size - body) {
      return false;
    }
    if (body >= d.size()) {
      return stop();
    }
    const Leb128 fus = read_leb128(d.subspan(body));
    if (fus.status == LebStatus::TooLong) {
      return false;
    }
    if (fus.status == LebStatus::Truncated) {
      return stop();
    }
    if (fus.value < 1 || fus.value > tus.value) {
      return false;
    }
    const uint64_t fbody = body + fus.bytes;
    if (fbody >= d.size()) {
      return stop();
    }
    const Leb128 ol = read_leb128(d.subspan(fbody));
    if (ol.status == LebStatus::TooLong) {
      return false;
    }
    if (ol.status == LebStatus::Truncated) {
      return stop();
    }
    if (ol.value < 1 || ol.value > fus.value) {
      return false;
    }
    const uint64_t obody = fbody + ol.bytes;
    if (obody >= d.size()) {
      return stop();
    }
    const ObuHeader h = parse_obu_header(d.subspan(obody, std::min<size_t>(2, d.size() - obody)));
    if (!h.valid || (units == 0 && h.obu_type != kObuSequenceHeader && h.obu_type != kObuTemporalDelimiter)) {
      return false;
    }
    if (h.has_size_field && obody + h.header_bytes < d.size()) {
      const Leb128 sz = read_leb128(d.subspan(obody + h.header_bytes));
      if (sz.status == LebStatus::Ok && h.header_bytes + sz.bytes + sz.value != ol.value) {
        return false;
      }
    }
    pos = body + tus.value;
  }
  return pos > 0;
}

// ---- walking -----------------------------------------------------------------------------------------------------

struct Walker {
  Window w;
  uint64_t size;
  const ObuVisitor& visitor;
  const FindingSink& sink;
  ObuWalkSummary summary;

  Walker(const Source& s, const ObuVisitor& v, const FindingSink& f) : w(s), size(s.size()), visitor(v), sink(f) {}

  void emit(const ObuNode& n) {
    ++summary.obu_count;
    if (visitor.on_obu) {
      visitor.on_obu(n);
    }
  }
  void emit(const TemporalUnitNode& n) {
    ++summary.temporal_unit_count;
    if (visitor.on_temporal_unit) {
      visitor.on_temporal_unit(n);
    }
  }
  void emit(const FrameUnitNode& n) const {
    if (visitor.on_frame_unit) {
      visitor.on_frame_unit(n);
    }
  }

  void clamp_note(uint64_t at, uint64_t kept) const {
    report(sink, DiagCode::CNT_OBU_0003, at,
           "length at offset " + std::to_string(at) + " exceeds the bytes left; unit clamped to " +
               std::to_string(kept) + " bytes");
  }
  void malformed(uint64_t at, const char* what) const {
    report(sink, DiagCode::CNT_OBU_0004, at, std::string(what) + " at offset " + std::to_string(at) + " is malformed");
  }

  // A leb128 at `at` that must lie inside [at, limit).
  Leb128 leb_at(uint64_t at, uint64_t limit) {
    if (at >= limit) {
      return {};
    }
    return read_leb128(w.at(at, static_cast<size_t>(std::min<uint64_t>(8, limit - at))));
  }

  void section5() {
    uint64_t pos = 0;
    bool have_tu = false;
    TemporalUnitNode tu;
    const auto close_tu = [&](uint64_t end) {
      tu.file_span.length = end - tu.file_span.offset;
      emit(tu);
      have_tu = false;
    };
    uint64_t next_index = 0;
    while (pos < size) {
      const ObuHeader h = parse_obu_header(w.at(pos, 2));
      if (!h.valid) {
        malformed(pos, "OBU header");
        break;
      }
      if (!h.has_size_field) {
        report(sink, DiagCode::CNT_OBU_0001, pos,
               "OBU at offset " + std::to_string(pos) + " has obu_has_size_field == 0 outside Annex B framing");
        break;
      }
      const Leb128 leb = leb_at(pos + h.header_bytes, size);
      if (leb.status != LebStatus::Ok) {
        malformed(pos + h.header_bytes, "obu_size");
        break;
      }
      const uint64_t payload = pos + h.header_bytes + leb.bytes;
      uint64_t end = payload + leb.value;
      if (end > size) {
        end = size;
        clamp_note(pos + h.header_bytes, end - pos);
      }
      if (h.obu_type == kObuTemporalDelimiter && have_tu) {
        close_tu(pos);
      }
      if (!have_tu) {
        have_tu = true;
        tu = TemporalUnitNode{};
        tu.index = next_index++;
        tu.file_span.offset = pos;
      }
      ObuNode node;
      node.temporal_unit_index = tu.index;
      node.file_span = FileSpan{pos, end - pos};
      node.header = h;
      node.obu_size = end - payload;
      ++tu.obu_count;
      emit(node);
      pos = end;
    }
    if (have_tu) {
      close_tu(pos);
    }
  }

  void annexb() {
    uint64_t pos = 0;
    uint64_t tu_index = 0;
    while (pos < size) {
      const Leb128 tl = leb_at(pos, size);
      if (tl.status != LebStatus::Ok) {
        malformed(pos, "temporal_unit_size");
        break;
      }
      const uint64_t body = pos + tl.bytes;
      uint64_t tu_end = body + tl.value;
      if (tu_end > size) {
        tu_end = size;
        clamp_note(pos, tu_end - body);
      }
      TemporalUnitNode tu;
      tu.index = tu_index++;
      tu.file_span = FileSpan{body, tu_end - body};
      tu.prefix_span = FileSpan{pos, tl.bytes};
      walk_temporal_unit(tu, tu_end);
      emit(tu);
      pos = tu_end;
    }
  }

  void walk_temporal_unit(TemporalUnitNode& tu, uint64_t tu_end) {
    uint64_t fpos = tu.file_span.offset;
    while (fpos < tu_end) {
      const Leb128 fl = leb_at(fpos, tu_end);
      if (fl.status != LebStatus::Ok) {
        malformed(fpos, "frame_unit_size");
        return;
      }
      const uint64_t fbody = fpos + fl.bytes;
      uint64_t fu_end = fbody + fl.value;
      if (fu_end > tu_end) {
        fu_end = tu_end;
        clamp_note(fpos, fu_end - fbody);
      }
      FrameUnitNode fu;
      fu.temporal_unit_index = tu.index;
      fu.frame_unit_index = tu.frame_unit_count++;
      fu.file_span = FileSpan{fbody, fu_end - fbody};
      fu.prefix_span = FileSpan{fpos, fl.bytes};
      walk_frame_unit(tu, fu, fu_end);
      emit(fu);
      fpos = fu_end;
    }
  }

  void walk_frame_unit(TemporalUnitNode& tu, FrameUnitNode& fu, uint64_t fu_end) {
    uint64_t opos = fu.file_span.offset;
    while (opos < fu_end) {
      const Leb128 ol = leb_at(opos, fu_end);
      if (ol.status != LebStatus::Ok) {
        malformed(opos, "obu_length");
        return;
      }
      const uint64_t obody = opos + ol.bytes;
      uint64_t oend = obody + ol.value;
      if (oend > fu_end) {
        oend = fu_end;
        clamp_note(opos, oend - obody);
      }
      const uint64_t prefix_at = opos;
      opos = oend;
      if (oend == obody) {
        malformed(prefix_at, "obu_length 0");
        continue;
      }
      const ObuHeader h = parse_obu_header(w.at(obody, static_cast<size_t>(std::min<uint64_t>(2, oend - obody))));
      if (!h.valid || h.header_bytes > oend - obody) {
        malformed(obody, "OBU header");
        continue;
      }
      ObuNode node;
      node.temporal_unit_index = tu.index;
      node.frame_unit_index = static_cast<int64_t>(fu.frame_unit_index);
      node.file_span = FileSpan{obody, oend - obody};
      node.prefix_span = FileSpan{prefix_at, ol.bytes};
      node.header = h;
      node.obu_length = oend - obody;
      node.obu_size = node.obu_length - h.header_bytes;
      if (h.has_size_field) {
        const Leb128 sz = leb_at(obody + h.header_bytes, oend);
        if (sz.status == LebStatus::Ok) {
          node.obu_size = sz.value;
        }
      }
      ++tu.obu_count;
      ++fu.obu_count;
      emit(node);
    }
  }
};

}  // namespace

std::string_view obu_framing_name(ObuFraming f) noexcept {
  switch (f) {
    case ObuFraming::Section5:
      return "section5";
    case ObuFraming::AnnexB:
      return "annexb";
    default:
      return "";
  }
}

Leb128 read_leb128(std::span<const uint8_t> data) noexcept {
  Leb128 r;
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    if (i >= data.size()) {
      r.status = LebStatus::Truncated;
      return r;
    }
    const uint8_t b = data[i];
    value |= static_cast<uint64_t>(b & 0x7FU) << (7U * i);
    if ((b & 0x80U) == 0) {
      r.status = LebStatus::Ok;
      r.value = value;
      r.bytes = static_cast<uint8_t>(i + 1);
      return r;
    }
  }
  r.status = LebStatus::TooLong;
  return r;
}

void append_leb128(std::vector<uint8_t>& out, uint64_t value) {
  do {
    auto byte = static_cast<uint8_t>(value & 0x7FU);
    value >>= 7U;
    if (value != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (value != 0);
}

ObuHeader parse_obu_header(std::span<const uint8_t> data) noexcept {
  ObuHeader h;
  if (data.empty()) {
    return h;
  }
  const uint8_t b = data[0];
  const unsigned type = (b >> 3U) & 0xFU;
  h.obu_type = static_cast<uint8_t>(type);
  h.extension_flag = ((b >> 2U) & 1U) != 0;
  h.has_size_field = ((b >> 1U) & 1U) != 0;
  h.header_bytes = h.extension_flag ? 2 : 1;
  if (h.extension_flag) {
    if (data.size() < 2) {
      return h;
    }
    h.temporal_id = static_cast<uint8_t>(data[1] >> 5U);
    h.spatial_id = static_cast<uint8_t>((data[1] >> 3U) & 3U);
  }
  h.valid = (b & 0x80U) == 0 && (b & 1U) == 0 && known_obu_type(type);
  return h;
}

ObuDetectResult obu_detect(std::span<const uint8_t> prefix, uint64_t file_size) noexcept {
  const std::span<const uint8_t> d = prefix.first(std::min<uint64_t>(prefix.size(), kDetectWindow));
  ObuDetectResult r;
  const Section5Attempt s5 = attempt_section5(d, file_size);
  const bool ab = attempt_annexb(d, file_size);
  if (s5.pass && ab) {
    r.both_framings = true;
    r.framing = d.size() >= 2 && d[0] == 0x12 && d[1] == 0x00 ? ObuFraming::Section5 : ObuFraming::AnnexB;
  } else if (s5.pass) {
    r.framing = ObuFraming::Section5;
  } else if (ab) {
    r.framing = ObuFraming::AnnexB;
  } else {
    r.missing_size_field = s5.missing_size_field;
  }
  return r;
}

namespace {

ObuWalkSummary walk(const Source& src, ObuFraming framing, const ObuVisitor& visitor, const FindingSink& findings) {
  Walker w(src, visitor, findings);
  if (framing == ObuFraming::Section5) {
    w.section5();
  } else if (framing == ObuFraming::AnnexB) {
    w.annexb();
  }
  return w.summary;
}

}  // namespace

ObuWalkSummary obu_walk(const io::RandomAccessFile& file, ObuFraming framing, const ObuVisitor& visitor,
                        const FindingSink& findings) {
  file.advise(0, file.size(), io::RandomAccessFile::Access::Sequential);
  return walk(FileSource(file), framing, visitor, findings);
}

ObuWalkSummary obu_walk(std::span<const uint8_t> data, uint64_t base_offset, ObuFraming framing,
                        const ObuVisitor& visitor, const FindingSink& findings) {
  return walk(SpanSource(data, base_offset), framing, visitor, findings);
}

bool reframe_annexb_obu(std::span<const uint8_t> obu, std::vector<uint8_t>& out) {
  out.clear();
  const ObuHeader h = parse_obu_header(obu.first(std::min<size_t>(2, obu.size())));
  if (!h.valid || obu.size() < h.header_bytes) {
    return false;
  }
  if (h.has_size_field) {
    const Leb128 sz = read_leb128(obu.subspan(h.header_bytes));
    if (sz.status != LebStatus::Ok || h.header_bytes + sz.bytes + sz.value != obu.size()) {
      return false;
    }
    out.assign(obu.begin(), obu.end());
    return true;
  }
  const uint64_t payload = obu.size() - h.header_bytes;
  out.reserve(obu.size() + 8);
  out.push_back(static_cast<uint8_t>(obu[0] | 0x02U));
  if (h.extension_flag) {
    out.push_back(obu[1]);
  }
  append_leb128(out, payload);
  out.insert(out.end(), obu.begin() + h.header_bytes, obu.end());
  return true;
}

}  // namespace ovt::container
