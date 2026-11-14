// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// §10.7.9 readback strategy (WI-4.452): the measurement passes run after the display render in the same submission,
// only while a consumer calls measure(); their results come back through two host-readable buffers that measure()
// polls without blocking. measure() returns the newest completed result, with measured_frame naming its frame.
// WI-4.453: the same M and Q also feed the false-colour and gamut-clip views (§10.7.5, §10.7.8), computed once per
// render.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/renderer.h>

#include "measure/measure.h"

namespace ovt::render::detail {

Measurer::~Measurer() {
  destroy();
}

bool Measurer::fail(const std::string& what) {
  error_ = "measurement unavailable: " + what;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  (void)std::fprintf(stderr, "[render] %s\n", error_.c_str());
  return false;
}

void Measurer::report(const std::string& err) {
  if (err != error_) {  // once per distinct failure, not once per frame
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)std::fprintf(stderr, "[render] %s\n", err.c_str());
  }
  error_ = err;
}

bool Measurer::init(pl_log log, pl_gpu gpu) {
  destroy();
  error_.clear();
  gpu_ = gpu;
  // Capabilities of the passes: float render targets for M, Q and the reduction chain, compute shaders with 4 KiB
  // of shared memory and storage buffers for the histogram.
  constexpr auto kTarget = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE);
  pl_fmt half = pl_find_named_fmt(gpu, "rgba16hf");
  pl_fmt full = pl_find_named_fmt(gpu, "rgba32f");
  if (half == nullptr || (half->caps & kTarget) != kTarget) {
    return fail("RGBA16F is not renderable and sampleable");
  }
  if (full == nullptr || (full->caps & kTarget) != kTarget || (full->caps & PL_FMT_CAP_HOST_READABLE) == 0) {
    return fail("RGBA32F is not renderable, sampleable and host-readable");
  }
  // SPEC-ASSUMPTION(#473): the histogram needs compute shaders; GPUs without them (GLES 3.0) get no measurements.
  if (!gpu->glsl.compute || gpu->glsl.max_shmem_size < kHistogramBytes || gpu->limits.max_ssbo_size < kReadbackBytes) {
    return fail("compute shaders with storage buffers and 4 KiB shared memory are required");
  }

  renderer_ = pl_renderer_create(log, gpu);
  dispatch_ = pl_dispatch_create(log, gpu);
  if (renderer_ == nullptr || dispatch_ == nullptr) {
    return fail("pl_renderer_create or pl_dispatch_create failed");
  }
  for (Readback& rb : readbacks_) {
    struct pl_buf_params bp = {};
    bp.size = kReadbackBytes;
    bp.storable = true;
    bp.host_writable = true;
    bp.host_readable = true;
    rb.buf = pl_buf_create(gpu, &bp);
    if (rb.buf == nullptr) {
      return fail("cannot create the readback buffers");
    }
  }
  available_ = true;
  return true;
}

void Measurer::destroy() {
  available_ = false;
  if (gpu_ != nullptr) {
    for (Readback& rb : readbacks_) {
      pl_buf_destroy(gpu_, &rb.buf);
      rb = Readback{};
    }
    destroy_levels(gpu_, levels_);
    pl_tex_destroy(gpu_, &view_);
    pl_tex_destroy(gpu_, &q_);
    pl_tex_destroy(gpu_, &m_);
  }
  pl_dispatch_destroy(&dispatch_);
  pl_renderer_destroy(&renderer_);
  gpu_ = nullptr;
  next_seq_ = 1;
  shown_seq_ = 0;
  renders_since_request_ = 2;
  prepared_ = false;
  last_ = Measurements{};
}

bool Measurer::due() {
  const bool wanted = renders_since_request_ < 2;
  if (renders_since_request_ < 2) {
    ++renders_since_request_;
  }
  return wanted && available_;
}

bool Measurer::prepare(const FrameSlot& slot, std::string& err) {
  if (prepared_) {
    return true;
  }
  if (!render_linear(gpu_, renderer_, slot, &m_, err) || !prep_pass(dispatch_, gpu_, m_, slot.frame.color, &q_, err)) {
    return false;
  }
  prepared_ = true;
  return true;
}

pl_tex Measurer::analysis_view(const FrameSlot& slot, AnalysisView mode) {
  if (!available_ || !slot.valid || mode == AnalysisView::None) {
    return nullptr;
  }
  std::string err;
  if (!prepare(slot, err) || !analysis_pass(dispatch_, gpu_, q_, mode, &view_, err)) {
    report(err);
    return nullptr;
  }
  return view_;
}

void Measurer::run(const FrameSlot& slot) {
  if (!available_ || !slot.valid) {
    return;
  }
  // A free readback buffer; when both are still in flight this frame is not measured (measure() never blocks).
  Readback* rb = nullptr;
  for (Readback& candidate : readbacks_) {
    if (candidate.pending) {
      collect(candidate);
    }
    if (!candidate.pending && (rb == nullptr || candidate.seq < rb->seq)) {
      rb = &candidate;
    }
  }
  if (rb == nullptr) {
    return;
  }

  std::string err;
  if (!prepare(slot, err) || !reduce_chain(dispatch_, gpu_, q_, levels_, err)) {
    report(err);
    return;
  }
  const auto width = static_cast<uint32_t>(q_->params.w);
  const auto height = static_cast<uint32_t>(q_->params.h);
  const uint64_t pixels = static_cast<uint64_t>(width) * height;
  const uint32_t s = subsample_factor(pixels);

  // The buffer is idle (not pending), so this write does not block. The histogram and the counters start from zero.
  static const std::array<uint32_t, kHistogramBins + kCounters> kZero{};
  pl_buf_write(gpu_, rb->buf, 0, kZero.data(), kHistogramBytes + kCountsBytes);
  if (!accumulate_histogram(dispatch_, q_, s, rb->buf, err)) {
    report(err);
    return;
  }
  struct pl_tex_transfer_params tp = {};
  tp.tex = levels_.back();
  tp.buf = rb->buf;
  tp.buf_offset = kTexelOffset;
  if (!pl_tex_download(gpu_, &tp)) {
    report("measurement: download of the reduction result failed");
    return;
  }
  rb->pending = true;
  rb->seq = next_seq_++;
  rb->frame = slot.id;
  rb->pixels = pixels;
  rb->sampled = sampled_extent(width, s) * sampled_extent(height, s);
  error_.clear();
}

void Measurer::collect(Readback& rb) {
  if (!rb.pending || pl_buf_poll(gpu_, rb.buf, 0)) {
    return;  // nothing queued, or still in use by the GPU
  }
  rb.pending = false;
  if (rb.seq <= shown_seq_) {
    return;  // an older frame than the result already shown
  }
  std::array<uint32_t, kHistogramBins> hist{};
  std::array<uint32_t, kCounters> counts{};
  std::array<float, 4> texel{};
  if (!pl_buf_read(gpu_, rb.buf, 0, hist.data(), kHistogramBytes) ||
      !pl_buf_read(gpu_, rb.buf, kCountsOffset, counts.data(), kCountsBytes) ||
      !pl_buf_read(gpu_, rb.buf, kTexelOffset, texel.data(), sizeof(texel))) {
    return;
  }
  Measurements m{};
  m.valid = true;
  m.measured_frame = rb.frame;
  m.max_cll_nits = texel[0];
  m.fall_nits = rb.pixels > 0 ? static_cast<float>(static_cast<double>(texel[1]) / static_cast<double>(rb.pixels)) : 0;
  m.min_nits = texel[2];
  std::memcpy(static_cast<void*>(m.histogram_pq), hist.data(), kHistogramBytes);
  m.sampled_pixels = rb.sampled;
  m.max_cll_p9999_nits = histogram_p9999_nits(hist, rb.sampled);
  // §10.7.5 / §10.7.8 (WI-4.453): over the same sampled pixels as the histogram.
  std::array<float, kFalseColorBands> fractions{};
  for (size_t b = 0; b < fractions.size(); ++b) {
    fractions.at(b) =
        rb.sampled > 0 ? static_cast<float>(static_cast<double>(counts.at(b)) / static_cast<double>(rb.sampled)) : 0;
  }
  static_assert(sizeof(fractions) == sizeof(m.false_color_band_fraction));
  std::memcpy(static_cast<void*>(m.false_color_band_fraction), fractions.data(), sizeof(fractions));
  m.out_of_gamut_709 = counts.at(kCountOutside709);
  m.out_of_gamut_p3 = counts.at(kCountOutsideP3);
  m.gpu_time_ms = 0;  // no GPU timers: libovt-render hides timestamp queries (vk_no_timestamps.cpp)
  last_ = m;
  shown_seq_ = rb.seq;
}

Measurements Measurer::poll() {
  if (available_) {
    for (Readback& rb : readbacks_) {
      collect(rb);
    }
  }
  return last_;
}

}  // namespace ovt::render::detail
