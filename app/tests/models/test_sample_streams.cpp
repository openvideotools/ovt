// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.464: the Sample streams list of the Welcome page (§11.8) reads the corpora manifest.

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariantMap>

#include "models/sample_streams.h"
#include "qt_test_app.h"

namespace {

QJsonObject entry(const char* id, double size, bool fast, const char* url = "https://example.invalid/x.zip") {
  return {{"id", id},
          {"url", url},
          {"licence", "Test licence"},
          {"licence_spdx", "CC0-1.0"},
          {"size_bytes", size},
          {"codecs", QJsonArray{"h264"}},
          {"tags", fast ? QJsonArray{"fast"} : QJsonArray{"slow"}}};
}

QString write_manifest(const QTemporaryDir& dir, const QJsonArray& entries) {
  const QString path = dir.filePath("manifest.json");
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly));
  file.write(QJsonDocument(QJsonObject{{"schema_version", 1}, {"entries", entries}}).toJson());
  return path;
}

}  // namespace

TEST_CASE("SampleStreams: the smallest fast entries with a URL, at most five", "[fast][gui][welcome]") {
  QTemporaryDir dir;
  QJsonArray entries;
  for (int i = 0; i < 8; ++i) {
    entries.append(entry(qPrintable(QString("s%1").arg(8 - i)), 2048.0 * (8 - i), true));
  }
  entries.append(entry("slow", 1.0, false));
  entries.append(entry("nourl", 2.0, true, ""));
  ovt::app::SampleStreams model(write_manifest(dir, entries));
  REQUIRE(model.available());
  const QVariantList list = model.entries();
  REQUIRE(list.size() == ovt::app::SampleStreams::kMaxEntries);
  CHECK(list.front().toMap().value("id").toString() == "s1");
  CHECK(list.back().toMap().value("id").toString() == "s5");
  const QVariantMap first = list.front().toMap();
  CHECK(first.value("licence").toString() == "Test licence");
  CHECK(first.value("licenceSpdx").toString() == "CC0-1.0");
  CHECK(first.value("sizeText").toString() == "2 KB");
}

TEST_CASE("SampleStreams: a missing or broken manifest gives an empty list", "[fast][gui][welcome]") {
  QTemporaryDir dir;
  ovt::app::SampleStreams missing(dir.filePath("none.json"));
  CHECK_FALSE(missing.available());
  QFile bad(dir.filePath("bad.json"));
  REQUIRE(bad.open(QIODevice::WriteOnly));
  bad.write("{not json");
  bad.close();
  CHECK_FALSE(missing.load(dir.filePath("bad.json")));
  CHECK(missing.entries().isEmpty());
}

TEST_CASE("SampleStreams::sizeText", "[fast][gui][welcome]") {
  CHECK(ovt::app::SampleStreams::sizeText(512) == "512 B");
  CHECK(ovt::app::SampleStreams::sizeText(960760) == "938 KB");
  CHECK(ovt::app::SampleStreams::sizeText(1766668) == "1.7 MB");
}
