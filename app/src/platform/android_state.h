// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// AndroidState (chapter 11 §11.5.7; WI-A.455): the GUI state that MainActivity keeps in its saved instance state, as
// one JSON document of at most 100 KB. The core handles survive an Activity recreation in OvtSession (WI-A.450); the
// QML side does not, because each QtQuickView builds its own QML engine. This class turns what the user looks at into
// JSON and back:
//
//   {"version": 1,
//    "project": {"path", "active_file", "files": [{"id", "path", "role", "track_index", "raw_format"?}]},
//    "selection": SelectionState (SelectionModel::Snapshot),
//    "view": ViewState of the Picture panel (PictureController::ViewSnapshot),
//    "layout": the DockModel layout document (layouts.json form; the layout preset is "layout.preset"),
//    "panel_state": a QVariantMap the shell fills (focused panel, drawer, single-pane tab, ...)}
//
// The serialiser is plain Qt and is built and tested on every platform; only the Kotlin glue is Android's.

#ifndef OVT_APP_PLATFORM_ANDROID_STATE_H
#define OVT_APP_PLATFORM_ANDROID_STATE_H

#include <QtQml/qqmlregistration.h>
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <optional>

#include "dock/dock_model.h"
#include "models/selection_model.h"
#include "picture_controller.h"
#include "project.h"

namespace ovt::app {

class AndroidState : public QObject {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(ovt::app::SelectionModel* selection READ selection WRITE setSelection NOTIFY sourcesChanged)
  Q_PROPERTY(ovt::app::DockModel* dock READ dock WRITE setDock NOTIFY sourcesChanged)
  Q_PROPERTY(ovt::app::Project* project READ project WRITE setProject NOTIFY sourcesChanged)
  // The Picture panel's controller (owner of the ViewState); null while the panel does not exist.
  Q_PROPERTY(ovt::app::PictureController* picture READ picture WRITE setPicture NOTIFY sourcesChanged)
  // Shell state kept as is (QVariantMap -> JSON object): written by the shell, replaced by restore().
  Q_PROPERTY(QVariantMap panelState READ panelState WRITE setPanelState NOTIFY panelStateChanged)

 public:
  // §11.5.7: the saved state is at most 100 KB. Bigger documents lose panel_state, then layout (capture()).
  static constexpr qsizetype kMaxBytes = qsizetype{100} * 1024;
  static constexpr int kVersion = 1;
  // The Selection origin of a restored selection (no panel ignores it as an echo).
  static constexpr const char* kOrigin = "androidState";

  explicit AndroidState(QObject* parent = nullptr);

  [[nodiscard]] SelectionModel* selection() const { return selection_.data(); }
  void setSelection(SelectionModel* selection);
  [[nodiscard]] DockModel* dock() const { return dock_.data(); }
  void setDock(DockModel* dock);
  [[nodiscard]] Project* project() const { return project_.data(); }
  void setProject(Project* project);
  [[nodiscard]] PictureController* picture() const { return picture_.data(); }
  void setPicture(PictureController* picture);
  [[nodiscard]] QVariantMap panelState() const { return panel_state_; }
  void setPanelState(const QVariantMap& state);

  // The state now, as compact JSON of at most kMaxBytes; empty when even the reduced document is too big.
  Q_INVOKABLE [[nodiscard]] QString capture() const;
  // Puts a captured state back into the objects set above. False (and nothing changed) when `json` is not a
  // document of this version. A project already holding files is kept; the selection's file id follows the file's
  // path into it.
  Q_INVOKABLE bool restore(const QString& json);

  // ---- The pieces, for the tests -----------------------------------------------------------------------------------
  [[nodiscard]] static QJsonObject selectionToJson(const SelectionModel::Snapshot& selection);
  [[nodiscard]] static SelectionModel::Snapshot selectionFromJson(const QJsonObject& object);
  [[nodiscard]] static QJsonObject viewToJson(const PictureController::ViewSnapshot& view);
  [[nodiscard]] static PictureController::ViewSnapshot viewFromJson(const QJsonObject& object);
  // Serialises `state`, dropping "panel_state" and then "layout" while it is bigger than `max_bytes`; the names
  // dropped go to `dropped` and to the document's "dropped" array. nullopt when it is still too big.
  [[nodiscard]] static std::optional<QByteArray> encode(QJsonObject state, qsizetype max_bytes,
                                                        QStringList* dropped = nullptr);

 Q_SIGNALS:
  void sourcesChanged();
  void panelStateChanged();
  // restore() put back a selection, view or layout: the shell publishes the state again.
  void restored();

 private:
  [[nodiscard]] QJsonObject state() const;
  // Re-creates the project's files when it has none; returns saved file id -> project file id.
  [[nodiscard]] QHash<QString, QString> restoreProject(const QJsonObject& object);

  QPointer<SelectionModel> selection_;
  QPointer<DockModel> dock_;
  QPointer<Project> project_;
  QPointer<PictureController> picture_;
  QVariantMap panel_state_;
};

}  // namespace ovt::app

#endif  // OVT_APP_PLATFORM_ANDROID_STATE_H
