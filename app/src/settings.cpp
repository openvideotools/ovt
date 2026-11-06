// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "platform/platform.h"

namespace ovt::app {

namespace {

constexpr int kSettingsVersion = 1;

QJsonObject load_schema(QString* error) {
  QFile file(QStringLiteral(":/qt/qml/OvtStudio/settings.schema.json"));
  if (!file.open(QIODevice::ReadOnly)) {
    *error = QStringLiteral("the bundled settings schema is missing");
    return {};
  }
  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (!doc.isObject()) {
    *error = QStringLiteral("the bundled settings schema is not JSON: %1").arg(parse_error.errorString());
    return {};
  }
  return doc.object();
}

bool matches_type(const QJsonValue& value, const QString& type) {
  if (type == QLatin1String("object")) {
    return value.isObject();
  }
  if (type == QLatin1String("array")) {
    return value.isArray();
  }
  if (type == QLatin1String("string")) {
    return value.isString();
  }
  if (type == QLatin1String("boolean")) {
    return value.isBool();
  }
  if (type == QLatin1String("null")) {
    return value.isNull();
  }
  if (type == QLatin1String("number")) {
    return value.isDouble();
  }
  if (type == QLatin1String("integer")) {
    return value.isDouble() && std::floor(value.toDouble()) == value.toDouble();
  }
  return true;  // an unknown type name constrains nothing here
}

bool check(const QJsonValue& value, const QJsonObject& schema, const QString& where, QString* reason);

// The violation text of const, enum, type, minimum and maximum, or an empty string when the value passes.
QString scalar_violation(const QJsonValue& value, const QJsonObject& schema) {
  if (schema.contains(QStringLiteral("const")) && schema.value(QStringLiteral("const")) != value) {
    return QStringLiteral("unexpected value");
  }
  if (schema.contains(QStringLiteral("enum")) && !schema.value(QStringLiteral("enum")).toArray().contains(value)) {
    return QStringLiteral("not one of the allowed values");
  }
  if (schema.contains(QStringLiteral("type"))) {
    const QJsonValue type = schema.value(QStringLiteral("type"));
    const QJsonArray types = type.isArray() ? type.toArray() : QJsonArray{type};
    const bool ok =
        std::ranges::any_of(types, [&value](const QJsonValue& t) { return matches_type(value, t.toString()); });
    if (!ok) {
      return QStringLiteral("wrong type");
    }
  }
  if (value.isDouble()) {
    if (schema.contains(QStringLiteral("minimum")) &&
        value.toDouble() < schema.value(QStringLiteral("minimum")).toDouble()) {
      return QStringLiteral("below the minimum");
    }
    if (schema.contains(QStringLiteral("maximum")) &&
        value.toDouble() > schema.value(QStringLiteral("maximum")).toDouble()) {
      return QStringLiteral("above the maximum");
    }
  }
  return {};
}

// required and properties of an object value.
// NOLINTNEXTLINE(misc-no-recursion): the schema is a tree, as deep as the document
bool check_object(const QJsonObject& object, const QJsonObject& schema, const QString& where, QString* reason) {
  for (const QJsonValue name : schema.value(QStringLiteral("required")).toArray()) {
    if (!object.contains(name.toString())) {
      *reason = where + QStringLiteral(": missing \"%1\"").arg(name.toString());
      return false;
    }
  }
  const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
  for (auto it = properties.begin(); it != properties.end(); ++it) {
    if (object.contains(it.key()) &&
        !check(object.value(it.key()), it.value().toObject(), where + QLatin1Char('/') + it.key(), reason)) {
      return false;
    }
  }
  return true;
}

// NOLINTNEXTLINE(misc-no-recursion): the schema is a tree, as deep as the document
bool check_items(const QJsonArray& array, const QJsonObject& items, const QString& where, QString* reason) {
  for (int i = 0; i < array.size(); ++i) {
    if (!check(array.at(i), items, where + QLatin1Char('/') + QString::number(i), reason)) {
      return false;
    }
  }
  return true;
}

// Validates the subset of JSON Schema 2020-12 that settings.schema.json uses. Returns false and sets `reason`
// ("/general/theme: not one of the allowed values") at the first violation. The schema is a tree, so the
// recursion is as deep as the document.
// NOLINTNEXTLINE(misc-no-recursion)
bool check(const QJsonValue& value, const QJsonObject& schema, const QString& where, QString* reason) {
  const QString violation = scalar_violation(value, schema);
  if (!violation.isEmpty()) {
    *reason = (where.isEmpty() ? QStringLiteral("/") : where) + QStringLiteral(": ") + violation;
    return false;
  }
  if (value.isObject() && !check_object(value.toObject(), schema, where, reason)) {
    return false;
  }
  if (value.isArray() && schema.contains(QStringLiteral("items"))) {
    return check_items(value.toArray(), schema.value(QStringLiteral("items")).toObject(), where, reason);
  }
  return true;
}

// Recursively overlays `over` onto `base`; objects merge, everything else is replaced. Unknown keys survive.
// NOLINTNEXTLINE(misc-no-recursion): as deep as the document
QJsonObject merge(QJsonObject base, const QJsonObject& over) {
  for (auto it = over.begin(); it != over.end(); ++it) {
    if (it.value().isObject() && base.value(it.key()).isObject()) {
      base.insert(it.key(), merge(base.value(it.key()).toObject(), it.value().toObject()));
    } else {
      base.insert(it.key(), it.value());
    }
  }
  return base;
}

}  // namespace

QJsonObject Settings::defaults() {
  QJsonObject general;
  general.insert(QStringLiteral("theme"), QStringLiteral("system"));
  general.insert(QStringLiteral("language"), QStringLiteral("en"));
  general.insert(QStringLiteral("recent_files_count"), 10);
  general.insert(QStringLiteral("confirm_on_close"), true);
  QJsonObject root;
  root.insert(QStringLiteral("settings_version"), kSettingsVersion);
  root.insert(QStringLiteral("general"), general);
  return root;
}

bool Settings::validate(const QJsonObject& document, QString* reason) {
  QString local_reason;
  QString* out = reason != nullptr ? reason : &local_reason;
  QString schema_error;
  const QJsonObject schema = load_schema(&schema_error);
  if (!schema_error.isEmpty()) {
    *out = schema_error;
    return false;
  }
  return check(document, schema, QString(), out);
}

Settings::Settings() : Settings(Platform::configDir()) {}

Settings::Settings(const QString& config_dir, QObject* parent)
    : QObject(parent), path_(QDir::cleanPath(config_dir) + QStringLiteral("/settings.json")), document_(defaults()) {
  load();
}

void Settings::raise(const QString& reason) {
  diagnostic_code_ = QStringLiteral("GUI-SET-0001");
  diagnostic_message_ = QStringLiteral("settings file unreadable, defaults used (%1)").arg(reason);
  // No logger in the GUI yet (WI-1.512 brings ovt::log); the diagnostics panel is WI-1.453.
  const QByteArray line =
      (diagnostic_code_ + QStringLiteral(": ") + diagnostic_message_ + QStringLiteral(": ") + path_).toLocal8Bit();
  static_cast<void>(std::fprintf(stderr, "%s\n", line.constData()));
  Q_EMIT diagnosticChanged();
}

void Settings::load() {
  QFile file(path_);
  if (!file.exists()) {
    return;  // first start: defaults, no diagnostic
  }
  if (!file.open(QIODevice::ReadOnly)) {
    raise(file.errorString());
    return;
  }
  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
    raise(parse_error.error != QJsonParseError::NoError ? parse_error.errorString()
                                                        : QStringLiteral("not a JSON object"));
    return;
  }
  QString reason;
  if (!validate(doc.object(), &reason)) {
    raise(reason);
    return;
  }
  document_ = merge(defaults(), doc.object());
}

QString Settings::theme() const {
  return document_.value(QStringLiteral("general"))
      .toObject()
      .value(QStringLiteral("theme"))
      .toString(QStringLiteral("system"));
}

void Settings::setTheme(const QString& theme) {
  if (theme == this->theme()) {
    return;
  }
  QJsonObject candidate = document_;
  QJsonObject general = candidate.value(QStringLiteral("general")).toObject();
  general.insert(QStringLiteral("theme"), theme);
  candidate.insert(QStringLiteral("general"), general);
  QString reason;
  if (!validate(candidate, &reason)) {
    static_cast<void>(std::fprintf(stderr, "settings: theme \"%s\" rejected (%s)\n", theme.toLocal8Bit().constData(),
                                   reason.toLocal8Bit().constData()));
    return;
  }
  document_ = candidate;
  Q_EMIT themeChanged();
  if (!save()) {
    // No diagnostics model yet (WI-1.453): say so on stderr instead of failing silently.
    static_cast<void>(std::fprintf(stderr, "settings: could not write %s\n", path_.toLocal8Bit().constData()));
  }
}

bool Settings::save() {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    return false;
  }
  QSaveFile file(path_);  // temp file in the same directory, renamed on commit
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(QJsonDocument(document_).toJson(QJsonDocument::Indented));
  return file.commit();
}

}  // namespace ovt::app
