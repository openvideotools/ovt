// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-0.087 acceptance tests 1–4: PictureStore budgets, LRU order, pinning, the hard cap, concurrency and
// zero-copy adopt (chapter 06 §6.6).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ovt/cxx/model/picture_store.hpp"
#include "ovt/cxx/model/plane.hpp"
#include "ovt/cxx/model/units.hpp"

namespace {

using ovt::fb::PlaneStage;
using ovt::model::NoMemError;
using ovt::model::PictureStore;
using ovt::model::PlaneBlob;
using ovt::model::PlaneView;
using ovt::model::SampleFormat;

using Hash = std::array<std::uint8_t, 32>;

Hash hash_of(std::uint8_t seed) {
  Hash h{};
  for (std::size_t i = 0; i < h.size(); ++i) {
    h.at(i) = static_cast<std::uint8_t>(seed + i);
  }
  return h;
}

const Hash kHash = hash_of(0xA0);

std::string id(int decode_index, std::uint8_t plane, PlaneStage stage = PlaneStage::Reconstruction,
               const Hash& hash = kHash) {
  return ovt::model::blob_id(hash, decode_index, stage, plane);
}

std::string map_id(int decode_index) {
  ovt::model::BlobIdParts parts;
  parts.source_hash = kHash;
  parts.decode_index = decode_index;
  parts.kind = ovt::model::BlobKind::MetricMap;
  parts.metric_id = "psnr";
  return ovt::model::blob_id(parts);
}

// Source plane with a tight stride and a recognisable pattern.
struct SourcePlane {
  std::vector<std::uint8_t> bytes;
  PlaneView view;
};

SourcePlane make_plane(std::uint32_t width, std::uint32_t height, SampleFormat format = SampleFormat::U8,
                       std::uint8_t bit_depth = 8, std::uint8_t plane = 0) {
  SourcePlane p;
  p.view.width = width;
  p.view.height = height;
  p.view.format = format;
  p.view.sample_bytes = ovt::model::sample_bytes_of(format);
  p.view.bit_depth = bit_depth;
  p.view.plane = plane;
  p.view.stride_bytes = width * p.view.sample_bytes;
  p.bytes.resize(std::size_t{p.view.stride_bytes} * height);
  for (std::size_t i = 0; i < p.bytes.size(); ++i) {
    p.bytes[i] = static_cast<std::uint8_t>(i * 7 + 3);
  }
  p.view.data = p.bytes.data();
  return p;
}

// Puts the three planes of a 4:2:0 8-bit recon frame.
void put_frame(PictureStore& store, int decode_index, const std::array<SourcePlane, 3>& planes) {
  for (std::uint8_t c = 0; c < 3; ++c) {
    (void)store.put(id(decode_index, c), planes.at(c).view);
  }
}

std::array<SourcePlane, 3> planes_1080p() {
  return {make_plane(1920, 1080, SampleFormat::U8, 8, 0), make_plane(960, 540, SampleFormat::U8, 8, 1),
          make_plane(960, 540, SampleFormat::U8, 8, 2)};
}

bool has_frame(PictureStore& store, int decode_index) {
  return store.get(id(decode_index, 0)) != nullptr && store.get(id(decode_index, 1)) != nullptr &&
         store.get(id(decode_index, 2)) != nullptr;
}

}  // namespace

TEST_CASE("WI-0.087 AT1: the plane budget keeps the 4 most recent 1080p frames", "[model][picture_store]") {
  PictureStore store(PictureStore::Limits{});
  const auto planes = planes_1080p();
  for (int f = 0; f < 5; ++f) {
    put_frame(store, f, planes);
  }
  CHECK(store.get(id(0, 0)) == nullptr);
  CHECK(store.get(id(0, 1)) == nullptr);
  for (int f = 1; f <= 4; ++f) {
    CHECK(has_frame(store, f));
  }
  const auto s = store.stats();
  CHECK(s.plane_frames == 4);
  CHECK(s.plane_blobs == 12);
  CHECK(s.evictions == 1);
  // Both strides are already multiples of 64 (1920 = 30 × 64, 960 = 15 × 64).
  CHECK(s.plane_bytes == 4ULL * (1920 * 1080 + 2 * 960 * 540));
}

TEST_CASE("PictureStore LRU order follows the last access, not the insertion", "[model][picture_store]") {
  PictureStore store(PictureStore::Limits{});
  const auto p = make_plane(64, 64);
  for (int f = 0; f < 4; ++f) {
    (void)store.put(id(f, 0), p.view);
  }
  REQUIRE(store.get(id(0, 0)) != nullptr);  // frame 0 becomes the most recently used
  (void)store.put(id(4, 0), p.view);
  CHECK(store.get(id(0, 0)) != nullptr);
  CHECK(store.get(id(1, 0)) == nullptr);  // the least recently used one went
  // Another stage or plane of a resident frame touches it and does not count as a new frame.
  (void)store.put(id(2, 1, PlaneStage::Prediction), p.view);
  (void)store.put(id(5, 0), p.view);
  CHECK(store.get(id(2, 0)) != nullptr);
  CHECK(store.get(id(3, 0)) == nullptr);
  CHECK(store.stats().plane_frames == 4);
}

TEST_CASE("WI-0.087 AT2: a pinned frame survives 5 more frames", "[model][picture_store]") {
  PictureStore store(PictureStore::Limits{});
  const auto planes = planes_1080p();
  for (int f = 0; f < 4; ++f) {  // the budget is full
    put_frame(store, f, planes);
  }
  store.pin(id(0, 0));
  for (int f = 4; f < 9; ++f) {  // 5 more frames
    put_frame(store, f, planes);
  }
  CHECK(has_frame(store, 0));
  const auto s = store.stats();
  CHECK(s.evictions == 5);
  CHECK(s.pinned_frames == 1);
  for (int f = 1; f <= 5; ++f) {
    CHECK(store.get(id(f, 0)) == nullptr);
  }
  for (int f = 6; f <= 8; ++f) {
    CHECK(has_frame(store, f));
  }
  // Once unpinned and the least recently used, it is the next frame evicted.
  store.unpin(id(0, 0));
  CHECK(store.stats().pinned_frames == 0);
  for (int f = 6; f <= 8; ++f) {
    REQUIRE(has_frame(store, f));
  }
  put_frame(store, 9, planes);
  CHECK(store.get(id(0, 0)) == nullptr);
}

TEST_CASE("PictureStore pins are counted per blob_id and may precede the put", "[model][picture_store]") {
  PictureStore store(
      PictureStore::Limits{.analysis_frames = 64, .plane_frames = 1, .map_frames = 16, .hard_cap_bytes = 1ULL << 30U});
  const auto p = make_plane(16, 16);
  store.pin(id(7, 2));  // before the blob exists
  store.pin(id(7, 2));
  (void)store.put(id(7, 0), p.view);
  (void)store.put(id(8, 0), p.view);  // over budget, but frame 7 is pinned
  CHECK(store.get(id(7, 0)) != nullptr);
  store.unpin(id(7, 2));
  store.unpin(id(9, 0));  // never pinned: no-op
  (void)store.put(id(9, 0), p.view);
  CHECK(store.get(id(7, 0)) != nullptr);  // one pin is still held
  store.unpin(id(7, 2));
  (void)store.put(id(10, 0), p.view);
  CHECK(store.get(id(7, 0)) == nullptr);
  CHECK_THROWS_AS(store.pin("not-a-blob-id"), std::invalid_argument);
}

TEST_CASE("WI-0.087 AT3: an 8K 10-bit frame over a 64 MiB cap throws NoMemError", "[model][picture_store]") {
  PictureStore store(
      PictureStore::Limits{.analysis_frames = 64, .plane_frames = 4, .map_frames = 16, .hard_cap_bytes = 64ULL << 20U});
  // 7680×4320 4:2:0 10-bit: Y 66.4 MB + U 16.6 MB + V 16.6 MB ≈ 100 MB. Frames are accounted as a whole, so
  // the luma plane fits (66.4 MB < 64 MiB) and the first chroma plane takes the frame over the cap.
  std::vector<std::uint8_t> luma_bytes(std::size_t{7680} * 2 * 4320, 0x01);
  const PlaneView luma{.data = luma_bytes.data(),
                       .width = 7680,
                       .height = 4320,
                       .stride_bytes = 7680 * 2,
                       .bit_depth = 10,
                       .sample_bytes = 2,
                       .format = SampleFormat::U16LE,
                       .plane = 0,
                       .stage = 0};
  // A chroma view without backing memory is enough: the store refuses before it reads or allocates.
  static const std::uint8_t kNoMemory = 0;
  PlaneView chroma = luma;
  chroma.data = &kNoMemory;
  chroma.width = 3840;
  chroma.height = 2160;
  chroma.stride_bytes = 3840 * 2;
  chroma.plane = 1;
  (void)store.put(id(0, 0), luma);
  const auto before = store.stats();
  CHECK_THROWS_AS(store.put(id(0, 1), chroma), NoMemError);
  chroma.plane = 2;
  CHECK_THROWS_AS(store.put(id(0, 2), chroma), NoMemError);
  const auto after = store.stats();
  CHECK(after.evictions == 0);
  CHECK(after.resident_bytes == before.resident_bytes);
  CHECK(after.plane_blobs == 1);
  CHECK(store.get(id(0, 1)) == nullptr);

  // A single plane over the cap is refused too, and resident frames stay; NoMemError is a
  // std::runtime_error (mapped to OVT_E_NOMEM at the C API).
  PlaneView huge = chroma;
  huge.width = 8192;
  huge.height = 4320;
  huge.stride_bytes = 8192 * 2;
  try {
    (void)store.put(id(1, 0), huge);
    FAIL("expected NoMemError");
  } catch (const std::runtime_error& e) {
    CHECK(dynamic_cast<const NoMemError*>(&e) != nullptr);
  }
  CHECK(store.stats().evictions == 0);
  CHECK(store.get(id(0, 0)) != nullptr);
}

TEST_CASE("PictureStore refuses to evict pinned frames to make room", "[model][picture_store]") {
  // Cap 1 MiB, soft trim at 0.6 MiB; 400 KiB frames.
  PictureStore store(
      PictureStore::Limits{.analysis_frames = 64, .plane_frames = 4, .map_frames = 16, .hard_cap_bytes = 1ULL << 20U});
  const auto p = make_plane(640, 640);  // 409 600 bytes
  (void)store.put(id(0, 0), p.view);
  (void)store.put(id(1, 0), p.view);  // 800 KiB > 0.6 MiB: frame 0 is trimmed
  CHECK(store.get(id(0, 0)) == nullptr);
  CHECK(store.stats().evictions == 1);
  store.pin(id(1, 0));
  (void)store.put(id(2, 0), p.view);  // fits under the hard cap, pinned frame 1 stays
  CHECK(store.get(id(1, 0)) != nullptr);
  store.pin(id(2, 0));
  const auto before = store.stats();
  CHECK_THROWS_AS(store.put(id(3, 0), p.view), NoMemError);  // would need to evict a pinned frame
  const auto after = store.stats();
  CHECK(after.evictions == before.evictions);
  CHECK(after.resident_bytes == before.resident_bytes);
  CHECK(store.get(id(3, 0)) == nullptr);
}

TEST_CASE("PictureStore copies into 64-byte aligned rows, or adopts memory without a copy", "[model][picture_store]") {
  PictureStore store(PictureStore::Limits{});
  const auto p = make_plane(33, 5, SampleFormat::U16LE, 10, 1);  // 66-byte rows → stride 128
  const auto blob = store.put(id(0, 1), p.view);
  const PlaneView& v = blob->view();
  CHECK(v.stride_bytes == 128);
  CHECK(reinterpret_cast<std::uintptr_t>(v.data) % 64 == 0);
  CHECK(v.width == 33);
  CHECK(v.plane == 1);
  CHECK(blob->size_bytes() == std::size_t{128} * 5);
  for (std::uint32_t y = 0; y < 5; ++y) {
    for (std::uint32_t x = 0; x < 66; ++x) {
      REQUIRE(v.data[y * 128 + x] == p.bytes[y * 66 + x]);
    }
    for (std::uint32_t x = 66; x < 128; ++x) {
      REQUIRE(v.data[y * 128 + x] == 0);  // padding is zeroed
    }
  }

  // Zero-copy: the blob points at the adopted memory.
  constexpr std::uint32_t kStride = 40;
  // NOLINTNEXTLINE(*-avoid-c-arrays): adopt takes unique_ptr<T[]>
  auto memory = std::make_unique<std::uint8_t[]>(std::size_t{kStride} * 4);
  memory[0] = 0x5A;
  std::uint8_t* raw = memory.get();
  PlaneView src{.data = raw,
                .width = 32,
                .height = 4,
                .stride_bytes = kStride,
                .bit_depth = 8,
                .sample_bytes = 1,
                .format = SampleFormat::U8,
                .plane = 0,
                .stage = 0};
  const auto adopted = store.put(id(1, 0), src, std::move(memory));
  CHECK(adopted->view().data == raw);
  CHECK(adopted->view().stride_bytes == kStride);
  CHECK(adopted->size_bytes() == std::size_t{kStride} * 4);
  CHECK(store.get(id(1, 0))->view().data[0] == 0x5A);

  // NOLINTNEXTLINE(*-avoid-c-arrays): adopt takes unique_ptr<T[]>
  auto other = std::make_unique<std::uint8_t[]>(std::size_t{kStride} * 4);
  CHECK_THROWS_AS(store.put(id(2, 0), src, std::move(other)), std::invalid_argument);  // data elsewhere
}

TEST_CASE("PictureStore rejects malformed blob_ids, views and limits", "[model][picture_store]") {
  PictureStore store(PictureStore::Limits{});
  const auto p = make_plane(8, 8);
  CHECK_THROWS_AS(store.put("abc/0/recon/y", p.view), std::invalid_argument);
  CHECK(store.get("abc/0/recon/y") == nullptr);
  PlaneView bad = p.view;
  bad.sample_bytes = 2;  // format is U8
  CHECK_THROWS_AS(store.put(id(0, 0), bad), std::invalid_argument);
  bad = p.view;
  bad.stride_bytes = 4;
  CHECK_THROWS_AS(store.put(id(0, 0), bad), std::invalid_argument);
  bad = p.view;
  bad.data = nullptr;
  CHECK_THROWS_AS(store.put(id(0, 0), bad), std::invalid_argument);
  bad = p.view;
  bad.bit_depth = 9;
  CHECK_THROWS_AS(store.put(id(0, 0), bad), std::invalid_argument);
  CHECK_THROWS_AS(PictureStore(PictureStore::Limits{.analysis_frames = 0}), std::invalid_argument);
  CHECK_THROWS_AS(store.put_analysis(-1, kHash, {}), std::invalid_argument);
  CHECK(store.stats().resident_bytes == 0);
}

TEST_CASE("PictureStore replaces a blob with the same id and accounts its bytes once", "[model][picture_store]") {
  PictureStore store(PictureStore::Limits{});
  const auto small = make_plane(64, 1);
  const auto big = make_plane(64, 2);
  const auto first = store.put(id(0, 0), small.view);
  const auto second = store.put(id(0, 0), big.view);
  CHECK(store.get(id(0, 0)) == second);
  CHECK(first->view().height == 1);  // the old blob stays valid for its holder
  const auto s = store.stats();
  CHECK(s.plane_blobs == 1);
  CHECK(s.plane_bytes == 128);
  CHECK(s.evictions == 0);
}

TEST_CASE("PictureStore metric maps and FrameAnalysis buffers have their own budgets", "[model][picture_store]") {
  PictureStore store(
      PictureStore::Limits{.analysis_frames = 2, .plane_frames = 1, .map_frames = 2, .hard_cap_bytes = 1ULL << 30U});
  const auto p = make_plane(16, 16, SampleFormat::U8, 8, 0);
  (void)store.put(map_id(0), p.view);
  (void)store.put(id(0, 0), p.view);  // same frame key, other category
  (void)store.put(map_id(1), p.view);
  CHECK(store.get(map_id(0)) != nullptr);
  CHECK(store.get(id(0, 0)) != nullptr);
  (void)store.put(map_id(2), p.view);
  CHECK(store.get(map_id(1)) == nullptr);

  const Hash other = hash_of(0x10);
  store.put_analysis(0, kHash, std::vector<std::uint8_t>(100, 1));
  store.put_analysis(0, other, std::vector<std::uint8_t>(50, 2));  // a second stream, same decode_index
  REQUIRE(store.get_analysis(0, kHash) != nullptr);                // touch: `other` becomes the LRU
  store.put_analysis(1, kHash, std::vector<std::uint8_t>(10, 3));
  CHECK(store.get_analysis(0, other) == nullptr);
  const auto fa = store.get_analysis(0, kHash);
  REQUIRE(fa != nullptr);
  CHECK(fa->size() == 100);
  const auto s = store.stats();
  CHECK(s.analysis_frames == 2);
  CHECK(s.analysis_bytes == 110);
  CHECK(s.map_frames == 2);
  CHECK(s.plane_frames == 1);
  CHECK(s.resident_bytes == s.analysis_bytes + s.plane_bytes + s.map_bytes);
}

TEST_CASE("PictureStore reports evicted planes to the callback; holders keep evicted blobs alive",
          "[model][picture_store]") {
  PictureStore store(
      PictureStore::Limits{.analysis_frames = 64, .plane_frames = 1, .map_frames = 16, .hard_cap_bytes = 1ULL << 30U});
  std::vector<std::string> reported;
  store.set_evict_callback([&reported](std::string_view blob_id, PlaneView view) {
    REQUIRE(view.data != nullptr);
    reported.emplace_back(blob_id);
  });
  const auto p = make_plane(16, 16);
  const auto held = store.put(id(0, 0), p.view);
  (void)store.put(id(0, 1, PlaneStage::Residual), make_plane(8, 8, SampleFormat::S16LE, 16, 1).view);
  (void)store.put(id(1, 0), p.view);
  CHECK(reported == std::vector<std::string>{id(0, 0), id(0, 1, PlaneStage::Residual)});
  CHECK(store.get(id(0, 0)) == nullptr);
  CHECK(held->view().data[5] == p.bytes[5]);  // still readable after eviction
  store.set_evict_callback({});
  (void)store.put(id(2, 0), p.view);
  CHECK(reported.size() == 2);
}

TEST_CASE("WI-0.087 AT4: 8 threads put and get concurrently", "[model][picture_store][threads]") {
  // Run under the linux-x64-tsan preset for the race check; under other presets it checks consistency.
  PictureStore store(
      PictureStore::Limits{.analysis_frames = 8, .plane_frames = 4, .map_frames = 4, .hard_cap_bytes = 64ULL << 20U});
  std::atomic<std::uint64_t> reported{0};
  store.set_evict_callback([&reported](std::string_view, PlaneView) { reported.fetch_add(1); });
  const auto small = make_plane(64, 32);
  const auto large = make_plane(1024, 1100);  // > 1 MiB: copied outside the lock
  constexpr int kThreads = 8;
  constexpr int kIterations = 200;
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kIterations; ++i) {
        const int frame = (t * 7 + i) % 16;
        const std::string blob_id = id(frame, static_cast<std::uint8_t>(i % 3));
        const auto& src = (i % 10 == 0) ? large : small;
        const auto put = store.put(blob_id, src.view);
        if (put->view().data[1] != src.bytes[1]) {
          errors.fetch_add(1);
        }
        if (const auto got = store.get(id((frame + 1) % 16, 0)); got && got->view().width == 0) {
          errors.fetch_add(1);
        }
        if (i % 4 == 0) {
          store.pin(blob_id);
          store.unpin(blob_id);
        }
        store.put_analysis(frame, kHash, std::vector<std::uint8_t>(64, static_cast<std::uint8_t>(t)));
        (void)store.get_analysis((frame + 3) % 16, kHash);
        (void)store.stats();
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }
  CHECK(errors.load() == 0);
  const auto s = store.stats();
  CHECK(s.plane_frames <= 4);
  CHECK(s.analysis_frames <= 8);
  CHECK(s.resident_bytes <= (64ULL << 20U));
  CHECK(s.resident_bytes == s.plane_bytes + s.map_bytes + s.analysis_bytes);
  CHECK(reported.load() > 0);
}
