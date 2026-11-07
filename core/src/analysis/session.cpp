// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// GOP-local analysis driver (chapter 7 §7.5.3 steps A0–A7, §7.5.4; WI-2.151). See session.hpp.

#include "session.hpp"

#include <flatbuffers/flatbuffers.h>
#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

#include "ovt/cxx/log.hpp"
#include "ovt/cxx/model/plane.hpp"

namespace ovt::analysis {
namespace {

constexpr std::string_view kModule = "analysis";
constexpr int kMaxConsecutiveCorrupt = 32;  // §7.5.3 A4, §7.5.4
constexpr uint8_t kH265Cra = 21;
constexpr uint8_t kH265RaslN = 8;
constexpr uint8_t kH265RaslR = 9;
constexpr uint8_t kH265FirstNonVcl = 32;
constexpr uint8_t kAv1SequenceHeader = 1;
constexpr uint8_t kObuExtensionFlag = 0x04;
constexpr uint8_t kObuHasSizeField = 0x02;

using ovt::fb::Codec;
using ovt::fb::Severity;

const char* stage_name(unsigned stage) {
  static constexpr std::array<const char*, 7> kNames = {"recon",      "pred",  "resid", "predeblock",
                                                        "presaocdef", "prelr", "coeff"};
  return stage < kNames.size() ? kNames.at(stage) : "unknown";
}

// The parameter-set id of a parsed header (#253 rule 2), or nullopt when the header is absent or carries none.
std::optional<uint64_t> header_ps_id(const ovt::fb::SyntaxNode* root) {
  static constexpr std::array<std::string_view, 5> kIdNames = {"vps_video_parameter_set_id", "seq_parameter_set_id",
                                                               "sps_seq_parameter_set_id", "pic_parameter_set_id",
                                                               "pps_pic_parameter_set_id"};
  std::vector<const ovt::fb::SyntaxNode*> stack;  // depth-first, children in order
  if (root != nullptr) {
    stack.push_back(root);
  }
  while (!stack.empty()) {
    const auto* node = stack.back();
    stack.pop_back();
    if (node->name() != nullptr &&
        std::find(kIdNames.begin(), kIdNames.end(), node->name()->string_view()) != kIdNames.end()) {
      return node->uvalue();
    }
    if (node->children() != nullptr) {
      for (uint32_t i = node->children()->size(); i > 0; --i) {
        stack.push_back(node->children()->Get(i - 1));
      }
    }
  }
  return std::nullopt;
}

void write_leb128(std::vector<uint8_t>& out, uint64_t value) {
  do {
    auto byte = static_cast<uint8_t>(value & 0x7FU);
    value >>= 7U;
    if (value != 0) {
      byte |= 0x80U;
    }
    out.push_back(byte);
  } while (value != 0);
}

ovt::model::SampleFormat sample_format_of(const ovt::fb::PlaneRef& ref) {
  switch (ref.sample_bytes()) {
    case 1:
      return ovt::model::SampleFormat::U8;
    case 2:
      return ref.is_signed() ? ovt::model::SampleFormat::S16LE : ovt::model::SampleFormat::U16LE;
    default:
      return ovt::model::SampleFormat::S32LE;
  }
}

void pack(const ovt::fb::FrameAnalysisT& fa, std::vector<uint8_t>& out) {
  flatbuffers::FlatBufferBuilder fbb(4096);
  ovt::fb::FinishFrameAnalysisBuffer(fbb, ovt::fb::FrameAnalysis::Pack(fbb, &fa));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  out.assign(bytes.begin(), bytes.end());
}

// (type, id) of a parameter set; without a parsed header (type, its bytes), so only identical copies merge.
// AV1 keeps one sequence header.
using ParameterSetKey = std::tuple<uint8_t, std::optional<uint64_t>, std::string>;
ParameterSetKey parameter_set_key(const ovt::fb::Unit& unit, bool av1, const ovt::io::RandomAccessFile& file) {
  if (av1) {
    return {unit.type(), 0, {}};
  }
  if (auto id = header_ps_id(unit.header())) {
    return {unit.type(), id, {}};
  }
  const auto* span = unit.file_span();
  std::string bytes(span != nullptr ? span->length() : 0U, '\0');
  if (!bytes.empty()) {
    bytes.resize(file.read(span->offset(), bytes.size(), reinterpret_cast<uint8_t*>(bytes.data())));  // NOLINT
  }
  return {unit.type(), std::nullopt, std::move(bytes)};
}

}  // namespace

std::vector<uint8_t> av1_obu_with_size_field(std::span<const uint8_t> obu) {
  if (obu.empty() || (obu[0] & kObuHasSizeField) != 0) {
    return {obu.begin(), obu.end()};
  }
  const std::size_t header_len = (obu[0] & kObuExtensionFlag) != 0 ? 2 : 1;
  if (obu.size() < header_len) {
    return {obu.begin(), obu.end()};  // truncated; the plugin reports it
  }
  std::vector<uint8_t> out;
  out.reserve(obu.size() + 8);
  out.push_back(static_cast<uint8_t>(obu[0] | kObuHasSizeField));
  if (header_len == 2) {
    out.push_back(obu[1]);
  }
  write_leb128(out, obu.size() - header_len);
  out.insert(out.end(), obu.begin() + static_cast<std::ptrdiff_t>(header_len), obu.end());
  return out;
}

Session::Session(const ovt::io::RandomAccessFile& file, std::span<const uint8_t> stream_index,
                 const LoadedPlugin& plugin, SessionOptions options)
    : file_(file),
      index_bytes_(stream_index.begin(), stream_index.end()),
      plugin_(plugin),
      options_(std::move(options)) {
  flatbuffers::Verifier verifier(index_bytes_.data(), index_bytes_.size());
  if (index_bytes_.empty() || !verifier.VerifyBuffer<ovt::fb::StreamIndex>(nullptr)) {
    throw std::invalid_argument("StreamIndex buffer fails verification");
  }
  index_ = flatbuffers::GetRoot<ovt::fb::StreamIndex>(index_bytes_.data());
  if (index_->codec() == Codec::Unknown || plugin_.api == nullptr || plugin_.caps == nullptr) {
    throw std::invalid_argument("Session needs a codec and a loaded plugin");
  }
  const auto n = index_->frames() != nullptr ? index_->frames()->size() : 0U;
  rap_before_.resize(n, -1);
  int last = -1;
  for (uint32_t d = 0; d < n; ++d) {
    if (index_->frames()->Get(d)->is_rap()) {
      last = static_cast<int>(d);
    }
    rap_before_[d] = last;
  }
}

Session::~Session() {
  destroy_decoder();
}

void Session::diag(Severity severity, std::string code, std::string message) {
  ovt::log::write(severity >= Severity::Error ? ovt::log::Level::Warn : ovt::log::Level::Debug, kModule, code, message);
  diagnostics_.push_back({severity, std::move(code), std::move(message)});
}

void Session::destroy_decoder() noexcept {
  if (dec_ != nullptr) {
    plugin_.api->destroy(dec_);
    dec_ = nullptr;
  }
  last_decoded_ = -1;
  current_rap_ = -1;
  consecutive_corrupt_ = 0;
}

ovt_status Session::frame(int decode_index, uint32_t plane_stage_mask, const ovt::jobs::Cancel* cancel,
                          std::vector<uint8_t>& out, FrameRequest request) {
  diagnostics_.clear();
  out.clear();
  try {
    return frame_impl(decode_index, plane_stage_mask, cancel, out, request);
  } catch (const std::bad_alloc&) {
    destroy_decoder();
    return OVT_E_NOMEM;
  } catch (const ovt::model::NoMemError&) {
    destroy_decoder();
    return OVT_E_NOMEM;
  } catch (const ovt::io::IoError& e) {
    destroy_decoder();
    diag(Severity::Error, "", e.what());
    return OVT_E_IO;
  } catch (const std::exception& e) {
    destroy_decoder();
    diag(Severity::Error, "", e.what());
    return OVT_E_INTERNAL;
  }
}

int Session::rap_for(int t, bool from_previous_rap) const {
  int r = rap_before_[static_cast<std::size_t>(t)];
  if (from_previous_rap && r > 0) {
    r = rap_before_[static_cast<std::size_t>(r - 1)];
  } else if (from_previous_rap) {
    r = -1;
  }
  if (r >= 0 && index().codec() == Codec::AV1) {
    // A show_existing KEY (forward key frame) starts decoding at the hidden key frame it shows (§7.4.3).
    const int shown = index().frames()->Get(static_cast<uint32_t>(r))->show_existing_of();
    if (shown >= 0) {
      r = shown;
    }
  }
  return r;
}

ovt_status Session::frame_impl(int t, uint32_t stage_mask, const ovt::jobs::Cancel* cancel, std::vector<uint8_t>& out,
                               FrameRequest request) {
  const auto* frames = index().frames();
  // A0
  if (frames == nullptr || t < 0 || static_cast<uint32_t>(t) >= frames->size()) {
    return OVT_E_INVALID_ARG;
  }
  const auto* f = frames->Get(static_cast<uint32_t>(t));
  const int shown = f->show_existing_of();
  if (shown < 0) {
    return decode_target(t, stage_mask, cancel, out, request);
  }
  // AV1 show_existing_frame: analyse the shown frame, then give the result this frame's identity.
  if (shown >= t || frames->Get(static_cast<uint32_t>(shown))->show_existing_of() >= 0) {
    diag(Severity::Error, "", fmt::format("frame {} shows frame {}, which is not a decoded frame", t, shown));
    return OVT_E_CORRUPT;
  }
  std::vector<uint8_t> decoded;
  if (const ovt_status st = decode_target(shown, stage_mask, cancel, decoded, request); st != OVT_OK) {
    return st;
  }
  auto fa = ovt::fb::UnPackFrameAnalysis(decoded.data());
  if (!fa->frame) {
    fa->frame = std::make_unique<ovt::fb::FrameInfoT>();
  }
  auto& info = *fa->frame;
  info.id = std::make_unique<ovt::fb::FrameId>(t, f->id() != nullptr ? f->id()->display_index() : -1);
  info.shown = true;
  info.show_existing_of = shown;
  info.pts = f->pts();
  info.dts = f->dts();
  info.poc = f->poc();
  info.size_bytes = f->size_bytes();
  info.units.clear();
  if (f->units() != nullptr) {
    info.units.assign(f->units()->begin(), f->units()->end());
  }
  pack(*fa, out);
  if (options_.cache != nullptr) {
    options_.cache->store(static_cast<uint32_t>(t), out, stage_mask);
  }
  return OVT_OK;
}

ovt_status Session::decode_target(int t, uint32_t stage_mask, const ovt::jobs::Cancel* cancel,
                                  std::vector<uint8_t>& out, FrameRequest request) {
  if (index().frames()->Get(static_cast<uint32_t>(t))->type() == ovt::fb::FrameType::Unknown) {
    diag(Severity::Error, "ANA-0007", fmt::format("frame {} has no parameter sets", t));
    return OVT_E_UNSUPPORTED;
  }
  // A1
  if (options_.cache != nullptr) {
    if (auto hit = options_.cache->lookup(static_cast<uint32_t>(t));
        hit && (hit->stage_mask & stage_mask) == stage_mask) {
      out = std::move(hit->frame_analysis);
      return OVT_OK;
    }
  }
  // A2
  const int r = rap_for(t, request.from_previous_rap);
  if (r < 0) {
    diag(Severity::Error, "ANA-0005", fmt::format("no random access point before frame {}", t));
    return OVT_E_UNSUPPORTED;
  }
  // A3
  const bool continue_ok = dec_ != nullptr && current_rap_ == r && last_decoded_ < t && last_decoded_ >= r - 1;
  if (!continue_ok) {
    destroy_decoder();
    if (const ovt_status st = create_decoder(r); st != OVT_OK) {
      return st;
    }
  }
  // A4
  Pulled result;
  if (const ovt_status st = decode_range(r, t, stage_mask, cancel, result); st != OVT_OK) {
    return st;
  }
  // A5
  if (!result.have) {
    if (plugin_.api->flush(dec_) == OVT_OK) {
      if (const ovt_status st = drain(t, stage_mask, result); st != OVT_OK) {
        return st;
      }
    }
    destroy_decoder();  // after a flush the decoder cannot continue
    if (!result.have) {
      diag(Severity::Error, "ANA-0004", fmt::format("plugin produced no analysis for frame {}", t));
      return OVT_E_CORRUPT;
    }
  }
  // A6, A7
  if (const ovt_status st = post_process(t, stage_mask, result.fb); st != OVT_OK) {
    return st;
  }
  out = std::move(result.fb);
  if (options_.cache != nullptr) {
    options_.cache->store(static_cast<uint32_t>(t), out, stage_mask);
  }
  return OVT_OK;
}

ovt_status Session::decode_range(int rap, int t, uint32_t stage_mask, const ovt::jobs::Cancel* cancel, Pulled& result) {
  for (int d = last_decoded_ + 1; d <= t; ++d) {
    if (cancel != nullptr && cancel->requested()) {
      return OVT_E_CANCELLED;  // between frames: the decoder stays consistent and is kept for reuse
    }
    if (is_rasl_of(d, rap)) {
      if (d == t) {
        diag(Severity::Error, "ANA-0006",
             "RASL picture after random access cannot be decoded; decode from the previous RAP");
        return OVT_E_UNSUPPORTED;
      }
      last_decoded_ = d;
      continue;
    }
    const auto* fd = index().frames()->Get(static_cast<uint32_t>(d));
    if (fd->units() != nullptr) {
      for (const uint32_t u : *fd->units()) {
        if (const ovt_status st = push_unit(u, d, fd->pts()); st != OVT_OK) {
          return st;
        }
      }
    }
    if (const ovt_status st = drain(t, d == t ? stage_mask : 0U, result); st != OVT_OK) {
      return st;
    }
    last_decoded_ = d;
  }
  return OVT_OK;
}

ovt_status Session::create_decoder(int rap) {
  int threads = options_.threads;
  if (threads <= 0) {
#ifdef __ANDROID__
    threads = 2;
#else
    threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 4U));
#endif
  }
  const uint8_t* cp = options_.codec_private.empty() ? nullptr : options_.codec_private.data();
  ovt_AnalysisDecoder* dec = nullptr;
  const ovt_status st = plugin_.api->create(cp, options_.codec_private.size(), threads, &dec);
  if (st != OVT_OK || dec == nullptr) {
    return st != OVT_OK ? st : OVT_E_PLUGIN;
  }
  dec_ = dec;
  current_rap_ = rap;
  last_decoded_ = rap - 1;
  consecutive_corrupt_ = 0;
  return feed_parameter_sets(rap);
}

// SPEC-ASSUMPTION(#253): the ActiveSets snapshot at R is the last parameter set per (type, id) before R's first unit.
ovt_status Session::feed_parameter_sets(int rap) {
  const auto* f = index().frames()->Get(static_cast<uint32_t>(rap));
  const auto* sets = index().parameter_sets();
  if (sets == nullptr || f->units() == nullptr) {
    return OVT_OK;
  }
  const auto first_it = std::min_element(f->units()->begin(), f->units()->end());
  if (first_it == f->units()->end()) {
    return OVT_OK;
  }
  const uint32_t first = *first_it;
  const bool av1 = index().codec() == Codec::AV1;
  // key → unit index of the last occurrence; the value pushed in stream order.
  std::map<ParameterSetKey, uint32_t> last;
  for (const auto* unit : *sets) {
    if (unit->index() >= first || (av1 && unit->type() != kAv1SequenceHeader)) {
      continue;
    }
    const auto key = parameter_set_key(*unit, av1, file_);
    last[key] = unit->index();
  }
  std::vector<uint32_t> order;
  order.reserve(last.size());
  for (const auto& [key, idx] : last) {
    order.push_back(idx);
  }
  std::sort(order.begin(), order.end());
  for (const uint32_t u : order) {
    if (const ovt_status st = push_unit(u, rap, index().frames()->Get(static_cast<uint32_t>(rap))->pts());
        st != OVT_OK) {
      return st;
    }
  }
  return OVT_OK;
}

ovt_status Session::push_unit(uint32_t unit_index, int32_t decode_index, int64_t pts) {
  const auto* units = index().units();
  if (units == nullptr || unit_index >= units->size()) {
    destroy_decoder();
    diag(Severity::Error, "", fmt::format("frame {} names unit {} outside the index", decode_index, unit_index));
    return OVT_E_CORRUPT;
  }
  const auto* unit = units->Get(unit_index);
  const auto* span = unit->file_span();
  scratch_.resize(span != nullptr ? span->length() : 0U);
  if (!scratch_.empty()) {
    scratch_.resize(file_.read(span->offset(), scratch_.size(), scratch_.data()));
  }
  if (index().codec() == Codec::AV1) {
    scratch_ = av1_obu_with_size_field(scratch_);
  }
  const ovt_status st = plugin_.api->push(dec_, scratch_.data(), scratch_.size(), decode_index, pts);
  if (st == OVT_OK) {
    consecutive_corrupt_ = 0;
    return OVT_OK;
  }
  if (st == OVT_E_CORRUPT) {
    diag(Severity::Warning, "ANA-0002", fmt::format("push failed for unit {} (frame {})", unit_index, decode_index));
    if (++consecutive_corrupt_ < kMaxConsecutiveCorrupt) {
      return OVT_OK;
    }
    diag(Severity::Error, "ANA-0003",
         fmt::format("{} consecutive push failures at frame {}", consecutive_corrupt_, decode_index));
  }
  destroy_decoder();  // §7.5.4: the plugin is assumed to be in an undefined state
  return st;
}

ovt_status Session::drain(int target, uint32_t stage_mask, Pulled& result) {
  for (;;) {
    ovt_Buffer fb{nullptr, 0};
    const ovt_status st = plugin_.api->pull(dec_, stage_mask, &fb);
    if (st != OVT_OK) {
      destroy_decoder();
      return st;
    }
    if (fb.data == nullptr) {
      return OVT_OK;
    }
    flatbuffers::Verifier verifier(fb.data, fb.size);
    if (!ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
      plugin_.api->release(dec_, fb);
      destroy_decoder();
      diag(Severity::Error, "", fmt::format("plugin {} returned an invalid FrameAnalysis", plugin_.id));
      return OVT_E_PLUGIN;
    }
    const auto* fa = ovt::fb::GetFrameAnalysis(fb.data);
    const int idx = fa->frame() != nullptr && fa->frame()->id() != nullptr ? fa->frame()->id()->decode_index() : -1;
    if (idx == target) {
      const std::span<const uint8_t> bytes(fb.data, fb.size);
      result.fb.assign(bytes.begin(), bytes.end());
      result.have = true;
      copy_planes(*fa);  // plugin plane buffers are valid only until release()
    } else if (idx >= 0 && idx < target && options_.cache != nullptr) {
      options_.cache->store(static_cast<uint32_t>(idx), {fb.data, fb.size}, 0);
    }
    plugin_.api->release(dec_, fb);
  }
}

void Session::copy_planes(const ovt::fb::FrameAnalysis& fa) {
  if (options_.pictures == nullptr || fa.planes() == nullptr) {
    return;
  }
  for (const auto* ref : *fa.planes()) {
    if (ref->blob_id() == nullptr) {
      continue;
    }
    const uint8_t* data = nullptr;
    size_t size = 0;
    if (plugin_.api->plane(dec_, ref->blob_id()->c_str(), &data, &size) != OVT_OK || data == nullptr) {
      continue;
    }
    ovt::model::PlaneView view;
    view.data = data;
    view.width = ref->width();
    view.height = ref->height();
    view.stride_bytes = ref->stride_bytes();
    view.bit_depth = ref->bit_depth();
    view.sample_bytes = ref->sample_bytes();
    view.format = sample_format_of(*ref);
    view.plane = ref->plane();
    view.stage = static_cast<uint8_t>(ref->stage());
    if (static_cast<uint64_t>(view.stride_bytes) * view.height > size) {
      diag(Severity::Warning, "", fmt::format("plane {} is shorter than its PlaneRef", ref->blob_id()->str()));
      continue;
    }
    try {
      options_.pictures->put(ref->blob_id()->str(), view);
    } catch (const std::invalid_argument& e) {
      diag(Severity::Warning, "", fmt::format("plane {} rejected: {}", ref->blob_id()->str(), e.what()));
    }
  }
}

bool Session::is_rasl_of(int d, int rap) const {
  if (index().codec() != Codec::H265 || d == rap || rap_before_[static_cast<std::size_t>(d)] != rap) {
    return false;
  }
  const auto first_vcl_type = [this](int frame) -> int {
    const auto* f = index().frames()->Get(static_cast<uint32_t>(frame));
    const auto* units = index().units();
    if (f->units() == nullptr || units == nullptr) {
      return -1;
    }
    for (const uint32_t u : *f->units()) {
      if (u < units->size() && units->Get(u)->type() < kH265FirstNonVcl) {
        return units->Get(u)->type();
      }
    }
    return -1;
  };
  if (first_vcl_type(rap) != kH265Cra) {
    return false;
  }
  const int type = first_vcl_type(d);
  return type == kH265RaslN || type == kH265RaslR;
}

ovt_status Session::post_process(int t, uint32_t stage_mask, std::vector<uint8_t>& fb) {
  auto fa = ovt::fb::UnPackFrameAnalysis(fb.data());
  if (!fa->frame || !fa->frame->id || fa->frame->id->decode_index() != t) {
    destroy_decoder();
    diag(Severity::Error, "", fmt::format("plugin {} returned the wrong frame for {}", plugin_.id, t));
    return OVT_E_PLUGIN;
  }
  fill_frame_info(t, *fa->frame);
  fill_av1_refs(t, *fa);
  // qp_hist (chapter 6 §6.3, V-028): qp + 128 for H.26x, qindex for AV1 where -1 cells are "not applicable".
  if (fa->qp_hist.empty() && fa->grid && !fa->grid->qp.empty()) {
    fa->qp_hist.assign(256, 0);
    const bool av1 = index().codec() == Codec::AV1;
    for (const int16_t qp : fa->grid->qp) {
      if (!av1 || qp >= 0) {
        ++fa->qp_hist[static_cast<std::size_t>(std::clamp(qp + (av1 ? 0 : 128), 0, 255))];
      }
    }
  }
  add_missing_stage_diagnostics(stage_mask, *fa);
  pack(*fa, fb);
  return OVT_OK;
}

// A6: the index is authoritative for identity; other fields only where the plugin left them empty.
void Session::fill_frame_info(int t, ovt::fb::FrameInfoT& info) const {
  const auto* f = index().frames()->Get(static_cast<uint32_t>(t));
  info.id = std::make_unique<ovt::fb::FrameId>(t, f->id() != nullptr ? f->id()->display_index() : -1);
  info.is_rap = f->is_rap();
  if (info.pts == 0) {
    info.pts = f->pts();
  }
  if (info.dts == 0) {
    info.dts = f->dts();
  }
  if (info.timebase_den == 0) {
    info.timebase_num = f->timebase_num();
    info.timebase_den = f->timebase_den();
  }
  if (info.size_bytes == 0) {
    info.size_bytes = f->size_bytes();
  }
  const auto fill = [](auto& dst, const auto* src) {
    if (dst.empty() && src != nullptr) {
      dst.assign(src->begin(), src->end());
    }
  };
  fill(info.units, f->units());
  fill(info.declared_refs, f->declared_refs());
  fill(info.dpb_after, f->dpb_after());
}

// AV1 RefSlot.frame from the slot model: the DPB before T is frames[T-1].dpb_after (#253 rule 5).
void Session::fill_av1_refs(int t, ovt::fb::FrameAnalysisT& fa) const {
  if (index().codec() != Codec::AV1 || t == 0) {
    return;
  }
  const auto* frames = index().frames();
  const auto* before = frames->Get(static_cast<uint32_t>(t - 1))->dpb_after();
  if (before == nullptr) {
    return;
  }
  for (auto& ref : fa.refs) {
    const bool unset = ref && (!ref->frame || (ref->frame->decode_index() == 0 && ref->frame->display_index() == 0));
    if (!unset || ref->slot < 0 || static_cast<uint32_t>(ref->slot) >= before->size()) {
      continue;
    }
    const int target = before->Get(static_cast<uint32_t>(ref->slot));
    const auto* id = target >= 0 && static_cast<uint32_t>(target) < frames->size()
                         ? frames->Get(static_cast<uint32_t>(target))->id()
                         : nullptr;
    ref->frame = std::make_unique<ovt::fb::FrameId>(target, id != nullptr ? id->display_index() : -1);
  }
}

// ANA-0001 for requested stages the plugin did not deliver (§7.5.3).
void Session::add_missing_stage_diagnostics(uint32_t stage_mask, ovt::fb::FrameAnalysisT& fa) {
  for (unsigned stage = 0; stage < 32; ++stage) {
    const bool delivered = std::any_of(fa.planes.begin(), fa.planes.end(), [stage](const auto& p) {
      return p && static_cast<unsigned>(p->stage) == stage;
    });
    if ((stage_mask & (1U << stage)) == 0 || delivered) {
      continue;
    }
    auto d = std::make_unique<ovt::fb::DiagnosticT>();
    d->severity = Severity::Info;
    d->code = "ANA-0001";
    d->message = fmt::format("stage {} not available from plugin {}", stage_name(stage), plugin_.id);
    diagnostics_.push_back({d->severity, d->code, d->message});
    fa.diagnostics.push_back(std::move(d));
  }
}

ovt_status Session::plane(std::string_view blob_id, std::shared_ptr<const ovt::model::PlaneBlob>& out) const {
  out.reset();
  if (options_.pictures == nullptr) {
    return OVT_E_INVALID_ARG;
  }
  out = options_.pictures->get(blob_id);
  return out ? OVT_OK : OVT_E_INVALID_ARG;
}

}  // namespace ovt::analysis
