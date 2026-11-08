// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/sample_streams.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QVariantMap>

#include <algorithm>
#include <vector>

namespace ovt::app {

namespace {

bool has_tag(const QJsonObject& entry, const QString& tag) {
  const QJsonArray tags = entry.value(QStringLiteral("tags")).toArray();
  return std::ranges::any_of(tags, [&](const QJsonValue t) { return t.toString() == tag; });
}

}  // namespace

SampleStreams::SampleStreams() {
  load(defaultManifestPath());
}

SampleStreams::SampleStreams(const QString& manifest_path, QObject* parent) : QObject(parent) {
  load(manifest_path);
}

QString SampleStreams::defaultManifestPath() {
  const QByteArray override_path = qgetenv("OVT_CORPORA_MANIFEST");
  if (!override_path.isEmpty()) {
    return QString::fromLocal8Bit(override_path);
  }
  return QDir::cleanPath(QCoreApplication::applicationDirPath() +
                         QStringLiteral("/../share/ovt/corpora/manifest.json"));
}

QString SampleStreams::sizeText(double bytes) {
  if (bytes < 1024.0) {
    return QStringLiteral("%1 B").arg(static_cast<qint64>(bytes));
  }
  if (bytes < 1024.0 * 1024.0) {
    return QStringLiteral("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 0));
  }
  return QStringLiteral("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1));
}

bool SampleStreams::load(const QString& manifest_path) {
  QVariantList list;
  QFile file(manifest_path);
  if (file.open(QIODevice::ReadOnly)) {
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    std::vector<QJsonObject> picked;
    for (const QJsonValue v : doc.object().value(QStringLiteral("entries")).toArray()) {
      const QJsonObject entry = v.toObject();
      if (entry.value(QStringLiteral("url")).toString().isEmpty() || !has_tag(entry, QStringLiteral("fast")) ||
          entry.value(QStringLiteral("size_bytes")).toDouble() <= 0) {
        continue;
      }
      picked.push_back(entry);
    }
    // Smallest first; the id breaks ties so the order is stable.
    std::ranges::sort(picked, [](const QJsonObject& a, const QJsonObject& b) {
      const double sa = a.value(QStringLiteral("size_bytes")).toDouble();
      const double sb = b.value(QStringLiteral("size_bytes")).toDouble();
      return sa != sb ? sa < sb : a.value(QStringLiteral("id")).toString() < b.value(QStringLiteral("id")).toString();
    });
    if (picked.size() > static_cast<std::size_t>(kMaxEntries)) {
      picked.resize(kMaxEntries);
    }
    for (const QJsonObject& entry : picked) {
      const double size = entry.value(QStringLiteral("size_bytes")).toDouble();
      list.push_back(
          QVariantMap{{QStringLiteral("id"), entry.value(QStringLiteral("id")).toString()},
                      {QStringLiteral("url"), entry.value(QStringLiteral("url")).toString()},
                      {QStringLiteral("licence"), entry.value(QStringLiteral("licence")).toString()},
                      {QStringLiteral("licenceSpdx"), entry.value(QStringLiteral("licence_spdx")).toString()},
                      {QStringLiteral("sizeBytes"), size},
                      {QStringLiteral("sizeText"), sizeText(size)},
                      {QStringLiteral("codecs"), entry.value(QStringLiteral("codecs")).toArray().toVariantList()}});
    }
  }
  entries_ = list;
  Q_EMIT entriesChanged();
  return !entries_.isEmpty();
}

}  // namespace ovt::app
