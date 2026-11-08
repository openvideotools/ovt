// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// SelectionModel (chapter 11 §11.4.1; WI-1.452): the single source of truth for what is selected (frame, block,
// motion vector, unit, sample, bytes, field, probe, rectangle), exposed to QML as the `Selection` singleton.
//
// Panels call the setters; the model fills every derived field itself (never a panel), applies the change
// synchronously and emits one `changed(mask)` whose mask has a bit for each part of the state that actually
// changed, derived parts included. Echo suppression keys on `origin` (the panel id that made the change): a panel
// ignores a `changed` for which isEcho(its id) is true. It never compares values.
//
// SPEC-ASSUMPTION(#219): the derivations need StreamIndex lookups, but the app does not link the FlatBuffers
// runtime or the generated ovt_model headers before the index model of WI-1.455. The model therefore reads the
// index through the small SelectionIndex interface below, and FrameId/ByteSpan/BitSpan are app-side mirrors of
// the ovt.fb structs of the same names (same fields, same meaning). WI-1.455 adds the adapter over fb::StreamIndex.
// The model never calls the core beyond these lookups (WI-1.452 pitfalls).

#ifndef OVT_APP_MODELS_SELECTION_MODEL_H
#define OVT_APP_MODELS_SELECTION_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QtGlobal>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace ovt::app {

// Mirrors of ovt.fb.FrameId / ByteSpan / BitSpan (core/schema/ovt_model.fbs).
struct FrameId {
  std::int32_t decode_index = -1;
  std::int32_t display_index = -1;
  friend constexpr bool operator==(FrameId, FrameId) noexcept = default;
};
struct ByteSpan {
  std::uint64_t offset = 0;
  std::uint64_t length = 0;  // 0 = none
  friend constexpr bool operator==(ByteSpan, ByteSpan) noexcept = default;
};
struct BitSpan {
  std::uint64_t bit_offset = 0;  // from bit 0 of the owning unit (NAL header bit 0 + RBSP, or obu_header bit 0)
  std::uint32_t bit_length = 0;  // 0 = none
  friend constexpr bool operator==(BitSpan, BitSpan) noexcept = default;
};

// What the model needs to know about one StreamIndex unit.
struct SelectionUnit {
  ByteSpan file_span;                            // start code / length prefix excluded
  std::span<const std::uint32_t> epb_positions;  // ascending raw offsets within file_span; empty for AV1
  FrameId frame;                                 // decode_index < 0: non-VCL unit
  int sample_index = -1;
};

// What the model needs to know about one container sample.
struct SelectionSample {
  ByteSpan span;
  FrameId frame;        // decode_index < 0: the sample holds no frame
  int first_unit = -1;  // -1 = none
};

// The StreamIndex lookups behind the derivations. All are cheap, synchronous and never touch the core.
class SelectionIndex {
 public:
  SelectionIndex() = default;
  virtual ~SelectionIndex() = default;
  SelectionIndex(const SelectionIndex&) = delete;
  SelectionIndex& operator=(const SelectionIndex&) = delete;
  SelectionIndex(SelectionIndex&&) = delete;
  SelectionIndex& operator=(SelectionIndex&&) = delete;

  // The unit with this index; nullopt when it does not exist. The span in the result stays valid while the index lives.
  [[nodiscard]] virtual std::optional<SelectionUnit> unit(int unit_index) const = 0;
  // Fills both indices of a frame given with either one (the other is -1); nullopt when the frame is unknown.
  [[nodiscard]] virtual std::optional<FrameId> resolve_frame(FrameId frame) const = 0;
  // The first VCL unit of the frame, or -1.
  [[nodiscard]] virtual int first_vcl_unit(FrameId frame) const = 0;
  // The container sample; nullopt when it does not exist.
  [[nodiscard]] virtual std::optional<SelectionSample> sample(int sample_index) const = 0;
  // The unit whose prefix or payload contains this file offset, or -1.
  [[nodiscard]] virtual int unit_at_byte(std::uint64_t file_offset) const = 0;
};

class SelectionModel : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(Selection)
  QML_SINGLETON
  Q_PROPERTY(QString fileId READ file_id NOTIFY changed)
  Q_PROPERTY(int trackIndex READ track_index NOTIFY changed)
  Q_PROPERTY(int frameDecodeIndex READ frame_decode_index NOTIFY changed)
  Q_PROPERTY(int frameDisplayIndex READ frame_display_index NOTIFY changed)
  Q_PROPERTY(bool displayOrder READ display_order NOTIFY changed)
  Q_PROPERTY(quint32 blockId READ block_id NOTIFY changed)
  Q_PROPERTY(int level READ level NOTIFY changed)
  Q_PROPERTY(int mvIndex READ mv_index NOTIFY changed)
  Q_PROPERTY(int unitIndex READ unit_index NOTIFY changed)
  Q_PROPERTY(int sampleIndex READ sample_index NOTIFY changed)
  Q_PROPERTY(qlonglong byteOffset READ byte_offset NOTIFY changed)
  Q_PROPERTY(qlonglong byteLength READ byte_length NOTIFY changed)
  Q_PROPERTY(qlonglong bitOffset READ bit_offset NOTIFY changed)
  Q_PROPERTY(int bitLength READ bit_length NOTIFY changed)
  Q_PROPERTY(QString fieldPath READ field_path NOTIFY changed)
  Q_PROPERTY(QPoint probe READ probe NOTIFY changed)
  Q_PROPERTY(QRect selectionRect READ selection_rect NOTIFY changed)
  Q_PROPERTY(QString origin READ origin NOTIFY changed)
  // "Lock pixel selection" (Picture panel): the probe survives a frame change.
  Q_PROPERTY(bool lockPixelSelection READ lock_pixel_selection WRITE set_lock_pixel_selection NOTIFY lockChanged)

 public:
  // The bits of changed(mask), §11.4.1.
  enum Change : unsigned {
    ChangeNone = 0,
    ChangeFrame = 1U << 0,
    ChangeBlock = 1U << 1,  // block_id or level
    ChangeMv = 1U << 2,
    ChangeUnit = 1U << 3,
    ChangeSample = 1U << 4,
    ChangeBytes = 1U << 5,
    ChangeField = 1U << 6,  // field_path or bit_range
    ChangeProbe = 1U << 7,
    ChangeRect = 1U << 8,
    ChangeOrder = 1U << 9,
  };
  Q_ENUM(Change)

  static constexpr std::uint32_t kNoBlock = 0xFFFFFFFFU;

  // Number of entries of the codec's block cycle chain at this leaf block (H.264 MB → sub-MB: 2; HEVC CTU → CU → PU
  // → TU: up to 4; AV1 SB → block → TXB: 3, §11.3.9). Supplied by the Picture panel from the FrameAnalysis.
  using ChainLength = std::function<int(std::uint32_t block_id)>;

  explicit SelectionModel(QObject* parent = nullptr);
  ~SelectionModel() override;
  SelectionModel(const SelectionModel&) = delete;
  SelectionModel& operator=(const SelectionModel&) = delete;
  SelectionModel(SelectionModel&&) = delete;
  SelectionModel& operator=(SelectionModel&&) = delete;

  // The StreamIndex lookups of the open stream (nullptr = none open; derivations then only store what they are given).
  void setIndex(std::shared_ptr<const SelectionIndex> index);
  void setChainLength(ChainLength chain_length);

  // ---- API of §11.4.1 -------------------------------------------------------------------------------------------
  // Sets the file and track (A side) and clears the rest of the selection.
  void setSource(const QString& file_id, int track_index, const QString& origin);
  void setFrame(FrameId frame, const QString& origin);
  // `level` >= 0 selects that level of the chain; -1 means "next": the level after the current one when the same leaf
  // is selected again (wrapping after the chain length), else 0. Setting a block clears mv_index.
  void setBlock(std::uint32_t block_id, int level, const QString& origin);
  void setMv(int mv_index, const QString& origin);
  void setUnit(int unit_index, const QString& origin);
  void setSample(int sample_index, const QString& origin);
  void setBytes(ByteSpan bytes, const QString& origin);
  void setField(int unit_index, const QString& path, BitSpan bits, const QString& origin);
  void setProbe(QPoint probe, const QString& origin);
  void setRect(QRect rect, const QString& origin);
  void setDisplayOrder(bool display_order, const QString& origin);
  void clearBlock(const QString& origin = QString());
  // Resets the whole selection except file, track and order.
  void clear(const QString& origin = QString());

  // QML entry points (the structs above are not QML types). -1 stands for "none" in every index argument.
  Q_INVOKABLE void selectFrame(int decode_index, int display_index, const QString& origin);
  Q_INVOKABLE void selectBlock(quint32 block_id, int level, const QString& origin);
  Q_INVOKABLE void selectMv(int mv_index, const QString& origin);
  Q_INVOKABLE void selectUnit(int unit_index, const QString& origin);
  Q_INVOKABLE void selectSample(int sample_index, const QString& origin);
  Q_INVOKABLE void selectBytes(qlonglong offset, qlonglong length, const QString& origin);
  Q_INVOKABLE void selectField(int unit_index, const QString& path, qlonglong bit_offset, int bit_length,
                               const QString& origin);
  Q_INVOKABLE void selectProbe(int x, int y, const QString& origin);
  Q_INVOKABLE void selectRect(const QRect& rect, const QString& origin);
  Q_INVOKABLE void selectDisplayOrder(bool display_order, const QString& origin);
  Q_INVOKABLE void clearBlockSelection(const QString& origin);
  Q_INVOKABLE void clearSelection(const QString& origin);
  // True when the change being signalled came from `panel_id`: the panel must ignore it.
  Q_INVOKABLE [[nodiscard]] bool isEcho(const QString& panel_id) const;

  [[nodiscard]] QString file_id() const { return s_.file_id; }
  [[nodiscard]] int track_index() const { return s_.track_index; }
  [[nodiscard]] FrameId frame() const { return s_.frame; }
  [[nodiscard]] int frame_decode_index() const { return s_.frame.decode_index; }
  [[nodiscard]] int frame_display_index() const { return s_.frame.display_index; }
  [[nodiscard]] bool display_order() const { return s_.display_order; }
  [[nodiscard]] quint32 block_id() const { return s_.block_id; }
  [[nodiscard]] int level() const { return s_.level; }
  [[nodiscard]] int mv_index() const { return s_.mv_index; }
  [[nodiscard]] int unit_index() const { return s_.unit_index; }
  [[nodiscard]] int sample_index() const { return s_.sample_index; }
  [[nodiscard]] ByteSpan byte_range() const { return s_.byte_range; }
  [[nodiscard]] BitSpan bit_range() const { return s_.bit_range; }
  [[nodiscard]] qlonglong byte_offset() const { return static_cast<qlonglong>(s_.byte_range.offset); }
  [[nodiscard]] qlonglong byte_length() const { return static_cast<qlonglong>(s_.byte_range.length); }
  [[nodiscard]] qlonglong bit_offset() const { return static_cast<qlonglong>(s_.bit_range.bit_offset); }
  [[nodiscard]] int bit_length() const { return static_cast<int>(s_.bit_range.bit_length); }
  [[nodiscard]] QString field_path() const { return s_.field_path; }
  [[nodiscard]] QPoint probe() const { return s_.probe; }
  [[nodiscard]] QRect selection_rect() const { return s_.selection_rect; }
  [[nodiscard]] QString origin() const { return s_.origin; }
  [[nodiscard]] bool lock_pixel_selection() const { return lock_pixel_; }
  void set_lock_pixel_selection(bool lock);

  // §11.3.5, the normative RBSP→raw mapping: file offset of bit `bit_offset` of a unit (the bit within the byte is
  // unchanged, bit_offset % 8). Exposed for the Hex panel and the tests.
  [[nodiscard]] static std::uint64_t raw_offset_of_bit(const SelectionUnit& unit, std::uint64_t bit_offset);
  // File bytes covered by a field: first byte of its first bit to the byte of its last bit, EPBs inside included.
  [[nodiscard]] static ByteSpan field_bytes(const SelectionUnit& unit, BitSpan bits);

 Q_SIGNALS:
  // One signal per applied change; `mask` is a bit set of Change values. The new state is already visible.
  void changed(int mask);
  void lockChanged();

 private:
  struct State {
    QString file_id;
    int track_index = 0;
    FrameId frame;
    bool display_order = true;
    std::uint32_t block_id = kNoBlock;
    int level = -1;
    int mv_index = -1;
    int unit_index = -1;
    int sample_index = -1;
    ByteSpan byte_range;
    BitSpan bit_range;
    QString field_path;
    QPoint probe{-1, -1};
    QRect selection_rect;
    QString origin;
  };

  // Fills unit-derived fields of `s` from unit `unit_index`: unit_index, frame (VCL only), sample_index and, when
  // `with_bytes`, byte_range = file_span. Field and bit range are cleared. Keeps the rest.
  void derive_from_unit(State& s, int unit_index, bool with_bytes) const;
  void set_frame_keep_rules(State& s, FrameId frame) const;
  void commit(State next, const QString& origin);
  [[nodiscard]] static unsigned diff(const State& a, const State& b);

  State s_;
  bool lock_pixel_ = false;
  std::shared_ptr<const SelectionIndex> index_;
  ChainLength chain_length_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_SELECTION_MODEL_H
