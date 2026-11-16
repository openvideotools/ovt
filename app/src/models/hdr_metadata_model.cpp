// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/hdr_metadata_model.h"

#include <flatbuffers/flatbuffers.h>

#include <QClipboard>
#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <QTimer>

#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

#include "ovt/color.h"
#include "ovt_container_generated.h"
#include "syntax/common/h273_names.h"

#ifdef OVT_HAVE_PICTURE_HOST
#include "ovt/render/display_caps.h"
#endif

namespace ovt::app {

namespace {

static_assert(sizeof(ovt_ColorDescription) == 44, "HdrMetadata.color is the 44-byte struct image (D4)");

QString h273(std::string_view name, int code) {
  if (name.empty()) {
    return QStringLiteral("reserved (%1)").arg(code);
  }
  return QStringLiteral("%1 (%2)").arg(QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()))).arg(code);
}

// Chromaticity in 0.00002 units as "(0.6800, 0.3200)".
QString xy(const std::array<int, 2>& v) {
  return QStringLiteral("(%1, %2)").arg(v[0] * 0.00002, 0, 'f', 4).arg(v[1] * 0.00002, 0, 'f', 4);
}

HdrColor color_from(const ovt_ColorDescription& c) {
  HdrColor out;
  out.primaries = c.primaries;
  out.transfer = c.transfer;
  out.matrix = c.matrix;
  out.full_range = c.full_range;
  for (std::size_t i = 0; i < 3; ++i) {
    out.mdcv_primaries.at(i) = {c.mdcv_primaries[i][0], c.mdcv_primaries[i][1]};  // NOLINT(*-constant-array-index)
  }
  out.mdcv_white_point = {c.mdcv_white_point[0], c.mdcv_white_point[1]};
  out.mdcv_max_luminance = c.mdcv_max_luminance;
  out.mdcv_min_luminance = c.mdcv_min_luminance;
  out.max_cll = c.max_cll;
  out.max_fall = c.max_fall;
  return out;
}

// The 44-byte little-endian struct image of HdrMetadata.color (every OVT target is little-endian).
bool color_from_bytes(const std::uint8_t* bytes, std::size_t size, HdrColor* out) {
  if (bytes == nullptr || size < sizeof(ovt_ColorDescription)) {
    return false;
  }
  ovt_ColorDescription c{};
  std::memcpy(&c, bytes, sizeof c);
  *out = color_from(c);
  return true;
}

HdrColor color_from_variant(const QVariant& value) {
  HdrColor out;
  if (value.typeId() == QMetaType::QVariantList) {
    const QVariantList list = value.toList();
    std::array<std::uint8_t, sizeof(ovt_ColorDescription)> bytes{};
    for (std::size_t i = 0; i < bytes.size() && i < static_cast<std::size_t>(list.size()); ++i) {
      bytes.at(i) = static_cast<std::uint8_t>(list.at(static_cast<qsizetype>(i)).toUInt());
    }
    if (list.size() >= static_cast<qsizetype>(bytes.size())) {
      color_from_bytes(bytes.data(), bytes.size(), &out);
    }
    return out;
  }
  const QVariantMap m = value.toMap();
  out.primaries = m.value(QStringLiteral("primaries"), 2).toInt();
  out.transfer = m.value(QStringLiteral("transfer"), 2).toInt();
  out.matrix = m.value(QStringLiteral("matrix"), 2).toInt();
  out.full_range = m.value(QStringLiteral("full_range"), 0).toInt();
  const QVariantList prim = m.value(QStringLiteral("mdcv_primaries")).toList();
  for (qsizetype i = 0; i < prim.size() && i < 3; ++i) {
    const QVariantList p = prim.at(i).toList();
    if (p.size() == 2) {
      out.mdcv_primaries.at(static_cast<std::size_t>(i)) = {p.at(0).toInt(), p.at(1).toInt()};
    }
  }
  const QVariantList wp = m.value(QStringLiteral("mdcv_white_point")).toList();
  if (wp.size() == 2) {
    out.mdcv_white_point = {wp.at(0).toInt(), wp.at(1).toInt()};
  }
  out.mdcv_max_luminance = m.value(QStringLiteral("mdcv_max_luminance")).toUInt();
  out.mdcv_min_luminance = m.value(QStringLiteral("mdcv_min_luminance")).toUInt();
  out.max_cll = m.value(QStringLiteral("max_cll")).toInt();
  out.max_fall = m.value(QStringLiteral("max_fall")).toInt();
  return out;
}

QString kind_name(int kind) {
  switch (kind) {
    case 1:
      return QStringLiteral("SDR");
    case 2:
      return QStringLiteral("HDR10");
    case 3:
      return QStringLiteral("HDR10+");
    case 4:
      return QStringLiteral("HLG");
    case 5:
      return QStringLiteral("Dolby Vision");
    case 6:
      return QStringLiteral("PQ without static metadata");
    default:
      return QStringLiteral("Unknown");
  }
}

QString nits(double value) {
  if (!std::isfinite(value)) {
    return QStringLiteral("relative");
  }
  return QStringLiteral("%1 cd/m²").arg(value, 0, 'g', 6);
}

}  // namespace

// ---- HdrMetadataModel ---------------------------------------------------------------------------------------------

bool HdrMetadataModel::setHdrMetadata(const QByteArray& flatbuffer) {
  const auto* data = reinterpret_cast<const std::uint8_t*>(flatbuffer.constData());  // NOLINT(*-reinterpret-cast)
  flatbuffers::Verifier verifier(data, static_cast<std::size_t>(flatbuffer.size()));
  if (flatbuffer.isEmpty() || !verifier.VerifyBuffer<fb::HdrMetadata>(nullptr)) {
    clear();
    return false;
  }
  const auto* root = flatbuffers::GetRoot<fb::HdrMetadata>(data);
  HdrInput in;
  in.kind = static_cast<int>(root->kind());
  if (const auto* color = root->color()) {
    color_from_bytes(color->data(), color->size(), &in.color);
  }
  in.dv_profile = root->dv_profile();
  in.dv_level = root->dv_level();
  in.dv_bl_compat_id = root->dv_bl_compat_id();
  in.dv_rpu_present = root->dv_rpu_present();
  in.hdr10plus_application_version = root->hdr10plus_application_version();
  in.hdr10plus_num_windows = root->hdr10plus_num_windows();
  in.box_vs_bitstream_mismatch = root->box_vs_bitstream_mismatch();
  if (const auto* list = root->signals()) {
    for (const auto* s : *list) {
      HdrSignalInfo info;
      info.source = s->source() != nullptr ? QString::fromStdString(s->source()->str()) : QString();
      info.box_offset = s->box_offset();
      info.unit_index = s->unit_index();
      info.present = s->present();
      info.summary = s->summary() != nullptr ? QString::fromStdString(s->summary()->str()) : QString();
      in.signal_list.push_back(std::move(info));
    }
  }
  if (const auto* list = root->diagnostics()) {
    for (const auto* d : *list) {
      HdrDiagnostic diag;
      diag.severity = static_cast<int>(d->severity());
      diag.code = d->code() != nullptr ? QString::fromStdString(d->code()->str()) : QString();
      diag.message = d->message() != nullptr ? QString::fromStdString(d->message()->str()) : QString();
      in.diagnostics.push_back(std::move(diag));
    }
  }
  setInput(std::move(in));
  return true;
}

bool HdrMetadataModel::setFromJson(const QVariantMap& json) {
  if (!json.contains(QStringLiteral("kind"))) {
    clear();
    return false;
  }
  HdrInput in;
  const QVariant kind = json.value(QStringLiteral("kind"));
  if (kind.typeId() == QMetaType::QString) {  // FlatBuffers JSON writes enums by name
    static const QStringList kNames{QStringLiteral("Unknown"),    QStringLiteral("SDR"), QStringLiteral("HDR10"),
                                    QStringLiteral("HDR10Plus"),  QStringLiteral("HLG"), QStringLiteral("DolbyVision"),
                                    QStringLiteral("PQ_NoStatic")};
    in.kind = std::max(0, static_cast<int>(kNames.indexOf(kind.toString())));
  } else {
    in.kind = kind.toInt();
  }
  in.color = color_from_variant(json.value(QStringLiteral("color")));
  in.dv_profile = json.value(QStringLiteral("dv_profile")).toInt();
  in.dv_level = json.value(QStringLiteral("dv_level")).toInt();
  in.dv_bl_compat_id = json.value(QStringLiteral("dv_bl_compat_id")).toInt();
  in.dv_rpu_present = json.value(QStringLiteral("dv_rpu_present")).toBool();
  in.hdr10plus_application_version = json.value(QStringLiteral("hdr10plus_application_version")).toInt();
  in.hdr10plus_num_windows = json.value(QStringLiteral("hdr10plus_num_windows")).toInt();
  in.box_vs_bitstream_mismatch = json.value(QStringLiteral("box_vs_bitstream_mismatch")).toBool();
  for (const QVariant& v : json.value(QStringLiteral("signals")).toList()) {
    const QVariantMap m = v.toMap();
    HdrSignalInfo info;
    info.source = m.value(QStringLiteral("source")).toString();
    info.box_offset = m.value(QStringLiteral("box_offset"), -1).toLongLong();
    info.unit_index = m.value(QStringLiteral("unit_index"), -1).toLongLong();
    info.present = m.value(QStringLiteral("present")).toBool();
    info.summary = m.value(QStringLiteral("summary")).toString();
    in.signal_list.push_back(std::move(info));
  }
  for (const QVariant& v : json.value(QStringLiteral("diagnostics")).toList()) {
    const QVariantMap m = v.toMap();
    HdrDiagnostic diag;
    const QVariant sev = m.value(QStringLiteral("severity"), 1);
    if (sev.typeId() == QMetaType::QString) {
      static const QStringList kSev{QStringLiteral("Info"), QStringLiteral("Warning"), QStringLiteral("Error"),
                                    QStringLiteral("Fatal")};
      diag.severity = std::max(0, static_cast<int>(kSev.indexOf(sev.toString())));
    } else {
      diag.severity = sev.toInt();
    }
    diag.code = m.value(QStringLiteral("code")).toString();
    diag.message = m.value(QStringLiteral("message")).toString();
    in.diagnostics.push_back(std::move(diag));
  }
  setInput(std::move(in));
  return true;
}

void HdrMetadataModel::setInput(HdrInput input) {
  beginResetModel();
  input_ = std::move(input);
  has_ = true;
  rebuild();
  endResetModel();
  Q_EMIT metadataChanged();
}

void HdrMetadataModel::clear() {
  beginResetModel();
  input_ = HdrInput{};
  has_ = false;
  rows_.clear();
  endResetModel();
  Q_EMIT metadataChanged();
}

QString HdrMetadataModel::provenance(const HdrSignalInfo& signal) {
  const QString& s = signal.source;
  const QString unit = signal.unit_index >= 0 ? QStringLiteral(" #%1").arg(signal.unit_index) : QString();
  if (s.startsWith(QLatin1String("sei_"))) {
    return QStringLiteral("SEI") + unit;
  }
  if (s.startsWith(QLatin1String("obu_metadata"))) {
    return QStringLiteral("OBU") + unit;
  }
  if (s == QLatin1String("nal_62_rpu")) {
    return QStringLiteral("DV RPU") + unit;
  }
  if (s == QLatin1String("sps_vui")) {
    return QStringLiteral("SPS VUI") + unit;
  }
  if (s == QLatin1String("sequence_header")) {
    return QStringLiteral("OBU") + unit;
  }
  return QStringLiteral("container ") + s;  // colr, mdcv, clli, dvcC, dvvC, av1C, vpcC
}

const HdrSignalInfo* HdrMetadataModel::first_present(std::initializer_list<const char*> sources) const {
  for (const char* source : sources) {
    for (const HdrSignalInfo& s : input_.signal_list) {
      if (s.present && s.source == QLatin1String(source)) {
        return &s;
      }
    }
  }
  return nullptr;
}

void HdrMetadataModel::rebuild() {
  rows_.clear();
  const HdrColor& c = input_.color;
  auto prov = [this](std::initializer_list<const char*> sources) {
    const HdrSignalInfo* s = first_present(sources);
    return s != nullptr ? provenance(*s) : QString();
  };
  // The code of the core's consistency diagnostic, which marks the colour rows (§11.3.14 consistency ⚠).
  QString mismatch_code;
  for (const HdrDiagnostic& d : input_.diagnostics) {
    if (d.code.startsWith(QLatin1String("DEC-HDR-"))) {
      mismatch_code = d.code;
      break;
    }
  }
  const bool mismatch = input_.box_vs_bitstream_mismatch || !mismatch_code.isEmpty();

  rows_.push_back({QStringLiteral("Classification"), kind_text(), QString(), false, QString()});
  // Boxes win over the bitstream for the colour fields when colr exists (HdrMetadata.color).
  const QString colour_prov = prov({"colr", "av1C", "vpcC", "sps_vui", "sequence_header"});
  rows_.push_back({QStringLiteral("Colour primaries"),
                   h273(syntax::h273_colour_primaries_name(static_cast<std::uint32_t>(c.primaries)), c.primaries),
                   colour_prov, mismatch, mismatch_code});
  rows_.push_back({QStringLiteral("Transfer characteristics"),
                   h273(syntax::h273_transfer_characteristics_name(static_cast<std::uint32_t>(c.transfer)), c.transfer),
                   colour_prov, mismatch, mismatch_code});
  rows_.push_back({QStringLiteral("Matrix coefficients"),
                   h273(syntax::h273_matrix_coefficients_name(static_cast<std::uint32_t>(c.matrix)), c.matrix),
                   colour_prov, mismatch, mismatch_code});
  rows_.push_back({QStringLiteral("Range"), c.full_range != 0 ? QStringLiteral("Full") : QStringLiteral("Limited"),
                   colour_prov, false, QString()});
  // Where the bitstream said something else, show its own signal too (the other side of the ⚠).
  if (mismatch) {
    for (const HdrSignalInfo& s : input_.signal_list) {
      if (s.present && (s.source == QLatin1String("sps_vui") || s.source == QLatin1String("sequence_header")) &&
          !s.summary.isEmpty()) {
        rows_.push_back({QStringLiteral("Bitstream colour"), s.summary, provenance(s), true, mismatch_code});
      }
    }
  }
  rebuild_static_rows();
  // Every diagnostic of the core, verbatim (the model never raises its own).
  for (const HdrDiagnostic& d : input_.diagnostics) {
    rows_.push_back({d.code, d.message, QStringLiteral("core"), d.severity >= 1, d.code});
  }
}

// Mastering display, content light level, HDR10+ and Dolby Vision rows.
void HdrMetadataModel::rebuild_static_rows() {
  const HdrColor& c = input_.color;
  auto prov = [this](std::initializer_list<const char*> sources) {
    const HdrSignalInfo* s = first_present(sources);
    return s != nullptr ? provenance(*s) : QString();
  };
  if (c.mdcv_max_luminance != 0 || c.mdcv_white_point[0] != 0) {
    const QString p = prov({"mdcv", "sei_137", "obu_metadata_2"});
    rows_.push_back({QStringLiteral("Mastering display primaries"),
                     QStringLiteral("R %1  G %2  B %3")
                         .arg(xy(c.mdcv_primaries[0]), xy(c.mdcv_primaries[1]), xy(c.mdcv_primaries[2])),
                     p, false, QString()});
    rows_.push_back({QStringLiteral("Mastering white point"), xy(c.mdcv_white_point), p, false, QString()});
    rows_.push_back({QStringLiteral("Mastering luminance"),
                     QStringLiteral("%1 – %2 cd/m²")
                         .arg(c.mdcv_min_luminance * 0.0001, 0, 'g', 6)
                         .arg(c.mdcv_max_luminance * 0.0001, 0, 'g', 6),
                     p, false, QString()});
  }
  if (c.max_cll != 0 || c.max_fall != 0) {
    const QString p = prov({"clli", "sei_144", "obu_metadata_1"});
    rows_.push_back({QStringLiteral("MaxCLL"), nits(c.max_cll), p, false, QString()});
    rows_.push_back({QStringLiteral("MaxFALL"), nits(c.max_fall), p, false, QString()});
  }
  if (const HdrSignalInfo* s = first_present({"sei_t35_hdr10plus", "obu_metadata_4"})) {
    rows_.push_back({QStringLiteral("HDR10+ (ST 2094-40)"),
                     QStringLiteral("application version %1, %2 window(s)")
                         .arg(input_.hdr10plus_application_version)
                         .arg(input_.hdr10plus_num_windows),
                     provenance(*s), false, QString()});
  }
  if (input_.dv_profile != 0 || input_.kind == 5) {
    rows_.push_back({QStringLiteral("Dolby Vision"), dolby_label(), prov({"dvvC", "dvcC", "dvwC"}), false, QString()});
  }
  if (const HdrSignalInfo* s = first_present({"nal_62_rpu"}); s != nullptr || input_.dv_rpu_present) {
    rows_.push_back({QStringLiteral("Dolby Vision RPU"), QStringLiteral("present"),
                     s != nullptr ? provenance(*s) : QStringLiteral("DV RPU"), false, QString()});
  }
}

QString HdrMetadataModel::kind_text() const {
  if (!has_) {
    return {};
  }
  if (input_.kind == 1) {
    const std::string_view name =
        syntax::h273_colour_primaries_name(static_cast<std::uint32_t>(input_.color.primaries));
    const bool known = !name.empty() && input_.color.primaries != 2;
    return QStringLiteral("SDR (%1)")
        .arg(known ? QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size())) : QStringLiteral("BT.709"));
  }
  return kind_name(input_.kind);
}

QString HdrMetadataModel::dolby_label() const {
  if (!has_ || (input_.dv_profile == 0 && input_.kind != 5)) {
    return {};
  }
  QString label = QStringLiteral("Dolby Vision profile %1").arg(input_.dv_profile);
  if (input_.dv_bl_compat_id != 0) {
    label += QStringLiteral(".%1").arg(input_.dv_bl_compat_id);
  }
  if (input_.dv_level != 0) {
    label += QStringLiteral(", level %1").arg(input_.dv_level);
  }
  return label;
}

int HdrMetadataModel::warning_count() const {
  int n = 0;
  for (const HdrRow& r : rows_) {
    n += r.warning ? 1 : 0;
  }
  return n;
}

QVariantMap HdrMetadataModel::row(int row) const {
  if (row < 0 || row >= rowCount()) {
    return {};
  }
  const HdrRow& r = rows_.at(static_cast<std::size_t>(row));
  return {{QStringLiteral("item"), r.item},
          {QStringLiteral("value"), r.value},
          {QStringLiteral("provenance"), r.provenance},
          {QStringLiteral("warning"), r.warning},
          {QStringLiteral("code"), r.code}};
}

int HdrMetadataModel::findCode(const QString& code) const {
  for (std::size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].warning && rows_[i].code == code && rows_[i].item == code) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int HdrMetadataModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant HdrMetadataModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rowCount()) {
    return {};
  }
  const HdrRow& r = rows_.at(static_cast<std::size_t>(index.row()));
  switch (role) {
    case Qt::DisplayRole:
    case ItemRole:
      return r.item;
    case ValueRole:
      return r.value;
    case ProvenanceRole:
      return r.provenance;
    case WarningRole:
      return r.warning;
    case CodeRole:
      return r.code;
    default:
      return {};
  }
}

QHash<int, QByteArray> HdrMetadataModel::roleNames() const {
  return {{ItemRole, "item"},
          {ValueRole, "value"},
          {ProvenanceRole, "provenance"},
          {WarningRole, "warning"},
          {CodeRole, "code"}};
}

// ---- HdrViewSettings ----------------------------------------------------------------------------------------------

void HdrViewSettings::reset() {
  tone_map_ = 0;
  gamut_map_ = 0;
  clip_nits_ = 1000.0;
  target_peak_override_ = 0.0;
  false_color_ = false;
  gamut_clip_overlay_ = false;
  dither_ = true;
  mac_encoding_ = 0;
  hdr_output_ = false;
  Q_EMIT changed();
}

// ---- DisplayCapsModel ---------------------------------------------------------------------------------------------

DisplayCapsModel::DisplayCapsModel() {
  // Re-probe triggers on the Qt side: a screen added, removed or made primary (§10.4.5).
  if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    connect(app, &QGuiApplication::screenAdded, this, &DisplayCapsModel::reprobe);
    connect(app, &QGuiApplication::screenRemoved, this, &DisplayCapsModel::reprobe);
    connect(app, &QGuiApplication::primaryScreenChanged, this, &DisplayCapsModel::reprobe);
  }
  probeNow();
}

DisplayCapsModel::~DisplayCapsModel() = default;

DisplayCapsInfo DisplayCapsModel::default_probe() {
  DisplayCapsInfo info;
#ifdef OVT_HAVE_PICTURE_HOST
  // OffscreenTexture: the probe looks at the main screen (the EMBEDDED Picture's window). The NATIVE host (WI-1.463)
  // passes its own target when it exists.
  const render::DisplayCaps caps = render::display_probe(render::DisplayTarget{});
  info.hdr_capable = caps.hdr_capable;
  info.hdr_active = caps.hdr_active;
  info.headroom = caps.headroom;
  info.potential_headroom = caps.potential_headroom;
  info.sdr_white_nits = caps.sdr_white_nits;
  info.max_nits = caps.max_nits;
  info.min_nits = caps.min_nits;
  info.max_fall_nits = caps.max_fall_nits;
  info.supports_pq = caps.supports_pq;
  info.supports_hlg = caps.supports_hlg;
  info.supports_scrgb = caps.supports_scrgb;
  info.wide_gamut = caps.wide_gamut;
  info.primaries_name = QString::fromUtf8(static_cast<const char*>(caps.primaries_name));
  info.source = QString::fromUtf8(static_cast<const char*>(caps.source));
  info.reason = QString::fromUtf8(static_cast<const char*>(caps.reason));
  info.platform_detail = QString::fromUtf8(static_cast<const char*>(caps.platform_detail));
#else
  info.reason = QStringLiteral("this build has no libovt-render (no display probe)");
#endif
  return info;
}

void DisplayCapsModel::setProber(Prober prober) {
  prober_ = std::move(prober);
}

void DisplayCapsModel::probeNow() {
  setInfo(prober_ ? prober_() : default_probe());
}

void DisplayCapsModel::reprobe() {
  if (probing_) {
    return;
  }
  probing_ = true;
  Q_EMIT probingChanged();
  QTimer::singleShot(0, this, [this] {
    probeNow();
    probing_ = false;
    Q_EMIT probingChanged();
  });
}

void DisplayCapsModel::setCaps(const QVariantMap& caps) {
  DisplayCapsInfo info;
  auto f = [&caps](const char* key, float def) {
    const QVariant v = caps.value(QLatin1String(key));
    return v.isValid() ? v.toFloat() : def;
  };
  auto b = [&caps](const char* key) { return caps.value(QLatin1String(key)).toBool(); };
  auto s = [&caps](const char* key, const QString& def) { return caps.value(QLatin1String(key), def).toString(); };
  info.hdr_capable = b("hdr_capable");
  info.hdr_active = b("hdr_active");
  info.headroom = f("headroom", 1.0F);
  info.potential_headroom = f("potential_headroom", 1.0F);
  info.sdr_white_nits = f("sdr_white_nits", 203.0F);
  info.max_nits = f("max_nits", 0.0F);
  info.min_nits = f("min_nits", 0.0F);
  info.max_fall_nits = f("max_fall_nits", 0.0F);
  info.supports_pq = b("supports_pq");
  info.supports_hlg = b("supports_hlg");
  info.supports_scrgb = b("supports_scrgb");
  info.wide_gamut = b("wide_gamut");
  info.primaries_name = s("primaries_name", QStringLiteral("unknown"));
  info.source = s("source", QStringLiteral("none"));
  info.reason = s("reason", QString());
  info.platform_detail = s("platform_detail", QString());
  setInfo(info);
}

void DisplayCapsModel::setInfo(const DisplayCapsInfo& info) {
  beginResetModel();
  info_ = info;
  last_probe_ = QDateTime::currentDateTime();
  rebuild();
  endResetModel();
  Q_EMIT capsChanged();
}

QString DisplayCapsModel::status_text() const {
  if (output_encoding_ == QLatin1String("Pq10")) {
    return QStringLiteral("PQ pass-through");
  }
  if (info_.hdr_capable) {
    return QStringLiteral("HDR headroom %1×").arg(std::max(info_.headroom, info_.potential_headroom), 0, 'f', 1);
  }
  return QStringLiteral("SDR");
}

void DisplayCapsModel::set_screen_name(const QString& name) {
  if (name != screen_name_) {
    screen_name_ = name;
    rebuild();
    Q_EMIT dataChanged(index(0), index(rowCount() - 1));
    Q_EMIT capsChanged();
  }
}

void DisplayCapsModel::set_output_encoding(const QString& encoding) {
  if (encoding != output_encoding_) {
    output_encoding_ = encoding;
    rebuild();
    Q_EMIT dataChanged(index(0), index(rowCount() - 1));
    Q_EMIT capsChanged();
  }
}

void DisplayCapsModel::set_renderer_info(const QString& info) {
  if (info != renderer_info_) {
    renderer_info_ = info;
    rebuild();
    Q_EMIT dataChanged(index(0), index(rowCount() - 1));
    Q_EMIT capsChanged();
  }
}

void DisplayCapsModel::set_test_pattern(bool on) {
  if (on != test_pattern_) {
    test_pattern_ = on;
    Q_EMIT testPatternChanged();
  }
}

// One row per field, in the order of chapter 10 §10.4.5 "Panel contents". The row count never changes.
void DisplayCapsModel::rebuild() {
  const DisplayCapsInfo& i = info_;
  auto yes = [](bool v) { return v ? QStringLiteral("yes") : QStringLiteral("no"); };
  auto lum = [](float v) { return v > 0.0F ? QStringLiteral("%1").arg(static_cast<double>(v), 0, 'g', 6) : QString(); };
  QStringList transfers;
  if (i.supports_pq) {
    transfers << QStringLiteral("PQ");
  }
  if (i.supports_hlg) {
    transfers << QStringLiteral("HLG");
  }
  if (i.supports_scrgb) {
    transfers << QStringLiteral("scRGB");
  }
  const QString lums = QStringLiteral("%1 / %2 / %3")
                           .arg(lum(i.max_nits).isEmpty() ? QStringLiteral("—") : lum(i.max_nits),
                                lum(i.min_nits).isEmpty() ? QStringLiteral("—") : lum(i.min_nits),
                                lum(i.max_fall_nits).isEmpty() ? QStringLiteral("—") : lum(i.max_fall_nits));
  const bool lum_unknown = i.max_nits <= 0.0F && i.min_nits <= 0.0F && i.max_fall_nits <= 0.0F;
  rows_ = {
      {QStringLiteral("Screen"), screen_name_, screen_name_.isEmpty()},
      {QStringLiteral("HDR capable"), i.hdr_capable ? yes(true) : QStringLiteral("no (%1)").arg(i.reason), false},
      {QStringLiteral("HDR active"), yes(i.hdr_active), false},
      {QStringLiteral("Output encoding"), output_encoding_, false},
      {QStringLiteral("Headroom now / potential"),
       QStringLiteral("%1 / %2")
           .arg(static_cast<double>(i.headroom), 0, 'f', 2)
           .arg(static_cast<double>(i.potential_headroom), 0, 'f', 2),
       false},
      {QStringLiteral("SDR white"),
       std::isnan(i.sdr_white_nits) ? QStringLiteral("relative") : nits(static_cast<double>(i.sdr_white_nits)), false},
      {QStringLiteral("Max / min / max-FALL luminance (nits)"), lums, lum_unknown},
      {QStringLiteral("Supported transfers"), transfers.join(QStringLiteral(", ")), transfers.isEmpty()},
      {QStringLiteral("Primaries"), i.primaries_name, i.primaries_name == QLatin1String("unknown")},
      {QStringLiteral("Source API"), i.source, i.source == QLatin1String("none")},
      {QStringLiteral("Platform detail"), i.platform_detail, i.platform_detail.isEmpty()},
      {QStringLiteral("libplacebo / GPU"), renderer_info_, renderer_info_.isEmpty()},
      {QStringLiteral("Last probe"), last_probe_.toString(Qt::ISODate), false},
  };
  // "unknown" values read "—" with the reason (§11.3.15 States).
  const QString why = i.reason.isEmpty() ? QStringLiteral("not reported") : i.reason;
  for (Row& r : rows_) {
    if (r.unknown) {
      r.value = QStringLiteral("— (%1)").arg(
          r.key == QLatin1String("libplacebo / GPU") ? QStringLiteral("reported by the native surface") : why);
    }
  }
}

QString DisplayCapsModel::reportText() const {
  QStringList lines;
  for (const Row& r : rows_) {
    lines << QStringLiteral("%1: %2").arg(r.key, r.value);
  }
  return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

void DisplayCapsModel::copyReport() const {
  if (QClipboard* clipboard = QGuiApplication::clipboard()) {
    clipboard->setText(reportText());
  }
}

QVariantMap DisplayCapsModel::row(int row) const {
  if (row < 0 || row >= rowCount()) {
    return {};
  }
  const Row& r = rows_.at(static_cast<std::size_t>(row));
  return {{QStringLiteral("key"), r.key}, {QStringLiteral("value"), r.value}, {QStringLiteral("unknown"), r.unknown}};
}

int DisplayCapsModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant DisplayCapsModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() >= rowCount()) {
    return {};
  }
  const Row& r = rows_.at(static_cast<std::size_t>(index.row()));
  switch (role) {
    case Qt::DisplayRole:
    case KeyRole:
      return r.key;
    case ValueRole:
      return r.value;
    case UnknownRole:
      return r.unknown;
    default:
      return {};
  }
}

QHash<int, QByteArray> DisplayCapsModel::roleNames() const {
  return {{KeyRole, "key"}, {ValueRole, "value"}, {UnknownRole, "unknown"}};
}

}  // namespace ovt::app
