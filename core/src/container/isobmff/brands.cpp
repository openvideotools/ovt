// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "container/isobmff/brands.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"

namespace ovt::container::isobmff {
namespace {

using fb::BoxNodeT;

constexpr size_t kMaxRead = size_t{1} << 20;  // payload bytes read from any one box
constexpr unsigned kMaxUnreferenced = 32;     // CNT-7032 findings per file
constexpr uint32_t kDefaultBaseIsMoof = 0x020000;

uint32_t be32(std::span<const uint8_t> p, size_t at) {
  if (p.size() < at + 4) {
    return 0;
  }
  return (uint32_t{p[at]} << 24U) | (uint32_t{p[at + 1]} << 16U) | (uint32_t{p[at + 2]} << 8U) | p[at + 3];
}

uint64_t be64(std::span<const uint8_t> p, size_t at) {
  return (uint64_t{be32(p, at)} << 32U) | be32(p, at + 4);
}

// The four bytes at `at` as text; empty when they are not all there.
std::string fourcc_at(std::span<const uint8_t> p, size_t at) {
  if (p.size() < at + 4) {
    return {};
  }
  return {static_cast<char>(p[at]), static_cast<char>(p[at + 1]), static_cast<char>(p[at + 2]),
          static_cast<char>(p[at + 3])};
}

uint16_t be16(std::span<const uint8_t> p, size_t at) {
  return p.size() < at + 2 ? uint16_t{0} : static_cast<uint16_t>((p[at] << 8U) | p[at + 1]);
}

const BoxNodeT* child(const BoxNodeT& n, std::string_view fourcc) {
  const auto it = std::ranges::find_if(n.children, [&](const auto& c) { return c && c->fourcc == fourcc; });
  return it == n.children.end() ? nullptr : it->get();
}

BoxNodeT* child(BoxNodeT& n, std::string_view fourcc) {
  return const_cast<BoxNodeT*>(child(std::as_const(n), fourcc));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

std::vector<BoxNodeT*> kids(BoxNodeT& n, std::string_view fourcc) {
  std::vector<BoxNodeT*> out;
  for (auto& c : n.children) {
    if (c && c->fourcc == fourcc) {
      out.push_back(c.get());
    }
  }
  return out;
}

// The box reached by following `path` ("mdia/minf/stbl") from `n`, or null.
BoxNodeT* descend(BoxNodeT& n, std::string_view path) {
  BoxNodeT* cur = &n;
  while (cur != nullptr && !path.empty()) {
    const size_t slash = path.find('/');
    cur = child(*cur, path.substr(0, slash));
    path = slash == std::string_view::npos ? std::string_view{} : path.substr(slash + 1);
  }
  return cur;
}

// True for a top-level `moof` that only has its header walked (§8.2.4 rule 3).
bool is_lazy(const BoxNodeT& n) {
  return std::ranges::any_of(n.diagnostics, [](const auto& d) { return d->code == "CNT-0007"; });
}

std::string str(std::string_view v) {
  return std::string(v);
}
std::string num(uint64_t v) {
  return std::to_string(v);
}

struct Run {
  const io::RandomAccessFile& file;
  BoxNodes& top;
  BrandOptions options;
  std::set<std::string, std::less<>> brands;  // major and compatible brands of every ftyp and styp
  BoxNodeT* anchor = nullptr;  // the node brand-level findings go to: the first ftyp, else the first styp
  BoxNodeT* moov = nullptr;
  std::vector<BoxNodeT*> moofs;  // top-level, expanded
  std::vector<BoxNodeT*> sidxs;
  size_t added = 0;

  [[nodiscard]] bool has(std::string_view brand) const { return brands.contains(brand); }
  [[nodiscard]] bool has_any(std::initializer_list<std::string_view> list) const {
    return std::ranges::any_of(list, [&](std::string_view b) { return has(b); });
  }
  void flag(BoxNodeT& on, DiagCode code, std::string message) {
    added += add_finding(on, code, std::move(message)) ? 1U : 0U;
  }
  void flag_brand(DiagCode code, std::string message) {
    if (anchor != nullptr) {
      flag(*anchor, code, std::move(message));
    }
  }
  // The payload of `n` after its header, at most `cap` bytes.
  [[nodiscard]] std::vector<uint8_t> payload(const BoxNodeT& n, size_t cap = kMaxRead) const {
    if (!n.file_span) {
      return {};
    }
    const uint64_t begin = n.file_span->offset() + n.header_length;
    const uint64_t end = n.file_span->offset() + n.file_span->length();
    if (end < begin) {
      return {};
    }
    std::vector<uint8_t> v(static_cast<size_t>(std::min<uint64_t>(end - begin, cap)));
    v.resize(file.read(begin, v.size(), v.data()));
    return v;
  }
};

// ---- brand collection ---------------------------------------------------------------------------------------------

// major_brand and every compatible brand of an `ftyp`/`styp` payload; duplicates are dropped (writers pad the list).
std::vector<std::string> brands_of(std::span<const uint8_t> p) {
  std::vector<std::string> out;
  const auto add = [&](size_t at) {
    std::string b = fourcc_at(p, at);
    if (std::ranges::find(out, b) == out.end()) {
      out.push_back(std::move(b));
    }
  };
  if (p.size() >= 4) {
    add(0);
  }
  for (size_t at = 8; at + 4 <= p.size(); at += 4) {
    add(at);
  }
  return out;
}

void collect(Run& r) {
  for (auto& n : r.top) {
    if (!n) {
      continue;
    }
    if (n->fourcc == "ftyp" || n->fourcc == "styp") {
      for (std::string& b : brands_of(r.payload(*n, 4096))) {
        r.brands.insert(std::move(b));
      }
      if (r.anchor == nullptr || (r.anchor->fourcc == "styp" && n->fourcc == "ftyp")) {
        r.anchor = n.get();
      }
    } else if (n->fourcc == "moov" && r.moov == nullptr) {
      r.moov = n.get();
    } else if (n->fourcc == "moof" && !is_lazy(*n)) {
      r.moofs.push_back(n.get());
    } else if (n->fourcc == "sidx") {
      r.sidxs.push_back(n.get());
    }
  }
}

// ---- sample entries (rows without a brand) --------------------------------------------------------------------------

struct HvccArrays {
  bool vps = false, sps = false, pps = false;
  bool incomplete = false;  // an array with array_completeness == 0
};

HvccArrays parse_hvcc(std::span<const uint8_t> p) {
  HvccArrays a;
  if (p.size() < 23) {
    return a;
  }
  size_t at = 23;
  for (unsigned i = 0; i < p[22] && at + 3 <= p.size(); ++i) {
    const unsigned type = p[at] & 0x3FU;
    const bool complete = (p[at] & 0x80U) != 0;
    const unsigned count = be16(p, at + 1);
    at += 3;
    unsigned seen = 0;
    for (; seen < count && at + 2 <= p.size(); ++seen) {
      at += 2U + be16(p, at);
    }
    if (count > 0 && seen > 0) {
      a.vps = a.vps || type == 32;
      a.sps = a.sps || type == 33;
      a.pps = a.pps || type == 34;
    }
    a.incomplete = a.incomplete || !complete;
  }
  return a;
}

// Number of OBU_SEQUENCE_HEADER OBUs in av1C.configOBUs (AV1 §5.3.1: header byte, optional extension, leb128 size).
unsigned sequence_headers_in_config(std::span<const uint8_t> p) {
  unsigned found = 0;
  size_t at = 4;
  while (at < p.size()) {
    const uint8_t h = p[at++];
    const unsigned type = (h >> 3U) & 0xFU;
    at += (h & 0x4U) != 0 ? 1U : 0U;
    uint64_t size = p.size() - std::min(at, p.size());
    if ((h & 0x2U) != 0) {
      size = 0;
      for (unsigned i = 0; i < 8 && at < p.size(); ++i) {
        const uint8_t b = p[at++];
        size |= uint64_t{b & 0x7FU} << (7U * i);
        if ((b & 0x80U) == 0) {
          break;
        }
      }
    }
    found += type == 1 ? 1U : 0U;
    at += static_cast<size_t>(std::min<uint64_t>(size, p.size()));
  }
  return found;
}

bool is_video(const Run& r, BoxNodeT& trak) {
  const BoxNodeT* hdlr = descend(trak, "mdia/hdlr");
  if (hdlr == nullptr) {
    return false;
  }
  const std::vector<uint8_t> p = r.payload(*hdlr, 16);
  return fourcc_at(p, 8) == "vide";
}

void check_esds(Run& r, BoxNodeT& entry) {
  const BoxNodeT* wave = child(entry, "wave");
  if (child(entry, "esds") == nullptr && (wave == nullptr || child(*wave, "esds") == nullptr)) {
    r.flag(entry, DiagCode::CNT_7002, entry.fourcc + " sample entry without esds in an mp42 file");
  }
}

void check_avc(Run& r, BoxNodeT& entry) {
  const BoxNodeT* c = child(entry, "avcC");
  if (c == nullptr) {
    return;
  }
  const std::vector<uint8_t> p = r.payload(*c, 16);
  if (p.size() >= 6 && (p[5] & 0x1FU) == 0) {
    r.flag(entry, DiagCode::CNT_7003, entry.fourcc + " sample entry with numOfSequenceParameterSets == 0");
  }
}

void check_hevc(Run& r, BoxNodeT& entry) {
  const BoxNodeT* c = child(entry, "hvcC");
  if (c == nullptr) {
    return;
  }
  const HvccArrays a = parse_hvcc(r.payload(*c));
  std::string missing;
  const std::array<std::pair<bool, std::string_view>, 3> sets = {{{a.vps, "VPS"}, {a.sps, "SPS"}, {a.pps, "PPS"}}};
  for (const auto& [present, name] : sets) {
    if (!present) {
      missing += (missing.empty() ? "" : ", ") + str(name);
    }
  }
  if (!missing.empty()) {
    r.flag(entry, DiagCode::CNT_7005, "hvc1 sample entry: hvcC arrays lack " + missing);
  }
  if (a.incomplete) {
    r.flag(entry, DiagCode::CNT_7006, "hvc1 sample entry: hvcC has an array with array_completeness == 0");
  }
}

void check_av1(Run& r, BoxNodeT& entry) {
  const BoxNodeT* c = child(entry, "av1C");
  if (c == nullptr) {
    return;
  }
  if (const unsigned n = sequence_headers_in_config(r.payload(*c)); n != 1) {
    r.flag(entry, DiagCode::CNT_7007,
           "av1C.configOBUs holds " + num(n) + " OBU_SEQUENCE_HEADER (exactly one required)");
  }
}

// CMAF and HLS rows that look at the sample entry itself.
void check_entry_profiles(Run& r, BoxNodeT& entry, bool video) {
  const std::string_view cc = entry.fourcc;
  const bool in_band = cc == "avc3" || cc == "avc4" || cc == "hev1" || cc == "hev2";
  if (in_band && r.has("cmf2")) {
    r.flag(entry, DiagCode::CNT_7022, str(cc) + " sample entry (parameter sets in-band) in a cmf2 file");
  }
  if ((cc == "avc3" || cc == "hev1") && r.options.hls_context && r.has("cmfc")) {
    r.flag(entry, DiagCode::CNT_7041, str(cc) + " sample entry in an HLS cmfc segment (Apple requires avc1/hvc1)");
  }
  if (r.has_any({"cmfc", "cmf2"}) && video) {
    if (child(entry, "colr") == nullptr) {
      r.flag(entry, DiagCode::CNT_7018, "CMAF video sample entry " + str(cc) + " without colr");
    }
    if (child(entry, "btrt") == nullptr) {
      r.flag(entry, DiagCode::CNT_7019, "CMAF video sample entry " + str(cc) + " without btrt");
    }
  }
}

void check_entry(Run& r, BoxNodeT& entry, bool video) {
  const std::string_view cc = entry.fourcc;
  if ((cc == "mp4a" || cc == "mp4v") && r.has("mp42")) {
    check_esds(r, entry);
  } else if (cc == "avc1" || cc == "avc2") {
    check_avc(r, entry);
  } else if (cc == "hvc1") {
    check_hevc(r, entry);
  } else if (cc == "av01") {
    check_av1(r, entry);
  }
  check_entry_profiles(r, entry, video);
}

void check_sample_entries(Run& r) {
  if (r.moov == nullptr) {
    return;
  }
  for (BoxNodeT* trak : kids(*r.moov, "trak")) {
    BoxNodeT* stsd = descend(*trak, "mdia/minf/stbl/stsd");
    if (stsd == nullptr) {
      continue;
    }
    const bool video = is_video(r, *trak);
    for (auto& e : stsd->children) {
      if (e) {
        check_entry(r, *e, video);
      }
    }
  }
}

// ---- brand rows (kBrandChecks) --------------------------------------------------------------------------------------

// 14496-12 Annex E: the boxes of `iso5` and later.
void check_iso(Run& r, std::string_view /*brand*/) {
  bool uses_new = false;
  for (BoxNodeT* moof : r.moofs) {
    for (BoxNodeT* traf : kids(*moof, "traf")) {
      const BoxNodeT* tfhd = child(*traf, "tfhd");
      uses_new =
          uses_new ||
          (tfhd != nullptr && tfhd->flags >= 0 && (static_cast<uint32_t>(tfhd->flags) & kDefaultBaseIsMoof) != 0) ||
          child(*traf, "tfdt") != nullptr;
    }
  }
  // SPEC-ASSUMPTION(#797): every brand that is defined on top of iso5 (the CMAF and DASH brands) counts as declaring
  // it.
  constexpr std::array<std::string_view, 17> kDeclaring = {"iso5", "iso6", "iso7", "iso8", "iso9", "isoa",
                                                           "isob", "cmfc", "cmf2", "cmfs", "cmff", "cmfl",
                                                           "dash", "msdh", "msix", "dums", "lmsg"};
  const bool declared = std::ranges::any_of(kDeclaring, [&](std::string_view b) { return r.has(b); });
  if (uses_new && !declared) {
    r.flag_brand(DiagCode::CNT_7001,
                 "default-base-is-moof or tfdt used, but no iso5+, cmfc, dash or msdh brand declares it");
  }
}

std::vector<BoxNodeT*> traks_of(Run& r) {
  return r.moov == nullptr ? std::vector<BoxNodeT*>{} : kids(*r.moov, "trak");
}

uint32_t table_count(Run& r, BoxNodeT& stbl, std::string_view box, size_t at) {
  const BoxNodeT* t = child(stbl, box);
  return t == nullptr ? 0U : be32(r.payload(*t, 16), at);
}

// CNT-7013 and CNT-7017 for the `stbl` of one trak of a `cmfc` file.
void check_cmfc_tables(Run& r, BoxNodeT& trak) {
  BoxNodeT* stbl = descend(trak, "mdia/minf/stbl");
  if (stbl == nullptr) {
    return;
  }
  constexpr std::array<std::pair<std::string_view, size_t>, 6> kTables = {
      {{"stts", 4}, {"stsc", 4}, {"stsz", 8}, {"stz2", 8}, {"stco", 4}, {"co64", 4}}};
  for (const auto& [box, at] : kTables) {
    if (const uint32_t n = table_count(r, *stbl, box, at); n != 0) {
      r.flag_brand(DiagCode::CNT_7013, "cmfc file whose " + str(box) + " has " + num(n) + " entries");
    }
  }
  if (child(*stbl, "sdtp") != nullptr || child(*stbl, "stss") != nullptr) {
    r.flag_brand(DiagCode::CNT_7017, "cmfc file with sdtp or stss (not used by CMAF)");
  }
}

// CNT-7010..7013 and CNT-7017: the `moov` of a CMAF track file.
void check_cmfc_movie(Run& r) {
  if (r.moov == nullptr) {
    return;
  }
  const std::vector<BoxNodeT*> traks = traks_of(r);
  if (traks.size() != 1) {
    r.flag_brand(DiagCode::CNT_7010, "cmfc file with " + num(traks.size()) + " trak boxes (exactly one required)");
  }
  const BoxNodeT* mvex = child(*r.moov, "mvex");
  if (mvex == nullptr || child(*mvex, "trex") == nullptr) {
    r.flag_brand(DiagCode::CNT_7011, "cmfc file without mvex/trex");
  }
  if (const BoxNodeT* mvhd = child(*r.moov, "mvhd")) {
    const std::vector<uint8_t> p = r.payload(*mvhd, 32);
    const uint64_t duration = !p.empty() && p[0] == 1 ? be64(p, 24) : be32(p, 16);
    if (duration != 0 && (mvex == nullptr || child(*mvex, "mehd") == nullptr)) {
      r.flag_brand(DiagCode::CNT_7012, "cmfc file with mvhd.duration " + num(duration) + " and no mehd");
    }
  }
  for (BoxNodeT* trak : traks) {
    check_cmfc_tables(r, *trak);
  }
}

// CNT-7014..7016: the fragments of a CMAF track file.
void check_cmfc_fragments(Run& r) {
  unsigned k = 0;
  for (BoxNodeT* moof : r.moofs) {
    ++k;
    const std::vector<BoxNodeT*> trafs = kids(*moof, "traf");
    if (trafs.size() != 1) {
      r.flag_brand(DiagCode::CNT_7014,
                   "cmfc moof " + num(k) + " has " + num(trafs.size()) + " traf boxes (exactly one required)");
    }
    for (BoxNodeT* traf : trafs) {
      const BoxNodeT* tfhd = child(*traf, "tfhd");
      if (tfhd != nullptr && (tfhd->flags < 0 || (static_cast<uint32_t>(tfhd->flags) & kDefaultBaseIsMoof) == 0)) {
        r.flag_brand(DiagCode::CNT_7015, "cmfc tfhd of fragment " + num(k) + " without default-base-is-moof");
      }
      if (child(*traf, "tfdt") == nullptr) {
        r.flag_brand(DiagCode::CNT_7016, "cmfc traf of fragment " + num(k) + " without tfdt");
      }
    }
  }
}

// Rows of the `cmfc` brand (23000-19 §7), also run for `cmf2`.
void check_cmfc(Run& r, std::string_view /*brand*/) {
  check_cmfc_movie(r);
  check_cmfc_fragments(r);
}

void check_cmf2(Run& r, std::string_view brand) {
  check_cmfc(r, brand);
  for (BoxNodeT* trak : traks_of(r)) {
    if (const BoxNodeT* elst = descend(*trak, "edts/elst")) {
      const std::vector<uint8_t> p = r.payload(*elst);
      const bool wide = !p.empty() && p[0] == 1;
      const size_t entry = wide ? 20U : 12U;
      const uint32_t n = std::min<uint32_t>(be32(p, 4), static_cast<uint32_t>(p.size() / entry));
      for (uint32_t i = 0; i < n; ++i) {
        const size_t at = 8U + i * entry + (wide ? 8U : 4U);
        const int64_t media_time = wide ? static_cast<int64_t>(be64(p, at)) : static_cast<int32_t>(be32(p, at));
        if (media_time > 0) {
          r.flag_brand(DiagCode::CNT_7020, "cmf2 file with elst media_time " + std::to_string(media_time));
        }
      }
    }
    if (BoxNodeT* stbl = descend(*trak, "mdia/minf/stbl"); stbl != nullptr && child(*stbl, "cslg") != nullptr) {
      r.flag_brand(DiagCode::CNT_7021, "cmf2 file with cslg");
    }
  }
}

// cmfs, cmff and cmfl: segment, fragment and chunk structure (23000-19 §7.3.3). One pass over the top-level boxes.
void check_cmaf_segments(Run& r, std::string_view /*brand*/) {
  const bool chunk = r.has("cmfl");
  bool saw_moof = false;      // a moof since the last mdat
  bool saw_fragment = false;  // a moof or mdat since the last styp
  unsigned moofs_in_run = 0;  // moofs since the last styp
  for (auto& n : r.top) {
    if (!n) {
      continue;
    }
    const std::string_view cc = n->fourcc;
    if (cc == "styp") {
      saw_moof = saw_fragment = false;
      moofs_in_run = 0;
    } else if (cc == "moof") {
      saw_moof = saw_fragment = true;
      if (++moofs_in_run == 2 && chunk) {
        r.flag(*n, DiagCode::CNT_7024, "cmfl chunk with more than one moof");
      }
    } else if (cc == "mdat") {
      saw_moof = false;
      saw_fragment = true;
    } else if (cc == "sidx" && saw_fragment) {
      r.flag(*n, DiagCode::CNT_7024, "sidx is not at the start of the segment");
    } else if ((cc == "emsg" || cc == "prft") && saw_moof) {
      r.flag(*n, DiagCode::CNT_7025, str(cc) + " after the moof of the chunk");
    }
  }
  // SPEC-ASSUMPTION(#797): a file that declares cmfs and holds a moof needs at least one styp (the row says "leading").
  const bool has_styp = std::ranges::any_of(r.top, [](const auto& b) { return b && b->fourcc == "styp"; });
  if (r.has("cmfs") && !r.moofs.empty() && !has_styp) {
    r.flag_brand(DiagCode::CNT_7023, "cmfs segment without a styp box");
  }
}

void check_dash(Run& r, std::string_view /*brand*/) {
  if (r.moov == nullptr) {
    return;
  }
  const BoxNodeT* mvex = child(*r.moov, "mvex");
  if (mvex == nullptr) {
    r.flag_brand(DiagCode::CNT_7030, "dash brand but moov has no mvex");
  }
  for (BoxNodeT* trak : traks_of(r)) {
    BoxNodeT* stbl = descend(*trak, "mdia/minf/stbl");
    if (stbl == nullptr) {
      continue;
    }
    for (const char* box : {"stsz", "stz2"}) {
      if (const uint32_t n = table_count(r, *stbl, box, 8); n > 0) {
        r.flag_brand(DiagCode::CNT_7030, "dash brand but " + str(box) + ".sample_count is " + num(n));
      }
    }
  }
}

// Byte ranges [begin, end) that the sidx boxes of the file reference; `resolved` is false when one is hierarchical
// (a reference to another sidx), because then the nested boxes have to be followed.
struct SidxRanges {
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  bool resolved = true;
};

SidxRanges sidx_ranges(Run& r) {
  SidxRanges out;
  for (const BoxNodeT* s : r.sidxs) {
    const std::vector<uint8_t> p = r.payload(*s);
    const bool wide = !p.empty() && p[0] != 0;
    size_t at = 12;
    uint64_t first_offset = 0;
    if (wide) {
      first_offset = be64(p, at + 8);
      at += 16;
    } else {
      first_offset = be32(p, at + 4);
      at += 8;
    }
    at += 2;
    const uint16_t n = be16(p, at);
    at += 2;
    uint64_t pos = s->file_span->offset() + s->file_span->length() + first_offset;
    for (uint16_t i = 0; i < n && at + 12 <= p.size(); ++i, at += 12) {
      const uint32_t word = be32(p, at);
      const uint64_t size = word & 0x7FFFFFFFU;
      out.resolved = out.resolved && (word & 0x80000000U) == 0;
      out.ranges.emplace_back(pos, pos + size);
      pos += size;
    }
  }
  return out;
}

void check_msix(Run& r, std::string_view /*brand*/) {
  if (r.sidxs.empty()) {
    r.flag_brand(DiagCode::CNT_7031, "msix brand but the file has no sidx");
    return;
  }
  const SidxRanges refs = sidx_ranges(r);
  if (!refs.resolved) {
    return;  // a hierarchical index: the referenced sidx boxes would have to be followed first
  }
  unsigned reported = 0;
  for (BoxNodeT* moof : r.moofs) {
    const uint64_t at = moof->file_span->offset();
    const bool referenced =
        std::ranges::any_of(refs.ranges, [&](const auto& rg) { return at >= rg.first && at < rg.second; });
    if (!referenced && ++reported <= kMaxUnreferenced) {
      r.flag(*moof, DiagCode::CNT_7032, "moof at offset " + num(at) + " is not referenced by any sidx");
    }
  }
}

void check_lmsg(Run& r, std::string_view /*brand*/) {
  // `lmsg` marks the last segment: any styp but the last one that carries it is wrong.
  std::vector<BoxNodeT*> styps;
  for (auto& n : r.top) {
    if (n && n->fourcc == "styp") {
      styps.push_back(n.get());
    }
  }
  for (size_t i = 0; i + 1 < styps.size(); ++i) {
    const std::vector<std::string> b = brands_of(r.payload(*styps[i], 4096));
    if (std::ranges::find(b, "lmsg") != b.end()) {
      r.flag(*styps[i], DiagCode::CNT_7033, "lmsg brand on segment " + num(i + 1) + ", which is not the last");
    }
  }
}

void check_itunes(Run& r, std::string_view brand) {
  const auto has_ilst = [&](BoxNodeT& root) {
    return descend(root, "udta/meta/ilst") != nullptr || descend(root, "meta/ilst") != nullptr;
  };
  const bool found = (r.moov != nullptr && has_ilst(*r.moov)) || std::ranges::any_of(r.top, [&](auto& n) {
                       return n && n->fourcc == "meta" && child(*n, "ilst") != nullptr;
                     });
  if (!found) {
    r.flag_brand(DiagCode::CNT_7050, "iTunes brand " + str(brand) + " without ilst");
  }
}

void check_mif1(Run& r, std::string_view /*brand*/) {
  BoxNodeT* meta = nullptr;
  for (auto& n : r.top) {
    if (n && n->fourcc == "meta") {
      meta = n.get();
      break;
    }
  }
  if (meta == nullptr) {
    r.flag_brand(DiagCode::CNT_8010, "mif1 file without a top-level meta box");
    return;
  }
  const BoxNodeT* hdlr = child(*meta, "hdlr");
  const std::vector<uint8_t> p = hdlr == nullptr ? std::vector<uint8_t>{} : r.payload(*hdlr, 16);
  if (fourcc_at(p, 8) != "pict") {
    r.flag(*meta, DiagCode::CNT_8010, "mif1 file: meta lacks hdlr with handler_type pict");
  }
  for (const char* box : {"pitm", "iinf", "iloc", "iprp"}) {
    if (child(*meta, box) == nullptr) {
      r.flag(*meta, DiagCode::CNT_8010, "mif1 file: meta lacks " + str(box));
    }
  }
}

void check_hls(Run& r) {
  if (!r.options.hls_context) {
    return;
  }
  unsigned k = 0;
  for (BoxNodeT* moof : r.moofs) {
    ++k;
    for (BoxNodeT* traf : kids(*moof, "traf")) {
      if (child(*traf, "tfdt") == nullptr) {
        r.flag(*moof, DiagCode::CNT_7040, "HLS fMP4 segment: traf of fragment " + num(k) + " without tfdt");
      }
    }
  }
}

// §8.4.5: each row maps brands (comma-separated) to the function that evaluates them. The `isom`..`isob` rows
// (CNT-7001) run for every file with an `ftyp`, whatever its brands, so they are called from check_brands().
struct BrandCheck {
  std::string_view brands;
  void (*run)(Run&, std::string_view brand);
};

constexpr std::array kBrandChecks = {
    BrandCheck{"cmfc", &check_cmfc},
    BrandCheck{"cmf2", &check_cmf2},
    BrandCheck{"cmfs,cmff,cmfl", &check_cmaf_segments},
    BrandCheck{"dash", &check_dash},
    BrandCheck{"msix", &check_msix},
    BrandCheck{"lmsg", &check_lmsg},
    BrandCheck{"M4A ,M4V ,M4P ,M4B ", &check_itunes},
    BrandCheck{"mif1", &check_mif1},
};

// First brand of the comma-separated `list` that the file declares, or empty.
std::string declared(const Run& r, std::string_view list) {
  while (!list.empty()) {
    const size_t comma = list.find(',');
    const std::string_view one = list.substr(0, comma);
    if (r.has(one)) {
      return str(one);
    }
    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
  }
  return {};
}

}  // namespace

size_t check_brands(const io::RandomAccessFile& file, BoxNodes& top_level, ValidationLevel level,
                    const BrandOptions& options) {
  if (level == ValidationLevel::Quick) {
    return 0;
  }
  Run r{file, top_level, options, {}, nullptr, nullptr, {}, {}, 0};
  collect(r);
  check_sample_entries(r);
  check_hls(r);
  if (r.anchor != nullptr && r.anchor->fourcc == "ftyp") {
    check_iso(r, "isom");
  }
  for (const BrandCheck& row : kBrandChecks) {
    if (const std::string brand = declared(r, row.brands); !brand.empty()) {
      row.run(r, brand);
    }
  }
  // The `mp41`/`mp42` row (CNT-7002) runs on the sample entries, and the `msdh` row has no diagnostic code of its own.
  return r.added;
}

}  // namespace ovt::container::isobmff
