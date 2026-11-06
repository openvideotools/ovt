// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The settings document of chapter 11 §11.2.5 (WI-1.450): one JSON file, read once at start-up, written
// atomically, validated against docs/spec/schemas/settings.schema.json. `QSettings` is not used (D8).

#ifndef OVT_APP_SETTINGS_H
#define OVT_APP_SETTINGS_H

#include <QtQml/qqmlregistration.h>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace ovt::app {

class Settings : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  // "system", "light" or "dark". Setting it writes the file.
  Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
  Q_PROPERTY(QString path READ path CONSTANT)
  // The last diagnostic raised while loading (GUI-SET-0001), empty if none.
  Q_PROPERTY(QString diagnosticCode READ diagnosticCode NOTIFY diagnosticChanged)
  Q_PROPERTY(QString diagnosticMessage READ diagnosticMessage NOTIFY diagnosticChanged)

 public:
  // For QML: the directory of Platform::configDir().
  Settings();
  explicit Settings(const QString& config_dir, QObject* parent = nullptr);

  QString theme() const;
  void setTheme(const QString& theme);
  QString path() const { return path_; }
  QString diagnosticCode() const { return diagnostic_code_; }
  QString diagnosticMessage() const { return diagnostic_message_; }

  // The document as it will be written: the defaults with the loaded values and unknown keys merged in.
  const QJsonObject& document() const { return document_; }
  static QJsonObject defaults();

  // Validates `document` against the bundled settings.schema.json (a subset of JSON Schema 2020-12: type, enum,
  // const, minimum, maximum, properties, required, items). On failure `reason` says where.
  static bool validate(const QJsonObject& document, QString* reason);

  // Writes the document next to the final path (temp file + rename). Creates the directory.
  bool save();

 Q_SIGNALS:
  void themeChanged();
  void diagnosticChanged();

 private:
  void load();
  void raise(const QString& reason);

  QString path_;
  QJsonObject document_;
  QString diagnostic_code_;
  QString diagnostic_message_;
};

}  // namespace ovt::app

#endif  // OVT_APP_SETTINGS_H
