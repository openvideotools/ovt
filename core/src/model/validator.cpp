// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The validator of chapter 06 §6.9 (WI-0.086). Rules are independent: each one walks the verified buffer and
// reports every violation it finds (capped per rule, see kMaxViolationsPerRule); no rule assumes another one
// passed, so every index read from the buffer is bounds-checked before use.

#include "ovt/cxx/model/validator.hpp"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt/cxx/model/tree.hpp"
#include "ovt_model_generated.h"

namespace ovt::model {

namespace {

using fb::BlockKind;
using fb::Codec;
using fb::Severity;

// ---- rule table -----------------------------------------------------------------------------------------------

constexpr std::array<RuleInfo, 32> kRules{{
    {"V-001", "buffer verifies, schema_version 1, plugin identity present", Severity::Error},
    {"V-002", "frame geometry, ids, chroma format and bit depths are in range", Severity::Error},
    {"V-003", "grid dimensions match coded_size and every grid vector has one value per cell", Severity::Error},
    {"V-004", "blocks[0] is the virtual root and every block's id is its index", Severity::Error},
    {"V-005", "parent/children links are consistent and acyclic", Severity::Error},
    {"V-006", "block rects are positive, 4-aligned and nested in their parent", Severity::Error},
    {"V-007", "siblings of the same kind do not overlap", Severity::Error},
    {"V-008", "block kinds and parent-child kind pairs fit the codec", Severity::Error},
    {"V-009", "level equals the depth in the tree", Severity::Error},
    {"V-010", "grid.block_id names an existing non-root block", Severity::Error},
    {"V-011", "grid.block_id names the leaf-most block of the codec's grid kinds", Severity::Error},
    {"V-012", "grid cells lie inside their block, and every leaf block owns a cell", Severity::Error},
    {"V-013", "grid.pred is a valid PredKind and agrees with its block", Severity::Error},
    {"V-014", "reference slots are contiguous and grid references name a slot", Severity::Error},
    {"V-015", "motion vectors name valid blocks and references and agree with the grid", Severity::Error},
    {"V-016", "RefSlot.usage_cells counts the cells referencing the slot", Severity::Error},
    {"V-017", "partitions are contiguous, codec-consistent and inside the frame", Severity::Error},
    {"V-018", "the motion-vector field flag matches the picture structure", Severity::Error},
    {"V-019", "a block's bits are at least the sum of its children's bits", Severity::Error},
    {"V-020", "grid.bits_x8 distributes each leaf block's bits floor-plus-remainder", Severity::Error},
    {"V-021", "extension type names match the block kind and codec", Severity::Error},
    {"V-022", "nested buffers verify and agree with their block", Severity::Error},
    {"V-023", "plane references follow the blob_id grammar and plane geometry", Severity::Error},
    {"V-024", "symbol costs name a symbol and a cell", Severity::Error},
    {"V-025", "vectors are in the canonical order of section 6.3.7", Severity::Error},
    {"V-026", "prediction kind and reference indices agree per cell", Severity::Error},
    {"V-027", "transform, filter, mode and partition values are in the codec's range", Severity::Error},
    {"V-028", "QP values are in range and qp_hist / Block.qp agree with grid.qp", Severity::Error},
    {"V-029", "diagnostic codes follow the registry format and carry a message", Severity::Error},
    {"V-030", "MetricsResult normalisation, order and map references are consistent", Severity::Error},
    {"V-031", "StreamIndex units, frames, display order and sizes are consistent", Severity::Error},
    {"V-032", "re-serialising with canonicalize() is byte-identical", Severity::Error},
}};

// Rules downgraded to Warning when the record is partial (a Fatal diagnostic is present, §6.9).
constexpr bool downgraded_when_partial(int rule) noexcept {
  return (rule >= 10 && rule <= 20) || (rule >= 24 && rule <= 28);
}

std::string rule_id(int rule) {
  std::string id = "V-000";
  id[2] = static_cast<char>('0' + ((rule / 100) % 10));
  id[3] = static_cast<char>('0' + ((rule / 10) % 10));
  id[4] = static_cast<char>('0' + (rule % 10));
  return id;
}

// Verifier limits of WI-0.094 (a 4K HEVC block tree exceeds the default max_tables).
flatbuffers::Verifier::Options verifier_options() {
  flatbuffers::Verifier::Options opts;
  opts.max_depth = 64;
  opts.max_tables = 10'000'000;
  return opts;
}

// ---- small helpers ---------------------------------------------------------------------------------------------

template <class T>
std::size_t vsize(const flatbuffers::Vector<T>* v) noexcept {
  return v == nullptr ? 0 : v->size();
}

std::string_view sv(const flatbuffers::String* s) noexcept {
  return s == nullptr ? std::string_view{} : std::string_view(s->c_str(), s->size());
}

std::string str(std::int64_t v) {
  return std::to_string(v);
}

std::string at(std::string_view base, std::size_t index) {
  std::string out(base);
  out += '[';
  out += std::to_string(index);
  out += ']';
  return out;
}

std::string at(std::string_view base, std::size_t index, std::string_view field) {
  std::string out = at(base, index);
  out += '.';
  out += field;
  return out;
}

std::string rect_str(const fb::Rect& r) {
  return "{" + str(r.x()) + "," + str(r.y()) + "," + str(r.w()) + "," + str(r.h()) + "}";
}

// a lies within b (inclusive edges), 64-bit so hostile values cannot overflow.
bool rect_within(const fb::Rect& a, const fb::Rect& b) noexcept {
  const std::int64_t ax2 = std::int64_t{a.x()} + a.w();
  const std::int64_t ay2 = std::int64_t{a.y()} + a.h();
  const std::int64_t bx2 = std::int64_t{b.x()} + b.w();
  const std::int64_t by2 = std::int64_t{b.y()} + b.h();
  return a.x() >= b.x() && a.y() >= b.y() && ax2 <= bx2 && ay2 <= by2;
}

bool rects_overlap(const fb::Rect& a, const fb::Rect& b) noexcept {
  const std::int64_t ax2 = std::int64_t{a.x()} + a.w();
  const std::int64_t ay2 = std::int64_t{a.y()} + a.h();
  const std::int64_t bx2 = std::int64_t{b.x()} + b.w();
  const std::int64_t by2 = std::int64_t{b.y()} + b.h();
  return a.x() < bx2 && b.x() < ax2 && a.y() < by2 && b.y() < ay2;
}

bool rects_equal(const fb::Rect& a, const fb::Rect& b) noexcept {
  return a.x() == b.x() && a.y() == b.y() && a.w() == b.w() && a.h() == b.h();
}

std::uint32_t ceil4(std::int64_t v) noexcept {
  return static_cast<std::uint32_t>((v + 3) / 4);
}

// The grid kinds of V-011 per codec.
bool is_grid_kind(Codec codec, BlockKind kind) noexcept {
  switch (codec) {
    case Codec::H264:
      return kind == BlockKind::MB || kind == BlockKind::SubMB;
    case Codec::H265:
      return kind == BlockKind::CU;
    case Codec::AV1:
      return kind == BlockKind::Block || kind == BlockKind::SB;
    default:
      return false;
  }
}

// ---- context --------------------------------------------------------------------------------------------------

class Ctx {
 public:
  Ctx(const fb::FrameAnalysis& fa, std::optional<std::span<const std::uint8_t, 32>> hash,
      std::optional<Codec> codec_override)
      : fa_(fa), hash_(hash) {
    partial_ = has_fatal(fa.diagnostics());
    codec_ = codec_override.value_or(infer_codec());
    const auto* frame = fa.frame();
    if (frame != nullptr && frame->coded_size() != nullptr) {
      coded_w_ = frame->coded_size()->w();
      coded_h_ = frame->coded_size()->h();
    }
    if (fa.grid() != nullptr) {
      cells_w_ = fa.grid()->cells_w();
      cells_h_ = fa.grid()->cells_h();
    }
    n_cells_ = std::uint64_t{cells_w_} * cells_h_;
    n_blocks_ = vsize(fa.blocks());
    compute_grid_leaves();
  }

  [[nodiscard]] const fb::FrameAnalysis& fa() const noexcept { return fa_; }
  [[nodiscard]] Codec codec() const noexcept { return codec_; }
  [[nodiscard]] std::int64_t coded_w() const noexcept { return coded_w_; }
  [[nodiscard]] std::int64_t coded_h() const noexcept { return coded_h_; }
  [[nodiscard]] std::uint32_t cells_w() const noexcept { return cells_w_; }
  [[nodiscard]] std::uint32_t cells_h() const noexcept { return cells_h_; }
  [[nodiscard]] std::size_t n_blocks() const noexcept { return n_blocks_; }
  [[nodiscard]] const std::optional<std::span<const std::uint8_t, 32>>& hash() const noexcept { return hash_; }

  // Block @p id, or nullptr when out of range.
  [[nodiscard]] const fb::Block* block(std::uint64_t id) const noexcept {
    return id < n_blocks_ ? fa_.blocks()->Get(static_cast<flatbuffers::uoffset_t>(id)) : nullptr;
  }

  // A grid vector usable for per-cell reads: non-null and exactly one value per cell (else V-003 reports).
  template <class T>
  [[nodiscard]] const flatbuffers::Vector<T>* cells(const flatbuffers::Vector<T>* v) const noexcept {
    return (v != nullptr && v->size() == n_cells_ && n_cells_ > 0) ? v : nullptr;
  }

  // Leaf-most block of the codec's grid kinds (V-011/V-012/V-020/V-028): a grid-kind block none of whose
  // children is of a grid kind.
  // SPEC-ASSUMPTION(#311): "no children of the same kind" read as "no children of the codec's grid kinds",
  // so an H.264 MB split into SubMBs is not a grid leaf (§6.3.2 rule 4: leaf-most of kind MB/SubMB).
  [[nodiscard]] bool is_grid_leaf(std::uint64_t id) const noexcept {
    return id < grid_leaf_.size() && grid_leaf_[static_cast<std::size_t>(id)];
  }

  void report(int rule, std::string message, std::string path) {
    auto& count = counts_.at(static_cast<std::size_t>(rule));
    ++count;
    if (count > kMaxViolationsPerRule) {
      return;
    }
    out_.push_back({rule_id(rule), severity(rule), std::move(message), std::move(path)});
  }

  std::vector<Violation> finish() {
    for (int rule = 1; rule <= 32; ++rule) {
      const auto count = counts_.at(static_cast<std::size_t>(rule));
      if (count > kMaxViolationsPerRule) {
        report_suppressed(out_, rule, count - kMaxViolationsPerRule, severity(rule));
      }
    }
    return std::move(out_);
  }

  static void report_suppressed(std::vector<Violation>& out, int rule, std::size_t more, Severity sev) {
    out.push_back({rule_id(rule), sev, "... and " + std::to_string(more) + " more violations of this rule", ""});
  }

 private:
  [[nodiscard]] Severity severity(int rule) const noexcept {
    return (partial_ && downgraded_when_partial(rule)) ? Severity::Warning : Severity::Error;
  }

  static bool has_fatal(const flatbuffers::Vector<flatbuffers::Offset<fb::Diagnostic>>* diags) {
    if (diags == nullptr) {
      return false;
    }
    return std::any_of(diags->begin(), diags->end(),
                       [](const fb::Diagnostic* d) { return d != nullptr && d->severity() == Severity::Fatal; });
  }

  // §6.9 implementation note: blocks[1].kind, else the ext_type strings.
  [[nodiscard]] Codec infer_codec() const {
    const auto* blocks = fa_.blocks();
    if (vsize(blocks) >= 2) {
      switch (blocks->Get(1)->kind()) {
        case BlockKind::MB:
          return Codec::H264;
        case BlockKind::CTU:
          return Codec::H265;
        case BlockKind::SB:
          return Codec::AV1;
        default:
          return Codec::Unknown;
      }
    }
    const auto from_name = [](std::string_view name) {
      if (name.starts_with("Avc")) {
        return Codec::H264;
      }
      if (name.starts_with("Hevc")) {
        return Codec::H265;
      }
      if (name.starts_with("Av1")) {
        return Codec::AV1;
      }
      return Codec::Unknown;
    };
    Codec codec = from_name(sv(fa_.filter_params_type()));
    if (codec == Codec::Unknown && fa_.frame() != nullptr) {
      codec = from_name(sv(fa_.frame()->ref_state_type()));
    }
    return codec;
  }

  void compute_grid_leaves() {
    grid_leaf_.assign(n_blocks_, false);
    for (std::size_t i = 0; i < n_blocks_; ++i) {
      const auto* b = fa_.blocks()->Get(static_cast<flatbuffers::uoffset_t>(i));
      if (!is_grid_kind(codec_, b->kind())) {
        continue;
      }
      bool leaf = true;
      if (const auto* children = b->children(); children != nullptr) {
        for (const auto c : *children) {
          const auto* child = block(c);
          if (child != nullptr && is_grid_kind(codec_, child->kind())) {
            leaf = false;
            break;
          }
        }
      }
      grid_leaf_[i] = leaf;
    }
  }

  const fb::FrameAnalysis& fa_;
  std::optional<std::span<const std::uint8_t, 32>> hash_;
  Codec codec_ = Codec::Unknown;
  bool partial_ = false;
  std::int64_t coded_w_ = 0;
  std::int64_t coded_h_ = 0;
  std::uint32_t cells_w_ = 0;
  std::uint32_t cells_h_ = 0;
  std::uint64_t n_cells_ = 0;
  std::size_t n_blocks_ = 0;
  std::vector<bool> grid_leaf_;
  std::array<std::size_t, 33> counts_{};
  std::vector<Violation> out_;
};

// ---- V-002 frame ----------------------------------------------------------------------------------------------

constexpr int kRuleV002 = 2;

// V-002, the codec-dependent part: coded size multiples and the crop rectangle.
void v002_codec_geometry(Ctx& c, const fb::FrameInfo& f, const fb::Rect& cs) {
  constexpr int kRule = kRuleV002;
  const auto* crop = f.crop();
  if (c.codec() == Codec::H264 || c.codec() == Codec::H265) {
    const int multiple = c.codec() == Codec::H264 ? 16 : 8;  // HEVC: MinCbSizeY >= 8, a power of two
    // SPEC-ASSUMPTION(#311): MinCbSizeY is not in FrameAnalysis; a multiple of 8 is the check that can be made.
    if (cs.w() % multiple != 0 || cs.h() % multiple != 0) {
      c.report(kRule, "frame.coded_size " + rect_str(cs) + " must be a multiple of " + str(multiple),
               "frame.coded_size");
    }
    if (crop == nullptr) {
      c.report(kRule, "frame.crop is missing", "frame.crop");
    } else if (crop->x() < 0 || crop->y() < 0 || crop->w() <= 0 || crop->h() <= 0 || !rect_within(*crop, cs)) {
      c.report(kRule, "frame.crop " + rect_str(*crop) + " must lie within coded_size " + rect_str(cs), "frame.crop");
    }
  } else if (c.codec() == Codec::AV1) {
    if (crop == nullptr) {
      c.report(kRule, "frame.crop is missing", "frame.crop");
    } else if (crop->x() != 0 || crop->y() != 0 || crop->w() < 1 || crop->h() < 1) {
      c.report(kRule, "AV1 frame.crop must be {0,0,w>=1,h>=1}, is " + rect_str(*crop), "frame.crop");
    }
  }
}

// V-002, the picture format: chroma format and bit depths.
void v002_format(Ctx& c, const fb::FrameInfo& f) {
  constexpr int kRule = kRuleV002;
  if (f.chroma_format() > 3) {
    c.report(kRule, "frame.chroma_format must be <= 3, is " + str(f.chroma_format()), "frame.chroma_format");
  }
  const int bdl = f.bit_depth_luma();
  const int bdc = f.bit_depth_chroma();
  if (bdl < 8 || bdl > 16) {
    c.report(kRule, "frame.bit_depth_luma must be 8..16, is " + str(bdl), "frame.bit_depth_luma");
  }
  if (bdc < 8 || bdc > 16) {
    c.report(kRule, "frame.bit_depth_chroma must be 8..16, is " + str(bdc), "frame.bit_depth_chroma");
  }
  if (f.chroma_format() == 0 && bdc != bdl) {
    c.report(kRule, "4:0:0 frame: bit_depth_chroma " + str(bdc) + " != bit_depth_luma " + str(bdl),
             "frame.bit_depth_chroma");
  }
}

void v002(Ctx& c) {
  constexpr int kRule = kRuleV002;
  const auto* f = c.fa().frame();
  if (f == nullptr) {
    c.report(kRule, "frame is missing", "frame");
    return;
  }
  if (f->timebase_den() <= 0) {
    c.report(kRule, "frame.timebase_den must be > 0, is " + str(f->timebase_den()), "frame.timebase_den");
  }
  const auto* cs = f->coded_size();
  if (cs == nullptr) {
    c.report(kRule, "frame.coded_size is missing", "frame.coded_size");
  } else if (cs->x() != 0 || cs->y() != 0 || cs->w() <= 0 || cs->h() <= 0) {
    c.report(kRule, "frame.coded_size must be {0,0,w>0,h>0}, is " + rect_str(*cs), "frame.coded_size");
  }
  if (const auto* id = f->id(); id == nullptr) {
    c.report(kRule, "frame.id is missing", "frame.id");
  } else {
    if (id->decode_index() < 0) {
      c.report(kRule, "frame.id.decode_index must be >= 0, is " + str(id->decode_index()), "frame.id.decode_index");
    }
    if (id->display_index() < -1) {
      c.report(kRule, "frame.id.display_index must be >= -1, is " + str(id->display_index()), "frame.id.display_index");
    }
  }
  v002_format(c, *f);
  if (cs != nullptr) {
    v002_codec_geometry(c, *f, *cs);
  }
}

// ---- V-003 grid shape -----------------------------------------------------------------------------------------

template <class T>
void check_cell_vector(Ctx& c, const flatbuffers::Vector<T>* v, std::string_view name, std::uint64_t n_cells) {
  if (v != nullptr && v->size() != 0 && v->size() != n_cells) {
    c.report(3,
             "grid." + std::string(name) + " has " + str(v->size()) +
                 " elements, expected cells_w*cells_h = " + std::to_string(n_cells),
             "grid." + std::string(name));
  }
}

void v003(Ctx& c) {
  constexpr int kRule = 3;
  const auto* g = c.fa().grid();
  if (g == nullptr) {
    c.report(kRule, "grid is missing", "grid");
  } else {
    if (c.coded_w() > 0 && c.coded_h() > 0) {
      if (g->cells_w() != ceil4(c.coded_w())) {
        c.report(kRule, "grid.cells_w " + str(g->cells_w()) + " != ceil(coded_size.w/4) = " + str(ceil4(c.coded_w())),
                 "grid.cells_w");
      }
      if (g->cells_h() != ceil4(c.coded_h())) {
        c.report(kRule, "grid.cells_h " + str(g->cells_h()) + " != ceil(coded_size.h/4) = " + str(ceil4(c.coded_h())),
                 "grid.cells_h");
      }
    }
    const std::uint64_t n = std::uint64_t{g->cells_w()} * g->cells_h();
    check_cell_vector(c, g->block_id(), "block_id", n);
    check_cell_vector(c, g->partition_id(), "partition_id", n);
    check_cell_vector(c, g->pred(), "pred", n);
    check_cell_vector(c, g->qp(), "qp", n);
    check_cell_vector(c, g->intra_mode_y(), "intra_mode_y", n);
    check_cell_vector(c, g->intra_mode_c(), "intra_mode_c", n);
    check_cell_vector(c, g->mv0_x(), "mv0_x", n);
    check_cell_vector(c, g->mv0_y(), "mv0_y", n);
    check_cell_vector(c, g->mv1_x(), "mv1_x", n);
    check_cell_vector(c, g->mv1_y(), "mv1_y", n);
    check_cell_vector(c, g->ref0(), "ref0", n);
    check_cell_vector(c, g->ref1(), "ref1", n);
    check_cell_vector(c, g->tx_log2_w(), "tx_log2_w", n);
    check_cell_vector(c, g->tx_log2_h(), "tx_log2_h", n);
    check_cell_vector(c, g->tx_type(), "tx_type", n);
    check_cell_vector(c, g->cbf(), "cbf", n);
    check_cell_vector(c, g->bits_x8(), "bits_x8", n);
    check_cell_vector(c, g->deblock_bs_v(), "deblock_bs_v", n);
    check_cell_vector(c, g->deblock_bs_h(), "deblock_bs_h", n);
    check_cell_vector(c, g->filter_idx(), "filter_idx", n);
  }
  const auto hist = vsize(c.fa().qp_hist());
  if (hist != 0 && hist != 256) {
    c.report(kRule, "qp_hist has " + str(static_cast<std::int64_t>(hist)) + " elements, expected 0 or 256", "qp_hist");
  }
}

// ---- V-004 root and ids ---------------------------------------------------------------------------------------

void v004(Ctx& c) {
  constexpr int kRule = 4;
  if (c.n_blocks() == 0) {
    c.report(kRule, "blocks is empty", "blocks");
    return;
  }
  for (std::size_t i = 0; i < c.n_blocks(); ++i) {
    const auto* b = c.block(i);
    if (b->id() != i) {
      c.report(kRule, "blocks[i].id must equal i, is " + str(b->id()), at("blocks", i, "id"));
    }
    if (i > 0 && b->parent() == -1) {
      c.report(kRule, "only blocks[0] may have parent -1", at("blocks", i, "parent"));
    }
  }
  const auto* root = c.block(0);
  if (root->parent() != -1) {
    c.report(kRule, "blocks[0].parent must be -1, is " + str(root->parent()), "blocks[0].parent");
  }
  if (root->kind() != BlockKind::Other) {
    c.report(kRule, std::string("blocks[0].kind must be Other, is ") + fb::EnumNameBlockKind(root->kind()),
             "blocks[0].kind");
  }
  const auto* r = root->rect();
  if (r == nullptr || r->x() != 0 || r->y() != 0 || r->w() != c.coded_w() || r->h() != c.coded_h()) {
    c.report(kRule,
             "blocks[0].rect must be {0,0,coded_size.w,coded_size.h}, is " + (r == nullptr ? "missing" : rect_str(*r)),
             "blocks[0].rect");
  }
  if (root->level() != 0) {
    c.report(kRule, "blocks[0].level must be 0, is " + str(root->level()), "blocks[0].level");
  }
  if (!sv(root->ext_type()).empty() || vsize(root->ext()) != 0) {
    c.report(kRule, "blocks[0] must have an empty ext_type and ext", "blocks[0].ext_type");
  }
}

// ---- V-005 links ----------------------------------------------------------------------------------------------

void v005(Ctx& c) {
  constexpr int kRule = 5;
  const std::size_t n = c.n_blocks();
  std::vector<std::uint32_t> appearances(n, 0);
  for (std::size_t p = 0; p < n; ++p) {
    const auto* children = c.block(p)->children();
    for (std::size_t k = 0; k < vsize(children); ++k) {
      const auto id = children->Get(static_cast<flatbuffers::uoffset_t>(k));
      const auto* child = c.block(id);
      const std::string path = at(at("blocks", p, "children"), k);
      if (child == nullptr) {
        c.report(kRule, "child id " + str(id) + " is outside blocks (" + str(static_cast<std::int64_t>(n)) + ")", path);
        continue;
      }
      if (child->parent() != static_cast<std::int64_t>(p)) {
        c.report(
            kRule,
            "child " + str(id) + " has parent " + str(child->parent()) + ", not " + str(static_cast<std::int64_t>(p)),
            path);
      }
      ++appearances[id];
    }
  }
  for (std::size_t i = 1; i < n; ++i) {
    const auto parent = c.block(i)->parent();
    if (parent < 0 || static_cast<std::size_t>(parent) >= i) {
      c.report(kRule, "parent " + str(parent) + " must satisfy 0 <= parent < id " + str(static_cast<std::int64_t>(i)),
               at("blocks", i, "parent"));
    }
    if (appearances[i] != 1) {
      c.report(kRule, "block appears in " + str(appearances[i]) + " children vectors, expected exactly 1",
               at("blocks", i));
    }
  }
}

// ---- V-006 rects ----------------------------------------------------------------------------------------------

void v006(Ctx& c) {
  constexpr int kRule = 6;
  for (std::size_t i = 0; i < c.n_blocks(); ++i) {
    const auto* b = c.block(i);
    const auto* r = b->rect();
    const std::string path = at("blocks", i, "rect");
    if (r == nullptr) {
      c.report(kRule, "rect is missing", path);
      continue;
    }
    if (r->w() <= 0 || r->h() <= 0 || r->x() < 0 || r->y() < 0 || r->x() % 4 != 0 || r->y() % 4 != 0 ||
        r->w() % 4 != 0 || r->h() % 4 != 0) {
      c.report(kRule, "rect " + rect_str(*r) + " must be non-negative, positive-sized and 4-aligned", path);
    }
    if (b->level() >= 2) {
      const auto* parent = b->parent() >= 0 ? c.block(static_cast<std::uint64_t>(b->parent())) : nullptr;
      if (parent != nullptr && parent->rect() != nullptr && !rect_within(*r, *parent->rect())) {
        c.report(kRule, "rect " + rect_str(*r) + " is not within the parent's rect " + rect_str(*parent->rect()), path);
      }
    } else if (b->level() == 1 && (r->x() >= c.coded_w() || r->y() >= c.coded_h())) {
      c.report(kRule, "top-level rect " + rect_str(*r) + " starts outside the coded picture", path);
    }
  }
}

// ---- V-007 sibling overlap ------------------------------------------------------------------------------------

// MBAFF field MB pair exception: both MBs of the pair carry mb_field_decoding_flag = 1 and share the rect.
bool mbaff_field_pair(const Ctx& c, const fb::Block& a, const fb::Block& b) {
  if (c.fa().frame() == nullptr || c.fa().frame()->structure() != fb::PictureStructure::Mbaff ||
      a.kind() != BlockKind::MB || b.kind() != BlockKind::MB || !rects_equal(*a.rect(), *b.rect())) {
    return false;
  }
  const auto* ea = ext_as<fb::AvcMb>(a);
  const auto* eb = ext_as<fb::AvcMb>(b);
  return ea != nullptr && eb != nullptr && ea->mb_field_decoding_flag() == 1 && eb->mb_field_decoding_flag() == 1;
}

void v007(Ctx& c) {
  constexpr int kRule = 7;
  // Sweep over children sorted by x: only rects starting left of the current rect's right edge can overlap
  // it. The comparison budget bounds hostile inputs (fuzzing, WI-0.094); real trees stay far below it.
  std::uint64_t budget = 64'000'000;
  std::vector<const fb::Block*> kids;
  for (std::size_t p = 0; p < c.n_blocks() && budget > 0; ++p) {
    const auto* children = c.block(p)->children();
    if (vsize(children) < 2) {
      continue;
    }
    kids.clear();
    for (const auto id : *children) {
      const auto* child = c.block(id);
      if (child != nullptr && child->rect() != nullptr) {
        kids.push_back(child);
      }
    }
    std::sort(kids.begin(), kids.end(), [](const fb::Block* a, const fb::Block* b) {
      return std::pair(a->rect()->x(), a->id()) < std::pair(b->rect()->x(), b->id());
    });
    for (std::size_t i = 0; i < kids.size() && budget > 0; ++i) {
      const auto& a = *kids[i];
      const std::int64_t right = std::int64_t{a.rect()->x()} + a.rect()->w();
      for (std::size_t j = i + 1; j < kids.size() && kids[j]->rect()->x() < right && budget > 0; ++j, --budget) {
        const auto& b = *kids[j];
        if (a.id() != b.id() && a.kind() == b.kind() && rects_overlap(*a.rect(), *b.rect()) &&
            !mbaff_field_pair(c, a, b)) {
          const auto lo = std::min(a.id(), b.id());
          const auto hi = std::max(a.id(), b.id());
          c.report(
              kRule,
              "siblings " + str(lo) + " and " + str(hi) + " of kind " + fb::EnumNameBlockKind(a.kind()) + " overlap",
              at("blocks", hi, "rect"));
        }
      }
    }
  }
}

// ---- V-008 kinds ----------------------------------------------------------------------------------------------

bool kind_in_codec(Codec codec, BlockKind kind) noexcept {
  if (kind == BlockKind::Other) {
    return true;
  }
  switch (codec) {
    case Codec::H264:
      return kind == BlockKind::MB || kind == BlockKind::SubMB;
    case Codec::H265:
      return kind == BlockKind::CTU || kind == BlockKind::CU || kind == BlockKind::PU || kind == BlockKind::TU;
    case Codec::AV1:
      return kind == BlockKind::SB || kind == BlockKind::Block || kind == BlockKind::TXB;
    default:
      return true;  // unknown codec: only the pair table below applies
  }
}

bool kind_pair_allowed(BlockKind parent, BlockKind child) noexcept {
  using K = BlockKind;
  constexpr std::array<std::pair<K, K>, 12> kPairs{{
      {K::Other, K::MB},
      {K::MB, K::SubMB},
      {K::Other, K::CTU},
      {K::CTU, K::CU},
      {K::CU, K::PU},
      {K::CU, K::TU},
      {K::TU, K::TU},
      {K::Other, K::SB},
      {K::SB, K::Block},
      {K::Block, K::Block},
      {K::Block, K::TXB},
      {K::SB, K::TXB},
  }};
  return std::find(kPairs.begin(), kPairs.end(), std::pair(parent, child)) != kPairs.end();
}

void v008(Ctx& c) {
  constexpr int kRule = 8;
  for (std::size_t i = 0; i < c.n_blocks(); ++i) {
    const auto* b = c.block(i);
    if (!kind_in_codec(c.codec(), b->kind())) {
      c.report(
          kRule,
          std::string("kind ") + fb::EnumNameBlockKind(b->kind()) + " is not used by " + fb::EnumNameCodec(c.codec()),
          at("blocks", i, "kind"));
    }
    if (i == 0 || b->parent() < 0) {
      continue;
    }
    const auto* parent = c.block(static_cast<std::uint64_t>(b->parent()));
    if (parent != nullptr && !kind_pair_allowed(parent->kind(), b->kind())) {
      c.report(kRule,
               std::string("parent-child kinds ") + fb::EnumNameBlockKind(parent->kind()) + "->" +
                   fb::EnumNameBlockKind(b->kind()) + " are not allowed",
               at("blocks", i, "kind"));
    }
  }
}

// ---- V-009 levels ---------------------------------------------------------------------------------------------

void v009(Ctx& c) {
  constexpr int kRule = 9;
  // Depth by parent hops; computable in id order where parent < id (V-005 reports the rest).
  std::vector<std::int64_t> depth(c.n_blocks(), -1);
  for (std::size_t i = 0; i < c.n_blocks(); ++i) {
    const auto* b = c.block(i);
    if (i == 0) {
      depth[0] = b->parent() == -1 ? 0 : -1;
    } else if (b->parent() >= 0 && static_cast<std::size_t>(b->parent()) < i &&
               depth[static_cast<std::size_t>(b->parent())] >= 0) {
      depth[i] = depth[static_cast<std::size_t>(b->parent())] + 1;
    }
    if (depth[i] >= 0 && depth[i] != b->level()) {
      c.report(kRule, "level " + str(b->level()) + " != depth " + str(depth[i]), at("blocks", i, "level"));
    }
  }
}

// ---- V-010 / V-011 / V-012 grid ownership ---------------------------------------------------------------------

void v010_v011_v012(Ctx& c) {
  const auto* ids = c.fa().grid() != nullptr ? c.cells(c.fa().grid()->block_id()) : nullptr;
  if (ids == nullptr) {
    return;
  }
  std::vector<std::uint32_t> owned(c.n_blocks(), 0);
  for (std::size_t i = 0; i < ids->size(); ++i) {
    const auto id = ids->Get(static_cast<flatbuffers::uoffset_t>(i));
    const std::string path = at("grid.block_id", i);
    const auto* b = c.block(id);
    if (b == nullptr || id == 0) {
      c.report(10,
               "block_id " + str(id) + " must be < blocks.size() " + str(static_cast<std::int64_t>(c.n_blocks())) +
                   " and != 0",
               path);
      continue;
    }
    ++owned[id];
    if (c.codec() != Codec::Unknown && !c.is_grid_leaf(id)) {
      c.report(11,
               "block " + str(id) + " (kind " + fb::EnumNameBlockKind(b->kind()) +
                   ") is not a leaf-most block of the codec's grid kinds",
               path);
    }
    const auto x = static_cast<std::int64_t>(i % c.cells_w()) * 4;
    const auto y = static_cast<std::int64_t>(i / c.cells_w()) * 4;
    const auto* r = b->rect();
    if (r == nullptr || x < r->x() || x >= std::int64_t{r->x()} + r->w() || y < r->y() ||
        y >= std::int64_t{r->y()} + r->h()) {
      c.report(12,
               "cell (" + str(x / 4) + "," + str(y / 4) + ") lies outside the rect of block " + str(id) +
                   (r == nullptr ? std::string() : " " + rect_str(*r)),
               path);
    }
  }
  // Converse: every grid leaf that intersects the coded picture owns a cell (AV1 border children that lie
  // entirely outside the picture are exempt, WI-0.086 notes).
  for (std::size_t i = 1; i < c.n_blocks(); ++i) {
    const auto* r = c.block(i)->rect();
    if (!c.is_grid_leaf(i) || owned[i] != 0 || r == nullptr) {
      continue;
    }
    const bool intersects = r->x() < c.coded_w() && r->y() < c.coded_h() && std::int64_t{r->x()} + r->w() > 0 &&
                            std::int64_t{r->y()} + r->h() > 0;
    if (intersects) {
      c.report(12, "leaf block " + str(static_cast<std::int64_t>(i)) + " intersects the picture but owns no cell",
               at("blocks", i));
    }
  }
}

// ---- driver ---------------------------------------------------------------------------------------------------

const fb::FrameAnalysis* verify_frame_analysis(std::span<const std::uint8_t> buffer) {
  const auto opts = verifier_options();
  {
    flatbuffers::Verifier verifier(buffer.data(), buffer.size(), opts);
    if (fb::VerifyFrameAnalysisBuffer(verifier)) {
      return fb::GetFrameAnalysis(buffer.data());
    }
  }
  flatbuffers::Verifier verifier(buffer.data(), buffer.size(), opts);
  if (fb::VerifySizePrefixedFrameAnalysisBuffer(verifier)) {
    return fb::GetSizePrefixedFrameAnalysis(buffer.data());
  }
  return nullptr;
}

std::vector<Violation> run_frame_analysis(std::span<const std::uint8_t> buffer,
                                          std::optional<std::span<const std::uint8_t, 32>> hash,
                                          std::optional<Codec> codec_override) {
  const auto* fa = buffer.empty() ? nullptr : verify_frame_analysis(buffer);
  if (fa == nullptr) {
    return {{"V-001", Severity::Error,
             "buffer (" + std::to_string(buffer.size()) +
                 " bytes) does not pass flatbuffers::Verifier for ovt.fb.FrameAnalysis (identifier OVTA)",
             ""}};
  }
  Ctx c(*fa, hash, codec_override);
  if (fa->schema_version() != 1) {
    c.report(1, "schema_version must be 1, is " + str(fa->schema_version()), "schema_version");
  }
  if (sv(fa->plugin_id()).empty()) {
    c.report(1, "plugin_id is empty", "plugin_id");
  }
  if (sv(fa->plugin_version()).empty()) {
    c.report(1, "plugin_version is empty", "plugin_version");
  }
  v002(c);
  v003(c);
  v004(c);
  v005(c);
  v006(c);
  v007(c);
  v008(c);
  v009(c);
  v010_v011_v012(c);
  return c.finish();
}

}  // namespace

std::vector<Violation> validate_frame_analysis(std::span<const std::uint8_t> buffer,
                                               std::span<const std::uint8_t, 32> expected_source_hash,
                                               std::optional<fb::Codec> codec_override) {
  return run_frame_analysis(buffer, expected_source_hash, codec_override);
}

std::vector<Violation> validate_frame_analysis(std::span<const std::uint8_t> buffer,
                                               std::optional<fb::Codec> codec_override) {
  return run_frame_analysis(buffer, std::nullopt, codec_override);
}

std::span<const RuleInfo> Validator::rules() noexcept {
  return kRules;
}

}  // namespace ovt::model
