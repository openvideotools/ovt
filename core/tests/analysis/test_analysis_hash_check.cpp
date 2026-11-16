// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-4.109: picture-hash checks (hash_check.hpp). The expected MD5 / CRC / checksum values of the plane fixtures
// were computed once with an independent Python implementation of H.265 D.3.19 (hashlib.md5 for MD5) and are
// hard-coded here; the MD5 class is also checked against the RFC 1321 test suite.

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <catch2/catch_test_macros.hpp>

#include "hash_check.hpp"
#include "ovt/cxx/model/picture_store.hpp"
#include "ovt/cxx/model/units.hpp"
#include "ovt_model_generated.h"

using namespace ovt::analysis;
using ovt::model::PlaneView;
using ovt::model::SampleFormat;

namespace {

struct Plane {
  std::vector<uint8_t> bytes;
  PlaneView view;
};

// Samples v(x, y) in a plane with `pad` extra stride bytes per row.
template <typename Fn>
Plane make_plane(uint32_t w, uint32_t h, uint8_t depth, Fn&& v, uint32_t pad = 3) {
  Plane p;
  const uint32_t sb = depth > 8 ? 2U : 1U;
  const uint32_t stride = (w * sb) + pad;
  p.bytes.assign(static_cast<std::size_t>(stride) * h, 0xEE);  // padding bytes must not matter
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t s = v(x, y);
      p.bytes[(static_cast<std::size_t>(y) * stride) + (x * sb)] = static_cast<uint8_t>(s & 0xFFU);
      if (sb == 2U) {
        p.bytes[(static_cast<std::size_t>(y) * stride) + (x * sb) + 1U] = static_cast<uint8_t>(s >> 8U);
      }
    }
  }
  p.view.data = p.bytes.data();
  p.view.width = w;
  p.view.height = h;
  p.view.stride_bytes = stride;
  p.view.bit_depth = depth;
  p.view.sample_bytes = static_cast<uint8_t>(sb);
  p.view.format = sb == 2U ? SampleFormat::U16LE : SampleFormat::U8;
  return p;
}

uint32_t f8(uint32_t x, uint32_t y) {
  return ((x * 7U) + (y * 13U) + 1U) & 0xFFU;
}
uint32_t f10(uint32_t x, uint32_t y) {
  return ((x * 37U) + (y * 101U) + 5U) & 0x3FFU;
}

Md5Digest from_hex(const std::string& s) {
  Md5Digest d{};
  for (std::size_t i = 0; i < 16; ++i) {
    d[i] = static_cast<uint8_t>(std::stoul(s.substr(2 * i, 2), nullptr, 16));
  }
  return d;
}

std::string to_hex(const Md5Digest& d) {
  std::string s;
  for (const uint8_t b : d) {
    s += "0123456789abcdef"[b >> 4U];
    s += "0123456789abcdef"[b & 15U];
  }
  return s;
}

std::string md5_of(const std::string& text) {
  Md5 m;
  m.update({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
  return to_hex(m.finish());
}

}  // namespace

TEST_CASE("Md5 matches the RFC 1321 test suite", "[analysis][hash][fast]") {
  CHECK(md5_of("") == "d41d8cd98f00b204e9800998ecf8427e");
  CHECK(md5_of("a") == "0cc175b9c0f1b6a831c399e269772661");
  CHECK(md5_of("abc") == "900150983cd24fb0d6963f7d28e17f72");
  CHECK(md5_of("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
  CHECK(md5_of("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
  CHECK(md5_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") == "d174ab98d277d9f5a5611c2c9f419d9f");
  CHECK(md5_of("1234567890123456789012345678901234567890123456789012345678901234567890"
               "1234567890") == "57edf4a22be3c955ac49da2e2107b67a");

  // Incremental updates split across block boundaries give the same digest.
  const std::string text(200, 'q');
  Md5 whole;
  whole.update({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
  Md5 parts;
  std::size_t off = 0;
  for (const std::size_t n : {1U, 62U, 1U, 64U, 72U}) {
    parts.update({reinterpret_cast<const uint8_t*>(text.data()) + off, n});
    off += n;
  }
  CHECK(parts.finish() == whole.finish());
}

TEST_CASE("D.3.19 plane hashes: 8-bit, 10-bit and a plane wider than 256", "[analysis][hash][fast]") {
  const Plane a = make_plane(5, 3, 8, f8);
  CHECK(to_hex(plane_md5(a.view)) == "680af2edbb30a2de23cc49b9f7a71c0b");
  CHECK(plane_crc(a.view) == 0x5cb4U);
  CHECK(plane_checksum(a.view) == 0x19dU);

  const Plane b = make_plane(6, 4, 10, f10);  // two bytes little-endian per sample
  CHECK(to_hex(plane_md5(b.view)) == "a692b0caa1226ca5d66ae021d34a8af9");
  CHECK(plane_crc(b.view) == 0xc990U);
  CHECK(plane_checksum(b.view) == 0xcbdU);

  const Plane c = make_plane(300, 2, 8, f8, 0);  // x >> 8 enters the checksum mask
  CHECK(to_hex(plane_md5(c.view)) == "da7fde018204cf74b5e7e7827ed5e381");
  CHECK(plane_crc(c.view) == 0x6914U);
  CHECK(plane_checksum(c.view) == 0x10c60U);
}

namespace {

ovt::syntax::h265::DecodedPictureHash hash_of(uint32_t type, const std::vector<Plane>& planes) {
  ovt::syntax::h265::DecodedPictureHash h;
  h.hash_type = type;
  h.num_components = static_cast<uint32_t>(planes.size());
  for (std::size_t c = 0; c < planes.size(); ++c) {
    h.picture_md5[c] = plane_md5(planes[c].view);
    h.picture_crc[c] = plane_crc(planes[c].view);
    h.picture_checksum[c] = plane_checksum(planes[c].view);
  }
  return h;
}

std::vector<PlaneView> views(const std::vector<Plane>& planes) {
  std::vector<PlaneView> v;
  for (const Plane& p : planes) {
    v.push_back(p.view);
  }
  return v;
}

}  // namespace

TEST_CASE("check_decoded_picture_hash: match, one flipped sample, cropped and reserved", "[analysis][hash][fast]") {
  for (const uint32_t type : {0U, 1U, 2U}) {
    DYNAMIC_SECTION("hash_type " << type) {
      std::vector<Plane> planes;
      planes.push_back(make_plane(16, 8, 8, f8));
      planes.push_back(make_plane(8, 4, 8, f8));
      planes.push_back(make_plane(8, 4, 8, f10));
      const auto hash = hash_of(type, planes);

      const auto ok = check_decoded_picture_hash(hash, views(planes), 16, 8, "de265");
      CHECK(ok.status == HashStatus::Match);

      planes[1].bytes[0] ^= 0x01U;  // one flipped Cb sample
      const auto bad = check_decoded_picture_hash(hash, views(planes), 16, 8, "de265");
      CHECK(bad.status == HashStatus::Mismatch);
      CHECK(bad.message == "decoded picture hash mismatch (plugin de265)");
      CHECK_FALSE(bad.component_mismatch[0]);
      CHECK(bad.component_mismatch[1]);
      CHECK_FALSE(bad.component_mismatch[2]);

      // The hash covers the uncropped picture: a plane of another size is never a mismatch.
      const auto cropped = check_decoded_picture_hash(hash, views(planes), 16, 6, "de265");
      CHECK(cropped.status == HashStatus::Skipped);
    }
  }
  std::vector<Plane> mono;
  mono.push_back(make_plane(16, 8, 8, f8));
  auto mono_hash = hash_of(0, mono);
  CHECK(check_decoded_picture_hash(mono_hash, views(mono), 16, 8, "p").status == HashStatus::Match);  // 4:0:0
  mono_hash.hash_type = 3;                                                                            // reserved
  CHECK(check_decoded_picture_hash(mono_hash, views(mono), 16, 8, "p").status == HashStatus::Skipped);
  mono_hash.hash_type = 0;
  CHECK(check_decoded_picture_hash(mono_hash, {}, 16, 8, "p").status == HashStatus::Skipped);  // no planes
}

TEST_CASE("AV1 frame MD5 and the side file", "[analysis][hash][fast]") {
  std::vector<Plane> planes;
  planes.push_back(make_plane(6, 4, 10, f10));
  planes.push_back(make_plane(3, 2, 10, f10));
  const Md5Digest expected = frame_md5(views(planes));
  CHECK(check_frame_md5(expected, views(planes), "aom").status == HashStatus::Match);
  planes[0].bytes[2] ^= 0x04U;
  const auto bad = check_frame_md5(expected, views(planes), "aom");
  CHECK(bad.status == HashStatus::Mismatch);
  CHECK(bad.message == "decoded picture hash mismatch (plugin aom)");

  const std::string text =
      "# frame md5\n680af2edbb30a2de23cc49b9f7a71c0b  frame0.yuv\r\n\n  a692B0CAA1226CA5D66AE021D34A8AF9\n"
      "not a hash line\n680af2edbb30a2de23cc49b9f7a71c0bff\n680af2edbb30a2de23cc49b9f7a71c0\n";
  const auto list = parse_md5_side_file(text);
  REQUIRE(list.size() == 2);
  CHECK(to_hex(list[0]) == "680af2edbb30a2de23cc49b9f7a71c0b");
  CHECK(to_hex(list[1]) == "a692b0caa1226ca5d66ae021d34a8af9");
  CHECK(parse_md5_side_file("").empty());
  CHECK(from_hex("00112233445566778899aabbccddeeff")[15] == 0xFF);
}

namespace {

// A FrameAnalysis whose Reconstruction PlaneRefs name `ids` (plane 0, 1, ...).
std::vector<uint8_t> frame_with_planes(int decode_index, const std::vector<std::string>& ids) {
  flatbuffers::FlatBufferBuilder fbb;
  std::vector<flatbuffers::Offset<ovt::fb::PlaneRef>> refs;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    refs.push_back(ovt::fb::CreatePlaneRef(fbb, ovt::fb::PlaneStage::Reconstruction, static_cast<uint8_t>(i), 16, 8, 16,
                                           8, 1, false, fbb.CreateString(ids[i])));
  }
  const ovt::fb::FrameId fid(decode_index, decode_index);
  const auto info = ovt::fb::CreateFrameInfo(fbb, &fid);
  const auto plane_refs = fbb.CreateVector(refs);
  ovt::fb::FrameAnalysisBuilder b(fbb);
  b.add_frame(info);
  b.add_planes(plane_refs);
  fbb.Finish(b.Finish());
  return {fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize()};
}

}  // namespace

TEST_CASE("PictureHashCheck reads the stored Reconstruction planes and raises VER-0004", "[analysis][hash][fast]") {
  ovt::model::PictureStore store({});
  const std::array<uint8_t, 32> src{};
  std::vector<Plane> planes;
  planes.push_back(make_plane(16, 8, 8, f8));
  planes.push_back(make_plane(8, 4, 8, f8));
  planes.push_back(make_plane(8, 4, 8, f8));
  std::vector<std::string> ids;
  for (std::uint8_t c = 0; c < 3; ++c) {
    ids.push_back(ovt::model::blob_id(src, 7, ovt::fb::PlaneStage::Reconstruction, c));
    (void)store.put(ids.back(), planes[c].view);
  }
  const auto buffer = frame_with_planes(7, ids);
  const auto* fa = ovt::fb::GetFrameAnalysis(buffer.data());

  PictureHashCheck check(store, "de265");
  std::vector<HostDiagnostic> out;

  check.check(7, *fa, out);  // nothing expected for frame 7: no hashing, no finding
  CHECK(out.empty());

  check.expect_h265(7, hash_of(1, planes), 16, 8);
  check.check(7, *fa, out);
  CHECK(out.empty());  // matching CRC

  auto wrong = hash_of(1, planes);
  wrong.picture_crc[2] ^= 1U;
  check.expect_h265(7, wrong, 16, 8);
  check.check(7, *fa, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].code == "VER-0004");
  CHECK(out[0].severity == ovt::fb::Severity::Error);
  CHECK(out[0].message == "decoded picture hash mismatch (plugin de265)");

  // AV1 expectation on the same frame planes: the digest of Y + U + V as the output frame.
  PictureHashCheck av1(store, "aom");
  out.clear();
  av1.expect_md5(7, frame_md5(views(planes)));
  av1.check(7, *fa, out);
  CHECK(out.empty());
  Md5Digest other = frame_md5(views(planes));
  other[0] ^= 0xFFU;
  av1.expect_md5(7, other);
  av1.check(7, *fa, out);
  REQUIRE(out.size() == 1);
  CHECK(out[0].message == "decoded picture hash mismatch (plugin aom)");

  // A frame whose planes the store no longer holds is not checked.
  out.clear();
  const auto gone = frame_with_planes(7, {ovt::model::blob_id(src, 99, ovt::fb::PlaneStage::Reconstruction, 0)});
  av1.check(7, *ovt::fb::GetFrameAnalysis(gone.data()), out);
  CHECK(out.empty());
}
