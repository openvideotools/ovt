// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// §10.7.3 histogram (WI-4.452): 1024 PQ bins of max(R,G,B) over the pixels sampled at factor s, accumulated on the
// GPU by render/src/shaders/accumulate.glsl together with the band and gamut counts of WI-4.453, and the
// 99.99th-percentile MaxCLL derived from it on the CPU.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <libplacebo/dispatch.h>
#include <libplacebo/gpu.h>
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/custom.h>

#include "measure/measure.h"
#include "measure_shaders.h"  // generated from render/src/shaders/ (render/CMakeLists.txt)

namespace ovt::render::detail {

namespace {
// ST 2084 constants (Eq. 5.1 / 5.2).
constexpr double kM1 = 0.1593017578125;
constexpr double kM2 = 78.84375;
constexpr double kC1 = 0.8359375;
constexpr double kC2 = 18.8515625;
constexpr double kC3 = 18.6875;
constexpr double kPqPeakNits = 10000.0;
constexpr int kGroupSize = 16;  // accumulate.glsl assumes 16 x 16 = 256 invocations per work group
}  // namespace

uint32_t subsample_factor(uint64_t pixels) {
  // Integer form of ceil(sqrt(pixels / 2073600)): the smallest s with s * s * 2073600 >= pixels.
  uint32_t s = 1;
  while (static_cast<uint64_t>(s) * s * kMaxHistogramSamples < pixels) {
    ++s;
  }
  return s;
}

uint32_t sampled_extent(uint32_t extent, uint32_t s) {
  if (s == 0) {
    return 0;
  }
  return (extent + s - 1) / s;
}

double pq_eotf_nits(double code) {
  const double e = std::pow(std::clamp(code, 0.0, 1.0), 1.0 / kM2);
  const double num = std::max(e - kC1, 0.0);
  const double den = kC2 - (kC3 * e);
  return kPqPeakNits * std::pow(num / den, 1.0 / kM1);
}

double pq_oetf(double nits) {
  const double y = std::clamp(nits / kPqPeakNits, 0.0, 1.0);
  const double p = std::pow(y, kM1);
  return std::pow((kC1 + (kC2 * p)) / (1.0 + (kC3 * p)), kM2);
}

float histogram_p9999_nits(const std::array<uint32_t, kHistogramBins>& histogram, uint32_t sampled) {
  if (sampled == 0) {
    return 0.0F;
  }
  // cumulative >= 0.9999 x sampled, in integers: cumulative x 10000 >= sampled x 9999.
  const uint64_t need = static_cast<uint64_t>(sampled) * 9999U;
  uint64_t cumulative = 0;
  for (uint32_t i = 0; i < kHistogramBins; ++i) {
    cumulative += histogram[i];
    if (cumulative * 10000U >= need) {
      return static_cast<float>(pq_eotf_nits(static_cast<double>(i + 1) / kHistogramBins));
    }
  }
  return static_cast<float>(kPqPeakNits);  // counts short of `sampled`: only possible with a broken readback
}

bool accumulate_histogram(pl_dispatch dp, pl_tex q, uint32_t s, pl_buf hist, std::string& err) {
  const auto step = static_cast<int>(s);
  const std::array<int, 2> grid = {static_cast<int>(sampled_extent(static_cast<uint32_t>(q->params.w), s)),
                                   static_cast<int>(sampled_extent(static_cast<uint32_t>(q->params.h), s))};

  pl_shader sh = pl_dispatch_begin(dp);
  std::array<struct pl_shader_desc, 2> descs = {};
  descs[0].desc.name = "ovt_q";
  descs[0].desc.type = PL_DESC_SAMPLED_TEX;
  descs[0].binding.object = q;
  descs[0].binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
  struct pl_buffer_var bins = {};
  bins.var = pl_var_uint("ovt_hist");
  bins.var.dim_a = static_cast<int>(kHistogramBins);
  bins.layout = pl_std430_layout(0, &bins.var);
  descs[1].desc.name = "ovt_hist_block";  // the block; its member ovt_hist is what the shader indexes
  descs[1].desc.type = PL_DESC_BUF_STORAGE;
  descs[1].desc.access = PL_DESC_ACCESS_READWRITE;
  descs[1].binding.object = hist;
  std::array<struct pl_buffer_var, 2> bufvars = {bins, {}};
  bufvars[1].var = pl_var_uint("ovt_counts");
  bufvars[1].var.dim_a = static_cast<int>(kCounters);
  bufvars[1].layout = pl_std430_layout(kHistogramBins * sizeof(uint32_t), &bufvars[1].var);
  descs[1].buffer_vars = bufvars.data();
  descs[1].num_buffer_vars = static_cast<int>(bufvars.size());
  std::array<struct pl_shader_var, 2> vars = {};
  vars[0].var = pl_var_int("ovt_step");
  vars[0].data = &step;
  vars[1].var = pl_var_ivec2("ovt_grid");
  vars[1].data = grid.data();
  vars[1].dynamic = true;

  struct pl_custom_shader custom = {};
  custom.header = static_cast<const char*>(kShaderAccumulate);
  custom.body = "ovt_accumulate();";
  custom.input = PL_SHADER_SIG_NONE;
  custom.output = PL_SHADER_SIG_NONE;
  custom.descriptors = descs.data();
  custom.num_descriptors = static_cast<int>(descs.size());
  custom.variables = vars.data();
  custom.num_variables = static_cast<int>(vars.size());
  custom.compute = true;
  custom.compute_group_size[0] = kGroupSize;
  custom.compute_group_size[1] = kGroupSize;
  custom.compute_shmem = kHistogramBins * sizeof(uint32_t);
  custom.description = "ovt accumulate (§10.7.3)";
  if (!pl_shader_custom(sh, &custom)) {
    pl_dispatch_abort(dp, &sh);
    err = "measurement: accumulate shader rejected";
    return false;
  }
  struct pl_dispatch_compute_params cparams = {};
  cparams.shader = &sh;
  cparams.dispatch_size[0] = (grid[0] + kGroupSize - 1) / kGroupSize;
  cparams.dispatch_size[1] = (grid[1] + kGroupSize - 1) / kGroupSize;
  cparams.dispatch_size[2] = 1;
  if (!pl_dispatch_compute(dp, &cparams)) {
    err = "measurement: accumulate pass failed";
    return false;
  }
  return true;
}

}  // namespace ovt::render::detail
