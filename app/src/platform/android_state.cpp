// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AndroidState (chapter 11 §11.5.7; WI-A.455). See android_state.h.

#include "platform/android_state.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QRect>

#include <utility>

namespace ovt::app {

namespace {

Q_LOGGING_CATEGORY(lcState, "ovt.android.state")

// Keys of the document (§11.5.7 saved-state keys).
constexpr auto kVersionKey = "version";
constexpr auto kProjectKey = "project";
constexpr auto kSelectionKey = "selection";
constexpr auto kViewKey = "view";
constexpr auto kLayoutKey = "layout";
constexpr auto kPanelStateKey = "panel_state";
constexpr auto kDroppedKey = "dropped";

QString key(const char* name) {
  return QString::fromLatin1(name);
}

// 64-bit offsets as JSON numbers: doubles hold every file offset below 2^53, i.e. any real file.
QJsonValue u64(std::uint64_t value) {
  return static_cast<double>(value);
}

std::uint64_t u64(const QJsonValue& value) {
  const double v = value.toDouble(0);
  return v > 0 ? static_cast<std::uint64_t>(v) : 0;
}

QJsonArray rectJson(const QRect& rect) {
  return {rect.x(), rect.y(), rect.width(), rect.height()};
}

QRect rectFromJson(const QJsonValue& value) {
  const QJsonArray a = value.toArray();
  if (a.size() != 4) {
    return {};
  }
  return {a.at(0).toInt(), a.at(1).toInt(), a.at(2).toInt(), a.at(3).toInt()};
}

}  // namespace

AndroidState::AndroidState(QObject* parent) : QObject(parent) {}

void AndroidState::setSelection(SelectionModel* selection) {
  if (selection_ != selection) {
    selection_ = selection;
    Q_EMIT sourcesChanged();
  }
}

void AndroidState::setDock(DockModel* dock) {
  if (dock_ != dock) {
    dock_ = dock;
    Q_EMIT sourcesChanged();
  }
}

void AndroidState::setProject(Project* project) {
  if (project_ != project) {
    project_ = project;
    Q_EMIT sourcesChanged();
  }
}

void AndroidState::setPicture(PictureController* picture) {
  if (picture_ != picture) {
    picture_ = picture;
    Q_EMIT sourcesChanged();
  }
}

void AndroidState::setPanelState(const QVariantMap& state) {
  if (panel_state_ != state) {
    panel_state_ = state;
    Q_EMIT panelStateChanged();
  }
}

// ---- Pieces ---------------------------------------------------------------------------------------------------------

QJsonObject AndroidState::selectionToJson(const SelectionModel::Snapshot& s) {
  QJsonObject o;
  o.insert(QStringLiteral("file_id"), s.file_id);
  o.insert(QStringLiteral("track_index"), s.track_index);
  o.insert(QStringLiteral("frame"), QJsonArray{s.frame.decode_index, s.frame.display_index});
  o.insert(QStringLiteral("display_order"), s.display_order);
  // kNoBlock (0xFFFFFFFF) is written as -1.
  o.insert(QStringLiteral("block_id"),
           s.block_id == SelectionModel::kNoBlock ? QJsonValue(-1) : QJsonValue(static_cast<double>(s.block_id)));
  o.insert(QStringLiteral("level"), s.level);
  o.insert(QStringLiteral("mv_index"), s.mv_index);
  o.insert(QStringLiteral("unit_index"), s.unit_index);
  o.insert(QStringLiteral("sample_index"), s.sample_index);
  o.insert(QStringLiteral("byte_range"), QJsonArray{u64(s.byte_range.offset), u64(s.byte_range.length)});
  o.insert(QStringLiteral("bit_range"),
           QJsonArray{u64(s.bit_range.bit_offset), static_cast<double>(s.bit_range.bit_length)});
  o.insert(QStringLiteral("field_path"), s.field_path);
  o.insert(QStringLiteral("probe"), QJsonArray{s.probe.x(), s.probe.y()});
  o.insert(QStringLiteral("selection_rect"), rectJson(s.selection_rect));
  return o;
}

SelectionModel::Snapshot AndroidState::selectionFromJson(const QJsonObject& o) {
  SelectionModel::Snapshot s;
  s.file_id = o.value(QStringLiteral("file_id")).toString();
  s.track_index = o.value(QStringLiteral("track_index")).toInt(0);
  const QJsonArray frame = o.value(QStringLiteral("frame")).toArray();
  s.frame = FrameId{frame.at(0).toInt(-1), frame.at(1).toInt(-1)};
  s.display_order = o.value(QStringLiteral("display_order")).toBool(true);
  const double block = o.value(QStringLiteral("block_id")).toDouble(-1);
  s.block_id = block < 0 || block > static_cast<double>(SelectionModel::kNoBlock) ? SelectionModel::kNoBlock
                                                                                  : static_cast<std::uint32_t>(block);
  s.level = o.value(QStringLiteral("level")).toInt(-1);
  s.mv_index = o.value(QStringLiteral("mv_index")).toInt(-1);
  s.unit_index = o.value(QStringLiteral("unit_index")).toInt(-1);
  s.sample_index = o.value(QStringLiteral("sample_index")).toInt(-1);
  const QJsonArray bytes = o.value(QStringLiteral("byte_range")).toArray();
  s.byte_range = ByteSpan{u64(bytes.at(0)), u64(bytes.at(1))};
  const QJsonArray bits = o.value(QStringLiteral("bit_range")).toArray();
  s.bit_range = BitSpan{u64(bits.at(0)), static_cast<std::uint32_t>(u64(bits.at(1)))};
  s.field_path = o.value(QStringLiteral("field_path")).toString();
  const QJsonArray probe = o.value(QStringLiteral("probe")).toArray();
  s.probe = QPoint(probe.at(0).toInt(-1), probe.at(1).toInt(-1));
  s.selection_rect = rectFromJson(o.value(QStringLiteral("selection_rect")));
  return s;
}

QJsonObject AndroidState::viewToJson(const PictureController::ViewSnapshot& v) {
  QJsonObject o;
  o.insert(QStringLiteral("fit"), v.fit);
  o.insert(QStringLiteral("zoom"), v.zoom);
  o.insert(QStringLiteral("pan"), QJsonArray{v.pan_x, v.pan_y});
  o.insert(QStringLiteral("plane_mode"), v.plane_mode);
  o.insert(QStringLiteral("stage"), v.stage);
  o.insert(QStringLiteral("show_cropped"), v.show_cropped);
  return o;
}

PictureController::ViewSnapshot AndroidState::viewFromJson(const QJsonObject& o) {
  PictureController::ViewSnapshot v;
  v.fit = o.value(QStringLiteral("fit")).toBool(true);
  v.zoom = o.value(QStringLiteral("zoom")).toDouble(1.0);
  const QJsonArray pan = o.value(QStringLiteral("pan")).toArray();
  v.pan_x = pan.at(0).toDouble(0);
  v.pan_y = pan.at(1).toDouble(0);
  v.plane_mode = o.value(QStringLiteral("plane_mode")).toInt(0);
  v.stage = o.value(QStringLiteral("stage")).toInt(0);
  v.show_cropped = o.value(QStringLiteral("show_cropped")).toBool(true);
  return v;
}

std::optional<QByteArray> AndroidState::encode(QJsonObject state, qsizetype max_bytes, QStringList* dropped) {
  QJsonArray names;
  QByteArray bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
  for (const char* droppable : {kPanelStateKey, kLayoutKey}) {
    if (bytes.size() <= max_bytes) {
      break;
    }
    if (!state.contains(key(droppable))) {
      continue;
    }
    state.remove(key(droppable));
    names.append(key(droppable));
    state.insert(key(kDroppedKey), names);
    bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
  }
  if (dropped != nullptr) {
    dropped->clear();
    for (const QJsonValue name : std::as_const(names)) {
      dropped->append(name.toString());
    }
  }
  if (bytes.size() > max_bytes) {
    return std::nullopt;
  }
  return bytes;
}

// ---- Capture
// ----------------------------------------------------------------------------------------------------------

QJsonObject AndroidState::state() const {
  QJsonObject state;
  state.insert(key(kVersionKey), kVersion);
  if (project_ != nullptr) {
    QJsonArray files;
    for (const ProjectFile& f : project_->files()) {
      QJsonObject file{{QStringLiteral("id"), f.id},
                       {QStringLiteral("path"), f.path},
                       {QStringLiteral("role"), roleName(f.role)},
                       {QStringLiteral("track_index"), f.track_index}};
      if (f.raw_format) {
        file.insert(QStringLiteral("raw_format"), f.raw_format->toJson());
      }
      files.append(file);
    }
    state.insert(key(kProjectKey), QJsonObject{{QStringLiteral("path"), project_->path()},
                                               {QStringLiteral("active_file"), project_->activeFile()},
                                               {QStringLiteral("files"), files}});
  }
  if (selection_ != nullptr) {
    state.insert(key(kSelectionKey), selectionToJson(selection_->snapshot()));
  }
  if (picture_ != nullptr) {
    state.insert(key(kViewKey), viewToJson(picture_->viewSnapshot()));
  }
  if (dock_ != nullptr) {
    state.insert(key(kLayoutKey), QJsonDocument::fromJson(dock_->toJson().toUtf8()).object());
  }
  if (!panel_state_.isEmpty()) {
    state.insert(key(kPanelStateKey), QJsonObject::fromVariantMap(panel_state_));
  }
  return state;
}

QString AndroidState::capture() const {
  QStringList dropped;
  const std::optional<QByteArray> bytes = encode(state(), kMaxBytes, &dropped);
  if (!dropped.isEmpty()) {
    qCWarning(lcState, "saved state over %lld bytes: dropped %s", static_cast<long long>(kMaxBytes),
              qPrintable(dropped.join(QLatin1Char(','))));
  }
  if (!bytes) {
    qCWarning(lcState, "saved state over %lld bytes even reduced: nothing saved", static_cast<long long>(kMaxBytes));
    return {};
  }
  return QString::fromUtf8(*bytes);
}

// ---- Restore
// ----------------------------------------------------------------------------------------------------------

QHash<QString, QString> AndroidState::restoreProject(const QJsonObject& object) {
  QHash<QString, QString> ids;
  if (project_ == nullptr) {
    return ids;
  }
  const QJsonArray files = object.value(QStringLiteral("files")).toArray();
  if (project_->fileCount() == 0) {
    const QString path = object.value(QStringLiteral("path")).toString();
    if (path.isEmpty() || !project_->load(path)) {
      for (const QJsonValue value : files) {
        const QJsonObject f = value.toObject();
        const QString file_path = f.value(QStringLiteral("path")).toString();
        if (file_path.isEmpty()) {
          continue;
        }
        std::optional<RawFormat> raw;
        if (f.contains(QStringLiteral("raw_format"))) {
          raw = RawFormat::fromJson(f.value(QStringLiteral("raw_format")).toObject());
        }
        static_cast<void>(project_->addFile(
            file_path, roleFromName(f.value(QStringLiteral("role")).toString()).value_or(FileRole::Single),
            f.value(QStringLiteral("track_index")).toInt(0), raw));
      }
    }
  }
  // Saved ids -> the ids the project has now, matched by path (load() and addFile() number files afresh).
  for (const QJsonValue value : files) {
    const QJsonObject f = value.toObject();
    const QString file_path = f.value(QStringLiteral("path")).toString();
    for (const ProjectFile& now : project_->files()) {
      if (now.path == file_path) {
        ids.insert(f.value(QStringLiteral("id")).toString(), now.id);
        break;
      }
    }
  }
  const QString active = ids.value(object.value(QStringLiteral("active_file")).toString());
  if (!active.isEmpty()) {
    project_->setActiveFile(active);
  }
  return ids;
}

bool AndroidState::restore(const QString& json) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    qCWarning(lcState, "saved state is not JSON: %s", qPrintable(error.errorString()));
    return false;
  }
  const QJsonObject state = document.object();
  if (state.value(key(kVersionKey)).toInt(0) != kVersion) {
    qCWarning(lcState, "saved state has version %d, not %d", state.value(key(kVersionKey)).toInt(0), kVersion);
    return false;
  }
  const QHash<QString, QString> ids = restoreProject(state.value(key(kProjectKey)).toObject());
  if (dock_ != nullptr && state.contains(key(kLayoutKey))) {
    const QJsonObject layout = state.value(key(kLayoutKey)).toObject();
    if (layout.contains(QStringLiteral("groups"))) {
      static_cast<void>(dock_->restoreJson(QString::fromUtf8(QJsonDocument(layout).toJson(QJsonDocument::Compact))));
    } else {
      // A layout preset alone (the §11.5.7 key at its smallest): the preset as it ships.
      static_cast<void>(dock_->applyPreset(layout.value(QStringLiteral("preset")).toString()));
    }
  }
  if (selection_ != nullptr && state.contains(key(kSelectionKey))) {
    SelectionModel::Snapshot snapshot = selectionFromJson(state.value(key(kSelectionKey)).toObject());
    snapshot.file_id = ids.value(snapshot.file_id, snapshot.file_id);
    selection_->restore(snapshot, QString::fromLatin1(kOrigin));
  }
  if (picture_ != nullptr && state.contains(key(kViewKey))) {
    picture_->restoreView(viewFromJson(state.value(key(kViewKey)).toObject()));
  }
  setPanelState(state.value(key(kPanelStateKey)).toObject().toVariantMap());
  Q_EMIT restored();
  return true;
}

}  // namespace ovt::app
