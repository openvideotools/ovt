// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The models behind the HDR panel (chapter 11 §11.3.14) and the Display Capabilities panel (§11.3.15); WI-1.462.
//
//  - HdrMetadataModel (QML singleton `HdrMetadataModel`): the Metadata tab. One row per HDR/colour item with its value
//    and provenance (`container colr/mdcv/clli`, `SEI #unit`, `OBU #unit`, `DV RPU`), merged by the core into one
//    HdrMetadata table (chapter 8 §8.10, `ovt_container_hdr_metadata`), and one ⚠ row per Diagnostic the core attached
//    (e.g. `DEC-HDR-0001` when container and bitstream disagree). The model only displays: the classification, the
//    consistency check and the diagnostics all come from the core (FR-GUI-101).
//  - HdrViewSettings (QML singleton `HdrView`): the Display tab controls, i.e. the HDR fields of the Picture panel's
//    ViewState (§10.2.3: tone map, gamut map, clip nits, target peak override, false colour, gamut-clip overlay,
//    dither) plus the "HDR output" request and the macOS encoding choice that the NATIVE host (WI-1.463) acts on.
//  - DisplayCapsModel (QML singleton `DisplayCaps`): the §10.4.5 "Panel contents" table of display_probe() (WI-1.407),
//    re-probe, the test-pattern toggle, Copy report, and the status-bar text of FR-GUI-016.
//
// SPEC-ASSUMPTION(#682): ovt_container_hdr_metadata (WI-1.223) and the container path of Project (#243) are not merged,
// so nothing feeds HdrMetadataModel in the application yet; the Measurements tab shows declared values only, and the
// test pattern is a property the Picture host draws once it can.
//
// None of the three needs a renderer: where libovt-render is not linked (distro Qt < 6.7, the model tests) the probe
// reports "none" with the reason, so the panels and the tests run everywhere.

#ifndef OVT_APP_MODELS_HDR_METADATA_MODEL_H
#define OVT_APP_MODELS_HDR_METADATA_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

namespace ovt::app {

// ---- Metadata ---------------------------------------------------------------------------------------------------

// The fields of ovt_ColorDescription (backbone §0.5.6) the Metadata tab shows, in the struct's units.
struct HdrColor {
  int primaries = 2;  // H.273 code points; 2 = unspecified
  int transfer = 2;
  int matrix = 2;
  int full_range = 0;
  std::array<std::array<int, 2>, 3> mdcv_primaries{};  // (x, y) in 0.00002 units, as signalled (G, B, R in HEVC)
  std::array<int, 2> mdcv_white_point{};
  std::uint32_t mdcv_max_luminance = 0;  // 0.0001 cd/m²
  std::uint32_t mdcv_min_luminance = 0;
  int max_cll = 0;  // cd/m²; 0 = absent
  int max_fall = 0;
};

// One place the core found or looked for HDR information (HdrSignal of ovt_container.fbs).
struct HdrSignalInfo {
  QString source;  // "colr", "mdcv", "clli", "dvcC", "dvvC", "av1C", "vpcC", "sps_vui", "sei_137", ...
  std::int64_t box_offset = -1;
  std::int64_t unit_index = -1;
  bool present = false;
  QString summary;
};

struct HdrDiagnostic {
  int severity = 1;  // Severity of ovt_model.fbs: 0 Info, 1 Warning, 2 Error, 3 Fatal
  QString code;
  QString message;
};

// The HdrMetadata table of one track, as the model needs it.
struct HdrInput {
  int kind = 0;  // HdrKind: 0 Unknown, 1 SDR, 2 HDR10, 3 HDR10Plus, 4 HLG, 5 DolbyVision, 6 PQ_NoStatic
  HdrColor color;
  int dv_profile = 0;
  int dv_level = 0;
  int dv_bl_compat_id = 0;
  bool dv_rpu_present = false;
  int hdr10plus_application_version = 0;
  int hdr10plus_num_windows = 0;
  std::vector<HdrSignalInfo> signal_list;
  bool box_vs_bitstream_mismatch = false;
  std::vector<HdrDiagnostic> diagnostics;
};

struct HdrRow {
  QString item;
  QString value;
  QString provenance;
  bool warning = false;  // ⚠: a core diagnostic, or a colour row the core found inconsistent
  QString code;          // the diagnostic code of a ⚠ row ("DEC-HDR-0001"), else empty
};

class HdrMetadataModel : public QAbstractListModel {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(bool hasMetadata READ has_metadata NOTIFY metadataChanged)
  // "HDR10", "HLG", "Dolby Vision", …; "SDR (BT.709)" for SDR content (§11.3.14 States).
  Q_PROPERTY(QString kindText READ kind_text NOTIFY metadataChanged)
  Q_PROPERTY(bool isSdr READ is_sdr NOTIFY metadataChanged)
  // The Display tab's Dolby label: "Dolby Vision profile 8.1, level 6"; empty without Dolby Vision.
  Q_PROPERTY(QString dolbyLabel READ dolby_label NOTIFY metadataChanged)
  // Declared CTA-861.3 values for the Measurements tab (0 = not declared).
  Q_PROPERTY(int declaredMaxCll READ declared_max_cll NOTIFY metadataChanged)
  Q_PROPERTY(int declaredMaxFall READ declared_max_fall NOTIFY metadataChanged)
  Q_PROPERTY(int warningCount READ warning_count NOTIFY metadataChanged)

 public:
  enum Role { ItemRole = Qt::UserRole + 1, ValueRole, ProvenanceRole, WarningRole, CodeRole };
  Q_ENUM(Role)

  HdrMetadataModel() = default;

  // An HdrMetadata FlatBuffer (as ovt_container_hdr_metadata returns it). False, and the model cleared, when the
  // buffer does not verify.
  bool setHdrMetadata(const QByteArray& flatbuffer);
  // The same table in the FlatBuffers JSON shape (field names of ovt_container.fbs; `color` as the 44-byte struct
  // image or as an object with the ovt_ColorDescription field names). For the test harness and `ovt probe --hdr`
  // output; false when `kind` is missing.
  Q_INVOKABLE bool setFromJson(const QVariantMap& json);
  void setInput(HdrInput input);
  Q_INVOKABLE void clear();

  // {item, value, provenance, warning, code} of `row`, for tests and Copy.
  [[nodiscard]] Q_INVOKABLE QVariantMap row(int row) const;
  // The ⚠ row of the core diagnostic `code`, or -1.
  [[nodiscard]] Q_INVOKABLE int findCode(const QString& code) const;

  [[nodiscard]] bool has_metadata() const noexcept { return has_; }
  [[nodiscard]] QString kind_text() const;
  [[nodiscard]] bool is_sdr() const noexcept { return has_ && input_.kind == 1; }
  [[nodiscard]] QString dolby_label() const;
  [[nodiscard]] int declared_max_cll() const noexcept { return input_.color.max_cll; }
  [[nodiscard]] int declared_max_fall() const noexcept { return input_.color.max_fall; }
  [[nodiscard]] int warning_count() const;
  [[nodiscard]] const std::vector<HdrRow>& rows() const noexcept { return rows_; }

  // Provenance label of a signal: "container colr", "SPS VUI", "SEI #12", "OBU #3", "DV RPU #40".
  [[nodiscard]] static QString provenance(const HdrSignalInfo& signal);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

 Q_SIGNALS:
  void metadataChanged();

 private:
  void rebuild();
  void rebuild_static_rows();
  // The first present signal among `sources`, or nullptr.
  [[nodiscard]] const HdrSignalInfo* first_present(std::initializer_list<const char*> sources) const;

  HdrInput input_;
  bool has_ = false;
  std::vector<HdrRow> rows_;
};

// ---- Display tab (ViewState HDR fields) ---------------------------------------------------------------------------

class HdrViewSettings : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(HdrView)
  QML_SINGLETON
  // ToneMap of §10.2.3: 0 Auto, 1 BT.2390, 2 BT.2446a, 3 Spline, 4 Clip, 5 None.
  Q_PROPERTY(int toneMap MEMBER tone_map_ NOTIFY changed)
  // GamutMap: 0 Perceptual, 1 Relative, 2 Saturation, 3 Absolute, 4 Desaturate, 5 Darken, 6 Clip, 7 Highlight.
  Q_PROPERTY(int gamutMap MEMBER gamut_map_ NOTIFY changed)
  Q_PROPERTY(double clipNits MEMBER clip_nits_ NOTIFY changed)                       // ToneMap Clip threshold
  Q_PROPERTY(double targetPeakOverride MEMBER target_peak_override_ NOTIFY changed)  // 0 = from the display
  Q_PROPERTY(bool falseColor MEMBER false_color_ NOTIFY changed)                     // F12, §10.7.5
  Q_PROPERTY(bool gamutClipOverlay MEMBER gamut_clip_overlay_ NOTIFY changed)        // §10.7.8
  Q_PROPERTY(bool dither MEMBER dither_ NOTIFY changed)
  // macOS NATIVE encoding (§11.2.5 HDR group): 0 scRGB (extended linear), 1 system PQ. Used by WI-1.463.
  Q_PROPERTY(int macEncoding MEMBER mac_encoding_ NOTIFY changed)
  // "HDR output" (§11.3.2, View ▸ HDR output): the user's request for NATIVE mode; only honoured when the display is
  // HDR capable (R-ARCH-2). The NATIVE host (WI-1.463) acts on it.
  Q_PROPERTY(bool hdrOutput MEMBER hdr_output_ NOTIFY changed)

 public:
  HdrViewSettings() = default;
  // Back to the ViewState defaults of §10.2.3.
  Q_INVOKABLE void reset();

 Q_SIGNALS:
  void changed();

 private:
  int tone_map_ = 0;
  int gamut_map_ = 0;
  double clip_nits_ = 1000.0;
  double target_peak_override_ = 0.0;
  bool false_color_ = false;
  bool gamut_clip_overlay_ = false;
  bool dither_ = true;
  int mac_encoding_ = 0;
  bool hdr_output_ = false;
};

// ---- Display Capabilities -----------------------------------------------------------------------------------------

// render::DisplayCaps (chapter 10 §10.4.5) without the render headers.
struct DisplayCapsInfo {
  bool hdr_capable = false;
  bool hdr_active = false;
  float headroom = 1.0F;
  float potential_headroom = 1.0F;
  float sdr_white_nits = 203.0F;  // NaN = "relative" (macOS)
  float max_nits = 0.0F;          // 0 = unknown
  float min_nits = 0.0F;
  float max_fall_nits = 0.0F;
  bool supports_pq = false;
  bool supports_hlg = false;
  bool supports_scrgb = false;
  bool wide_gamut = false;
  QString primaries_name = QStringLiteral("unknown");
  QString source = QStringLiteral("none");
  QString reason;
  QString platform_detail;
};

class DisplayCapsModel : public QAbstractListModel {
  Q_OBJECT
  QML_NAMED_ELEMENT(DisplayCaps)
  QML_SINGLETON
  Q_PROPERTY(bool hdrCapable READ hdr_capable NOTIFY capsChanged)
  Q_PROPERTY(bool hdrActive READ hdr_active NOTIFY capsChanged)
  // Why the display is not HDR capable ("display has no EDR headroom", "X11 session", …); empty when capable.
  Q_PROPERTY(QString reason READ reason NOTIFY capsChanged)
  // FR-GUI-016 status-bar text: "SDR", "HDR headroom 4.0×" or "PQ pass-through".
  Q_PROPERTY(QString statusText READ status_text NOTIFY capsChanged)
  Q_PROPERTY(bool probing READ probing NOTIFY probingChanged)
  Q_PROPERTY(QDateTime lastProbe READ last_probe NOTIFY capsChanged)
  // Set by the panel from the Screen attached property of the window hosting the Picture.
  Q_PROPERTY(QString screenName READ screen_name WRITE set_screen_name NOTIFY capsChanged)
  // The surface's output encoding in use: "Srgb8" (EMBEDDED), "ScRgbLinear16F", "Pq10", "Hlg10"; set by the host.
  Q_PROPERTY(QString outputEncoding READ output_encoding WRITE set_output_encoding NOTIFY capsChanged)
  // libplacebo version and pl_gpu name, reported by the surface's host; empty = not reported yet.
  Q_PROPERTY(QString rendererInfo READ renderer_info WRITE set_renderer_info NOTIFY capsChanged)
  // "Show test pattern" (pq_patches_1000 into the surface; the Picture host draws it while this is on).
  Q_PROPERTY(bool testPattern READ test_pattern WRITE set_test_pattern NOTIFY testPatternChanged)

 public:
  enum Role { KeyRole = Qt::UserRole + 1, ValueRole, UnknownRole };
  Q_ENUM(Role)
  using Prober = std::function<DisplayCapsInfo()>;

  DisplayCapsModel();
  ~DisplayCapsModel() override;
  DisplayCapsModel(const DisplayCapsModel&) = delete;
  DisplayCapsModel& operator=(const DisplayCapsModel&) = delete;
  DisplayCapsModel(DisplayCapsModel&&) = delete;
  DisplayCapsModel& operator=(DisplayCapsModel&&) = delete;

  // Replaces display_probe() (tests). An empty function restores the default prober.
  void setProber(Prober prober);
  // Re-probe: `probing` goes true, the probe runs on the next event-loop turn, then the table updates.
  Q_INVOKABLE void reprobe();
  // Probes now (no spinner).
  void probeNow();
  // Shows `caps` as if probed (tests; the §10.4.5 field names, e.g. {"hdr_capable": false, "reason": "…"}).
  Q_INVOKABLE void setCaps(const QVariantMap& caps);
  void setInfo(const DisplayCapsInfo& info);
  // The table as "key: value" lines, and the same copied to the clipboard.
  [[nodiscard]] Q_INVOKABLE QString reportText() const;
  Q_INVOKABLE void copyReport() const;
  [[nodiscard]] Q_INVOKABLE QVariantMap row(int row) const;

  [[nodiscard]] bool hdr_capable() const noexcept { return info_.hdr_capable; }
  [[nodiscard]] bool hdr_active() const noexcept { return info_.hdr_active; }
  [[nodiscard]] QString reason() const { return info_.hdr_capable ? QString() : info_.reason; }
  [[nodiscard]] QString status_text() const;
  [[nodiscard]] bool probing() const noexcept { return probing_; }
  [[nodiscard]] QDateTime last_probe() const { return last_probe_; }
  [[nodiscard]] QString screen_name() const { return screen_name_; }
  void set_screen_name(const QString& name);
  [[nodiscard]] QString output_encoding() const { return output_encoding_; }
  void set_output_encoding(const QString& encoding);
  [[nodiscard]] QString renderer_info() const { return renderer_info_; }
  void set_renderer_info(const QString& info);
  [[nodiscard]] bool test_pattern() const noexcept { return test_pattern_; }
  void set_test_pattern(bool on);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

 Q_SIGNALS:
  void capsChanged();
  void probingChanged();
  void testPatternChanged();

 private:
  struct Row {
    QString key;
    QString value;
    bool unknown = false;  // shown as "—" with the reason
  };
  void rebuild();
  static DisplayCapsInfo default_probe();

  Prober prober_;
  DisplayCapsInfo info_;
  std::vector<Row> rows_;
  QDateTime last_probe_;
  QString screen_name_;
  QString output_encoding_ = QStringLiteral("Srgb8");
  QString renderer_info_;
  bool probing_ = false;
  bool test_pattern_ = false;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_HDR_METADATA_MODEL_H
