// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// FieldBuilder: the toolkit hand decoders read box payloads with (chapter 08 §8.2.8, WI-1.202). Every reader pulls
// bits from the BitReader, appends one `Field` to the box node (or to the open group) and records its span, in
// bits, relative to the box start. Derivations (dates, durations, languages, matrices, fixed point) are attached
// as text.
//
// SPEC-ASSUMPTION(#415): ovt.fb.Field (core/schema/ovt_container.fbs) has no `derived`, `enum_name` or
// `array_index` member and the schema cannot change here. The derived / enum text goes into `Field.svalue` of every
// non-String field, `enum_name()` also sets `kind = Enum`, and an array element is named `<name>[<index>]`.

#ifndef OVT_CONTAINER_ISOBMFF_FIELD_BUILDER_H
#define OVT_CONTAINER_ISOBMFF_FIELD_BUILDER_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "ovt/cxx/bitreader.hpp"
#include "ovt_container_generated.h"

namespace ovt::container::isobmff {

class FieldBuilder {
 public:
  // Span bit offsets = `payload_bit_base` + bits read since construction. The default base is the box header
  // (`node.header_length * 8`); a walker that already read FullBox version/flags passes `header_length * 8 + 32`.
  FieldBuilder(fb::BoxNodeT& node, uint64_t box_file_offset, BitReader& br);
  FieldBuilder(fb::BoxNodeT& node, uint64_t box_file_offset, BitReader& br, uint64_t payload_bit_base);

  FieldBuilder(const FieldBuilder&) = delete;
  FieldBuilder& operator=(const FieldBuilder&) = delete;

  // ---- integers (span = the bits consumed by the call) ----
  uint64_t u(std::string_view name, unsigned nbits, std::string_view descriptor = "");  // descriptor is documentation
  int64_t s(std::string_view name, unsigned nbits);
  uint8_t u8(std::string_view name);
  uint16_t u16(std::string_view name);
  uint32_t u24(std::string_view name);
  uint32_t u32(std::string_view name);
  uint64_t u64(std::string_view name);
  int16_t i16(std::string_view name);
  int32_t i32(std::string_view name);
  int64_t i64(std::string_view name);
  uint64_t u32or64(std::string_view name, bool wide);  // version-dependent widths
  int64_t i32or64(std::string_view name, bool wide);

  // ---- fixed point: kind Fixed, ivalue = raw, derived = decimal with up to 4 places ("1.0", "0.5") ----
  // 16.16 and 8.8 are signed by default (mvhd.rate, volume); `is_signed = false` for tkhd.width/height.
  double fixed16_16(std::string_view name, bool is_signed = true);
  double fixed8_8(std::string_view name, bool is_signed = true);
  double fixed2_30(std::string_view name);

  // ---- text and blobs ----
  std::array<char, 4> fourcc(std::string_view name);  // kind String; uvalue = the four bytes big-endian
  // Reads through the NUL (or `max_bytes` / end of data); the span includes the NUL.
  std::string str_nul(std::string_view name, uint64_t max_bytes);
  // Reads exactly `nbytes`; the value ends at the first NUL.
  std::string str_fixed(std::string_view name, uint32_t nbytes);
  // kind Bytes. n <= 1 MiB: all bytes kept in `Field.bytes`; larger: the first 4096 only, uvalue = n and svalue says
  // so. The returned view always covers all n bytes of the reader's data.
  std::span<const uint8_t> bytes(std::string_view name, uint64_t n);

  // ---- derived text ----
  std::string lang_packed(std::string_view name);  // 1 pad bit + 3 x 5 bits -> "eng"
  // Records a field named "reserved". CNT-1007 (Info) when the value differs from `expected`. nbits may exceed 64
  // (then `expected` must be 0).
  void reserved(unsigned nbits, uint64_t expected = 0);
  // u32/u64 seconds since 1904-01-01T00:00:00Z; derived "YYYY-MM-DDThh:mm:ssZ". Returns the derived text.
  std::string datetime1904(std::string_view name, bool wide);
  // u32/u64 in `timescale` units; derived "x.xxx s", or "unknown" when all ones. Returns the raw value.
  uint64_t duration(std::string_view name, bool wide, uint32_t timescale);
  // 9 x i32 (16.16 for a,b,c,d,x,y; 2.30 for u,v,w). A parent field `name` with children `name[0]`..`name[8]`;
  // derived "identity", "rotation 90°", "rotation 180°", "rotation 270°" or "custom".
  void matrix(std::string_view name = "matrix");

  // ---- grouping ----
  FieldBuilder& group(std::string_view name);  // push a parent field (struct)
  // Parent of an array: a group with uvalue = `count` (svalue "see Sample Table view" when count > 1024). Read only
  // the elements materialise() accepts through array_element().
  FieldBuilder& array(std::string_view name, uint64_t count);
  FieldBuilder& array_element(std::string_view name, uint32_t index);  // push a child named "<name>[<index>]"
  void end();                                                          // pop; sets the group's span length

  // ---- annotate the most recent field ----
  void derived(std::string_view text);  // no-op on String fields (their svalue is the value)
  void enum_name(std::string_view text);
  void spec_ref(std::string_view text);
  // Diagnostic on the box. span = the last field's span (or an empty span at the reader position).
  void diag(Severity sev, std::string_view code, std::string_view msg);

  [[nodiscard]] BitReader& reader() { return *br_; }
  [[nodiscard]] uint64_t bytes_consumed() const;  // from the payload begin (construction)
  [[nodiscard]] uint64_t box_file_offset() const noexcept { return box_off_; }
  [[nodiscard]] fb::BoxNodeT& node() noexcept { return *node_; }

  // Large-array policy (§8.2.5): true when `index` should be materialised as a Field.
  [[nodiscard]] static bool materialise(uint32_t index, uint32_t count) noexcept {
    return count <= 1024 || index < 64 || static_cast<uint64_t>(index) + 16 >= count;
  }

  // Walker side: a ParseError thrown inside a decoder becomes CNT-1002 on the box; earlier fields are kept.
  void report_truncated(const ParseError& e);

 private:
  fb::FieldT& add(std::string_view name, fb::SyntaxValueKind kind, uint64_t start_pos);
  [[nodiscard]] uint64_t rel(uint64_t abs_pos) const noexcept { return base_ + (abs_pos - origin_); }
  [[nodiscard]] std::vector<std::unique_ptr<fb::FieldT>>& sink();
  fb::FieldT& integer(std::string_view name, fb::SyntaxValueKind kind, unsigned nbits);
  double fixed(std::string_view name, unsigned nbits, unsigned frac_bits, bool is_signed);
  void push_diag(Severity sev, std::string_view code, std::string_view msg, uint64_t bit_offset, uint32_t bit_length);

  fb::BoxNodeT* node_;
  uint64_t box_off_;
  BitReader* br_;
  uint64_t base_;
  uint64_t origin_;
  std::vector<fb::FieldT*> stack_;  // open groups, outermost first
  std::vector<uint64_t> stack_start_;
  fb::FieldT* last_ = nullptr;
};

}  // namespace ovt::container::isobmff

#endif  // OVT_CONTAINER_ISOBMFF_FIELD_BUILDER_H
