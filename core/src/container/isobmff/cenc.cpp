// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The CENC per-sample encryption map (chapter 08 §8.4.4, WI-1.221). See cenc.h.

#include "container/isobmff/cenc.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/decoders/pssh_systems.h"
#include "container/isobmff/sample_table.h"

namespace ovt::container::isobmff {

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint64_t kMaxPayload = uint64_t{256} << 20U;  // largest box payload read (a senc of 16 Mi samples)
constexpr uint64_t kCancelEvery = 4096;                 // samples between two cancel checks
constexpr uint64_t kBlock = 16;                         // AES block
constexpr size_t kMaxNalWalk = size_t{1} << 20U;        // NAL length prefixes read per sample for CNT-6007
constexpr uint32_t kMaxGroupEntries = 1U << 20U;

// ---- reading -------------------------------------------------------------------------------------------------

// Big-endian reads that never throw: a read past the end sets ok() false and returns zero.
class Cursor {
 public:
  explicit Cursor(std::span<const uint8_t> d) : d_(d) {}
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] size_t pos() const noexcept { return pos_; }
  [[nodiscard]] size_t remaining() const noexcept { return d_.size() - pos_; }
  void seek(size_t p) noexcept {
    if (p > d_.size()) {
      ok_ = false;
    } else {
      pos_ = p;
    }
  }
  uint64_t be(size_t n) noexcept {
    if (!ok_ || remaining() < n) {
      ok_ = false;
      return 0;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) {
      v = (v << 8U) | d_[pos_++];
    }
    return v;
  }
  uint8_t u8() noexcept { return static_cast<uint8_t>(be(1)); }
  uint16_t u16() noexcept { return static_cast<uint16_t>(be(2)); }
  uint32_t u32() noexcept { return static_cast<uint32_t>(be(4)); }
  uint64_t u64() noexcept { return be(8); }
  Bytes take(size_t n) {
    if (!ok_ || remaining() < n) {
      ok_ = false;
      return {};
    }
    Bytes v(d_.begin() + static_cast<std::ptrdiff_t>(pos_), d_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
    pos_ += n;
    return v;
  }
  std::string four() {
    const Bytes b = take(4);
    return {b.begin(), b.end()};
  }

 private:
  std::span<const uint8_t> d_;
  size_t pos_ = 0;
  bool ok_ = true;
};

// One box header read straight from the file (the sgpd that a GroupMembership points at).
struct Hdr {
  std::string type;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t header = 8;
  [[nodiscard]] uint64_t end() const noexcept { return offset + size; }
  [[nodiscard]] uint64_t payload() const noexcept { return offset + header; }
};

std::optional<Hdr> read_hdr(const io::RandomAccessFile& file, uint64_t pos, uint64_t end) {
  std::array<uint8_t, 16> b{};
  if (end <= pos || end - pos < 8 || file.read(pos, 8, b.data()) != 8) {
    return std::nullopt;
  }
  Cursor c(std::span<const uint8_t>(b).first(8));
  const uint64_t size32 = c.u32();
  Hdr h{c.four(), pos, size32, 8};
  if (size32 == 1) {
    if (end - pos < 16 || file.read(pos + 8, 8, b.data() + 8) != 8) {
      return std::nullopt;
    }
    Cursor w(std::span<const uint8_t>(b).subspan(8, 8));
    h.size = w.u64();
    h.header = 16;
  } else if (size32 == 0) {
    h.size = end - pos;
  }
  if (h.type == "uuid") {
    h.header += 16;
  }
  if (h.size < h.header || h.size > end - pos) {
    return std::nullopt;
  }
  return h;
}

Bytes payload(const io::RandomAccessFile& file, const Hdr& h) {
  Bytes v(std::min(h.size - h.header, kMaxPayload));
  v.resize(file.read(h.payload(), v.size(), v.data()));
  return v;
}

const fb::BoxNodeT* child(const fb::BoxNodeT* n, std::string_view type) {
  if (n == nullptr) {
    return nullptr;
  }
  const auto it = std::ranges::find_if(n->children, [&](const auto& c) { return c && c->fourcc == type; });
  return it == n->children.end() ? nullptr : it->get();
}

Bytes payload(const io::RandomAccessFile& file, const fb::BoxNodeT* n) {
  if (n == nullptr || !n->file_span || n->file_span->length() < n->header_length) {
    return {};
  }
  Bytes v(std::min<uint64_t>(n->file_span->length() - n->header_length, kMaxPayload));
  v.resize(file.read(n->file_span->offset() + n->header_length, v.size(), v.data()));
  return v;
}

uint64_t offset_of(const fb::BoxNodeT* n) {
  return n != nullptr && n->file_span ? n->file_span->offset() : 0;
}

// ---- CENC parameters of a sample (tenc, seig) -----------------------------------------------------------------

struct Params {
  bool is_protected = false;
  bool iv_known = false;  // false: no tenc or seig entry gave the IV size
  uint8_t iv_size = 0;
  uint8_t crypt = 0;
  uint8_t skip = 0;
  Bytes kid;
  Bytes constant_iv;
};

// The fields `tenc` and a `seig` entry share (23001-7 §8.2, §6): reserved, pattern byte, isProtected, IV size, KID and,
// for a protected sample without per-sample IV, the constant IV. `tenc` v0 has no pattern.
bool parse_params(Cursor& c, bool use_pattern, Params& p) {
  c.u8();
  const uint8_t pattern = c.u8();
  p.is_protected = c.u8() == 1;
  p.iv_size = c.u8();
  p.kid = c.take(16);
  if (use_pattern) {
    p.crypt = static_cast<uint8_t>(pattern >> 4U);
    p.skip = pattern & 0xFU;
  }
  if (p.is_protected && p.iv_size == 0) {
    p.constant_iv = c.take(c.u8());
  }
  p.iv_known = c.ok();
  return c.ok();
}

// The entries of a `seig` sgpd (14496-12 §8.9.3): v0 entries are self-delimiting, v1 with default_length 0 carries a
// length per entry, v2 adds default_sample_description_index (not applied, #808).
std::vector<Params> parse_seig_sgpd(std::span<const uint8_t> bytes) {
  Cursor c(bytes);
  const uint8_t version = c.u8();
  c.take(3);
  std::vector<Params> out;
  if (c.four() != "seig") {
    return out;
  }
  const uint32_t default_length = version >= 1 ? c.u32() : 0;
  if (version >= 2) {
    c.u32();
  }
  const uint32_t n = std::min(c.u32(), kMaxGroupEntries);
  for (uint32_t i = 0; i < n && c.ok(); ++i) {
    const uint32_t length = version == 1 && default_length == 0 ? c.u32() : default_length;
    const size_t start = c.pos();
    Params p;
    if (!parse_params(c, true, p)) {
      break;
    }
    if (length != 0) {
      c.seek(start + length);
    }
    out.push_back(std::move(p));
  }
  return out;
}

// What one sample entry says about its protection (`sinf`).
struct EntryInfo {
  bool encrypted = false;
  std::string scheme;
  std::string original_format;
  uint32_t scheme_version = 0;
  bool has_tenc = false;
  Params defaults;
  uint8_t nal_length_size = 0;  // avcC / hvcC lengthSizeMinusOne + 1; 0 for other codecs
};

EntryInfo read_entry(const io::RandomAccessFile& file, const fb::BoxNodeT& entry) {
  EntryInfo e;
  if (const fb::BoxNodeT* avcc = child(&entry, "avcC")) {
    const Bytes p = payload(file, avcc);
    e.nal_length_size = p.size() > 4 ? static_cast<uint8_t>((p[4] & 3U) + 1U) : 0;
  } else if (const fb::BoxNodeT* hvcc = child(&entry, "hvcC")) {
    const Bytes p = payload(file, hvcc);
    e.nal_length_size = p.size() > 21 ? static_cast<uint8_t>((p[21] & 3U) + 1U) : 0;
  }
  // A PIFF-signalled entry carries two `sinf` (piff and cenc); the CENC-family one is the one with the tenc.
  const fb::BoxNodeT* sinf = nullptr;
  for (const auto& c : entry.children) {
    if (!c || c->fourcc != "sinf") {
      continue;
    }
    const Bytes schm = payload(file, child(c.get(), "schm"));
    const std::string type = schm.size() >= 8 ? std::string(schm.begin() + 4, schm.begin() + 8) : std::string{};
    if (sinf == nullptr || type == "cenc" || type == "cens" || type == "cbc1" || type == "cbcs") {
      sinf = c.get();
    }
  }
  if (sinf == nullptr) {
    return e;
  }
  e.encrypted = true;
  e.original_format = entry.fourcc;
  if (const Bytes p = payload(file, child(sinf, "frma")); p.size() >= 4) {
    e.original_format.assign(p.begin(), p.begin() + 4);
  }
  if (const Bytes p = payload(file, child(sinf, "schm")); !p.empty()) {
    Cursor c(p);
    c.take(4);
    e.scheme = c.four();
    e.scheme_version = c.u32();
  }
  if (const Bytes p = payload(file, child(child(sinf, "schi"), "tenc")); !p.empty()) {
    Cursor c(p);
    const uint8_t version = c.u8();
    c.take(3);
    e.has_tenc = parse_params(c, version >= 1, e.defaults);
  }
  return e;
}

// ---- auxiliary information (saiz / saio) -----------------------------------------------------------------------

bool aux_type_ok(Cursor& c, uint32_t flags) {
  if ((flags & 1U) == 0) {
    return true;
  }
  const std::string type = c.four();
  c.u32();
  return type == "cenc" || type == "cens" || type == "cbc1" || type == "cbcs";
}

struct Saiz {
  uint8_t default_size = 0;
  uint32_t count = 0;
  Bytes sizes;  // per sample when default_size == 0
  [[nodiscard]] uint32_t size_of(uint64_t i) const { return default_size != 0 ? default_size : sizes[i]; }
};

std::optional<Saiz> parse_saiz(std::span<const uint8_t> bytes) {
  Cursor c(bytes);
  const uint32_t flags = c.u32() & 0xFFFFFFU;
  if (!aux_type_ok(c, flags)) {
    return std::nullopt;
  }
  Saiz s;
  s.default_size = c.u8();
  s.count = c.u32();
  if (s.default_size == 0) {
    s.sizes = c.take(std::min<size_t>(s.count, c.remaining()));
    s.count = static_cast<uint32_t>(s.sizes.size());
  }
  return c.ok() ? std::optional<Saiz>(std::move(s)) : std::nullopt;
}

std::optional<std::vector<uint64_t>> parse_saio(std::span<const uint8_t> bytes) {
  Cursor c(bytes);
  const uint32_t word = c.u32();
  if (!aux_type_ok(c, word & 0xFFFFFFU)) {
    return std::nullopt;
  }
  const bool wide = (word >> 24U) != 0;
  const uint32_t n = c.u32();
  std::vector<uint64_t> out;
  for (uint32_t i = 0; i < n && c.ok() && c.remaining() >= (wide ? 8U : 4U); ++i) {
    out.push_back(wide ? c.u64() : c.u32());
  }
  return c.ok() ? std::optional(std::move(out)) : std::nullopt;
}

// Where the auxiliary data of the samples of one run set starts: `offsets[r]` is the absolute offset of run r, which
// begins at sample `run_first[r]` (numbered as in `saiz`); the data of a sample follows the sizes of the samples before
// it in its run (14496-12 §8.7.8, §8.7.9).
class AuxLocator {
 public:
  AuxLocator(Saiz saiz, std::vector<uint64_t> offsets, std::vector<uint64_t> run_first)
      : saiz_(std::move(saiz)), offsets_(std::move(offsets)), run_first_(std::move(run_first)) {}

  struct Where {
    uint64_t offset;
    uint32_t size;
  };

  // Sample `i` of the saiz numbering; nullopt when `saiz` does not cover it or an offset overflows.
  [[nodiscard]] std::optional<Where> locate(uint64_t i) {
    if (i >= saiz_.count || offsets_.empty() || run_first_.size() != offsets_.size()) {
      return std::nullopt;
    }
    const auto it = std::ranges::upper_bound(run_first_, i);
    if (it == run_first_.begin()) {
      return std::nullopt;
    }
    const size_t run = static_cast<size_t>(it - run_first_.begin()) - 1;
    if (run != run_ || i < at_) {  // forward-only cache, restarted at the first sample of the run
      run_ = run;
      at_ = run_first_[run];
      sum_ = 0;
    }
    for (; at_ < i; ++at_) {
      sum_ += saiz_.size_of(at_);
    }
    uint64_t start = 0;
    if (__builtin_add_overflow(offsets_[run], sum_, &start)) {
      return std::nullopt;
    }
    return Where{start, saiz_.size_of(i)};
  }

 private:
  Saiz saiz_;
  std::vector<uint64_t> offsets_;
  std::vector<uint64_t> run_first_;
  size_t run_ = SIZE_MAX;
  uint64_t at_ = 0;
  uint64_t sum_ = 0;
};

// ---- one sample's record as written in senc / aux data --------------------------------------------------------

struct Raw {
  Bytes iv;
  std::vector<std::pair<uint16_t, uint32_t>> subs;
  uint64_t offset = 0;  // file offset of the record
  bool ok = false;
};

// `iv_size` bytes of IV and, when `with_subs`, a u16 count and that many (clear u16, protected u32) pairs.
bool parse_record(Cursor& c, uint8_t iv_size, bool with_subs, Raw& r) {
  r.iv = c.take(iv_size);
  if (with_subs) {
    const uint16_t n = c.u16();
    for (uint16_t i = 0; i < n && c.ok(); ++i) {
      const uint16_t clear = c.u16();
      const uint32_t prot = c.u32();
      r.subs.emplace_back(clear, prot);
    }
  }
  return c.ok();
}

// ---- the builder ----------------------------------------------------------------------------------------------

class Builder {
 public:
  Builder(const io::RandomAccessFile& file, const BoxNodes& top_level, size_t trak_ordinal,
          const fb::SampleTableT& table, jobs::Cancel& cancel)
      : file_(file),
        top_level_(top_level),
        trak_(find_trak(top_level, trak_ordinal)),
        table_(table),
        cancel_(cancel),
        info_(std::make_unique<fb::EncryptionInfoT>()) {}

  std::unique_ptr<fb::EncryptionInfoT> run(uint64_t first, uint64_t count) {
    info_->track_id = table_.track_id;
    read_entries();
    count_moov_samples();
    if (!track_encrypted()) {
      return std::move(info_);
    }
    fill_track();
    load_groups();
    load_moov_aux();
    const uint64_t total = moov_count_;
    const uint64_t last = count == 0 || count > total - std::min(first, total) ? total : first + count;
    for (uint64_t i = first; i < last; ++i) {
      if (i % kCancelEvery == 0) {
        cancel_.throw_if_requested();
      }
      info_->samples.push_back(one(i));
    }
    summarise();
    return std::move(info_);
  }

 private:
  const io::RandomAccessFile& file_;
  const BoxNodes& top_level_;
  const fb::BoxNodeT* trak_;
  const fb::SampleTableT& table_;
  jobs::Cancel& cancel_;
  std::unique_ptr<fb::EncryptionInfoT> info_;
  std::vector<EntryInfo> entries_;
  const fb::GroupMembershipT* seig_group_ = nullptr;
  std::vector<Params> global_seig_;
  uint64_t moov_count_ = 0;
  std::optional<AuxLocator> moov_aux_;
  bool moov_has_source_ = false;
  std::map<std::string, uint64_t> found_;

  void diag(std::string_view code, std::string message, uint64_t offset) {
    if (++found_[std::string(code)] > kMaxListedCencFindings) {
      return;
    }
    add_diag(code, std::move(message), offset);
  }

  void add_diag(std::string_view code, std::string message, uint64_t offset) {
    const DiagInfo* di = ovt::container::diag(code);
    auto d = std::make_unique<fb::DiagnosticT>();
    d->severity = di != nullptr ? static_cast<fb::Severity>(di->severity) : fb::Severity::Error;
    d->code = std::string(code);
    d->message = std::move(message);
    d->spec_ref = di != nullptr ? std::string(di->spec_ref) : std::string{};
    d->span = std::make_unique<fb::BitSpan>(offset * 8U, 0U);
    info_->diagnostics.push_back(std::move(d));
  }

  // One summary line per code that had more findings than are listed.
  void summarise() {
    for (const auto& [code, n] : found_) {
      if (n > kMaxListedCencFindings) {
        add_diag(code, std::to_string(n - kMaxListedCencFindings) + " more " + code + " findings are not listed", 0);
      }
    }
  }

  void read_entries() {
    const fb::BoxNodeT* node = trak_;
    for (const char* name : {"mdia", "minf", "stbl", "stsd"}) {
      node = child(node, name);
    }
    if (node == nullptr) {
      return;
    }
    for (const auto& e : node->children) {
      if (e) {
        entries_.push_back(read_entry(file_, *e));
      }
    }
  }

  // The samples of the moov part: the table's leading samples without a fragment (WI-1.221b adds the fragment part).
  void count_moov_samples() {
    moov_count_ = table_.dts.size();
    const auto it = std::ranges::find_if(table_.fragment_index, [](int32_t f) { return f >= 0; });
    if (it != table_.fragment_index.end()) {
      moov_count_ = static_cast<uint64_t>(it - table_.fragment_index.begin());
    }
  }

  [[nodiscard]] bool track_encrypted() const {
    return std::ranges::any_of(entries_, [](const EntryInfo& e) { return e.encrypted; });
  }

  [[nodiscard]] const EntryInfo* entry_of(uint64_t i) const {
    if (entries_.empty()) {
      return nullptr;
    }
    const size_t sdi = i < table_.sample_description_index.size() ? table_.sample_description_index[i] : 1;
    return &entries_[sdi >= 1 && sdi <= entries_.size() ? sdi - 1 : 0];
  }

  void fill_track() {
    const auto it = std::ranges::find_if(entries_, [](const EntryInfo& e) { return e.encrypted; });
    if (it != entries_.end()) {
      info_->scheme_type = it->scheme;
      info_->scheme_version = it->scheme_version;
      info_->original_format = it->original_format;
      info_->default_kid = it->defaults.kid;
      info_->default_iv_size = it->defaults.iv_size;
      info_->default_is_protected = it->defaults.is_protected;
      info_->default_constant_iv = it->defaults.constant_iv;
      info_->pattern_crypt = it->defaults.crypt;
      info_->pattern_skip = it->defaults.skip;
      if (it->scheme == "cbcs" && it->has_tenc && it->defaults.iv_size != 0) {
        diag("CNT-6008",
             "cbcs track " + std::to_string(table_.track_id) + " has per-sample IVs of " +
                 std::to_string(it->defaults.iv_size) + " bytes",
             0);
      }
    }
    read_pssh();
  }

  // The `pssh` boxes of the first `moov` (the ones of moofs are not listed, #808).
  void read_pssh() {
    const auto moov = std::ranges::find_if(top_level_, [](const auto& b) { return b && b->fourcc == "moov"; });
    if (moov == top_level_.end()) {
      return;
    }
    for (const auto& b : (*moov)->children) {
      if (!b || b->fourcc != "pssh") {
        continue;
      }
      const Bytes p = payload(file_, b.get());
      Cursor c(p);
      const uint8_t version = c.u8();
      c.take(3);
      auto info = std::make_unique<fb::PsshInfoT>();
      info->box_offset = offset_of(b.get());
      info->system_id = c.take(16);
      info->version = version;
      if (version >= 1) {
        info->kid_count = c.u32();
        info->kids = c.take(std::min<size_t>(size_t{info->kid_count} * 16U, c.remaining()));
      }
      info->data_size = c.u32();
      if (!c.ok()) {
        continue;  // the pssh decoder reports the truncation (CNT-6004)
      }
      info->system_name = std::string(pssh_system_name(std::span<const uint8_t, 16>(info->system_id)));
      info->location = "moov";
      info_->pssh.push_back(std::move(info));
    }
  }

  void load_groups() {
    for (const auto& g : table_.groups) {
      if (g && g->grouping_type == "seig") {
        seig_group_ = g.get();
        break;
      }
    }
    if (seig_group_ != nullptr && seig_group_->sgpd_offset != 0) {
      if (const std::optional<Hdr> h = read_hdr(file_, seig_group_->sgpd_offset, file_.size())) {
        global_seig_ = parse_seig_sgpd(payload(file_, *h));
      }
    }
    const bool any_seig = seig_group_ != nullptr &&
                          std::ranges::any_of(seig_group_->description_index, [](uint32_t d) { return d != 0; });
    const auto it = std::ranges::find_if(entries_, [](const EntryInfo& e) { return e.encrypted; });
    if (it != entries_.end() && it->has_tenc && !it->defaults.is_protected && !any_seig) {
      diag("CNT-6006", "every sample of track " + std::to_string(table_.track_id) + " is not protected", 0);
    }
  }

  void load_moov_aux() {
    const fb::BoxNodeT* stbl = child(child(child(trak_, "mdia"), "minf"), "stbl");
    if (stbl == nullptr || moov_count_ == 0) {
      return;
    }
    std::optional<Saiz> saiz;
    std::optional<std::vector<uint64_t>> saio;
    for (const auto& b : stbl->children) {
      if (!b || (b->fourcc != "saiz" && b->fourcc != "saio")) {
        continue;
      }
      if (b->fourcc == "saiz" && !saiz) {
        saiz = parse_saiz(payload(file_, b.get()));
      } else if (b->fourcc == "saio" && !saio) {
        saio = parse_saio(payload(file_, b.get()));
      }
    }
    if (!saiz || !saio || saio->empty()) {
      return;
    }
    moov_has_source_ = true;
    // One saio entry: a single run. Otherwise one entry per chunk (§8.7.9), which the stsc-derived chunk numbers give.
    std::vector<uint64_t> run_first{0};
    if (saio->size() > 1) {
      for (uint64_t i = 1; i < moov_count_ && i < table_.chunk_index.size(); ++i) {
        if (table_.chunk_index[i] != table_.chunk_index[i - 1]) {
          run_first.push_back(i);
        }
      }
    }
    if (run_first.size() != saio->size()) {
      diag("CNT-6015",
           "saio has " + std::to_string(saio->size()) + " entries for " + std::to_string(run_first.size()) +
               " chunks; the auxiliary data of track " + std::to_string(table_.track_id) + " is not located",
           0);
      return;
    }
    moov_aux_.emplace(std::move(*saiz), std::move(*saio), std::move(run_first));
  }

  // §8.4.4 step 1: the seig entry of the sample, else the tenc defaults of its sample entry.
  [[nodiscard]] Params params_for(uint64_t i) const {
    const EntryInfo* e = entry_of(i);
    Params p = e != nullptr ? e->defaults : Params{};
    if (e == nullptr || !e->has_tenc) {
      p.is_protected = true;  // senc or aux data without tenc: an encrypted sample of unknown IV size
      p.iv_known = false;
    }
    if (seig_group_ == nullptr || i >= seig_group_->description_index.size() ||
        seig_group_->description_index[i] == 0) {
      return p;
    }
    const uint32_t index = seig_group_->description_index[i];
    return index - 1 < global_seig_.size() ? global_seig_[index - 1] : p;
  }

  Raw fetch_moov(uint64_t i, const Params& p) {
    if (moov_aux_) {
      if (const std::optional<AuxLocator::Where> w = moov_aux_->locate(i)) {
        return read_aux(*w, p);
      }
    }
    return {};
  }

  // A record of saiz-sized auxiliary data: the IV, then subsamples when the size says there is more.
  Raw read_aux(const AuxLocator::Where& w, const Params& p) {
    Raw r;
    r.offset = w.offset;
    Bytes b(w.size);
    b.resize(file_.read(w.offset, b.size(), b.data()));
    if (b.size() != w.size) {
      return {};
    }
    const uint8_t iv_size = p.iv_known ? p.iv_size : (w.size <= 16 ? static_cast<uint8_t>(w.size) : uint8_t{8});
    Cursor c(b);
    r.ok = parse_record(c, iv_size, w.size > iv_size, r);
    return r;
  }

  std::unique_ptr<fb::SampleEncryptionT> one(uint64_t i) {
    const Params p = params_for(i);
    auto rec = std::make_unique<fb::SampleEncryptionT>();
    rec->sample_index = i;
    rec->is_protected = p.is_protected;
    rec->kid = p.kid;
    rec->scheme = info_->scheme_type;
    rec->crypt_byte_block = p.crypt;
    rec->skip_byte_block = p.skip;
    const Raw raw = fetch_moov(i, p);
    if (!p.is_protected) {
      return rec;
    }
    const bool needs_record = !p.iv_known || p.iv_size != 0;
    if (!raw.ok && (needs_record || moov_has_source_)) {
      diag("CNT-6015",
           "no encryption record for sample " + std::to_string(i) + " of track " + std::to_string(table_.track_id),
           i < table_.offset.size() ? table_.offset[i] : 0);
      if (needs_record) {
        rec->is_protected = false;  // #808: without its record the sample is reported as not protected
        return rec;
      }
    }
    rec->senc_entry_offset = raw.offset;
    rec->iv = p.iv_known && p.iv_size == 0 ? p.constant_iv : raw.iv;
    for (const auto& [clear, prot] : raw.subs) {
      auto s = std::make_unique<fb::SubsampleRangeT>();
      s->clear_bytes = clear;
      s->protected_bytes = prot;
      rec->subsamples.push_back(std::move(s));
    }
    check_sum(i, *rec);
    check_nal_headers(i, *rec, p);
    return rec;
  }

  void check_sum(uint64_t i, const fb::SampleEncryptionT& rec) {
    if (rec.subsamples.empty() || i >= table_.size.size()) {
      return;
    }
    uint64_t total = 0;
    for (const auto& s : rec.subsamples) {
      total += uint64_t{s->clear_bytes} + s->protected_bytes;
    }
    if (total != table_.size[i]) {
      diag("CNT-6002",
           "subsamples of sample " + std::to_string(i) + " cover " + std::to_string(total) + " bytes, the sample has " +
               std::to_string(table_.size[i]),
           rec.senc_entry_offset);
    }
  }

  // §8.4.4 step 5: a subsample that starts at a NAL unit must leave the length field and the NAL header clear.
  void check_nal_headers(uint64_t i, const fb::SampleEncryptionT& rec, const Params& p) {
    const EntryInfo* e = entry_of(i);
    if (e == nullptr || e->nal_length_size == 0 || rec.subsamples.empty() || i >= table_.size.size()) {
      return;
    }
    const uint64_t size = table_.size[i];
    uint64_t total = 0;
    for (const auto& s : rec.subsamples) {
      total += uint64_t{s->clear_bytes} + s->protected_bytes;
    }
    if (total != size) {
      return;  // CNT-6002 already; the subsample starts mean nothing
    }
    const std::vector<uint64_t> nals = nal_starts(table_.offset[i], size, e->nal_length_size);
    const bool pattern = p.crypt != 0 || p.skip != 0;
    uint64_t start = 0;
    for (const auto& s : rec.subsamples) {
      const auto at = std::ranges::lower_bound(nals, start);
      const bool starts_nal = at != nals.end() && *at == start;
      const bool legal_zero = pattern && s->clear_bytes == 0;
      if (starts_nal && s->clear_bytes < e->nal_length_size + 1U && !legal_zero) {
        diag("CNT-6007",
             "subsample protects NAL length/header bytes of NAL " + std::to_string(at - nals.begin()) + " of sample " +
                 std::to_string(i),
             table_.offset[i] + start);
      }
      start += uint64_t{s->clear_bytes} + s->protected_bytes;
    }
  }

  // The offsets inside the sample at which a length-prefixed NAL unit starts.
  [[nodiscard]] std::vector<uint64_t> nal_starts(uint64_t sample_offset, uint64_t size, uint8_t length_size) const {
    std::vector<uint64_t> out;
    uint64_t pos = 0;
    while (pos + length_size <= size && out.size() < kMaxNalWalk) {
      std::array<uint8_t, 4> b{};
      if (file_.read(sample_offset + pos, length_size, b.data()) != length_size) {
        break;
      }
      uint64_t len = 0;
      for (uint8_t k = 0; k < length_size; ++k) {
        len = (len << 8U) | b[k];
      }
      out.push_back(pos);
      pos += length_size + len;
    }
    return out;
  }
};

}  // namespace

std::unique_ptr<fb::EncryptionInfoT> build_encryption_map(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                                          size_t trak_ordinal, const fb::SampleTableT& table,
                                                          uint64_t first, uint64_t count, jobs::Cancel& cancel) {
  Builder b(file, top_level, trak_ordinal, table, cancel);
  return b.run(first, count);
}

namespace {

// Appends [offset, offset + length) of kind `protect`, keeping the clear / protected alternation.
void add_range(std::vector<syntax::ByteRange>& out, bool protect, uint64_t offset, uint64_t length) {
  if (length == 0) {
    return;
  }
  // Even indices are clear, odd ones protected: the last entry is protected when the size is even.
  if (!out.empty() && (out.size() % 2 == 0) == protect) {
    out.back().length += length;  // the ranges of a sample are added in file order, so this one is adjacent
    return;
  }
  if ((out.size() % 2 == 1) != protect) {
    out.push_back({offset, 0});  // an empty range of the other kind keeps the alternation
  }
  out.push_back({offset, length});
}

}  // namespace

std::vector<syntax::ByteRange> sample_clear_protected_ranges(const fb::SampleEncryptionT& sample,
                                                             uint64_t sample_offset, uint32_t sample_size) {
  std::vector<syntax::ByteRange> out;
  if (!sample.is_protected) {
    add_range(out, false, sample_offset, sample_size);
    return out;
  }
  const bool partial_clear = sample.scheme == "cbc1" || sample.scheme == "cbcs";
  const uint64_t cycle = uint64_t{sample.crypt_byte_block} + sample.skip_byte_block;
  const bool pattern =
      (sample.scheme == "cens" || sample.scheme == "cbcs") && cycle != 0 && sample.crypt_byte_block != 0;
  // One protected range [at, at + length): which of its bytes are encrypted.
  const auto protect_range = [&](uint64_t at, uint64_t length) {
    if (!pattern) {
      const uint64_t whole = partial_clear ? length / kBlock * kBlock : length;
      add_range(out, true, at, whole);
      add_range(out, false, at + whole, length - whole);
      return;
    }
    const uint64_t blocks = length / kBlock;
    uint64_t b = 0;
    while (b < blocks) {
      const uint64_t phase = b % cycle;
      const bool crypt = phase < sample.crypt_byte_block;
      const uint64_t run = std::min(crypt ? sample.crypt_byte_block - phase : cycle - phase, blocks - b);
      add_range(out, crypt, at + b * kBlock, run * kBlock);
      b += run;
    }
    const uint64_t tail = length - blocks * kBlock;
    add_range(out, !partial_clear && blocks % cycle < sample.crypt_byte_block, at + blocks * kBlock, tail);
  };
  if (sample.subsamples.empty()) {
    protect_range(sample_offset, sample_size);
    return out;
  }
  uint64_t pos = sample_offset;
  const uint64_t end = sample_offset + sample_size;
  for (const auto& s : sample.subsamples) {
    const uint64_t clear = std::min<uint64_t>(s->clear_bytes, end - pos);
    add_range(out, false, pos, clear);
    pos += clear;
    const uint64_t prot = std::min<uint64_t>(s->protected_bytes, end - pos);
    protect_range(pos, prot);
    pos += prot;
  }
  add_range(out, false, pos, end - pos);  // bytes the subsamples do not cover (CNT-6002) stay clear
  return out;
}

flatbuffers::DetachedBuffer serialise(const fb::EncryptionInfoT& info) {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::EncryptionInfo::Pack(fbb, &info));
  return fbb.Release();
}

}  // namespace ovt::container::isobmff
