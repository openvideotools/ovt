// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// HexModel (chapter 11 §11.3.5; WI-1.458): the Hex panel's table. Rows are 16 bytes (offset, 16 byte cells, ASCII);
// the bytes are paged in from a ByteSource in 64 KiB pages (4096 rows) and never loaded whole. The model also owns
// everything the panel computes from the stream: the unit / box bands, the highlight of the selected field after the
// normative RBSP→raw mapping (SelectionModel::raw_offset_of_bit; partial bytes keep their bit mask), the EPB marks,
// Go to and the reverse lookup byte → unit / box / field.
//
// SPEC-ASSUMPTION(#704): the app has no SyntaxNode reader yet (the Syntax panel is WI-1.457), so the per-unit flattened
// field spans of §11.3.5 are supplied through setFieldProvider(); without one the reverse lookup stops at the unit.
// The byte source is likewise given by the caller (ContainerByteSource over the core's byte service).

#ifndef OVT_APP_MODELS_HEX_MODEL_H
#define OVT_APP_MODELS_HEX_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractTableModel>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "models/jobs_model.h"
#include "models/selection_model.h"
#include "models/stream_index_adapter.h"
#include "models/stream_index_model.h"
#include "ovt/container.h"

namespace ovt::app {

// Random access to the bytes of the open file. read() is thread-safe.
class ByteSource {
 public:
  ByteSource() = default;
  virtual ~ByteSource() = default;
  ByteSource(const ByteSource&) = delete;
  ByteSource& operator=(const ByteSource&) = delete;
  ByteSource(ByteSource&&) = delete;
  ByteSource& operator=(ByteSource&&) = delete;

  [[nodiscard]] virtual std::uint64_t size() const = 0;
  // Copies up to `len` bytes at `offset`; returns the number copied (short at the end of the file, 0 on error).
  [[nodiscard]] virtual std::size_t read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const = 0;
};

class MemoryByteSource final : public ByteSource {
 public:
  explicit MemoryByteSource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }
  [[nodiscard]] std::size_t read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const override;

 private:
  std::vector<std::uint8_t> bytes_;
};

// The bytes of an open container through the core's byte service (chapter 08 §8.8, `ovt_container_bytes`: at most
// 1 MiB per call, bounded by the core's page cache). The app never opens or maps the media file itself. The container
// must outlive the source.
class ContainerByteSource final : public ByteSource {
 public:
  explicit ContainerByteSource(ovt_Container* container);
  [[nodiscard]] std::uint64_t size() const override;
  [[nodiscard]] std::size_t read(std::uint64_t offset, std::uint8_t* dst, std::size_t len) const override;

 private:
  ovt_Container* container_;
};

// One band of the file: a unit (start code / length prefix + payload) or a container box (header + payload).
struct HexBand {
  enum class Kind : std::uint8_t { Unit, Box };
  std::uint64_t offset = 0;  // first byte of the prefix / header
  std::uint64_t prefix = 0;  // length of the prefix / header
  std::uint64_t length = 0;  // payload length
  Kind kind = Kind::Unit;
  int index = -1;  // unit index / box ordinal
  [[nodiscard]] std::uint64_t end() const { return offset + prefix + length; }
};

// The unit bands of a StreamIndex (prefix_span + file_span), sorted by offset.
[[nodiscard]] std::vector<HexBand> hex_bands_from_index(const fb::StreamIndex& index);

// A field of a unit, for the reverse lookup: bits are relative to the unit like SyntaxNode.span.
struct HexField {
  QString path;
  BitSpan bits;
};
using HexFieldProvider = std::function<std::vector<HexField>(int unit_index)>;

// A byte pattern with wildcards (`00 00 01`, `00 ?? 01`, `000001`).
struct HexPattern {
  std::vector<std::uint8_t> bytes;
  std::vector<std::uint8_t> mask;  // 0xFF: must match, 0: wildcard
  [[nodiscard]] std::size_t size() const { return bytes.size(); }
};
[[nodiscard]] std::optional<HexPattern> parse_hex_pattern(const QString& text, QString* error = nullptr);

// Every offset in [begin, end) where `pattern` starts (overlapping matches included), ascending, at most
// `max_results`. `cancelled` is polled once per chunk. Reads through the byte source in chunks.
[[nodiscard]] std::vector<std::uint64_t> hex_find_all(const ByteSource& source, const HexPattern& pattern,
                                                      std::uint64_t begin, std::uint64_t end, std::size_t max_results,
                                                      const std::function<bool()>& cancelled = {});

// The result of Go to: a file offset, or the reason.
struct HexGoto {
  std::optional<std::uint64_t> offset;
  int unit = -1;  // for `unit:N` / `frame:N`
  QString error;
};

class HexModel : public QAbstractTableModel {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(StreamIndexModel* streamIndex READ stream_index WRITE set_stream_index NOTIFY streamIndexChanged)
  Q_PROPERTY(SelectionModel* selection READ selection WRITE set_selection NOTIFY selectionChanged)
  Q_PROPERTY(JobsModel* jobs READ jobs WRITE set_jobs NOTIFY jobsChanged)
  Q_PROPERTY(qulonglong fileSize READ file_size NOTIFY sourceChanged)
  Q_PROPERTY(bool hasSource READ has_source NOTIFY sourceChanged)
  // 0 File, 1 Unit, 2 Box: restricts the rows to the selected unit / box (offsets stay absolute).
  Q_PROPERTY(int scope READ scope WRITE set_scope NOTIFY scopeChanged)
  Q_PROPERTY(bool showRibbon READ show_ribbon WRITE set_show_ribbon NOTIFY ribbonChanged)
  // 8 bytes around the field start: [{offset, value, bits, mask, epb}] and the field's name.
  Q_PROPERTY(QVariantList ribbon READ ribbon NOTIFY highlightChanged)
  Q_PROPERTY(QString ribbonField READ ribbon_field NOTIFY highlightChanged)
  Q_PROPERTY(int matchCount READ match_count NOTIFY searchChanged)
  Q_PROPERTY(int currentMatch READ current_match NOTIFY searchChanged)
  Q_PROPERTY(bool searching READ searching NOTIFY searchChanged)
  Q_PROPERTY(bool truncated READ truncated NOTIFY searchChanged)

 public:
  static constexpr int kBytesPerRow = 16;
  static constexpr int kPageRows = 4096;
  static constexpr std::uint64_t kPageBytes = static_cast<std::uint64_t>(kBytesPerRow) * kPageRows;  // 64 KiB
  static constexpr std::size_t kMaxCachedPages = 32;
  static constexpr std::size_t kMaxMatches = 1000000;
  static constexpr int kRibbonBytes = 8;

  enum Column : int { ColOffset = 0, ColByte0 = 1, ColAscii = 17, ColumnCount = 18 };
  Q_ENUM(Column)

  enum Role : int {
    ByteRole = Qt::UserRole + 1,  // 0..255, -1 when not loaded or outside the view
    BandRole,                     // BandTint
    EpbRole,                      // emulation-prevention byte (dim + strikethrough)
    HighlightRole,                // bits of the byte inside the selected field (MSB = bit 0 of the byte), 0 none
    LoadedRole,                   // the page is in memory (false: grey cell)
    SelectedRole,                 // inside the selected byte range
  };
  Q_ENUM(Role)

  // The tint classes of the bands (§11.3.5): unit A/B alternate, the prefix is darker; the same for boxes.
  enum BandTint : int { TintNone = 0, TintUnitA, TintUnitB, TintUnitPrefix, TintBoxA, TintBoxB, TintBoxHeader };
  Q_ENUM(BandTint)

  enum Scope : int { ScopeFile = 0, ScopeUnit, ScopeBox };
  Q_ENUM(Scope)

  explicit HexModel(QObject* parent = nullptr);
  ~HexModel() override;
  HexModel(const HexModel&) = delete;
  HexModel& operator=(const HexModel&) = delete;
  HexModel(HexModel&&) = delete;
  HexModel& operator=(HexModel&&) = delete;

  // ---- inputs -----------------------------------------------------------------------------------------------
  [[nodiscard]] StreamIndexModel* stream_index() const { return stream_index_; }
  void set_stream_index(StreamIndexModel* model);
  [[nodiscard]] SelectionModel* selection() const { return selection_; }
  void set_selection(SelectionModel* selection);
  [[nodiscard]] JobsModel* jobs() const { return jobs_.data(); }
  void set_jobs(JobsModel* jobs);
  // The open file's bytes (nullptr clears).
  void setSource(std::shared_ptr<const ByteSource> source);
  Q_INVOKABLE void closeFile() { setSource(nullptr); }
  // Shows these bytes as the file (tests and the panel's own QML tests; the app proper uses ContainerByteSource).
  Q_INVOKABLE void openBytes(const QByteArray& bytes);
  // The StreamIndex of the stream (tests and callers without a StreamIndexModel): bands and unit lookups.
  void setHandle(std::shared_ptr<const StreamIndexHandle> handle);
  // Extra bands (container boxes) beside the unit bands; replaces the previous box bands.
  void setBoxBands(std::vector<HexBand> boxes);
  void setFieldProvider(HexFieldProvider provider);
  // The byte span of the selected container box for the Box scope.
  void setBoxSpan(ByteSpan span);

  [[nodiscard]] std::uint64_t file_size() const;
  [[nodiscard]] bool has_source() const { return source_ != nullptr; }
  [[nodiscard]] int scope() const noexcept { return static_cast<int>(scope_); }
  void set_scope(int scope);
  [[nodiscard]] bool show_ribbon() const noexcept { return show_ribbon_; }
  void set_show_ribbon(bool show);

  // ---- QAbstractTableModel ----------------------------------------------------------------------------------
  [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] QVariant data(const QModelIndex& idx, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                    int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  // ---- geometry ---------------------------------------------------------------------------------------------
  // The row showing a file offset, or -1 when it is outside the view; the offset of the first byte of a row.
  Q_INVOKABLE [[nodiscard]] int rowOfOffset(qlonglong offset) const;
  Q_INVOKABLE [[nodiscard]] qlonglong offsetOfRow(int row) const;
  Q_INVOKABLE [[nodiscard]] QString offsetText(qlonglong offset) const;
  // Loads every page that was asked for and is still missing, now (the panel lets the event loop do it).
  Q_INVOKABLE void loadPendingPages();

  // ---- navigation -------------------------------------------------------------------------------------------
  // Parses `0x…`, decimal, `unit:N` and `frame:N` (decode index; the frame's first VCL unit).
  [[nodiscard]] HexGoto parseGoto(const QString& text) const;
  // Go to: selects the target and asks the view to show it. Returns "" or the reason.
  Q_INVOKABLE QString goTo(const QString& text, const QString& origin = QStringLiteral("hex"));
  // Click / drag on bytes: selects them and, through the reverse lookup, the unit and the deepest field.
  Q_INVOKABLE void selectByteRange(qlonglong first, qlonglong last, const QString& origin = QStringLiteral("hex"));
  // Byte → {unit, box, fieldPath, bitOffset, bitLength} (-1 / empty when there is none).
  Q_INVOKABLE [[nodiscard]] QVariantMap lookup(qlonglong offset) const;
  Q_INVOKABLE [[nodiscard]] int bandTintAt(qlonglong offset) const;
  // Copy helpers of the context menu.
  Q_INVOKABLE [[nodiscard]] QString copyHex(qlonglong offset, qlonglong length) const;
  Q_INVOKABLE [[nodiscard]] QString copyCArray(qlonglong offset, qlonglong length) const;

  // ---- search -----------------------------------------------------------------------------------------------
  // Starts the search job; returns "" or why the pattern is invalid. Replaces the previous search.
  Q_INVOKABLE QString search(const QString& pattern);
  Q_INVOKABLE void cancelSearch();
  Q_INVOKABLE bool findNext();
  Q_INVOKABLE bool findPrevious();
  [[nodiscard]] const std::vector<std::uint64_t>& matches() const noexcept { return matches_; }
  [[nodiscard]] int match_count() const noexcept { return static_cast<int>(matches_.size()); }
  [[nodiscard]] int current_match() const noexcept { return current_match_; }
  [[nodiscard]] bool searching() const noexcept { return searching_; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }

  [[nodiscard]] QVariantList ribbon() const;
  [[nodiscard]] QString ribbon_field() const { return highlight_.name; }

 Q_SIGNALS:
  void streamIndexChanged();
  void selectionChanged();
  void jobsChanged();
  void sourceChanged();
  void scopeChanged();
  void ribbonChanged();
  void highlightChanged();
  void searchChanged();
  // The view should scroll so that this row is visible (Go to, find next, selection from another panel).
  void revealRequested(int row);

 private:
  // Called on the GUI thread (queued, by name) when the search job has stored its result.
  Q_INVOKABLE void takeSearchResult(quint64 generation);

  struct Highlight {
    bool active = false;
    std::uint64_t first = 0;  // file offsets of the first and last covered byte
    std::uint64_t last = 0;
    std::uint8_t first_mask = 0xFF;  // covered bits of those bytes
    std::uint8_t last_mask = 0xFF;
    bool is_field = false;
    QString name;
  };
  struct Page {
    QByteArray bytes;
  };

  void apply_index(std::shared_ptr<const SelectionIndex> index, std::shared_ptr<const StreamIndexHandle> handle);
  void rebuild_bands();
  void rebuild_view();
  void on_selection_changed(int mask);
  void recompute_highlight();
  void reveal(std::uint64_t offset);
  [[nodiscard]] std::optional<std::uint64_t> first_view_row() const;
  [[nodiscard]] const HexBand* band_at(std::uint64_t offset, std::size_t* ordinal = nullptr) const;
  [[nodiscard]] bool is_epb(std::uint64_t offset) const;
  [[nodiscard]] std::uint8_t highlight_mask(std::uint64_t offset) const;
  void finish_search(quint64 generation, std::vector<std::uint64_t> found, std::size_t pattern_size);
  void select_match(int index);
  [[nodiscard]] const Page* page_for(std::uint64_t offset) const;
  [[nodiscard]] std::optional<std::uint8_t> byte_at(std::uint64_t offset) const;
  [[nodiscard]] QString ascii_of_row(std::uint64_t row_offset) const;
  [[nodiscard]] QVariant byte_cell(std::uint64_t off, int role) const;

  QPointer<JobsModel> jobs_;
  StreamIndexModel* stream_index_ = nullptr;
  SelectionModel* selection_ = nullptr;
  std::shared_ptr<const ByteSource> source_;
  std::shared_ptr<const SelectionIndex> index_;
  std::shared_ptr<const StreamIndexHandle> handle_;
  HexFieldProvider field_provider_;
  mutable QHash<int, std::vector<HexField>> field_cache_;

  std::vector<HexBand> unit_bands_;
  std::vector<HexBand> box_bands_;
  std::vector<HexBand> bands_;  // both, sorted by offset
  ByteSpan box_span_;
  Scope scope_ = ScopeFile;
  bool show_ribbon_ = true;
  std::uint64_t view_begin_ = 0;
  std::uint64_t view_end_ = 0;
  Highlight highlight_;

  std::vector<std::uint64_t> matches_;
  int current_match_ = -1;
  std::size_t match_size_ = 0;
  bool searching_ = false;
  bool truncated_ = false;
  quint64 search_generation_ = 0;
  quint64 search_job_ = 0;
  std::shared_ptr<std::atomic<bool>> search_cancel_;
  // State shared with the search job, which may outlive the model: the job stores its matches here under the lock,
  // tagged with its generation, and the destructor clears `alive` under the same lock.
  struct SearchShared {
    std::mutex mutex;
    bool alive = true;
    HexModel* model = nullptr;
    quint64 result_generation = 0;
    std::vector<std::uint64_t> result;
  };
  std::shared_ptr<SearchShared> shared_ = std::make_shared<SearchShared>();

  mutable QHash<std::uint64_t, Page> pages_;
  mutable QList<std::uint64_t> page_order_;
  mutable QSet<std::uint64_t> pending_;
  mutable bool load_queued_ = false;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_HEX_MODEL_H
