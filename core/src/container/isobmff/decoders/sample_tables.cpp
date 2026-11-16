// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group D part 1: the time, chunk and size tables of `stbl` (chapter 08 §8.3.4, WI-1.209): `stts`,
// `ctts`, `cslg`, `stsc`, `stsz`, `stz2`, `stco`, `co64`, `stss`, `stps` and `sdtp`.
//
// Contract (§8.2.9): the walker has read FullBox version/flags, so `br` starts at the first field after them. The
// tables can be larger than the 1 MiB a decoder's BitReader sees, so the entries are read window by window from the
// PagedSpan of the box payload (BoxContext::payload_pages, or the BitReader's own bytes when a decoder runs outside the
// walker): at most 1 MiB is in memory at a time, only the entries FieldBuilder::materialise() accepts become Fields
// (§8.2.5), and the statistics in the derived text cover every entry. Derived text goes into `Field.svalue`
// (SPEC-ASSUMPTION(#415), field_builder.h). The checks that need two boxes whatever their order (`stts` against
// `stsz`, `sdtp` length, ...) are finish_sample_tables(), which the walk calls once the children of `stbl` are known.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "container/isobmff/paged_span.h"
#include "container/isobmff/sample_tables.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

constexpr std::string_view kRefStts = "ISO/IEC 14496-12:2022 §8.6.1.2";
constexpr std::string_view kRefCtts = "ISO/IEC 14496-12:2022 §8.6.1.3";
constexpr std::string_view kRefCslg = "ISO/IEC 14496-12:2022 §8.6.1.4";
constexpr std::string_view kRefStsc = "ISO/IEC 14496-12:2022 §8.7.4";
constexpr std::string_view kRefStsz = "ISO/IEC 14496-12:2022 §8.7.3.2";
constexpr std::string_view kRefStz2 = "ISO/IEC 14496-12:2022 §8.7.3.3";
constexpr std::string_view kRefStco = "ISO/IEC 14496-12:2022 §8.7.5";
constexpr std::string_view kRefStss = "ISO/IEC 14496-12:2022 §8.6.2";
constexpr std::string_view kRefStps = "QTFF Partial Sync Sample Atom";
constexpr std::string_view kRefSdtp = "ISO/IEC 14496-12:2022 §8.6.4";

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

uint8_t version_of(FieldBuilder& fb) {
  const int v = fb.node().version;
  return v < 0 ? 0 : static_cast<uint8_t>(v);
}

// CNT-1003 for a version the decoder does not know; the payload is then left undecoded.
bool known_version(FieldBuilder& fb, uint8_t max_version) {
  const uint8_t v = version_of(fb);
  if (v <= max_version) {
    return true;
  }
  fb.diag(Severity::Warning, "CNT-1003",
          "unknown " + fb.node().fourcc + " version " + std::to_string(v) + "; fields not decoded");
  return false;
}

std::string fixed3(double v) {
  std::array<char, 40> buf{};
  const int n = std::snprintf(buf.data(), buf.size(), "%.3f", v);
  return {buf.data(), static_cast<size_t>(std::max(n, 0))};
}

// "12.345 s" for `ticks` of `timescale`, or "" when the timescale is unknown.
std::string seconds_text(double ticks, uint32_t timescale) {
  return timescale == 0 ? std::string{} : fixed3(ticks / timescale) + " s";
}

// ---- reading a table window by window ------------------------------------------------------------------------

// One table entry being read: straight off the window when it is not materialised, through FieldBuilder when it is.
struct Entry {
  FieldBuilder& fb;
  BitReader& r;
  bool mat;

  [[nodiscard]] uint32_t u32(std::string_view name) const {
    return mat ? fb.u32(name) : static_cast<uint32_t>(r.u(32));
  }
  void skip_u32(std::string_view name) const { static_cast<void>(u32(name)); }
  [[nodiscard]] int32_t i32(std::string_view name) const { return mat ? fb.i32(name) : static_cast<int32_t>(r.i(32)); }
  [[nodiscard]] uint64_t u64(std::string_view name) const { return mat ? fb.u64(name) : r.u(64); }
  [[nodiscard]] uint64_t u(std::string_view name, unsigned nbits) const { return mat ? fb.u(name, nbits) : r.u(nbits); }
};

// The entries of a table that starts at the current position of the decoder's reader.
class Table {
 public:
  Table(BoxContext& ctx, BitReader& br, FieldBuilder& fb)
      : br_(br),
        fb_(fb),
        local_(br.bytes(0, br.size_bits() / 8U)),
        pages_(ctx.payload_pages != nullptr ? *ctx.payload_pages : local_),
        start_(fb.bytes_consumed()) {}

  // How many of the `declared` entries of `entry_bits` bits the payload holds. CNT-1014 when fewer.
  uint32_t fit(uint64_t declared, unsigned entry_bits) {
    const uint64_t avail = pages_.size() - std::min(start_, pages_.size());
    const uint64_t fit = avail * 8U / entry_bits;
    if (fit >= declared) {
      return static_cast<uint32_t>(declared);
    }
    fb_.diag(Severity::Error, "CNT-1014",
             fb_.node().fourcc + ".entry_count " + std::to_string(declared) + " needs " +
                 std::to_string((declared * entry_bits + 7U) / 8U) + " bytes; " + std::to_string(fit) +
                 " entries fit in the box");
    return static_cast<uint32_t>(fit);  // fit < declared <= UINT32_MAX
  }

  // Bytes after the payload offset `byte` that the decoder did not read.
  [[nodiscard]] uint64_t remaining_after(uint64_t byte) const { return pages_.size() - std::min(byte, pages_.size()); }
  [[nodiscard]] uint64_t start() const noexcept { return start_; }
  [[nodiscard]] uint64_t size() const noexcept { return pages_.size(); }

  // Reads `count` entries of `entry_bits` bits in windows of at most 1 MiB, as the array `array` whose materialised
  // entries are the groups `<elem>[i]`; `read(Entry, index)` reads one entry (all its bits, materialised or not).
  template <typename ReadEntry>
  void walk(uint32_t count, unsigned entry_bits, std::string_view array, std::string_view elem, ReadEntry&& read) {
    fb_.array(array, count);
    const uint64_t per_window =
        PagedSpan::kMaxWindow * 8U / entry_bits;  // a multiple of 8, so windows are byte aligned
    for (uint64_t first = 0; first < count; first += per_window) {
      const uint64_t n = std::min<uint64_t>(per_window, count - first);
      const uint64_t at = start_ + first * entry_bits / 8U;
      const size_t need = (n * entry_bits + 7U) / 8U;
      const std::span<const uint8_t> w = pages_.window(at, need);
      if (w.size() < need) {  // the file shrank under us
        fb_.rebind(br_, at);
        throw ParseError("BR-0001", "table window shorter than the box payload", at * 8U);
      }
      BitReader wr(w);
      fb_.rebind(wr, at);
      try {
        for (uint64_t i = first; i < first + n; ++i) {
          const bool mat = FieldBuilder::materialise(static_cast<uint32_t>(i), count);
          if (mat) {
            fb_.array_element(elem, static_cast<uint32_t>(i));
          }
          read(Entry{fb_, wr, mat}, i);
          if (mat) {
            fb_.end();
          }
        }
      } catch (...) {
        fb_.rebind(br_, at);  // `wr` dies with this scope; the builder must not keep a pointer to it
        throw;
      }
    }
    fb_.rebind(br_, end_of(count, entry_bits));
    fb_.end();
  }

  [[nodiscard]] uint64_t end_of(uint64_t count, unsigned entry_bits) const {
    return start_ + (count * entry_bits + 7U) / 8U;
  }

  // CNT-1005 (Info) for payload bytes after the table.
  void check_trailing(uint64_t count, unsigned entry_bits) {
    if (const uint64_t extra = remaining_after(end_of(count, entry_bits)); extra > 0) {
      fb_.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in " + fb_.node().fourcc);
    }
  }

 private:
  BitReader& br_;
  FieldBuilder& fb_;
  PagedSpan local_;
  PagedSpan& pages_;
  uint64_t start_;
};

// ---- stts (14496-12 §8.6.1.2) -----------------------------------------------------------------------------------

uint64_t decode_stts(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefStts);
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t declared = fb.u32("entry_count");
  Table table(ctx, br, fb);
  const uint32_t count = table.fit(declared, 64);
  uint64_t samples = 0;
  double ticks = 0;
  uint64_t zero_deltas = 0;
  uint64_t first_zero = 0;
  table.walk(count, 64, "entries", "entry", [&](const Entry& e, uint64_t i) {
    const uint32_t n = e.u32("sample_count");
    const uint32_t delta = e.u32("sample_delta");
    samples += n;
    ticks += static_cast<double>(n) * delta;
    // Only the last sample of the track may have a zero delta (CNT-2001).
    if (delta == 0 && n > 0 && (i + 1 != count || n != 1) && zero_deltas++ == 0) {
      first_zero = i;
    }
  });
  const double mean = samples > 0 ? ticks / static_cast<double>(samples) : 0.0;
  std::string text = std::to_string(samples) + " samples, duration " + fixed3(ticks) + " ticks";
  if (const std::string s = seconds_text(ticks, ctx.track_timescale); !s.empty()) {
    text += " (" + s + ")";
  }
  text += ", mean delta " + fixed3(mean);
  if (mean > 0 && ctx.track_timescale != 0) {
    text += " (" + fixed3(ctx.track_timescale / mean) + " fps)";
  }
  fb.derived(text);
  if (zero_deltas > 0) {
    fb.diag(Severity::Warning, "CNT-2001",
            std::to_string(zero_deltas) + " entries with sample_delta 0 before the last sample (first: entry " +
                std::to_string(first_zero) + ")");
  }
  ctx.stbl.has_stts = true;
  ctx.stbl.stts_samples = samples;
  if (declared == count) {
    table.check_trailing(count, 64);
  }
  return 0;
}

// ---- ctts (14496-12 §8.6.1.3) -----------------------------------------------------------------------------------

uint64_t decode_ctts(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefCtts);
  if (!known_version(fb, 1)) {
    return 0;
  }
  const bool v0 = version_of(fb) == 0;
  const uint32_t declared = fb.u32("entry_count");
  Table table(ctx, br, fb);
  const uint32_t count = table.fit(declared, 64);
  uint64_t samples = 0;
  int32_t lo = std::numeric_limits<int32_t>::max();
  int32_t hi = std::numeric_limits<int32_t>::min();
  uint64_t negative = 0;
  uint64_t first_negative = 0;
  // A version 0 offset is unsigned, but a value >= 2^31 is read as negative like libavformat and GPAC do (CNT-2002).
  table.walk(count, 64, "entries", "entry", [&](const Entry& e, uint64_t i) {
    samples += e.u32("sample_count");
    const int32_t offset = e.i32("sample_offset");
    lo = std::min(lo, offset);
    hi = std::max(hi, offset);
    if (v0 && offset < 0 && negative++ == 0) {
      first_negative = i;
    }
  });
  std::string text = std::to_string(samples) + " samples";
  if (count > 0) {
    text += ", offsets " + std::to_string(lo) + " to " + std::to_string(hi) + " ticks";
  }
  fb.derived(text);
  if (negative > 0) {
    fb.diag(Severity::Warning, "CNT-2002",
            "ctts version 0 with " + std::to_string(negative) +
                " offsets >= 2^31, interpreted as negative (first: entry " + std::to_string(first_negative) + ")");
  }
  ctx.stbl.has_ctts = true;
  ctx.stbl.ctts_samples = samples;
  if (declared == count) {
    table.check_trailing(count, 64);
  }
  return 0;
}

// ---- cslg (14496-12 §8.6.1.4) -----------------------------------------------------------------------------------

uint64_t decode_cslg(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefCslg);
  if (!known_version(fb, 1)) {
    return 0;
  }
  const bool wide = version_of(fb) == 1;
  for (const std::string_view name : {"compositionToDTSShift", "leastDecodeToDisplayDelta",
                                      "greatestDecodeToDisplayDelta", "compositionStartTime", "compositionEndTime"}) {
    const int64_t v = fb.i32or64(name, wide);
    fb.derived(seconds_text(static_cast<double>(v), ctx.track_timescale));
  }
  if (const uint64_t extra = br.bits_left() / 8U; extra > 0) {
    fb.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in cslg");
  }
  return 0;
}

// ---- stsc (14496-12 §8.7.4) -------------------------------------------------------------------------------------

uint64_t decode_stsc(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefStsc);
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t declared = fb.u32("entry_count");
  Table table(ctx, br, fb);
  const uint32_t count = table.fit(declared, 96);
  uint32_t prev = 0;
  bool order_ok = true;
  uint64_t bad_order = 0;
  uint64_t bad_index = 0;
  uint64_t first_bad_index = 0;
  table.walk(count, 96, "entries", "entry", [&](const Entry& e, uint64_t i) {
    const uint32_t first_chunk = e.u32("first_chunk");
    e.skip_u32("samples_per_chunk");
    const uint32_t index = e.u32("sample_description_index");
    // The first entry starts at chunk 1; the others strictly increase (CNT-2003).
    if (order_ok && (i == 0 ? first_chunk != 1 : first_chunk <= prev)) {
      order_ok = false;
      bad_order = i;
    }
    prev = first_chunk;
    const bool out_of_range = ctx.stbl.has_stsd && index > ctx.stbl.stsd_entries;
    if ((index == 0 || out_of_range) && bad_index++ == 0) {
      first_bad_index = i;
    }
  });
  if (!order_ok) {
    fb.diag(Severity::Error, "CNT-2003",
            "stsc.first_chunk of entry " + std::to_string(bad_order) +
                (bad_order == 0 ? " is not 1" : " is not greater than the previous entry"));
  }
  if (bad_index > 0) {
    fb.diag(Severity::Error, "CNT-2004",
            std::to_string(bad_index) + " entries with sample_description_index 0 or above the stsd entry count " +
                std::to_string(ctx.stbl.stsd_entries) + " (first: entry " + std::to_string(first_bad_index) + ")");
  }
  if (declared == count) {
    table.check_trailing(count, 96);
  }
  return 0;
}

// ---- stsz / stz2 (14496-12 §8.7.3) ------------------------------------------------------------------------------

struct SizeStats {
  uint64_t total = 0;
  uint64_t min = std::numeric_limits<uint64_t>::max();
  uint64_t max = 0;
  uint64_t zero = 0;
  uint64_t first_zero = 0;

  void add(uint64_t size, uint64_t index) {
    total += size;
    min = std::min(min, size);
    max = std::max(max, size);
    if (size == 0 && zero++ == 0) {
      first_zero = index;
    }
  }
};

// Derived text and CNT-2013 of the per-sample sizes just walked (the last field is the array group).
void report_sizes(FieldBuilder& fb, const SizeStats& s, uint32_t count) {
  if (count > 0) {
    fb.derived("total " + std::to_string(s.total) + " bytes, min " + std::to_string(s.min) + ", max " +
               std::to_string(s.max) + ", mean " + fixed3(static_cast<double>(s.total) / count));
  }
  if (s.zero > 0) {
    fb.diag(Severity::Info, "CNT-2013",
            std::to_string(s.zero) + " zero-size samples (first: sample " + std::to_string(s.first_zero) + ")");
  }
}

void note_sample_count(BoxContext& ctx, uint32_t sample_count) {
  ctx.stbl.has_sample_count = true;
  ctx.stbl.sample_count = sample_count;
}

uint64_t decode_stsz(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefStsz);
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t sample_size = fb.u32("sample_size");
  const uint32_t declared = fb.u32("sample_count");
  note_sample_count(ctx, declared);
  if (sample_size != 0) {  // one size for every sample: no entries
    fb.derived("total " + std::to_string(uint64_t{sample_size} * declared) + " bytes");
    if (const uint64_t extra = br.bits_left() / 8U; extra > 0) {
      fb.diag(Severity::Info, "CNT-1005", std::to_string(extra) + " unparsed trailing bytes in stsz");
    }
    return 0;
  }
  Table table(ctx, br, fb);
  const uint32_t count = table.fit(declared, 32);
  SizeStats stats;
  table.walk(count, 32, "entries", "entry", [&](const Entry& e, uint64_t i) { stats.add(e.u32("entry_size"), i); });
  report_sizes(fb, stats, count);
  if (declared == count) {
    table.check_trailing(count, 32);
  }
  return 0;
}

uint64_t decode_stz2(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefStz2);
  if (!known_version(fb, 0)) {
    return 0;
  }
  fb.reserved(24);
  const uint8_t field_size = fb.u8("field_size");
  const uint32_t declared = fb.u32("sample_count");
  note_sample_count(ctx, declared);
  if (field_size != 4 && field_size != 8 && field_size != 16) {
    fb.diag(Severity::Error, "CNT-2006", "stz2.field_size " + std::to_string(field_size) + " is not 4, 8 or 16");
    return 0;
  }
  Table table(ctx, br, fb);
  const uint32_t count = table.fit(declared, field_size);
  SizeStats stats;
  // Two 4-bit entries per byte, the high nibble first (BitReader reads MSB first).
  table.walk(count, field_size, "entries", "entry",
             [&](const Entry& e, uint64_t i) { stats.add(e.u("entry_size", field_size), i); });
  report_sizes(fb, stats, count);
  if (declared == count) {
    table.check_trailing(count, field_size);
  }
  return 0;
}

// ---- stco / co64 (14496-12 §8.7.5) ------------------------------------------------------------------------------

uint64_t decode_chunk_offsets(BoxContext& ctx, BitReader& br, FieldBuilder& fb, bool wide) {
  const SpecRefScope scope(fb, kRefStco);
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t declared = fb.u32("entry_count");
  Table table(ctx, br, fb);
  const unsigned bits = wide ? 64U : 32U;
  const uint32_t count = table.fit(declared, bits);
  uint64_t lo = std::numeric_limits<uint64_t>::max();
  uint64_t hi = 0;
  uint64_t beyond = 0;
  uint64_t first_beyond = 0;
  table.walk(count, bits, "entries", "entry", [&](const Entry& e, uint64_t i) {
    const uint64_t offset = wide ? e.u64("chunk_offset") : e.u32("chunk_offset");
    lo = std::min(lo, offset);
    hi = std::max(hi, offset);
    if (ctx.file_size != 0 && offset > ctx.file_size && beyond++ == 0) {
      first_beyond = i;
    }
  });
  if (count > 0) {
    fb.derived(std::to_string(count) + " chunks, offsets " + std::to_string(lo) + " to " + std::to_string(hi));
  }
  if (beyond > 0) {
    fb.diag(Severity::Error, "CNT-2007",
            std::to_string(beyond) + " chunk offsets beyond the end of the file (" + std::to_string(ctx.file_size) +
                " bytes; first: entry " + std::to_string(first_beyond) + ")");
  }
  (wide ? ctx.stbl.has_co64 : ctx.stbl.has_stco) = true;
  if (declared == count) {
    table.check_trailing(count, bits);
  }
  return 0;
}

uint64_t decode_stco(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  return decode_chunk_offsets(ctx, br, fb, false);
}

uint64_t decode_co64(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  return decode_chunk_offsets(ctx, br, fb, true);
}

// ---- stss / stps (14496-12 §8.6.2, QTFF) ------------------------------------------------------------------------

uint64_t decode_sample_numbers(BoxContext& ctx, BitReader& br, FieldBuilder& fb, bool partial) {
  const SpecRefScope scope(fb, partial ? kRefStps : kRefStss);
  if (!known_version(fb, 0)) {
    return 0;
  }
  const uint32_t declared = fb.u32("entry_count");
  Table table(ctx, br, fb);
  const uint32_t count = table.fit(declared, 32);
  uint32_t prev = 0;
  uint32_t first = 0;
  uint64_t bad = 0;
  uint64_t first_bad = 0;
  table.walk(count, 32, "entries", "entry", [&](const Entry& e, uint64_t i) {
    const uint32_t number = e.u32("sample_number");
    if (i == 0) {
      first = number;
    }
    if ((number == 0 || number <= prev) && bad++ == 0) {  // 1-based and strictly increasing (CNT-2009)
      first_bad = i;
    }
    prev = std::max(prev, number);
  });
  std::string text = std::to_string(count) + (partial ? " partial sync samples" : " sync samples");
  if (count > 1) {
    text += ", mean GOP length " + fixed3(static_cast<double>(prev - first) / (count - 1));
  }
  fb.derived(text);
  if (bad > 0) {
    fb.diag(Severity::Error, "CNT-2009",
            std::to_string(bad) + " sample numbers that are 0 or not increasing (first: entry " +
                std::to_string(first_bad) + ")");
  }
  if (partial) {
    ctx.stbl.has_stps = true;
    ctx.stbl.stps_max = prev;
  } else {
    ctx.stbl.has_stss = true;
    ctx.stbl.stss_entries = count;
    ctx.stbl.stss_max = prev;
  }
  if (declared == count) {
    table.check_trailing(count, 32);
  }
  return 0;
}

uint64_t decode_stss(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  return decode_sample_numbers(ctx, br, fb, false);
}

uint64_t decode_stps(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  return decode_sample_numbers(ctx, br, fb, true);
}

// ---- sdtp (14496-12 §8.6.4) -------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 4> kLeading = {"unknown",
                                                      "leading with a dependency before the referenced I picture",
                                                      "not a leading sample", "leading without a dependency"};
constexpr std::array<std::string_view, 4> kDependsOn = {"unknown", "depends on others (not an I picture)",
                                                        "does not depend on others (I picture)", "reserved"};
constexpr std::array<std::string_view, 4> kIsDependedOn = {"unknown", "others may depend on it",
                                                           "no other sample depends on it (disposable)", "reserved"};
constexpr std::array<std::string_view, 4> kRedundancy = {"unknown", "has redundant coding", "no redundant coding",
                                                         "reserved"};

void two_bit_enum(FieldBuilder& fb, std::string_view name, const std::array<std::string_view, 4>& names) {
  fb.enum_name(names.at(fb.u(name, 2)));
}

uint64_t decode_sdtp(BoxContext& ctx, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, kRefSdtp);
  if (!known_version(fb, 0)) {
    return 0;
  }
  Table table(ctx, br, fb);
  // One byte per sample; the sample count comes from stsz / stz2, which finish_sample_tables() compares with this
  // length.
  const uint64_t bytes = table.remaining_after(table.start());
  const auto count = static_cast<uint32_t>(std::min<uint64_t>(bytes, std::numeric_limits<uint32_t>::max()));
  table.walk(count, 8, "entries", "entry", [&](const Entry& e, uint64_t /*i*/) {
    if (!e.mat) {
      e.r.skip_bits(8);
      return;
    }
    two_bit_enum(e.fb, "is_leading", kLeading);
    two_bit_enum(e.fb, "sample_depends_on", kDependsOn);
    two_bit_enum(e.fb, "sample_is_depended_on", kIsDependedOn);
    two_bit_enum(e.fb, "sample_has_redundancy", kRedundancy);
  });
  if (ctx.parent_is("stbl")) {
    ctx.stbl.has_sdtp = true;
    ctx.stbl.sdtp_bytes = bytes;
  }
  return 0;
}

// ---- the checks between boxes ---------------------------------------------------------------------------------

fb::BoxNodeT* find_child(std::vector<std::unique_ptr<fb::BoxNodeT>>& children, std::string_view fourcc) {
  const auto it = std::ranges::find_if(children, [&](const auto& c) { return c && c->fourcc == fourcc; });
  return it == children.end() ? nullptr : it->get();
}

// A diagnostic on `node`, spanning its `field` when it has one (else its header end).
void add_box_diag(fb::BoxNodeT& node, std::string_view code, std::string message, std::string_view field) {
  const DiagInfo* info = diag(code);
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = info != nullptr ? static_cast<fb::Severity>(info->severity) : fb::Severity::Error;
  d->code = std::string(code);
  d->message = std::move(message);
  d->spec_ref = info != nullptr ? std::string(info->spec_ref) : std::string{};
  d->span = std::make_unique<fb::BitSpan>(uint64_t{node.header_length} * 8U, 0U);
  for (const auto& f : node.fields) {
    if (f && f->name == field && f->span != nullptr) {
      d->span = std::make_unique<fb::BitSpan>(f->span->bit_offset(), f->span->bit_length());
      break;
    }
  }
  node.diagnostics.push_back(std::move(d));
}

}  // namespace

void finish_sample_tables(std::vector<std::unique_ptr<fb::BoxNodeT>>& children, const BoxContext& ctx) {
  if (!ctx.parent_is("stbl")) {
    return;
  }
  const SampleTableState& s = ctx.stbl;
  const auto report = [&](std::string_view box, std::string_view code, std::string message, std::string_view field) {
    if (fb::BoxNodeT* n = find_child(children, box); n != nullptr) {
      add_box_diag(*n, code, std::move(message), field);
    }
  };
  const std::string count = std::to_string(s.sample_count);
  if (s.has_stts && s.has_sample_count && s.stts_samples != s.sample_count) {
    const char* sizes = find_child(children, "stsz") != nullptr ? "stsz" : "stz2";
    report(sizes, "CNT-2005", "stts covers " + std::to_string(s.stts_samples) + " samples, " + sizes + " " + count,
           "sample_count");
  }
  if (s.has_ctts && s.has_sample_count && s.ctts_samples != s.sample_count) {
    report("ctts", "CNT-2011", "ctts runs cover " + std::to_string(s.ctts_samples) + " samples, sample count " + count,
           "entry_count");
  }
  if (s.has_stco && s.has_co64) {
    report("co64", "CNT-2008", "both stco and co64 are present", "entry_count");
  }
  if (s.has_sample_count && s.has_stss && s.stss_max > s.sample_count) {
    report("stss", "CNT-2009", "sample number " + std::to_string(s.stss_max) + " is above the sample count " + count,
           "entry_count");
  }
  if (s.has_sample_count && s.has_stps && s.stps_max > s.sample_count) {
    report("stps", "CNT-2009", "sample number " + std::to_string(s.stps_max) + " is above the sample count " + count,
           "entry_count");
  }
  if (s.has_sample_count && s.has_stss && s.sample_count > 0 && s.stss_entries == s.sample_count) {
    report("stss", "CNT-2019", "stss lists every sample (" + count + "); the box should be omitted", "entry_count");
  }
  if (s.has_sample_count && s.has_sdtp && s.sdtp_bytes != s.sample_count) {
    report("sdtp", "CNT-2010", "sdtp has " + std::to_string(s.sdtp_bytes) + " bytes for " + count + " samples", "");
  }
}

OVT_BOX_DECODER("stts", "*", &decode_stts)
OVT_BOX_DECODER("ctts", "*", &decode_ctts)
OVT_BOX_DECODER("cslg", "*", &decode_cslg)
OVT_BOX_DECODER("stsc", "*", &decode_stsc)
OVT_BOX_DECODER("stsz", "*", &decode_stsz)
OVT_BOX_DECODER("stz2", "*", &decode_stz2)
OVT_BOX_DECODER("stco", "*", &decode_stco)
OVT_BOX_DECODER("co64", "*", &decode_co64)
OVT_BOX_DECODER("stss", "*", &decode_stss)
OVT_BOX_DECODER("stps", "*", &decode_stps)
OVT_BOX_DECODER("sdtp", "*", &decode_sdtp)

}  // namespace ovt::container::isobmff
