// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 timing_info(), decoder_model_info() and operating_parameters_info() (chapter 7 §7.3.6 rows 3–5;
// WI-1.116).

#include <fmt/format.h>

#include <cstdint>

#include "ovt/cxx/syntax/av1/sequence_header.hpp"

namespace ovt::syntax::av1 {

TimingInfo parse_timing_info(BitReader& br, SyntaxBuilder& b) {
  TimingInfo t;
  const auto start = br.mark();
  auto s = b.scoped("timing_info", "AV1 5.5.3");
  t.num_units_in_display_tick = static_cast<uint32_t>(b.read_u("num_units_in_display_tick", 32));
  t.time_scale = static_cast<uint32_t>(b.read_u("time_scale", 32));
  t.equal_picture_interval = b.read_flag("equal_picture_interval");
  if (t.equal_picture_interval) {
    t.num_ticks_per_picture_minus_1 = b.read_uvlc("num_ticks_per_picture_minus_1");
    const double ticks =
        static_cast<double>(t.num_units_in_display_tick) * (static_cast<double>(t.num_ticks_per_picture_minus_1) + 1.0);
    if (ticks > 0.0) {
      t.frame_rate = static_cast<double>(t.time_scale) / ticks;
      b.derived("frame_rate", fmt::format("frame_rate = {:.6g} fps", t.frame_rate));
    }
  }
  t.span = br.span_since(start);
  return t;
}

DecoderModelInfo parse_decoder_model_info(BitReader& br, SyntaxBuilder& b) {
  DecoderModelInfo d;
  const auto start = br.mark();
  auto s = b.scoped("decoder_model_info", "AV1 5.5.4");
  d.buffer_delay_length_minus_1 = static_cast<uint8_t>(b.read_u("buffer_delay_length_minus_1", 5));
  d.num_units_in_decoding_tick = static_cast<uint32_t>(b.read_u("num_units_in_decoding_tick", 32));
  d.buffer_removal_time_length_minus_1 = static_cast<uint8_t>(b.read_u("buffer_removal_time_length_minus_1", 5));
  d.frame_presentation_time_length_minus_1 =
      static_cast<uint8_t>(b.read_u("frame_presentation_time_length_minus_1", 5));
  d.span = br.span_since(start);
  return d;
}

OperatingParametersInfo parse_operating_parameters_info(BitReader& br, SyntaxBuilder& b, int op,
                                                        const DecoderModelInfo& model) {
  OperatingParametersInfo o;
  const auto start = br.mark();
  b.begin_array_element("operating_parameters_info", op, "AV1 5.5.5");
  const unsigned n = model.buffer_delay_length_minus_1 + 1U;
  o.decoder_buffer_delay = static_cast<uint32_t>(b.read_u("decoder_buffer_delay", n));
  o.encoder_buffer_delay = static_cast<uint32_t>(b.read_u("encoder_buffer_delay", n));
  o.low_delay_mode_flag = b.read_flag("low_delay_mode_flag");
  b.end_array_element();
  o.span = br.span_since(start);
  return o;
}

}  // namespace ovt::syntax::av1
