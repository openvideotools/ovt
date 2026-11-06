// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PlaneBlob and PictureStore (chapter 06 §6.6, WI-0.087).

#include "ovt/cxx/model/picture_store.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ovt/cxx/model/plane.hpp"
#include "ovt/cxx/model/units.hpp"

namespace ovt::model {

// ---- PlaneView helpers ---------------------------------------------------------------------------------

std::uint32_t aligned_stride(std::uint32_t width, std::uint8_t sample_bytes) {
  const std::uint64_t row = std::uint64_t{width} * sample_bytes;
  const std::uint64_t stride = (row + kPlaneAlignment - 1) / kPlaneAlignment * kPlaneAlignment;
  if (stride > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("plane row does not fit in a 32-bit stride");
  }
  return static_cast<std::uint32_t>(stride);
}

void check_plane_view(const PlaneView& view) {
  const std::uint8_t expected_bytes = sample_bytes_of(view.format);
  if (expected_bytes == 0 || view.sample_bytes != expected_bytes) {
    throw std::invalid_argument("plane sample_bytes does not match its format");
  }
  if (view.bit_depth == 0 || view.bit_depth > expected_bytes * 8U) {
    throw std::invalid_argument("plane bit_depth does not fit its format");
  }
  if (std::uint64_t{view.stride_bytes} < std::uint64_t{view.width} * view.sample_bytes) {
    throw std::invalid_argument("plane stride_bytes is smaller than width * sample_bytes");
  }
  if (view.width != 0 && view.height != 0 && view.data == nullptr) {
    throw std::invalid_argument("plane data is null");
  }
}

// ---- PlaneBlob -------------------------------------------------------------------------------------------

PlaneBlob::PlaneBlob(Token /*unused*/, std::string blob_id, const PlaneView& view, std::uint8_t* memory, bool aligned,
                     std::size_t size_bytes) noexcept
    : blob_id_(std::move(blob_id)), view_(view), memory_(memory), aligned_(aligned), size_bytes_(size_bytes) {}

PlaneBlob::~PlaneBlob() {
  if (memory_ == nullptr) {
    return;
  }
  if (aligned_) {
    ::operator delete(memory_, std::align_val_t{kPlaneAlignment});
  } else {
    delete[] memory_;  // NOLINT(cppcoreguidelines-owning-memory): released from the adopted unique_ptr<T[]>
  }
}

namespace {

bool is_empty(const PlaneView& view) noexcept {
  return view.width == 0 || view.height == 0;
}

std::size_t checked_bytes(std::uint32_t stride, std::uint32_t height) {
  const std::uint64_t bytes = std::uint64_t{stride} * height;
  if (bytes > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("plane does not fit in memory");
  }
  return static_cast<std::size_t>(bytes);
}

// Bytes a put of @p src accounts for, before the blob exists (see PlaneBlob::size_bytes()).
std::size_t planned_bytes(const PlaneView& src, bool adopted) {
  if (is_empty(src)) {
    return 0;
  }
  const std::uint32_t stride = adopted ? src.stride_bytes : aligned_stride(src.width, src.sample_bytes);
  return checked_bytes(stride, src.height);
}

}  // namespace

std::shared_ptr<const PlaneBlob> PlaneBlob::copy_of(std::string blob_id, const PlaneView& src) {
  check_plane_view(src);
  PlaneView view = src;
  if (is_empty(src)) {
    view.data = nullptr;
    view.stride_bytes = aligned_stride(src.width, src.sample_bytes);
    return std::make_shared<const PlaneBlob>(Token{}, std::move(blob_id), view, nullptr, true, 0);
  }
  view.stride_bytes = aligned_stride(src.width, src.sample_bytes);
  const std::size_t bytes = checked_bytes(view.stride_bytes, src.height);
  const std::size_t row = std::size_t{src.width} * src.sample_bytes;
  auto* memory = static_cast<std::uint8_t*>(::operator new(bytes, std::align_val_t{kPlaneAlignment}));
  // Rows are copied exactly; the padding is zeroed so hashing or dumping the whole buffer is deterministic.
  const std::span<std::uint8_t> dst(memory, bytes);
  const std::span<const std::uint8_t> from(src.data, std::size_t{src.stride_bytes} * (src.height - 1) + row);
  for (std::uint32_t y = 0; y < src.height; ++y) {
    const auto dst_row = dst.subspan(std::size_t{y} * view.stride_bytes, view.stride_bytes);
    const auto src_row = from.subspan(std::size_t{y} * src.stride_bytes, row);
    std::copy(src_row.begin(), src_row.end(), dst_row.begin());
    std::fill(dst_row.begin() + static_cast<std::ptrdiff_t>(row), dst_row.end(), std::uint8_t{0});
  }
  view.data = memory;
  try {
    return std::make_shared<const PlaneBlob>(Token{}, std::move(blob_id), view, memory, true, bytes);
  } catch (...) {
    ::operator delete(memory, std::align_val_t{kPlaneAlignment});
    throw;
  }
}

std::shared_ptr<const PlaneBlob> PlaneBlob::adopt(
    std::string blob_id, const PlaneView& src,
    std::unique_ptr<std::uint8_t[]> memory) {  // NOLINT(*-avoid-c-arrays): WI-0.087 interface
  check_plane_view(src);
  if (!is_empty(src) && src.data != memory.get()) {
    throw std::invalid_argument("adopted plane data must point at the start of the adopted memory");
  }
  const std::size_t bytes = planned_bytes(src, true);
  auto blob = std::make_shared<const PlaneBlob>(Token{}, std::move(blob_id), src, memory.get(), false, bytes);
  (void)memory.release();  // owned by the blob now
  return blob;
}

// ---- PictureStore ----------------------------------------------------------------------------------------

namespace {

enum class Category : std::uint8_t { Analysis = 0, Plane = 1, Map = 2 };
constexpr std::size_t kCategories = 3;

struct FrameKey {
  std::array<std::uint8_t, 32> hash{};
  std::int32_t decode_index = 0;
  friend auto operator<=>(const FrameKey&, const FrameKey&) = default;
};

struct PinKey {
  Category category = Category::Plane;
  FrameKey frame;
  friend auto operator<=>(const PinKey&, const PinKey&) = default;
};

struct Frame {
  FrameKey key;
  std::uint64_t bytes = 0;
  std::uint64_t last_use = 0;  // store-wide access tick; orders frames across categories
  std::map<std::string, std::shared_ptr<const PlaneBlob>, std::less<>> blobs;  // Plane / Map
  std::shared_ptr<const std::vector<std::uint8_t>> analysis;                   // Analysis
};

struct Lru {
  std::list<Frame> frames;  // most recently used first
  std::map<FrameKey, std::list<Frame>::iterator> index;
};

struct ParsedId {
  Category category = Category::Plane;
  FrameKey frame;
};

bool parse_id(std::string_view blob_id, ParsedId& out) {
  BlobIdParts parts;
  if (!parse_blob_id(blob_id, parts)) {
    return false;
  }
  out.category = parts.kind == BlobKind::MetricMap ? Category::Map : Category::Plane;
  out.frame.hash = parts.source_hash;
  out.frame.decode_index = parts.decode_index;
  return true;
}

ParsedId parse_id_or_throw(std::string_view blob_id) {
  ParsedId id;
  if (!parse_id(blob_id, id)) {
    throw std::invalid_argument("blob_id does not follow the chapter 06 §6.6.2 grammar");
  }
  return id;
}

FrameKey key_of(std::span<const std::uint8_t, 32> hash, int decode_index) {
  FrameKey key;
  std::copy(hash.begin(), hash.end(), key.hash.begin());
  key.decode_index = decode_index;
  return key;
}

// 0.6 × cap, without overflow (§6.6.3 trim threshold).
std::uint64_t soft_cap(std::uint64_t cap) noexcept {
  return cap / 10 * 6 + cap % 10 * 6 / 10;
}

}  // namespace

struct PictureStore::Impl {
  explicit Impl(const Limits& l) : limits(l) {}

  Limits limits;
  mutable std::mutex mutex;
  std::array<Lru, kCategories> lru;
  std::uint64_t resident = 0;
  std::array<std::uint64_t, kCategories> category_bytes{};
  std::uint64_t tick = 0;
  std::uint64_t evictions = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::map<std::string, std::uint32_t, std::less<>> blob_pins;
  std::map<PinKey, std::uint32_t> frame_pins;
  std::shared_ptr<const EvictCallback> on_evict;

  Lru& of(Category c) { return lru.at(static_cast<std::size_t>(c)); }

  [[nodiscard]] std::uint32_t budget(Category c) const {
    switch (c) {
      case Category::Analysis:
        return limits.analysis_frames;
      case Category::Plane:
        return limits.plane_frames;
      case Category::Map:
        return limits.map_frames;
    }
    return 0;
  }

  [[nodiscard]] bool pinned(Category c, const FrameKey& key) const { return frame_pins.contains(PinKey{c, key}); }

  // Whether the frame (@p c, @p key) may be evicted while @p protect is being written.
  [[nodiscard]] bool evictable(Category c, const FrameKey& key, const PinKey& protect) const {
    return (c != protect.category || key != protect.frame) && !pinned(c, key);
  }

  // The existing frame, or nullptr.
  Frame* find(Category c, const FrameKey& key) {
    Lru& l = of(c);
    auto it = l.index.find(key);
    return it == l.index.end() ? nullptr : &*it->second;
  }

  // Makes the frame the most recently used of its category.
  void touch(Category c, const FrameKey& key) {
    Lru& l = of(c);
    auto it = l.index.find(key);
    if (it == l.index.end()) {
      return;
    }
    l.frames.splice(l.frames.begin(), l.frames, it->second);
    it->second->last_use = ++tick;
  }

  // The existing frame or a new, empty, most recently used one.
  Frame& find_or_create(Category c, const FrameKey& key) {
    Lru& l = of(c);
    auto it = l.index.find(key);
    if (it != l.index.end()) {
      touch(c, key);
      return *it->second;
    }
    l.frames.push_front(Frame{key, 0, ++tick, {}, {}});
    l.index.emplace(key, l.frames.begin());
    return l.frames.front();
  }

  // The bytes of the frame @p id names, and of its blob @p blob_id; zero when absent.
  std::pair<std::uint64_t, std::uint64_t> current_bytes(const ParsedId& id, std::string_view blob_id) {
    const Frame* f = find(id.category, id.frame);
    if (f == nullptr) {
      return {0, 0};
    }
    const auto it = f->blobs.find(blob_id);
    return {f->bytes, it == f->blobs.end() ? 0 : it->second->size_bytes()};
  }

  // Bytes held by unpinned frames other than @p protect (which may not exist yet).
  [[nodiscard]] std::uint64_t evictable_bytes(const PinKey& protect) const {
    std::uint64_t total = 0;
    for (std::size_t c = 0; c < kCategories; ++c) {
      const auto cat = static_cast<Category>(c);
      for (const Frame& f : lru.at(c).frames) {
        if (evictable(cat, f.key, protect)) {
          total += f.bytes;
        }
      }
    }
    return total;
  }

  // Throws NoMemError, changing nothing, when replacing @p old_bytes of frame @p target by @p new_bytes
  // cannot fit under the hard cap (§6.6.3).
  void check_fits(const PinKey& target, std::uint64_t frame_bytes_now, std::uint64_t old_bytes,
                  std::uint64_t new_bytes) const {
    const std::uint64_t cap = limits.hard_cap_bytes;
    const std::uint64_t frame_rest = frame_bytes_now - old_bytes;
    if (new_bytes > cap || frame_rest > cap - new_bytes) {
      throw NoMemError("frame exceeds the picture store hard memory cap (MDL-GEN-0004)");
    }
    const std::uint64_t base = resident - old_bytes;
    if (base > cap - new_bytes) {
      // Over the cap: evicting every unpinned frame except the target must make enough room.
      const std::uint64_t needed = base - (cap - new_bytes);
      if (evictable_bytes(target) < needed) {
        throw NoMemError("picture store hard memory cap reached; only pinned frames are left to evict");
      }
    }
  }

  // Removes the least recently used frame of @p c that is neither pinned nor @p protect; false if none.
  // Moves its blobs into @p evicted.
  bool evict_one(Category c, const PinKey& protect, std::vector<std::shared_ptr<const PlaneBlob>>& evicted) {
    Lru& l = of(c);
    for (auto it = l.frames.end(); it != l.frames.begin();) {
      --it;
      if (!evictable(c, it->key, protect)) {
        continue;
      }
      for (auto& [id, blob] : it->blobs) {
        evicted.push_back(std::move(blob));
      }
      resident -= it->bytes;
      category_bytes.at(static_cast<std::size_t>(c)) -= it->bytes;
      l.index.erase(it->key);
      l.frames.erase(it);
      ++evictions;
      return true;
    }
    return false;
  }

  // The category whose least recently used evictable frame is the oldest store-wide.
  bool oldest_evictable(const PinKey& protect, Category& out) const {
    bool found = false;
    std::uint64_t best = 0;
    for (std::size_t c = 0; c < kCategories; ++c) {
      const auto cat = static_cast<Category>(c);
      const auto& frames = lru.at(c).frames;
      for (const Frame& f : std::ranges::reverse_view(frames)) {
        if (!evictable(cat, f.key, protect)) {
          continue;
        }
        if (!found || f.last_use < best) {
          found = true;
          best = f.last_use;
          out = cat;
        }
        break;
      }
    }
    return found;
  }

  // Frame budgets first, then the 0.6 × cap trim (§6.6.3). Pinned frames may keep a category over budget.
  void trim(const PinKey& protect, std::vector<std::shared_ptr<const PlaneBlob>>& evicted) {
    const Category c = protect.category;
    while (of(c).frames.size() > budget(c) && evict_one(c, protect, evicted)) {
    }
    const std::uint64_t soft = soft_cap(limits.hard_cap_bytes);
    Category victim = Category::Plane;
    while (resident > soft && oldest_evictable(protect, victim)) {
      evict_one(victim, protect, evicted);
    }
  }

  void add_bytes(Category c, Frame& f, std::uint64_t old_bytes, std::uint64_t new_bytes) {
    f.bytes = f.bytes - old_bytes + new_bytes;
    resident = resident - old_bytes + new_bytes;
    auto& cb = category_bytes.at(static_cast<std::size_t>(c));
    cb = cb - old_bytes + new_bytes;
  }
};

PictureStore::PictureStore(Limits limits) {
  if (limits.analysis_frames == 0 || limits.plane_frames == 0 || limits.map_frames == 0 || limits.hard_cap_bytes == 0) {
    throw std::invalid_argument("PictureStore limits must be non-zero");
  }
  impl_ = std::make_unique<Impl>(limits);
}

PictureStore::~PictureStore() = default;

namespace {

void notify(const std::shared_ptr<const PictureStore::EvictCallback>& callback,
            const std::vector<std::shared_ptr<const PlaneBlob>>& evicted) noexcept {
  if (!callback || !*callback) {
    return;
  }
  for (const auto& blob : evicted) {
    (*callback)(blob->blob_id(), blob->view());
  }
}

}  // namespace

std::shared_ptr<const PlaneBlob> PictureStore::put(
    std::string blob_id, PlaneView src,
    std::unique_ptr<std::uint8_t[]> adopt) {  // NOLINT(*-avoid-c-arrays): WI-0.087 interface
  const ParsedId id = parse_id_or_throw(blob_id);
  const PinKey target{id.category, id.frame};
  check_plane_view(src);
  const std::uint64_t new_bytes = planned_bytes(src, static_cast<bool>(adopt));

  // Refuse an oversized frame before allocating or copying anything (acceptance test 3).
  {
    const std::scoped_lock lock(impl_->mutex);
    const auto [frame_now, old_bytes] = impl_->current_bytes(id, blob_id);
    impl_->check_fits(target, frame_now, old_bytes, new_bytes);
  }

  // The copy runs without the lock (WI-0.087 step 4).
  std::shared_ptr<const PlaneBlob> blob =
      adopt ? PlaneBlob::adopt(std::move(blob_id), src, std::move(adopt)) : PlaneBlob::copy_of(std::move(blob_id), src);

  std::vector<std::shared_ptr<const PlaneBlob>> evicted;
  std::shared_ptr<const EvictCallback> callback;
  {
    const std::scoped_lock lock(impl_->mutex);
    // Re-check: another thread may have changed the frame or the totals since the first check.
    const auto [frame_now, old_bytes] = impl_->current_bytes(id, blob->blob_id());
    impl_->check_fits(target, frame_now, old_bytes, blob->size_bytes());
    Frame& f = impl_->find_or_create(id.category, id.frame);
    auto [it, inserted] = f.blobs.try_emplace(blob->blob_id(), blob);
    if (!inserted) {
      it->second = blob;  // a replacement is not an eviction and is never reported
    }
    impl_->add_bytes(id.category, f, old_bytes, blob->size_bytes());
    impl_->trim(target, evicted);
    callback = impl_->on_evict;
  }
  notify(callback, evicted);
  return blob;
}

std::shared_ptr<const PlaneBlob> PictureStore::get(std::string_view blob_id) {
  ParsedId id;
  const bool valid = parse_id(blob_id, id);
  const std::scoped_lock lock(impl_->mutex);
  if (valid) {
    if (Frame* f = impl_->find(id.category, id.frame); f != nullptr) {
      if (auto it = f->blobs.find(blob_id); it != f->blobs.end()) {
        impl_->touch(id.category, id.frame);
        ++impl_->hits;
        return it->second;
      }
    }
  }
  ++impl_->misses;
  return nullptr;
}

void PictureStore::pin(std::string_view blob_id) {
  const ParsedId id = parse_id_or_throw(blob_id);
  const std::scoped_lock lock(impl_->mutex);
  auto it = impl_->blob_pins.find(blob_id);
  if (it == impl_->blob_pins.end()) {
    it = impl_->blob_pins.emplace(std::string(blob_id), 0).first;
  }
  ++it->second;
  ++impl_->frame_pins[PinKey{id.category, id.frame}];
}

void PictureStore::unpin(std::string_view blob_id) {
  ParsedId id;
  if (!parse_id(blob_id, id)) {
    return;
  }
  const std::scoped_lock lock(impl_->mutex);
  auto it = impl_->blob_pins.find(blob_id);
  if (it == impl_->blob_pins.end()) {
    return;
  }
  if (--it->second == 0) {
    impl_->blob_pins.erase(it);
  }
  auto fit = impl_->frame_pins.find(PinKey{id.category, id.frame});
  if (fit != impl_->frame_pins.end() && --fit->second == 0) {
    impl_->frame_pins.erase(fit);
  }
}

void PictureStore::put_analysis(int decode_index, std::span<const std::uint8_t, 32> hash,
                                std::vector<std::uint8_t> fa) {
  if (decode_index < 0) {
    throw std::invalid_argument("decode_index must not be negative");
  }
  const FrameKey key = key_of(hash, decode_index);
  const PinKey target{Category::Analysis, key};
  auto buffer = std::make_shared<const std::vector<std::uint8_t>>(std::move(fa));
  const std::uint64_t new_bytes = buffer->size();

  std::vector<std::shared_ptr<const PlaneBlob>> evicted;
  std::shared_ptr<const EvictCallback> callback;
  {
    const std::scoped_lock lock(impl_->mutex);
    const Frame* existing = impl_->find(Category::Analysis, key);
    const std::uint64_t old_bytes = existing != nullptr ? existing->bytes : 0;
    impl_->check_fits(target, old_bytes, old_bytes, new_bytes);
    Frame& f = impl_->find_or_create(Category::Analysis, key);
    f.analysis = std::move(buffer);
    impl_->add_bytes(Category::Analysis, f, old_bytes, new_bytes);
    impl_->trim(target, evicted);
    callback = impl_->on_evict;
  }
  notify(callback, evicted);  // only plane/map frames are reported; analysis evictions add nothing here
}

std::shared_ptr<const std::vector<std::uint8_t>> PictureStore::get_analysis(int decode_index,
                                                                            std::span<const std::uint8_t, 32> hash) {
  const FrameKey key = key_of(hash, decode_index);
  const std::scoped_lock lock(impl_->mutex);
  if (Frame* f = impl_->find(Category::Analysis, key); f != nullptr) {
    impl_->touch(Category::Analysis, key);
    ++impl_->hits;
    return f->analysis;
  }
  ++impl_->misses;
  return nullptr;
}

PictureStore::Stats PictureStore::stats() const {
  const std::scoped_lock lock(impl_->mutex);
  Stats s;
  s.resident_bytes = impl_->resident;
  s.analysis_bytes = impl_->category_bytes.at(static_cast<std::size_t>(Category::Analysis));
  s.plane_bytes = impl_->category_bytes.at(static_cast<std::size_t>(Category::Plane));
  s.map_bytes = impl_->category_bytes.at(static_cast<std::size_t>(Category::Map));
  const auto count_blobs = [](const Lru& l) {
    std::uint32_t n = 0;
    for (const Frame& f : l.frames) {
      n += static_cast<std::uint32_t>(f.blobs.size());
    }
    return n;
  };
  s.analysis_frames = static_cast<std::uint32_t>(impl_->of(Category::Analysis).frames.size());
  s.plane_frames = static_cast<std::uint32_t>(impl_->of(Category::Plane).frames.size());
  s.map_frames = static_cast<std::uint32_t>(impl_->of(Category::Map).frames.size());
  s.plane_blobs = count_blobs(impl_->of(Category::Plane));
  s.map_blobs = count_blobs(impl_->of(Category::Map));
  for (std::size_t c = 0; c < kCategories; ++c) {
    for (const Frame& f : impl_->lru.at(c).frames) {
      if (impl_->pinned(static_cast<Category>(c), f.key)) {
        ++s.pinned_frames;
      }
    }
  }
  s.evictions = impl_->evictions;
  s.hits = impl_->hits;
  s.misses = impl_->misses;
  return s;
}

void PictureStore::set_evict_callback(EvictCallback callback) {
  auto shared = callback ? std::make_shared<const EvictCallback>(std::move(callback)) : nullptr;
  const std::scoped_lock lock(impl_->mutex);
  impl_->on_evict = std::move(shared);
}

}  // namespace ovt::model
