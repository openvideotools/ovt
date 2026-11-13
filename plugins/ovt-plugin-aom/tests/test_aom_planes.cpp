// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// Reconstruction planes of ovt-plugin-aom (WI-2.158, chapter 07 §7.6.1 plane rows): the frame-buffer pool and the
// PlaneRefs on their own, then the plugin through the host's PluginRegistry on the libaom test vectors.
// AT1: the planes of av1-1-b8-01-size-16x16 hash to the MD5s of the vector's .md5 file.
// AT2: without Reconstruction in plane_stage_mask, planes[] is empty and the frame buffer goes straight back.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <aom/aom_frame_buffer.h>
#include <aom/aom_image.h>
#include <flatbuffers/flatbuffers.h>

extern "C" {
#include <libavutil/md5.h>  // the MD5 of the core FFmpeg flavour that the test host links anyway
#include <libavutil/mem.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aom_test_util.inc"
#include "ovt/abi/analysis_decoder_v1.h"
#include "ovt/cxx/model/validator.hpp"
#include "ovt_model_generated.h"
#include "planes.hpp"
#include "test_support/corpus.h"

namespace fb = ovt::fb;
using ovt::aom::FramePool;
using ovt::aom::Picture;
using ovt::aom::PlaneView;

namespace {

constexpr uint32_t kReconMask = 1U << static_cast<unsigned>(fb::PlaneStage::Reconstruction);

// ---- one analysed stream --------------------------------------------------------------------------------------

struct PlaneOut {
  uint8_t plane = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint8_t bit_depth = 0;
  uint8_t sample_bytes = 0;
  std::string blob_id;
  std::vector<uint8_t> rows;  // width × sample_bytes bytes per row, the stride padding dropped
};

struct FrameOut {
  int32_t decode_index = -1;
  bool shown = false;
  int32_t coded_w = 0;
  bool use_superres = false;
  uint32_t frame_width = 0;  // Av1FilterParams.FrameWidth / UpscaledWidth
  uint32_t upscaled_width = 0;
  std::vector<PlaneOut> planes;
  std::vector<std::string> info;     // Info diagnostics: "<code> <message>"
  std::vector<std::string> invalid;  // validator findings other than the ones the host fixes (see check())
};

// Validator findings the plugin's buffers have before the host's post-processing (§7.5.3 A6, as in
// test_aom_map_mi.cpp), plus V-023's blob_id grammar: plugins emit the local form without the source hash
// (chapter 06 §6.6.2), which the host prefixes. Plane geometry (V-023 size, stride) is still checked.
bool host_fixes(const ovt::model::Violation& v) {
  if (v.rule == "V-002" || v.rule == "V-017" || v.rule == "V-006") {
    return true;
  }
  return v.rule == "V-023" && v.path.ends_with(".blob_id") && v.message.find("source-hash") != std::string::npos;
}

FrameOut read_frame(const ovt_AnalysisDecoderApi& api, ovt_AnalysisDecoder* dec, ovt_Buffer buf) {
  flatbuffers::Verifier verifier(buf.data, buf.size);
  REQUIRE(fb::VerifyFrameAnalysisBuffer(verifier));
  const fb::FrameAnalysis* fa = fb::GetFrameAnalysis(buf.data);
  FrameOut out;
  out.decode_index = fa->frame()->id()->decode_index();
  out.shown = fa->frame()->shown();
  out.coded_w = fa->frame()->coded_size()->w();
  if (fa->filter_params() != nullptr) {
    const auto* fp = flatbuffers::GetRoot<fb::Av1FilterParams>(fa->filter_params()->data());
    out.use_superres = fp->use_superres() != 0;
    out.frame_width = fp->FrameWidth();
    out.upscaled_width = fp->UpscaledWidth();
  }
  for (const fb::PlaneRef* p : *fa->planes()) {
    PlaneOut po{p->plane(),     p->width(),        p->height(),         p->stride_bytes(),
                p->bit_depth(), p->sample_bytes(), p->blob_id()->str(), {}};
    const uint8_t* data = nullptr;
    size_t size = 0;
    REQUIRE(api.plane(dec, p->blob_id()->c_str(), &data, &size) == OVT_OK);
    const size_t row = size_t{po.width} * po.sample_bytes;
    REQUIRE(po.stride >= row);
    REQUIRE(size == size_t{po.stride} * (po.height - 1) + row);
    const std::span<const uint8_t> bytes(data, size);
    for (uint32_t y = 0; y < po.height; ++y) {
      const auto r = bytes.subspan(size_t{po.stride} * y, row);
      po.rows.insert(po.rows.end(), r.begin(), r.end());
    }
    out.planes.push_back(std::move(po));
  }
  for (const fb::Diagnostic* d : *fa->diagnostics()) {
    if (d->severity() == fb::Severity::Info) {
      out.info.push_back(d->code()->str() + " " + d->message()->str());
    }
  }
  for (const auto& v :
       ovt::model::validate_frame_analysis(std::span<const uint8_t>(buf.data, buf.size), fb::Codec::AV1)) {
    if (!host_fixes(v)) {
      out.invalid.push_back(v.rule + " " + v.path + ": " + v.message);
    }
  }
  return out;
}

// Pushes the stream like the host (one OBU per call, hints from the frame OBUs) and pulls with @p mask.
std::vector<FrameOut> analyse(const std::filesystem::path& path, uint32_t mask, size_t max_frames = 1000) {
  const auto tus = ovt::aom::test::read_ivf(path);
  ovt::aom::test::Instance inst;
  REQUIRE(inst.status == OVT_OK);
  std::vector<FrameOut> out;
  int32_t next = 0;
  auto drain = [&] {
    for (;;) {
      ovt_Buffer buf{};
      REQUIRE(inst.api.pull(inst.dec, mask, &buf) == OVT_OK);
      if (buf.data == nullptr) {
        return;
      }
      out.push_back(read_frame(inst.api, inst.dec, buf));
      inst.api.release(inst.dec, buf);
    }
  };
  for (const auto& tu : tus) {
    int32_t cur = -1;
    for (const auto& o : tu) {
      if (o.type == 3 || o.type == 6) {
        cur = next++;
      }
      const int32_t hint = cur >= 0 ? cur : next;
      REQUIRE(inst.api.push(inst.dec, o.bytes.data(), o.bytes.size(), hint, hint) == OVT_OK);
      drain();
    }
    if (out.size() >= max_frames) {
      break;
    }
  }
  REQUIRE(inst.api.flush(inst.dec) == OVT_OK);
  drain();
  return out;
}

// MD5 of the shown picture as libaom's test/md5_helper.h hashes it: Y, U, V rows of the visible samples.
std::string md5_of(const FrameOut& f) {
  std::array<uint8_t, 16> digest{};
  AVMD5* ctx = av_md5_alloc();
  REQUIRE(ctx != nullptr);
  av_md5_init(ctx);
  for (const PlaneOut& p : f.planes) {
    av_md5_update(ctx, p.rows.data(), p.rows.size());
  }
  av_md5_final(ctx, digest.data());
  av_free(ctx);
  std::string hex;
  for (const uint8_t b : digest) {
    constexpr std::string_view kHex = "0123456789abcdef";
    hex += kHex[b >> 4U];
    hex += kHex[b & 0xFU];
  }
  return hex;
}

// MD5 of every shown picture back to back, as `aomdec --rawvideo --md5` hashes the whole output.
std::string stream_md5(const std::vector<FrameOut>& frames) {
  std::vector<uint8_t> all;
  for (const FrameOut& f : frames) {
    for (const PlaneOut& p : f.planes) {
      all.insert(all.end(), p.rows.begin(), p.rows.end());
    }
  }
  FrameOut one;
  one.planes.push_back(PlaneOut{});
  one.planes[0].rows = std::move(all);
  return md5_of(one);
}

// An aom_image_t over a frame buffer of @p fb, as decoder_get_frame() fills one for an I420 8-bit picture.
aom_image_t fake_image(const aom_codec_frame_buffer_t& fb, unsigned w, unsigned h, int stride) {
  aom_image_t img{};
  img.fmt = AOM_IMG_FMT_I420;
  img.bit_depth = 8;
  img.d_w = w;
  img.d_h = h;
  img.x_chroma_shift = 1;
  img.y_chroma_shift = 1;
  const std::span<uint8_t> mem(fb.data, fb.size);
  img.planes[0] = mem.data();
  img.planes[1] = mem.subspan(size_t(stride) * h).data();
  img.planes[2] = mem.subspan(size_t(stride) * h + size_t(stride / 2) * ((h + 1) / 2)).data();
  img.stride[0] = stride;
  img.stride[1] = stride / 2;
  img.stride[2] = stride / 2;
  img.fb_priv = fb.priv;
  return img;
}

}  // namespace

// ---- the pool and the PlaneRefs on their own ------------------------------------------------------------------

TEST_CASE("aom planes: the frame-buffer pool keeps a picture alive after libaom releases it", "[fast]") {
  FramePool pool;
  aom_codec_frame_buffer_t fb{};
  REQUIRE(FramePool::get(&pool, 4096, &fb) == 0);
  REQUIRE(fb.size >= 4096);
  CHECK(std::all_of(fb.data, fb.data + fb.size, [](uint8_t b) { return b == 0; }));
  std::fill(fb.data, fb.data + fb.size, uint8_t{0x5A});
  const aom_image_t img = fake_image(fb, 33, 17, 48);

  Picture pic = pool.retain(img);
  REQUIRE(!pic.empty());
  CHECK(pic.num_planes == 3);
  CHECK(pic.planes[0].width == 33);
  CHECK(pic.planes[0].height == 17);
  CHECK(pic.planes[0].stride_bytes == 48);
  CHECK(pic.planes[1].width == 17);  // chroma rounded up
  CHECK(pic.planes[1].height == 9);
  CHECK(pic.planes[0].span_bytes() == 48U * 16U + 33U);

  // libaom lets go of the buffer; the picture still holds it, so the next request gets a different one.
  REQUIRE(FramePool::release(&pool, &fb) == 0);
  CHECK(pool.held_by_pictures() == 1);
  aom_codec_frame_buffer_t fb2{};
  REQUIRE(FramePool::get(&pool, 4096, &fb2) == 0);
  CHECK(fb2.priv != fb.priv);
  CHECK(pic.planes[0].data[0] == 0x5A);  // untouched

  // Once the picture is gone the buffer is reused, zeroed again.
  pic = Picture{};
  CHECK(pool.held_by_pictures() == 0);
  aom_codec_frame_buffer_t fb3{};
  REQUIRE(FramePool::get(&pool, 4096, &fb3) == 0);
  CHECK(fb3.priv == fb.priv);
  CHECK(std::all_of(fb3.data, fb3.data + fb3.size, [](uint8_t b) { return b == 0; }));

  // An image whose buffer is not the pool's gives no picture.
  aom_image_t foreign = img;
  int other = 0;
  foreign.fb_priv = &other;
  CHECK(pool.retain(foreign).empty());
}

TEST_CASE("aom planes: blob ids use the chapter 06 stage tokens", "[fast]") {
  CHECK(ovt::aom::blob_id(0, fb::PlaneStage::Reconstruction, 0) == "0/recon/y");
  CHECK(ovt::aom::blob_id(142, fb::PlaneStage::Prediction, 1) == "142/pred/u");
  CHECK(ovt::aom::blob_id(7, fb::PlaneStage::PreSaoOrCdef, 2) == "7/presaocdef/v");
  CHECK(ovt::aom::blob_id(7, fb::PlaneStage::PreLoopRestoration, 0) == "7/prelr/y");
}

TEST_CASE("aom planes AT2 (unit): a mask without Reconstruction adds no PlaneRef", "[fast]") {
  FramePool pool;
  aom_codec_frame_buffer_t fb{};
  REQUIRE(FramePool::get(&pool, 4096, &fb) == 0);
  const Picture pic = pool.retain(fake_image(fb, 16, 16, 32));
  flatbuffers::FlatBufferBuilder fbb;
  std::vector<flatbuffers::Offset<fb::PlaneRef>> refs;
  std::map<std::string, PlaneView> blobs;
  const uint32_t mask = GENERATE(0U, 1U << static_cast<unsigned>(fb::PlaneStage::Prediction));
  ovt::aom::add_plane_refs(fbb, 3, pic, mask, &refs, &blobs);
  CHECK(refs.empty());
  CHECK(blobs.empty());
  ovt::aom::add_plane_refs(fbb, 3, pic, kReconMask, &refs, &blobs);
  CHECK(refs.size() == 3);
  CHECK(blobs.count("3/recon/v") == 1);
  CHECK(blobs.at("3/recon/u").data == pic.planes[1].data);  // a view, not a copy
}

// ---- the plugin on the libaom test vectors ----------------------------------------------------------------------

TEST_CASE("aom planes AT1: av1-1-b8-01-size-16x16 planes match the vector's .md5 file",
          "[fast][third_party][needs-corpus]") {
  // https://storage.googleapis.com/aom-test-data/av1-1-b8-01-size-16x16.ivf.md5, one line per output frame.
  const std::vector<std::string> want = {"6353b245c305a5f4f2845ee7ad2b128b", "f4b0078dfbc8b581fa959d4512b9940a"};
  const auto frames = analyse(ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_16x16"), kReconMask);
  std::vector<std::string> got;
  for (const FrameOut& f : frames) {
    INFO("decode index " << f.decode_index);
    CHECK(f.invalid.empty());
    if (!f.shown) {
      CHECK(f.planes.empty());
      continue;
    }
    REQUIRE(f.planes.size() == 3);
    CHECK(f.planes[0].width == 16);
    CHECK(f.planes[0].blob_id == std::to_string(f.decode_index) + "/recon/y");
    CHECK(f.planes[0].sample_bytes == 1);
    CHECK(f.info.empty());  // no film grain
    got.push_back(md5_of(f));
  }
  CHECK(got == want);
}

TEST_CASE("aom planes: Reconstruction equals aomdec --skip-film-grain output", "[fast][third_party][needs-corpus]") {
  // `aomdec --rawvideo --skip-film-grain --md5 <vector>` (libaom v3.12.1): every output picture back to back, 16-bit
  // samples for 10-bit streams. These two vectors have no show_existing_frame, so each output picture is the
  // planes of one shown frame.
  using Case = std::pair<std::string, std::string>;
  const auto [id, want] = GENERATE(Case{"aom_av1_1_b10_00_quantizer_10", "2387e415ebed841a1a6a3c3c070ebdcb"},
                                   Case{"aom_av1_1_b8_01_size_196x196", "e88e97f333e4c9fa38e18c0b9a63a90d"});
  INFO(id);
  CHECK(stream_md5(analyse(ovt::test::corpus_or_skip(id), kReconMask)) == want);
}

TEST_CASE("aom planes: Reconstruction of film grain streams is the picture before grain synthesis",
          "[fast][third_party][needs-corpus]") {
  // Chapter 06 §6.6.1 rule 5. MD5s of the first two pictures of `aomdec --rawvideo --skip-film-grain` (libaom
  // v3.12.1), which are the first two shown frames; later pictures of these vectors come from show_existing_frame
  // headers, whose frames have no planes with stock libaom. The vector's own .md5 file hashes the grain-applied
  // output, which differs.
  using Case = std::pair<std::string, std::vector<std::string>>;
  const auto [id, want] = GENERATE(
      Case{"aom_av1_1_b8_23_film_grain_50", {"d10c573803f06e81d05a8d2be2baa8d6", "db6f8a14fe5c4baf8a2a35263ef9706f"}},
      Case{"aom_av1_1_b10_23_film_grain_50", {"81cafab436f87512789a4b84e449c0e7", "142f82782be05e918a631120c7fca965"}});
  INFO(id);
  const auto frames = analyse(ovt::test::corpus_or_skip(id), kReconMask, 4);
  std::vector<std::string> got;
  for (const FrameOut& f : frames) {
    if (f.shown && got.size() < want.size()) {
      got.push_back(md5_of(f));
    }
  }
  CHECK(got == want);
}

TEST_CASE("aom planes AT2: no planes when Reconstruction is not requested", "[fast][third_party][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_01_size_16x16");
  const auto frames = analyse(path, 0);
  REQUIRE(!frames.empty());
  for (const FrameOut& f : frames) {
    CHECK(f.planes.empty());
  }
  // The plane lookup knows no blob of an analysis pulled without planes.
  const auto tus = ovt::aom::test::read_ivf(path);
  const ovt::aom::test::Instance inst;
  REQUIRE(inst.status == OVT_OK);
  int32_t next = 0;
  for (const auto& o : tus.at(0)) {
    const int32_t hint = (o.type == 3 || o.type == 6) ? next++ : next;
    REQUIRE(inst.api.push(inst.dec, o.bytes.data(), o.bytes.size(), hint, hint) == OVT_OK);
  }
  REQUIRE(inst.api.flush(inst.dec) == OVT_OK);
  ovt_Buffer buf{};
  REQUIRE(inst.api.pull(inst.dec, 0, &buf) == OVT_OK);
  REQUIRE(buf.data != nullptr);
  const uint8_t* data = nullptr;
  size_t size = 0;
  CHECK(inst.api.plane(inst.dec, "0/recon/y", &data, &size) == OVT_E_INVALID_ARG);
  CHECK(data == nullptr);
  inst.api.release(inst.dec, buf);
}

TEST_CASE("aom planes: film grain streams carry the film grain note", "[fast][third_party][needs-corpus]") {
  const auto id = GENERATE(as<std::string>{}, "aom_av1_1_b8_23_film_grain_50", "aom_av1_1_b10_23_film_grain_50");
  INFO(id);
  const auto frames = analyse(ovt::test::corpus_or_skip(id), kReconMask, 10);
  REQUIRE(!frames.empty());
  size_t noted = 0;
  for (const FrameOut& f : frames) {
    INFO("decode index " << f.decode_index);
    CHECK(f.invalid.empty());
    noted += std::count(f.info.begin(), f.info.end(), "DEC-AV1-0001 film grain present (not applied)") > 0 ? 1U : 0U;
    for (const PlaneOut& p : f.planes) {
      CHECK(p.sample_bytes == (p.bit_depth > 8 ? 2 : 1));
    }
  }
  CHECK(noted > 0);
}

TEST_CASE("aom planes: 10-bit planes are 16-bit little-endian samples below 1 << bit_depth",
          "[fast][third_party][needs-corpus]") {
  const auto frames = analyse(ovt::test::corpus_or_skip("aom_av1_1_b10_00_quantizer_10"), kReconMask, 3);
  REQUIRE(!frames.empty());
  for (const FrameOut& f : frames) {
    INFO("decode index " << f.decode_index);
    CHECK(f.invalid.empty());
    for (const PlaneOut& p : f.planes) {
      CHECK(p.bit_depth == 10);
      CHECK(p.sample_bytes == 2);
      uint16_t max = 0;
      for (size_t i = 0; i + 1 < p.rows.size(); i += 2) {
        max = std::max<uint16_t>(max, static_cast<uint16_t>(p.rows[i] | (p.rows[i + 1] << 8U)));
      }
      CHECK(max < 1024);
    }
  }
}

TEST_CASE("aom planes: super-resolution frames have UpscaledWidth planes over a FrameWidth grid",
          "[fast][third_party][needs-corpus]") {
  // §14.3.3 recipe 62: --superres-mode=1 --superres-denominator=12, every frame downscaled by 8/12.
  const auto frames = analyse(ovt::test::corpus_or_skip("syn_av1_tools_all"), kReconMask, 6);
  REQUIRE(!frames.empty());
  size_t superres = 0;
  for (const FrameOut& f : frames) {
    INFO("decode index " << f.decode_index);
    CHECK(f.invalid.empty());  // V-023: plane 0 is UpscaledWidth wide when use_superres = 1
    CHECK(f.frame_width == static_cast<uint32_t>(f.coded_w));
    if (!f.use_superres) {
      CHECK(f.upscaled_width == f.frame_width);
      continue;
    }
    ++superres;
    CHECK(f.upscaled_width > f.frame_width);
    if (f.shown) {
      REQUIRE(!f.planes.empty());
      CHECK(f.planes[0].width == f.upscaled_width);
    }
  }
  CHECK(superres > 0);
}

TEST_CASE("aom planes: the same stream gives byte-identical planes", "[fast][third_party][needs-corpus]") {
  const auto path = ovt::test::corpus_or_skip("aom_av1_1_b8_05_mv");
  const auto a = analyse(path, kReconMask, 20);
  const auto b = analyse(path, kReconMask, 20);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i].planes.size() == b[i].planes.size());
    for (size_t p = 0; p < a[i].planes.size(); ++p) {
      CHECK(a[i].planes[p].rows == b[i].planes[p].rows);  // determinism, §0.7.5
    }
  }
}
