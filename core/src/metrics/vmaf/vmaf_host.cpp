// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The libvmaf host plugins (chapter 9 §9.5.1-§9.5.5; WI-3.309). See vmaf_host.h for the contract.
//
//   values (model id)     = [vmaf, <every feature libvmaf computed for the model>]   (vmaf_models.inc outputs)
//   values (collection)   = [bagging, ci_p95_lo, ci_p95_hi, stddev]                   (vmaf_b, vmaf_float_b)
//   options               = {"phone": bool}  (VMAF_MODEL_FLAG_ENABLE_TRANSFORM; single models only)
//   values (feature id)   = the features of one libvmaf extractor (kFeatures)
//   options (feature id)  = kFeatures' options, passed to libvmaf; `vif` also {"float": bool} (float_vif)

#include "metrics/vmaf/vmaf_host.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

extern "C" {
#include <libvmaf/feature.h>
#include <libvmaf/libvmaf.h>
#include <libvmaf/model.h>
#include <libvmaf/picture.h>
}

#include "metrics/host/plugin_registry.h"
#include "ovt/cxx/log.hpp"
#include "ovt_metrics_generated.h"

namespace ovt::metrics::vmaf {

namespace {

constexpr std::string_view kModule = "metrics";

// NOLINTBEGIN(cppcoreguidelines-macro-usage): the generated table is an X-macro list
#define OVT_VMAF_MODEL(id, version, collection, needs_float, ...) \
  ModelInfo{id, version, collection, needs_float, {__VA_ARGS__}},
constexpr std::array kModels = {
#include "metrics/vmaf/vmaf_models.inc"
};
#undef OVT_VMAF_MODEL
// NOLINTEND(cppcoreguidelines-macro-usage)

static_assert(!kModels.empty());

// The feature ids of §9.5.3. Output names are those of the `vmaf --feature <extractor>` JSON; keys are the names the
// libvmaf feature collector holds them under (default options).
// The ranges are libvmaf 3.0.0 cambi.c's option table.
constexpr std::array kCambiOptions = {
    FeatureOption{"enc_bitdepth", OptionType::Int, 6, 16},
    FeatureOption{"enc_height", OptionType::Int, 150, 7680},
    FeatureOption{"enc_width", OptionType::Int, 180, 7680},
    FeatureOption{"eotf", OptionType::String},
    FeatureOption{"full_ref", OptionType::Bool},
    FeatureOption{"heatmaps_path", OptionType::String},
    FeatureOption{"max_log_contrast", OptionType::Int, 0, 5},
    FeatureOption{"src_height", OptionType::Int, 200, 4320},
    FeatureOption{"src_width", OptionType::Int, 320, 7680},
    FeatureOption{"topk", OptionType::Double, 0.0001, 1.0},
    FeatureOption{"tvi_threshold", OptionType::Double, 0.0001, 1.0},
    FeatureOption{"window_size", OptionType::Int, 15, 127},
};
constexpr std::array<std::array<const char*, 2>, 2> kVmafPsnrFixed = {
    {{"enable_chroma", "true"}, {"enable_apsnr", "true"}}};

// SPEC-ASSUMPTION(#460): the integer VIF extractor of libvmaf 3.0.0 is named "vif"; "integer_vif" is only a feature
// name, so vmaf_use_feature(ctx, "integer_vif", ...) fails. `cambi` is NoReference (full_ref=true is unsupported).
constexpr std::array kFeatures = {
    FeatureInfo{"cambi",
                "CAMBI (banding, libvmaf)",
                "cambi",
                nullptr,
                true,
                false,
                0.0,
                24.0,
                {"cambi"},
                {"cambi"},
                {},
                kCambiOptions,
                {}},
    FeatureInfo{"ciede2000",
                "CIEDE2000 (libvmaf)",
                "ciede",
                nullptr,
                false,
                true,
                0.0,
                100.0,
                {"ciede2000"},
                {"ciede2000"},
                {},
                {},
                {}},
    FeatureInfo{"ms_ssim",
                "MS-SSIM (libvmaf)",
                "float_ms_ssim",
                nullptr,
                false,
                true,
                0.0,
                1.0,
                {"float_ms_ssim"},
                {"float_ms_ssim"},
                {},
                {},
                {}},
    FeatureInfo{"psnr_hvs",
                "PSNR-HVS (libvmaf)",
                "psnr_hvs",
                nullptr,
                false,
                true,
                0.0,
                100.0,
                {"psnr_hvs", "psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr"},
                {"psnr_hvs", "psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr"},
                {},
                {},
                {}},
    FeatureInfo{"vif",
                "VIF (libvmaf)",
                "vif",
                "float_vif",
                false,
                true,
                0.0,
                1.0,
                {"integer_vif_scale0", "integer_vif_scale1", "integer_vif_scale2", "integer_vif_scale3"},
                {"VMAF_integer_feature_vif_scale0_score", "VMAF_integer_feature_vif_scale1_score",
                 "VMAF_integer_feature_vif_scale2_score", "VMAF_integer_feature_vif_scale3_score"},
                {"VMAF_feature_vif_scale0_score", "VMAF_feature_vif_scale1_score", "VMAF_feature_vif_scale2_score",
                 "VMAF_feature_vif_scale3_score"},
                {},
                {}},
    FeatureInfo{"vmaf_psnr",
                "PSNR (libvmaf)",
                "psnr",
                nullptr,
                false,
                true,
                0.0,
                108.0,
                {"psnr_y", "psnr_cb", "psnr_cr"},
                {"psnr_y", "psnr_cb", "psnr_cr"},
                {},
                {},
                kVmafPsnrFixed},
};

ovt_status from_errno(int err) noexcept {
  if (err == -ENOMEM) {
    return OVT_E_NOMEM;
  }
  if (err == -EINVAL) {
    return OVT_E_INVALID_ARG;
  }
  return OVT_E_PLUGIN;
}

// RAII owners of the libvmaf objects of one instance.
struct ModelHandles {
  VmafModel* model = nullptr;
  VmafModelCollection* collection = nullptr;
  ModelHandles() = default;
  ModelHandles(const ModelHandles&) = delete;
  ModelHandles& operator=(const ModelHandles&) = delete;
  ModelHandles(ModelHandles&&) = delete;
  ModelHandles& operator=(ModelHandles&&) = delete;
  ~ModelHandles() {
    if (collection != nullptr) {
      vmaf_model_collection_destroy(collection);
    }
    if (model != nullptr) {
      vmaf_model_destroy(model);
    }
  }
};

// Loads the built-in model (or collection) under the score name "vmaf". 0 or a negative errno.
int load_model(const ModelInfo& info, uint64_t flags, ModelHandles& h) {
  VmafModelConfig cfg{};
  cfg.name = "vmaf";
  cfg.flags = flags;
  if (info.collection) {
    return vmaf_model_collection_load(&h.model, &h.collection, &cfg, info.version);
  }
  return vmaf_model_load(&h.model, &cfg, info.version);
}

// The fields of a libvmaf id's MetricDescriptor that differ between ids.
struct DescriptorFields {
  const char* id;
  std::string display_name;
  std::span<const char* const> outputs;
  bool no_reference;
  bool higher_is_better;
  double range_min;
  double range_max;
};

std::vector<uint8_t> build_descriptor(const DescriptorFields& f) {
  flatbuffers::FlatBufferBuilder fbb(1024);
  const auto id = fbb.CreateString(f.id);
  const auto name = fbb.CreateString(f.display_name);
  const std::vector<std::string> outs(f.outputs.begin(), f.outputs.end());
  const auto outputs = fbb.CreateVectorOfStrings(outs);
  const auto license = fbb.CreateString("BSD-2-Clause-Patent");
  const auto version = fbb.CreateString(vmaf_version());
  ovt::fb::MetricDescriptorBuilder d(fbb);
  d.add_id(id);
  d.add_display_name(name);
  d.add_kind(f.no_reference ? ovt::fb::MetricKind::NoReference : ovt::fb::MetricKind::FullReference);
  d.add_domain(ovt::fb::WorkingDomain::CodeValue);
  d.add_per_plane(false);
  d.add_higher_is_better(f.higher_is_better);
  d.add_range_min(f.range_min);
  d.add_range_max(f.range_max);
  d.add_temporal_unit_frames(1);
  d.add_outputs(outputs);
  d.add_license(license);
  d.add_version(version);
  fbb.Finish(d.Finish());
  const uint8_t* p = fbb.GetBufferPointer();
  return {p, p + fbb.GetSize()};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
}

std::vector<uint8_t> build_descriptor(const ModelInfo& info) {
  return build_descriptor({info.id, std::string("VMAF (") + info.version + ")",
                           std::span(info.outputs.data(), info.output_count()), false, true, 0.0, 100.0});
}

std::vector<uint8_t> build_descriptor(const FeatureInfo& info) {
  return build_descriptor({info.id, info.display_name, std::span(info.outputs.data(), info.output_count()),
                           info.no_reference, info.higher_is_better, info.range_min, info.range_max});
}

// True when libvmaf knows the feature extractor `name`.
bool extractor_exists(const char* name) noexcept {
  VmafContext* ctx = nullptr;
  VmafConfiguration cfg{};
  cfg.log_level = VMAF_LOG_LEVEL_NONE;
  if (vmaf_init(&ctx, cfg) != 0) {
    return false;
  }
  const bool ok = vmaf_use_feature(ctx, name, nullptr) == 0;
  vmaf_close(ctx);
  return ok;
}

// One libvmaf metric instance (the ovt_Metric of the ABI; a distinct type so that it never clashes with other
// in-tree metrics' definitions of the opaque struct).
struct Instance {
  const ModelInfo* model = nullptr;      // a model id, or
  const FeatureInfo* feature = nullptr;  // a feature id
  bool use_float = false;                // feature id with {"float": true}
  ModelHandles handles;
  VmafContext* ctx = nullptr;
  std::vector<ovt_FrameId> ids;   // context index -> frame id (§9.5.4)
  std::vector<std::string> keys;  // model id: feature-collector key of each output (resolve_keys), once resolved
  bool flushed = false;
  // Geometry fixed by the first frame.
  VmafPixelFormat fmt = VMAF_PIX_FMT_UNKNOWN;
  uint8_t bit_depth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::unique_ptr<flatbuffers::FlatBufferBuilder> last;  // the FrameScore of the last call (rule 4)

  Instance() = default;
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  Instance(Instance&&) = delete;
  Instance& operator=(Instance&&) = delete;
  ~Instance() {
    if (ctx != nullptr) {
      vmaf_close(ctx);
    }
  }
};

Instance* as_instance(ovt_Metric* m) noexcept {
  return reinterpret_cast<Instance*>(m);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): opaque handle
}

bool parse_model_options(const char* options_json, const ModelInfo& info, uint64_t& flags) {
  const auto j = nlohmann::json::parse(options_json, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    return false;
  }
  flags = VMAF_MODEL_FLAGS_DEFAULT;
  for (const auto& [key, value] : j.items()) {
    if (key == "phone" && !info.collection && value.is_boolean()) {
      if (value.get<bool>()) {
        flags |= VMAF_MODEL_FLAG_ENABLE_TRANSFORM;
      }
      continue;
    }
    return false;  // unknown key or wrong type (rule 6)
  }
  return true;
}

int init_context(Instance& inst) {
  VmafConfiguration cfg{};
  cfg.log_level = VMAF_LOG_LEVEL_WARNING;
  cfg.n_threads = 0;  // SPEC-ASSUMPTION(#394): synchronous, see vmaf_host.h
  cfg.n_subsample = 1;
  cfg.cpumask = 0;
  return vmaf_init(&inst.ctx, cfg);
}

// The libvmaf dictionary value of one option, or false when the JSON type does not match or a number is outside
// the option's range (libvmaf would reject it too, but leaks its extractor context on that path).
bool option_value(const nlohmann::json& v, const FeatureOption& opt, std::string& out) {
  const auto in_range = [&opt](double x) { return x >= opt.min && x <= opt.max; };
  switch (opt.type) {
    case OptionType::Bool:
      if (!v.is_boolean()) {
        return false;
      }
      out = v.get<bool>() ? "true" : "false";
      return true;
    case OptionType::Int:
      if (!v.is_number_integer() || !in_range(v.get<double>())) {
        return false;
      }
      out = std::to_string(v.get<int64_t>());
      return true;
    case OptionType::Double: {
      if (!v.is_number() || !in_range(v.get<double>())) {
        return false;
      }
      // %.17g round-trips a double; std::to_chars(double) needs macOS 13.3, above the deployment target.
      std::array<char, 32> buf{};
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::snprintf(buf.data(), buf.size(), "%.17g", v.get<double>());
      out = buf.data();
      return true;
    }
    case OptionType::String:
      if (!v.is_string()) {
        return false;
      }
      out = v.get<std::string>();
      return true;
  }
  return false;
}

// Parses the options of a feature id: the libvmaf dictionary entries (fixed ones first) and the float switch.
// OVT_OK, OVT_E_INVALID_ARG (unknown key, wrong type, rule 6) or OVT_E_UNSUPPORTED.
ovt_status parse_feature_options(const char* options_json, const FeatureInfo& info, bool& use_float,
                                 std::vector<std::pair<std::string, std::string>>& entries) {
  const auto j = nlohmann::json::parse(options_json, nullptr, /*allow_exceptions=*/false);
  if (!j.is_object()) {
    return OVT_E_INVALID_ARG;
  }
  use_float = false;
  for (const auto& kv : info.fixed) {
    entries.emplace_back(kv[0], kv[1]);
  }
  for (const auto& [key, value] : j.items()) {
    if (key == "float" && info.float_extractor != nullptr && value.is_boolean()) {
      use_float = value.get<bool>();
      continue;
    }
    const auto opt = std::find_if(info.options.begin(), info.options.end(),
                                  [&key](const FeatureOption& o) { return key == o.name; });
    std::string text;
    if (opt == info.options.end() || !option_value(value, *opt, text)) {
      return OVT_E_INVALID_ARG;
    }
    if (key == "full_ref" && text == "true") {
      return OVT_E_UNSUPPORTED;  // SPEC-ASSUMPTION(#460): cambi is NoReference only
    }
    if (key == "eotf" && text != "bt1886" && text != "pq") {
      return OVT_E_INVALID_ARG;
    }
    entries.emplace_back(key, std::move(text));
  }
  return OVT_OK;
}

ovt_status create_feature_instance(const FeatureInfo& info, const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    bool use_float = false;
    std::vector<std::pair<std::string, std::string>> entries;
    if (const ovt_status st = parse_feature_options(options_json, info, use_float, entries); st != OVT_OK) {
      return st;
    }
    const char* extractor = use_float ? info.float_extractor : info.extractor;
    // vmaf_use_feature() leaks the dictionary of an unknown extractor; probe first.
    if (!extractor_exists(extractor)) {
      return OVT_E_UNSUPPORTED;
    }
    auto inst = std::make_unique<Instance>();
    inst->feature = &info;
    inst->use_float = use_float;
    if (const int err = init_context(*inst); err != 0) {
      return from_errno(err);
    }
    VmafFeatureDictionary* dict = nullptr;
    for (const auto& [key, value] : entries) {
      if (const int err = vmaf_feature_dictionary_set(&dict, key.c_str(), value.c_str()); err != 0) {
        vmaf_feature_dictionary_free(&dict);
        return from_errno(err);
      }
    }
    // libvmaf takes the dictionary (it copies and frees it).
    if (const int err = vmaf_use_feature(inst->ctx, extractor, dict); err != 0) {
      return from_errno(err);
    }
    *out = reinterpret_cast<ovt_Metric*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): opaque handle
        inst.release());
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

ovt_status create_instance(const ModelInfo& info, const char* options_json, ovt_Metric** out) noexcept {
  if (options_json == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = nullptr;
  try {
    uint64_t flags = 0;
    if (!parse_model_options(options_json, info, flags)) {
      return OVT_E_INVALID_ARG;
    }
    auto inst = std::make_unique<Instance>();
    inst->model = &info;
    if (const int err = load_model(info, flags, inst->handles); err != 0) {
      return err == -ENOMEM ? OVT_E_NOMEM : OVT_E_UNSUPPORTED;
    }
    if (const int err = init_context(*inst); err != 0) {
      return from_errno(err);
    }
    const int err = info.collection ? vmaf_use_features_from_model_collection(inst->ctx, inst->handles.collection)
                                    : vmaf_use_features_from_model(inst->ctx, inst->handles.model);
    if (err != 0) {
      return from_errno(err);
    }
    *out = reinterpret_cast<ovt_Metric*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): opaque handle
        inst.release());
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

void destroy_instance(ovt_Metric* m) noexcept {
  delete as_instance(m);  // NOLINT(cppcoreguidelines-owning-memory): the ABI hands the instance over as a raw pointer
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic):
// the C arrays of ovt_MetricPlanes, indexed by plane < planes (checked by planes_ok()).
bool planes_ok(const ovt_MetricPlanes& p) {
  if (p.struct_size < sizeof(ovt_MetricPlanes) || (p.planes != 1 && p.planes != 3) ||
      p.domain != OVT_METRIC_DOMAIN_CODE_VALUE || p.sample_format != OVT_METRIC_SAMPLE_UINT) {
    return false;
  }
  const bool depth_ok = p.bit_depth == 8 || p.bit_depth == 10 || p.bit_depth == 12 || p.bit_depth == 16;
  if (!depth_ok || p.sample_bytes != (p.bit_depth == 8 ? 1 : 2)) {
    return false;
  }
  for (uint8_t i = 0; i < p.planes; ++i) {
    const auto row = static_cast<int64_t>(p.width[i]) * p.sample_bytes;
    if (p.data[i] == nullptr || p.width[i] == 0 || p.height[i] == 0 || p.stride_bytes[i] < row) {
      return false;
    }
  }
  return true;
}

// The libvmaf pixel format of a validated picture, or UNKNOWN when the chroma planes are not 4:2:0, 4:2:2 or 4:4:4
// sizes. A subsampled chroma dimension of an odd luma dimension may be rounded up or down.
VmafPixelFormat pixel_format(const ovt_MetricPlanes& p) {
  if (p.planes == 1) {
    return VMAF_PIX_FMT_YUV400P;
  }
  const uint32_t w = p.width[0];
  const uint32_t h = p.height[0];
  if (p.width[1] != p.width[2] || p.height[1] != p.height[2]) {
    return VMAF_PIX_FMT_UNKNOWN;
  }
  const bool half_w = (p.width[1] == (w + 1) / 2 || p.width[1] == w / 2) && w > 1;
  const bool half_h = (p.height[1] == (h + 1) / 2 || p.height[1] == h / 2) && h > 1;
  if (half_w && half_h) {
    return VMAF_PIX_FMT_YUV420P;
  }
  if (half_w && p.height[1] == h) {
    return VMAF_PIX_FMT_YUV422P;
  }
  if (p.width[1] == w && p.height[1] == h) {
    return VMAF_PIX_FMT_YUV444P;
  }
  return VMAF_PIX_FMT_UNKNOWN;
}

// Copies the planes into a libvmaf picture (§9.5.4; libvmaf takes ownership, so a copy is unavoidable).
// libvmaf sizes subsampled chroma planes w >> 1 and h >> 1 (rounded down, libvmaf picture.c), as its CLI reads raw
// YUV: of an odd-sized picture the last chroma column or row is not passed, exactly as with the CLI.
int fill_picture(const ovt_MetricPlanes& p, VmafPixelFormat fmt, VmafPicture& pic) {
  if (const int err = vmaf_picture_alloc(&pic, fmt, p.bit_depth, p.width[0], p.height[0]); err != 0) {
    return err;
  }
  for (uint8_t i = 0; i < p.planes; ++i) {
    const size_t row = static_cast<size_t>(std::min(p.width[i], pic.w[i])) * p.sample_bytes;
    auto* dst = static_cast<uint8_t*>(pic.data[i]);
    const uint8_t* src = p.data[i];
    for (uint32_t y = 0; y < std::min(p.height[i], pic.h[i]); ++y) {
      std::memcpy(dst + (static_cast<ptrdiff_t>(y) * pic.stride[i]),
                  src + (static_cast<ptrdiff_t>(y) * p.stride_bytes[i]), row);
    }
  }
  return 0;
}

// libvmaf keeps a feature under its internal name (`VMAF_integer_feature_adm2_score`) and writes the alias
// (`integer_adm2`) in its JSON; a feature computed with non-default options is kept under the alias plus the option
// suffix (`integer_adm2_egl_1`). The outputs are the JSON names, so each is looked up under its internal name
// first, then as is. Resolved once per instance, on its first frame. 0 or a negative errno.
int resolve_keys(Instance& inst, unsigned idx) {
  const ModelInfo& info = *inst.model;
  std::vector<std::string> keys(info.output_count());
  for (size_t i = 1; i < keys.size(); ++i) {
    const std::string_view name = info.outputs.at(i);
    constexpr std::string_view kInteger = "integer_";
    const bool integer = name.substr(0, kInteger.size()) == kInteger;
    const std::string internal = std::string(integer ? "VMAF_integer_feature_" : "VMAF_feature_") +
                                 std::string(integer ? name.substr(kInteger.size()) : name) + "_score";
    double v = 0.0;
    if (vmaf_feature_score_at_index(inst.ctx, internal.c_str(), &v, idx) == 0) {
      keys.at(i) = internal;
    } else if (const int err = vmaf_feature_score_at_index(inst.ctx, std::string(name).c_str(), &v, idx); err == 0) {
      keys.at(i) = name;
    } else {
      return err;
    }
  }
  inst.keys = std::move(keys);
  return 0;
}

// The values of a feature id at context index `idx`. Of a 4:0:0 picture libvmaf computes no chroma features: those
// values are NaN. 0 or a negative errno.
int feature_values(const Instance& inst, unsigned idx, std::vector<double>& values) {
  const FeatureInfo& info = *inst.feature;
  const auto& keys = inst.use_float ? info.float_keys : info.keys;
  for (size_t i = 0; i < info.output_count(); ++i) {
    double v = 0.0;
    if (const int err = vmaf_feature_score_at_index(inst.ctx, keys.at(i), &v, idx); err != 0) {
      if (inst.fmt != VMAF_PIX_FMT_YUV400P) {
        return err;
      }
      v = std::numeric_limits<double>::quiet_NaN();
    }
    values.push_back(v);
  }
  return 0;
}

// Builds the FrameScore of context index `idx` into inst.last. 0 or a negative errno.
int emit(Instance& inst, unsigned idx, ovt_Buffer* out) {
  std::vector<double> values;
  if (inst.feature != nullptr) {
    if (const int err = feature_values(inst, idx, values); err != 0) {
      return err;
    }
  } else if (const ModelInfo& info = *inst.model; info.collection) {
    VmafModelCollectionScore s{};
    if (const int err = vmaf_score_at_index_model_collection(inst.ctx, inst.handles.collection, &s, idx); err != 0) {
      return err;
    }
    values = {s.bootstrap.bagging_score, s.bootstrap.ci.p95.lo, s.bootstrap.ci.p95.hi, s.bootstrap.stddev};
  } else {
    double score = 0.0;
    if (const int err = vmaf_score_at_index(inst.ctx, inst.handles.model, &score, idx); err != 0) {
      return err;
    }
    values.reserve(info.output_count());
    values.push_back(score);
    if (inst.keys.empty()) {
      if (const int err = resolve_keys(inst, idx); err != 0) {
        return err;
      }
    }
    for (size_t i = 1; i < info.output_count(); ++i) {
      double v = 0.0;
      if (const int err = vmaf_feature_score_at_index(inst.ctx, inst.keys.at(i).c_str(), &v, idx); err != 0) {
        return err;
      }
      values.push_back(v);
    }
  }
  auto fbb = std::make_unique<flatbuffers::FlatBufferBuilder>(256);
  const auto vec = fbb->CreateVector(values);
  const ovt_FrameId id = inst.ids.at(idx);
  const ovt::fb::FrameId fid(id.decode_index, id.display_index);
  fbb->Finish(ovt::fb::CreateFrameScore(*fbb, &fid, vec));
  inst.last = std::move(fbb);
  out->data = inst.last->GetBufferPointer();
  out->size = inst.last->GetSize();
  return 0;
}

// Checks a frame pair against the planes rules and the instance's geometry, fixed by its first frame (libvmaf fixes
// it per context). OVT_OK with the libvmaf pixel format in `fmt`, OVT_E_INVALID_ARG or OVT_E_UNSUPPORTED.
ovt_status check_frame(Instance& inst, const ovt_MetricPlanes& ref, const ovt_MetricPlanes& dist, ovt_FrameId id,
                       VmafPixelFormat& fmt) {
  if (inst.flushed || !planes_ok(ref) || !planes_ok(dist) || ref.planes != dist.planes ||
      ref.bit_depth != dist.bit_depth) {
    return OVT_E_INVALID_ARG;
  }
  for (uint8_t i = 0; i < ref.planes; ++i) {
    if (ref.width[i] != dist.width[i] || ref.height[i] != dist.height[i]) {
      return OVT_E_INVALID_ARG;
    }
  }
  fmt = pixel_format(ref);
  if (fmt == VMAF_PIX_FMT_UNKNOWN) {
    return OVT_E_UNSUPPORTED;
  }
  if (inst.ids.empty()) {
    inst.fmt = fmt;
    inst.bit_depth = ref.bit_depth;
    inst.width = ref.width[0];
    inst.height = ref.height[0];
    return OVT_OK;
  }
  if (fmt != inst.fmt || ref.bit_depth != inst.bit_depth || ref.width[0] != inst.width ||
      ref.height[0] != inst.height) {
    return OVT_E_INVALID_ARG;
  }
  return id.decode_index > inst.ids.back().decode_index ? OVT_OK : OVT_E_INVALID_ARG;  // rule 2
}

ovt_status score_instance(ovt_Metric* m, const ovt_MetricPlanes* ref, const ovt_MetricPlanes* dist, ovt_FrameId id,
                          ovt_Buffer* out) noexcept {
  if (m == nullptr || dist == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  Instance& inst = *as_instance(m);
  const bool no_reference = inst.feature != nullptr && inst.feature->no_reference;
  if (no_reference) {
    ref = dist;  // libvmaf reads a picture pair; a NoReference extractor reads only dist
  } else if (ref == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  try {
    inst.last.reset();
    VmafPixelFormat fmt = VMAF_PIX_FMT_UNKNOWN;
    if (const ovt_status st = check_frame(inst, *ref, *dist, id, fmt); st != OVT_OK) {
      return st;
    }
    VmafPicture rp{};
    VmafPicture dp{};
    if (const int err = fill_picture(*ref, fmt, rp); err != 0) {
      return from_errno(err);
    }
    if (const int err = fill_picture(*dist, fmt, dp); err != 0) {
      vmaf_picture_unref(&rp);
      return from_errno(err);
    }
    const auto idx = static_cast<unsigned>(inst.ids.size());
    inst.ids.push_back(id);
    if (const int err = vmaf_read_pictures(inst.ctx, &rp, &dp, idx); err != 0) {
      // Without a thread pool libvmaf returns before releasing the pictures on every error path.
      vmaf_picture_unref(&rp);
      vmaf_picture_unref(&dp);
      inst.ids.pop_back();
      return from_errno(err);
    }
    if (inst.feature != nullptr) {
      if (const int err = emit(inst, idx, out); err != 0) {  // a feature id reads no neighbouring frame
        *out = ovt_Buffer{nullptr, 0};
        return from_errno(err);
      }
      return OVT_OK;
    }
    if (idx == 0) {
      return OVT_OK;  // integer_motion2 of this frame needs the next one (rule 3)
    }
    if (const int err = emit(inst, idx - 1, out); err != 0) {
      *out = ovt_Buffer{nullptr, 0};
      return from_errno(err);
    }
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

// NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-pointer-arithmetic)

ovt_status finish_instance(ovt_Metric* m, ovt_Buffer* out) noexcept {
  if (m == nullptr || out == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  *out = ovt_Buffer{nullptr, 0};
  Instance& inst = *as_instance(m);
  try {
    inst.last.reset();
    if (inst.flushed) {
      return OVT_E_INVALID_ARG;
    }
    inst.flushed = true;
    if (inst.ids.empty()) {
      return OVT_OK;
    }
    if (const int err = vmaf_read_pictures(inst.ctx, nullptr, nullptr, 0); err != 0) {
      return from_errno(err);
    }
    if (inst.feature != nullptr) {
      return OVT_OK;  // every frame was delivered by score(); SPEC-ASSUMPTION(#460): no sequence-level score
    }
    // SPEC-ASSUMPTION(#394): the last frame's per-frame score, frame present.
    if (const int err = emit(inst, static_cast<unsigned>(inst.ids.size() - 1), out); err != 0) {
      *out = ovt_Buffer{nullptr, 0};
      return from_errno(err);
    }
    return OVT_OK;
  } catch (const std::bad_alloc&) {
    return OVT_E_NOMEM;
  } catch (...) {
    return OVT_E_PLUGIN;
  }
}

// Per-model entry points: descriptor() and create() take no instance, so each id gets its own pair.
template <size_t I>
ovt_Buffer descriptor_of() noexcept {
  try {
    static const std::vector<uint8_t> bytes = build_descriptor(kModels.at(I));
    return ovt_Buffer{bytes.data(), bytes.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}

template <size_t I>
ovt_status create_of(const char* options_json, ovt_Metric** out) noexcept {
  return create_instance(std::get<I>(kModels), options_json, out);
}

template <size_t... I>
constexpr std::array<ovt_MetricApi, sizeof...(I)> make_apis(std::index_sequence<I...> /*unused*/) {
  return {ovt_MetricApi{sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, &descriptor_of<I>, &create_of<I>,
                        &destroy_instance, &score_instance, &finish_instance}...};
}

constexpr std::array kApis = make_apis(std::make_index_sequence<kModels.size()>{});

template <size_t I>
ovt_Buffer feature_descriptor_of() noexcept {
  try {
    static const std::vector<uint8_t> bytes = build_descriptor(kFeatures.at(I));
    return ovt_Buffer{bytes.data(), bytes.size()};
  } catch (...) {
    return ovt_Buffer{nullptr, 0};
  }
}

template <size_t I>
ovt_status feature_create_of(const char* options_json, ovt_Metric** out) noexcept {
  return create_feature_instance(std::get<I>(kFeatures), options_json, out);
}

template <size_t... I>
constexpr std::array<ovt_MetricApi, sizeof...(I)> make_feature_apis(std::index_sequence<I...> /*unused*/) {
  return {ovt_MetricApi{sizeof(ovt_MetricApi), OVT_METRIC_ABI_VERSION, &feature_descriptor_of<I>, &feature_create_of<I>,
                        &destroy_instance, &score_instance, &finish_instance}...};
}

constexpr std::array kFeatureApis = make_feature_apis(std::make_index_sequence<kFeatures.size()>{});

template <class Info>
size_t count_outputs(const Info& info) noexcept {
  return static_cast<size_t>(std::find(info.outputs.begin(), info.outputs.end(), nullptr) - info.outputs.begin());
}

}  // namespace

size_t ModelInfo::output_count() const noexcept {
  return count_outputs(*this);
}

size_t FeatureInfo::output_count() const noexcept {
  return count_outputs(*this);
}

std::span<const ModelInfo> models() noexcept {
  return kModels;
}

std::span<const FeatureInfo> features() noexcept {
  return kFeatures;
}

const ovt_MetricApi* metric_api(std::string_view id) noexcept {
  const auto* it = std::find_if(kModels.begin(), kModels.end(), [id](const ModelInfo& m) { return id == m.id; });
  if (it != kModels.end()) {
    return &*std::next(kApis.begin(), std::distance(kModels.begin(), it));
  }
  const auto* f = std::find_if(kFeatures.begin(), kFeatures.end(), [id](const FeatureInfo& x) { return id == x.id; });
  if (f != kFeatures.end()) {
    return &*std::next(kFeatureApis.begin(), std::distance(kFeatures.begin(), f));
  }
  return nullptr;
}

bool feature_available(const FeatureInfo& feature) noexcept {
  return extractor_exists(feature.extractor);
}

Overlap overlap(std::string_view id) noexcept {
  const bool model = std::any_of(kModels.begin(), kModels.end(), [id](const ModelInfo& m) { return id == m.id; });
  return model ? Overlap{kLookbehind, kLookahead} : Overlap{};
}

bool model_available(const ModelInfo& model) noexcept {
  ModelHandles h;
  return load_model(model, VMAF_MODEL_FLAGS_DEFAULT, h) == 0;
}

std::string engine() {
  return std::string("libvmaf ") + vmaf_version();
}

namespace {

size_t register_models(PluginRegistry& registry) {
  size_t n = 0;
  for (size_t i = 0; i < kModels.size(); ++i) {
    const ModelInfo& model = kModels.at(i);
    if (!model_available(model)) {
      OVT_LOG(ovt::log::Level::Debug, kModule, "libvmaf {} has no built-in model {}: metric {} not registered",
              vmaf_version(), model.version, model.id);
      continue;
    }
    if (registry.register_builtin(&kApis.at(i), "(built-in)") == OVT_OK) {
      ++n;
    }
  }
  return n;
}

size_t register_features(PluginRegistry& registry) {
  size_t n = 0;
  for (size_t i = 0; i < kFeatures.size(); ++i) {
    const FeatureInfo& feature = kFeatures.at(i);
    if (!feature_available(feature)) {
      OVT_LOG(ovt::log::Level::Debug, kModule, "libvmaf {} has no feature extractor {}: metric {} not registered",
              vmaf_version(), feature.extractor, feature.id);
      continue;
    }
    if (registry.register_builtin(&kFeatureApis.at(i), "(built-in)") == OVT_OK) {
      ++n;
    }
  }
  return n;
}

}  // namespace

size_t register_builtin_vmaf(PluginRegistry& registry) {
  return register_models(registry) + register_features(registry);
}

std::vector<Chunk> plan_chunks(uint32_t frames, uint32_t chunks, uint32_t lookbehind, uint32_t lookahead) {
  std::vector<Chunk> plan;
  if (frames == 0) {
    return plan;
  }
  const uint32_t n = std::min(std::max(chunks, 1U), frames);
  const uint32_t base = frames / n;
  const uint32_t extra = frames % n;
  uint32_t begin = 0;
  plan.reserve(n);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t end = begin + base + (k < extra ? 1U : 0U);
    Chunk c;
    c.keep_begin = begin;
    c.keep_end = end;
    c.feed_begin = begin - std::min(begin, lookbehind);
    c.feed_end = end + std::min(frames - end, lookahead);
    plan.push_back(c);
    begin = end;
  }
  return plan;
}

}  // namespace ovt::metrics::vmaf
