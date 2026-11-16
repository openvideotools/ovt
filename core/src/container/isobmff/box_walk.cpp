// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ISOBMFF box header walk, chapter 08 §8.2.3 steps 1-3 (WI-1.201). Each comment "2.n" names the numbered step of the
// algorithm. Every offset addition is checked for overflow.

#include "container/isobmff/box_walk.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "container/diagnostics.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"

namespace ovt::container::isobmff {

namespace {

using fb::BoxNodeT;

constexpr uint64_t kHeaderProbe = 32;  // 8 + 8 (largesize) + 16 (usertype)
// No PagedSpan yet (issue #630): the first decoder of a >1 MiB leaf, WI-1.209, adds it. Until then decoders see at most
// 1 MiB (§8.2.3 reading strategy).
constexpr uint64_t kMaxDecodedPayload = 1 << 20;

std::string_view sv(const FourCC& t) {
  return {t.data(), 4};
}

using Span = std::span<const uint8_t>;

FourCC fourcc_at(Span s) {
  FourCC t{};
  std::copy_n(s.begin(), 4, t.begin());
  return t;
}

uint32_t be32(Span s) {
  return (static_cast<uint32_t>(s[0]) << 24U) | (static_cast<uint32_t>(s[1]) << 16U) |
         (static_cast<uint32_t>(s[2]) << 8U) | static_cast<uint32_t>(s[3]);
}
uint64_t be64(Span s) {
  return (static_cast<uint64_t>(be32(s.first(4))) << 32U) | be32(s.subspan(4, 4));
}

bool printable_byte(uint8_t b) {
  return b >= 0x20 && b <= 0x7E;
}

// §8.2.3 step 2.9: four printable ASCII bytes, or a QuickTime `©xxx` (first byte 0xA9), or an `ilst` numeric key.
bool plausible_type(const FourCC& t, const BoxContext& ctx) {
  const std::array<uint8_t, 4> u{static_cast<uint8_t>(t[0]), static_cast<uint8_t>(t[1]), static_cast<uint8_t>(t[2]),
                                 static_cast<uint8_t>(t[3])};
  const bool tail_ok = printable_byte(u[1]) && printable_byte(u[2]) && printable_byte(u[3]);
  if (tail_ok && (printable_byte(u[0]) || u[0] == 0xA9)) {
    return true;
  }
  return ctx.parent_is("ilst") && u[0] == 0 && u[1] == 0 && u[2] == 0;
}

// Display form of a fourcc: ISO-8859-1 to UTF-8 (so `©nam` shows), or `\xNN\xNN\xNN\xNN` when not plausible.
// SPEC-ASSUMPTION(#323): the non-printable form is four \xNN escapes.
std::string display_fourcc(const FourCC& t, bool plausible) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const char c : t) {
    const auto b = static_cast<uint8_t>(c);
    if (!plausible) {
      out += "\\x";
      out += kHex[b >> 4U];
      out += kHex[b & 0xFU];
    } else if (b < 0x80) {
      out += c;
    } else {
      out += static_cast<char>(0xC0U | (b >> 6U));
      out += static_cast<char>(0x80U | (b & 0x3FU));
    }
  }
  return out;
}

std::unique_ptr<fb::DiagnosticT> make_diag(DiagCode code, std::string message, uint64_t rel_byte_offset) {
  const DiagInfo& info = diag(code);
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = static_cast<fb::Severity>(info.severity);
  d->code = std::string(info.code);
  d->message = std::move(message);
  d->span = std::make_unique<fb::BitSpan>(rel_byte_offset * 8U, 0U);
  d->spec_ref = std::string(info.spec_ref);
  return d;
}

std::string at(uint64_t pos) {
  return " at offset " + std::to_string(pos);
}

// Spans of diagnostics on a node are relative to the node start (§8.2.5 `diagnostics`).
void add_diag(BoxNodeT& n, DiagCode code, std::string message, uint64_t abs_pos) {
  n.diagnostics.push_back(make_diag(code, std::move(message), abs_pos - n.file_span->offset()));
}

std::unique_ptr<BoxNodeT> make_node(uint64_t offset, uint64_t length) {
  auto n = std::make_unique<BoxNodeT>();
  n->file_span = std::make_unique<fb::ByteSpan>(offset, length);
  return n;
}

// SPEC-ASSUMPTION(#323): every stop turns the rest of the parent into a trailing-bytes node.
// The rest of the parent, [pos, parent_end), as a node the sum-of-spans invariant can count.
std::unique_ptr<BoxNodeT> trailing_node(uint64_t pos, uint64_t parent_end, DiagCode code, std::string message) {
  auto n = make_node(pos, parent_end - pos);
  n->payload_span = std::make_unique<fb::ByteSpan>(pos, parent_end - pos);
  n->description = "trailing bytes";
  add_diag(*n, code, std::move(message), pos);
  return n;
}

void add_uint_field(BoxNodeT& n, std::string name, uint64_t value, uint64_t bit_offset, uint32_t bits) {
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::move(name);
  f->kind = fb::SyntaxValueKind::UInt;
  f->uvalue = value;
  f->span = std::make_unique<fb::BitSpan>(bit_offset, bits);
  n.fields.push_back(std::move(f));
}

// §8.2.6 rule 4: is the `meta` payload at `payload_begin` QuickTime style (no version/flags)?
bool meta_is_quicktime(const io::RandomAccessFile& file, uint64_t payload_begin, uint64_t end) {
  if (end < payload_begin || end - payload_begin < 8) {
    return false;
  }
  std::array<uint8_t, 8> b{};
  if (file.read(payload_begin, b.size(), b.data()) != b.size()) {
    return false;
  }
  static constexpr std::array<std::string_view, 11> kChildren = {"hdlr", "keys", "ilst", "pitm", "iinf", "iloc",
                                                                 "ipco", "iprp", "iref", "idat", "dinf"};
  const std::string_view next = std::string_view(reinterpret_cast<const char*>(b.data()), b.size()).substr(4);
  return be32(b) != 0 && std::ranges::find(kChildren, next) != kChildren.end();
}

// What §8.2.3 steps 2.2-2.7 learn from the header bytes at `pos`.
struct Header {
  FourCC type{};
  uint64_t size = 0;
  uint64_t length = 8;  // header_length so far (a uuid usertype is added by the caller)
  bool wide = false;    // step 2.3
  bool stop = false;    // steps 2.4, 2.6, 2.7: the rest of the parent becomes a trailing node
  DiagCode finding = DiagCode::CNT_0106;
  std::string finding_msg;  // set: a finding to attach to the box (stop == false) or the trailing node (stop == true)
};

Header read_header(Span hdr, uint64_t pos, uint64_t remaining, unsigned depth, const BoxContext& ctx) {
  Header h;
  const uint32_t size32 = be32(hdr);
  h.type = fourcc_at(hdr.subspan(4, 4));
  h.size = size32;
  if (sv(h.type) == "wide" && size32 == 8) {  // 2.3
    h.wide = true;
    return h;
  }
  const auto stop_with = [&h](DiagCode code, std::string msg) {
    h.stop = true;
    h.finding = code;
    h.finding_msg = std::move(msg);
  };
  if (size32 == 1) {  // 2.4 largesize
    if (remaining < 16 || hdr.size() < 16) {
      stop_with(DiagCode::CNT_0102, "truncated largesize" + at(pos));
      return h;
    }
    h.size = be64(hdr.subspan(8, 8));
    h.length = 16;
    if (h.size < 16) {
      stop_with(DiagCode::CNT_0103, "largesize " + std::to_string(h.size) + " is smaller than its header" + at(pos));
      return h;
    }
  } else if (size32 == 0) {  // 2.5 extends to the end of the parent
    h.size = remaining;
    if (depth > 0) {
      h.finding = DiagCode::CNT_0104;
      h.finding_msg = "size 0 box inside a container (extends to the end of its parent)" + at(pos);
    }
  } else if (size32 < 8) {  // 2.6 impossible size; recover when the type is a known one
    const std::string what = "box size " + std::to_string(size32) + " < 8" + at(pos);
    // SPEC-ASSUMPTION(#323): a `uuid` box can never be 8 bytes, so it is never recovered (no size 8): CNT-0106, stop.
    if (sv(h.type) != "uuid" && plausible_type(h.type, ctx) && lookup(sv(h.type), ctx) != nullptr) {
      h.size = 8;
      h.finding = DiagCode::CNT_0106;
      h.finding_msg = what + "; treated as 8";
    } else {
      stop_with(DiagCode::CNT_0106, what);
      return h;
    }
  }
  if (sv(h.type) == "uuid" && (h.size < h.length + 16 || remaining < h.length + 16 || hdr.size() < h.length + 16)) {
    stop_with(DiagCode::CNT_0107, "uuid box too short for its usertype" + at(pos));  // 2.7
  }
  return h;
}

// Steps 2.8-2.10: spans, truncation clamp, type check; the node is not yet filled with its payload.
std::unique_ptr<BoxNodeT> build_node(const Header& h, Span hdr, uint64_t pos, uint64_t parent_end,
                                     const BoxContext& ctx) {
  uint64_t length = h.length;
  uint64_t end = 0;
  const bool truncated = __builtin_add_overflow(pos, h.size, &end) || end > parent_end;
  const uint64_t overshoot = (truncated && end > parent_end) ? end - parent_end : 0;
  if (truncated) {
    end = parent_end;
  }
  auto n = make_node(pos, end - pos);
  if (sv(h.type) == "uuid") {
    const Span usertype = hdr.subspan(length, 16);
    n->uuid.assign(usertype.begin(), usertype.end());
    length += 16;
  }
  n->header_length = static_cast<uint8_t>(length);
  if (!h.finding_msg.empty()) {
    add_diag(*n, h.finding, h.finding_msg, pos);
  }
  const bool plausible = plausible_type(h.type, ctx);
  n->fourcc = display_fourcc(h.type, plausible);
  if (truncated) {
    n->description = "[truncated]";
    add_diag(*n, DiagCode::CNT_0108,
             "box '" + n->fourcc + "'" + at(pos) + " claims size " + std::to_string(h.size) + ", exceeds parent end" +
                 (overshoot != 0 ? " by " + std::to_string(overshoot) + " bytes" : std::string{}),
             pos);
  }
  if (!plausible) {
    add_diag(*n, DiagCode::CNT_0109, "non-printable box type " + n->fourcc + at(pos), pos);
  }
  return n;
}

// Step 2.13 and the file-level flags.
void track_top_level_flags(WalkState& st, const FourCC& type, BoxNodeT& n) {
  if (sv(type) == "mdat") {
    st.saw_mdat = true;
    st.mdat_before_moov = st.mdat_before_moov || !st.saw_moov;
  } else if (sv(type) == "moov") {
    if (st.saw_mdat && !st.saw_moov) {
      add_diag(n, DiagCode::CNT_0003, "`moov` after `mdat` (not progressive/fast-start)", n.file_span->offset());
    }
    st.saw_moov = true;
  } else if (sv(type) == "moof") {
    st.saw_moof = true;
    ++st.moof_count;
  }
}

// §8.2.6 rule 4. Returns the offset where the children of `meta` start.
uint64_t read_meta_prefix(const io::RandomAccessFile& file, BoxNodeT& n, uint64_t payload_begin, uint64_t end,
                          bool& leaf) {
  const uint64_t pos = n.file_span->offset();
  const uint64_t bits = static_cast<uint64_t>(n.header_length) * 8U;
  std::array<uint8_t, 4> vf{};
  if (meta_is_quicktime(file, payload_begin, end)) {
    add_diag(n, DiagCode::CNT_1006, "QuickTime-style `meta` without version/flags", pos);
  } else if (end - payload_begin < 4) {
    add_diag(n, DiagCode::CNT_0110, "FullBox shorter than 4 bytes" + at(pos), pos);
    leaf = true;
  } else if (file.read(payload_begin, 4, vf.data()) == 4) {
    n.version = vf[0];
    n.flags = static_cast<int32_t>(be32(vf) & 0xFFFFFFU);
    add_uint_field(n, "version", vf[0], bits, 8);
    add_uint_field(n, "flags", static_cast<uint32_t>(n.flags), bits + 8U, 24);
    return payload_begin + 4;
  }
  return payload_begin;
}

// Step 2.10: the registry row of a box; nullptr for an unknown (or implausible) type.
const RegistryEntry* registry_row(const BoxNodeT& n, const FourCC& type, const BoxContext& ctx) {
  if (!plausible_type(type, ctx)) {
    return nullptr;  // never decode junk types (step 2.9)
  }
  if (sv(type) == "uuid") {
    std::array<uint8_t, 16> usertype{};
    std::copy_n(n.uuid.begin(), std::min(n.uuid.size(), usertype.size()), usertype.begin());
    return lookup_uuid(usertype);
  }
  return lookup(sv(type), ctx);
}

// Step 2.11 for a FullBox other than `meta`: version/flags into the node and as its first two fields. False (with
// CNT-0110) when the payload is shorter than 4 bytes.
bool read_fullbox_header(const io::RandomAccessFile& file, BoxNodeT& n, uint64_t payload_begin, uint64_t end) {
  std::array<uint8_t, 4> vf{};
  if (end - payload_begin < 4 || file.read(payload_begin, 4, vf.data()) != 4) {
    add_diag(n, DiagCode::CNT_0110, "FullBox shorter than 4 bytes" + at(n.file_span->offset()), n.file_span->offset());
    return false;
  }
  const uint64_t bits = static_cast<uint64_t>(n.header_length) * 8U;
  n.version = vf[0];
  n.flags = static_cast<int32_t>(be32(vf) & 0xFFFFFFU);
  add_uint_field(n, "version", vf[0], bits, 8);
  add_uint_field(n, "flags", static_cast<uint32_t>(n.flags), bits + 8U, 24);
  return true;
}

// Step 2.12 hand decoder: the payload (at most 1 MiB) through a BitReader; returns the payload offset where the
// children of a ContainerWithPrefix start (clamped to the payload), else 0.
uint64_t run_hand_decoder(const io::RandomAccessFile& file, BoxNodeT& n, const BoxDecoder& d, BoxContext& ctx,
                          uint64_t payload_begin, uint64_t end, bool fullbox) {
  const uint64_t len = std::min(end - payload_begin, kMaxDecodedPayload);
  std::vector<uint8_t> payload(static_cast<size_t>(len));
  if (len > 0) {  // an empty vector's data() may be null, which read() must not get
    payload.resize(file.read(payload_begin, payload.size(), payload.data()));
  }
  BitReader br(payload);
  const uint64_t base_bits = (static_cast<uint64_t>(n.header_length) + (fullbox ? 4U : 0U)) * 8U;
  FieldBuilder fb(n, n.file_span->offset(), br, base_bits);
  const DecodeOutcome out = run_decoder(d, ctx, br, fb);
  return out.truncated ? end - payload_begin : std::min(out.children_offset, end - payload_begin);
}

// Step 2.10: registry metadata, or CNT-1001 for an unknown type. build_node already marked a truncated box by
// setting its description to "[truncated]"; keep that suffix.
void apply_registry_metadata(BoxNodeT& n, const RegistryEntry* entry, const FourCC& type, const BoxContext& ctx,
                             uint64_t pos) {
  const bool truncated = !n.description.empty();
  if (entry != nullptr) {
    n.spec_ref = entry->spec_ref;
    n.description = std::string(entry->description) + (truncated ? " [truncated]" : "");
  } else if (plausible_type(type, ctx)) {
    n.description = truncated ? "unknown [truncated]" : "unknown";
    add_diag(n, DiagCode::CNT_1001, "unknown box type " + n.fourcc + at(pos), pos);
  }
}

// The context of the children of the box `n` of type `type`: `trun` data offsets are relative to the enclosing `moof`.
BoxContext child_context(const BoxContext& ctx, const FourCC& type, const BoxNodeT& n) {
  BoxContext c = ctx.push(type, &n);
  if (sv(type) == "moof") {
    c.moof_offset = n.file_span->offset();
  }
  return c;
}

// Chapter 8 §8.2.4 rule 3 (issue #631): an unexpanded `moof` keeps only its `mfhd` child (and CNT-0007), so the
// sum-of-spans invariant holds for expanded boxes only. §8.2.4 rule 3: the header walk of a `moof`: only its first
// child, when that is the `mfhd` (which the spec puts first), so the tree carries `mfhd.sequence_number` and correct
// spans. CNT-0007 marks the box unexpanded. Recursion is bounded by kMaxBoxDepth (step 2.1).
// NOLINTNEXTLINE(misc-no-recursion)
void defer_moof(WalkState& st, BoxNodeT& n, uint64_t children_begin, uint64_t end, const BoxContext& ctx) {
  const uint64_t pos = n.file_span->offset();
  std::array<uint8_t, 8> h{};
  if (end - children_begin >= 8 && st.file->read(children_begin, h.size(), h.data()) == h.size()) {
    const uint32_t size = be32(h);
    if (sv(fourcc_at(Span(h).subspan(4, 4))) == "mfhd" && size >= 8 && size <= end - children_begin) {
      n.children = walk_children(st, pos, children_begin, children_begin + size, 1,
                                 child_context(ctx, FourCC{'m', 'o', 'o', 'f'}, n), n.diagnostics);
    }
  }
  add_diag(n, DiagCode::CNT_0007,
           "box at offset " + std::to_string(pos) + " not yet expanded (lazy; call ovt_container_expand)", pos);
  st.deferred->push_back(DeferredBox{pos, ctx});
}

// Steps 2.10-2.13 for one box: registry metadata, FullBox header, the top-level bookkeeping, then the hand decoder,
// children (containers) or the payload span. `ctx` is the sibling context a decoder may update for later siblings.
// Recursion is bounded by kMaxBoxDepth (step 2.1).
// NOLINTNEXTLINE(misc-no-recursion)
void fill_payload(WalkState& st, BoxNodeT& n, const FourCC& type, unsigned depth, BoxContext& ctx) {
  const uint64_t pos = n.file_span->offset();
  const uint64_t end = pos + n.file_span->length();
  uint64_t payload_begin = pos + n.header_length;
  const RegistryEntry* entry = registry_row(n, type, ctx);
  apply_registry_metadata(n, entry, type, ctx, pos);
  BoxKind kind = entry != nullptr ? entry->kind : BoxKind::Leaf;
  if (depth == 0) {
    track_top_level_flags(st, type, n);
  }
  bool decode = entry != nullptr && kind != BoxKind::Opaque;
  if (sv(type) == "meta" && kind != BoxKind::Opaque) {  // §8.2.6 rule 4 replaces step 2.11 for `meta`
    bool leaf = false;
    payload_begin = read_meta_prefix(*st.file, n, payload_begin, end, leaf);
    kind = leaf ? BoxKind::Leaf : BoxKind::Container;
    decode = !leaf;
  } else if (decode && entry->is_fullbox) {  // 2.11
    decode = read_fullbox_header(*st.file, n, payload_begin, end);
    payload_begin += decode ? 4U : 0U;
  }
  const BoxDecoder* dec = decode ? entry->decoder : nullptr;
  uint64_t children_begin = payload_begin;
  if (dec != nullptr && kind != BoxKind::Container) {
    children_begin += run_hand_decoder(*st.file, n, *dec, ctx, payload_begin, end, entry->is_fullbox);
  }
  // 2.12. SPEC-ASSUMPTION(#574): a ContainerWithPrefix without a decoder cannot know where its children start, so it
  // stays a leaf; a decoder's children offset counts from the reader start (after FullBox version/flags). A decoder
  // that returns a children offset inside the payload is walked whatever the row's kind: a "<parent>/*" decoder
  // (the `stsd/*` sample entries) reaches unlisted fourccs through a row that has no kind of its own.
  const bool prefix_children = dec != nullptr && children_begin > payload_begin && children_begin < end;
  const bool walk = decode && (kind == BoxKind::Container || (kind == BoxKind::ContainerWithPrefix && dec != nullptr) ||
                               prefix_children);
  if (walk && depth == 0 && st.deferred != nullptr && sv(type) == "moof") {
    defer_moof(st, n, children_begin, end, ctx);
  } else if (walk) {
    n.children = walk_children(st, pos, children_begin, end, depth + 1, child_context(ctx, type, n), n.diagnostics);
  } else if (dec == nullptr) {
    n.payload_span = std::make_unique<fb::ByteSpan>(payload_begin, end > payload_begin ? end - payload_begin : 0);
  }
}

}  // namespace

// Recursion is bounded by kMaxBoxDepth (step 2.1).
// NOLINTNEXTLINE(misc-no-recursion)
BoxNodes walk_children(WalkState& st, uint64_t parent_offset, uint64_t parent_payload_begin, uint64_t parent_end,
                       unsigned depth, const BoxContext& ctx, DiagList& parent_diags) {
  BoxNodes out;
  if (depth > kMaxBoxDepth) {  // 2.1
    parent_diags.push_back(
        make_diag(DiagCode::CNT_0105, "box nesting deeper than 32", parent_payload_begin - parent_offset));
    return out;
  }
  const io::RandomAccessFile& file = *st.file;
  // SPEC-ASSUMPTION(#574): decoders update this copy for the siblings that follow and their subtrees (§8.2.9).
  BoxContext sib_ctx = ctx;
  uint64_t pos = parent_payload_begin;
  while (pos < parent_end) {  // 2.
    const uint64_t remaining = parent_end - pos;
    // SPEC-ASSUMPTION(#323): node bound. Once kMaxBoxNodes BoxNodes exist the rest of every open parent becomes one
    // trailing-bytes node (so the sum of child spans still equals the parent size); CNT-0111 is emitted once, on the
    // parent that hit the bound. The trailing nodes themselves are not counted against the bound.
    if (st.nodes >= kMaxBoxNodes) {
      if (!st.node_bound_hit) {
        st.node_bound_hit = true;
        parent_diags.push_back(
            make_diag(DiagCode::CNT_0111, "more than 1000000 boxes; remaining bytes not walked", pos - parent_offset));
      }
      auto n = make_node(pos, remaining);
      n->payload_span = std::make_unique<fb::ByteSpan>(pos, remaining);
      n->description = "trailing bytes";
      out.push_back(std::move(n));
      break;
    }
    std::array<uint8_t, kHeaderProbe> buf{};
    const size_t got =
        remaining < 8 ? 0
                      : file.read(pos, static_cast<size_t>(std::min<uint64_t>(remaining, kHeaderProbe)), buf.data());
    if (got < 8) {  // 2.1 too few bytes for a header (or the file shrank under us)
      out.push_back(
          trailing_node(pos, parent_end, DiagCode::CNT_0101,
                        std::to_string(remaining) + " trailing bytes that cannot hold a box header" + at(pos)));
      break;
    }
    const Span hdr = Span(buf).first(got);
    const Header h = read_header(hdr, pos, remaining, depth, sib_ctx);
    if (h.wide) {  // 2.3
      ++st.nodes;
      auto n = make_node(pos, 8);
      n->fourcc = "wide";
      n->header_length = 8;
      out.push_back(std::move(n));
      pos += 8;
      continue;
    }
    if (h.stop) {
      out.push_back(trailing_node(pos, parent_end, h.finding, h.finding_msg));
      break;
    }
    auto n = build_node(h, hdr, pos, parent_end, sib_ctx);
    ++st.nodes;
    const uint64_t end = pos + n->file_span->length();
    fill_payload(st, *n, h.type, depth, sib_ctx);
    out.push_back(std::move(n));
    pos = end;  // 2.14
  }
  return out;
}

WalkResult walk(const io::RandomAccessFile& file, std::vector<DeferredBox>* deferred) {
  WalkResult r;
  const uint64_t size = file.size();
  if (size < 8) {
    r.diagnostics.push_back(
        make_diag(DiagCode::CNT_0006, "file is shorter than 8 bytes (" + std::to_string(size) + ")", 0));
    return r;
  }
  std::array<uint8_t, 8> head{};
  if (file.read(0, head.size(), head.data()) != head.size() ||
      !plausible_type(fourcc_at(Span(head).subspan(4, 4)), BoxContext{})) {
    r.diagnostics.push_back(make_diag(DiagCode::CNT_0006, "first 8 bytes are not a plausible box header", 0));
    return r;
  }

  WalkState st;
  st.file = &file;
  st.deferred = deferred;
  DiagList top_diags;  // CNT-0105 cannot occur at depth 0
  r.top_level = walk_children(st, 0, 0, size, 0, r.root_ctx, top_diags);
  for (auto& d : top_diags) {
    r.diagnostics.push_back(std::move(d));
  }

  const auto name_at = [&](size_t i) { return r.top_level[i]->fourcc; };
  const bool first_is_ftyp = !r.top_level.empty() && (name_at(0) == "ftyp" || name_at(0) == "styp");
  if (!first_is_ftyp) {
    r.diagnostics.push_back(make_diag(DiagCode::CNT_0002, "first box is not `ftyp`/`styp`", 0));
    if (!r.top_level.empty()) {
      const std::string& f = name_at(0);
      r.root_ctx.quicktime = f == "moov" || f == "mdat" || f == "wide" || f == "free";
    }
  }
  if (deferred != nullptr && st.moof_count <= kEagerMoofLimit) {  // §8.2.4 rule 3 exception
    for (const DeferredBox& d : *deferred) {
      const auto it =
          std::ranges::find_if(r.top_level, [&](const auto& b) { return b->file_span->offset() == d.offset; });
      expand_node(file, **it, d.ctx);
    }
    deferred->clear();
  }
  r.root_ctx.saw_moov = st.saw_moov;
  r.root_ctx.mdat_before_moov = st.mdat_before_moov;
  if (!st.saw_moov && !st.saw_moof) {
    r.diagnostics.push_back(make_diag(DiagCode::CNT_0004, "no movie or fragment (`moov`/`moof`)", 0));
  } else if (!st.saw_moov && st.saw_moof) {
    r.root_ctx.fragment_only = true;
    r.diagnostics.push_back(
        make_diag(DiagCode::CNT_0005, "fragment-only stream; open with an initialisation segment to get tracks", 0));
  }
  return r;
}

void expand_node(const io::RandomAccessFile& file, fb::BoxNodeT& node, const BoxContext& ctx) {
  const uint64_t pos = node.file_span->offset();
  const uint64_t end = pos + node.file_span->length();
  std::erase_if(node.diagnostics, [](const auto& d) { return d->code == "CNT-0007"; });
  WalkState st;  // a fresh node bound per expansion (§8.2.3 step 1)
  st.file = &file;
  node.children = walk_children(st, pos, pos + node.header_length, end, 1,
                                child_context(ctx, FourCC{'m', 'o', 'o', 'f'}, node), node.diagnostics);
}

// Chapter 8 §8.2.4 rule 4 (issue #631): the located offset is a hint the container reader keeps only if the top-level
// walk finds an `mfra` there.
std::optional<uint64_t> locate_mfra(const io::RandomAccessFile& file) {
  constexpr uint64_t kMfroSize = 16;  // size, 'mfro', version/flags, parent_size
  const uint64_t size = file.size();
  if (size < kMfroSize + 8) {
    return std::nullopt;
  }
  std::array<uint8_t, kMfroSize> tail{};
  if (file.read(size - kMfroSize, tail.size(), tail.data()) != tail.size() || be32(tail) != kMfroSize ||
      sv(fourcc_at(Span(tail).subspan(4, 4))) != "mfro") {
    return std::nullopt;
  }
  const uint64_t mfra_size = be32(Span(tail).subspan(12, 4));
  if (mfra_size < kMfroSize + 8 || mfra_size > size) {
    return std::nullopt;
  }
  const uint64_t offset = size - mfra_size;
  std::array<uint8_t, 8> h{};
  if (file.read(offset, h.size(), h.data()) != h.size() || be32(h) != mfra_size ||
      sv(fourcc_at(Span(h).subspan(4, 4))) != "mfra") {
    return std::nullopt;
  }
  return offset;
}

}  // namespace ovt::container::isobmff
