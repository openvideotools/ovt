// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of Settings and Platform (WI-1.450; chapter 11 §11.2.1, §11.2.5). Acceptance test 3: a theme change is
// restored after a "restart" (a second Settings on the same directory), the written settings.json validates against
// settings.schema.json, and a corrupt file gives the defaults and GUI-SET-0001.

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSGRendererInterface>
#include <QString>
#include <QTemporaryDir>

#include "platform/platform.h"
#include "settings.h"

namespace {

using ovt::app::Platform;
using ovt::app::Settings;

QJsonObject read_json(const QString& path) {
  QFile file(path);
  REQUIRE(file.open(QIODevice::ReadOnly));
  return QJsonDocument::fromJson(file.readAll()).object();
}

void write_file(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
}

}  // namespace

TEST_CASE("Settings: an empty directory gives the defaults without a diagnostic", "[fast][gui][settings]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const Settings settings(dir.path());
  CHECK(settings.theme() == QStringLiteral("system"));
  CHECK(settings.diagnosticCode().isEmpty());
  CHECK_FALSE(QFile::exists(settings.path()));  // nothing is written until something changes
  QString reason;
  CHECK(Settings::validate(settings.document(), &reason));
}

TEST_CASE("Settings: the theme change survives a restart and the file validates", "[fast][gui][settings]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  {
    Settings first(dir.path());
    first.setTheme(QStringLiteral("dark"));
  }
  const QJsonObject written = read_json(dir.path() + QStringLiteral("/settings.json"));
  QString reason;
  CHECK(Settings::validate(written, &reason));
  CHECK(reason.isEmpty());
  CHECK(written.value(QStringLiteral("settings_version")).toInt() == 1);

  const Settings second(dir.path());
  CHECK(second.theme() == QStringLiteral("dark"));
  CHECK(second.diagnosticCode().isEmpty());
  // No temp file is left behind by the atomic write.
  CHECK(QDir(dir.path()).entryList(QDir::Files) == QStringList{QStringLiteral("settings.json")});
}

TEST_CASE("Settings: a corrupt file loads the defaults and raises GUI-SET-0001", "[fast][gui][settings]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  write_file(dir.path() + QStringLiteral("/settings.json"), "{ not json");
  const Settings settings(dir.path());
  CHECK(settings.theme() == QStringLiteral("system"));
  CHECK(settings.diagnosticCode() == QStringLiteral("GUI-SET-0001"));
  CHECK(settings.diagnosticMessage().contains(QStringLiteral("defaults used")));
}

TEST_CASE("Settings: a file that parses but violates the schema is rejected", "[fast][gui][settings]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  write_file(dir.path() + QStringLiteral("/settings.json"),
             R"({"settings_version": 1, "general": {"theme": "purple"}})");
  const Settings settings(dir.path());
  CHECK(settings.theme() == QStringLiteral("system"));
  CHECK(settings.diagnosticCode() == QStringLiteral("GUI-SET-0001"));
  CHECK(settings.diagnosticMessage().contains(QStringLiteral("/general/theme")));
}

TEST_CASE("Settings: unknown keys are preserved on save", "[fast][gui][settings]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  write_file(dir.path() + QStringLiteral("/settings.json"),
             R"({"settings_version": 1, "future": {"x": [1, 2]}, "general": {"theme": "light", "extra": true}})");
  Settings settings(dir.path());
  REQUIRE(settings.theme() == QStringLiteral("light"));
  settings.setTheme(QStringLiteral("dark"));
  const QJsonObject written = read_json(dir.path() + QStringLiteral("/settings.json"));
  CHECK(written.value(QStringLiteral("future")).toObject().value(QStringLiteral("x")).toArray().size() == 2);
  CHECK(written.value(QStringLiteral("general")).toObject().value(QStringLiteral("extra")).toBool());
  CHECK(written.value(QStringLiteral("general")).toObject().value(QStringLiteral("theme")).toString() ==
        QStringLiteral("dark"));
}

TEST_CASE("Settings: an invalid theme is refused", "[fast][gui][settings]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  Settings settings(dir.path());
  settings.setTheme(QStringLiteral("purple"));
  CHECK(settings.theme() == QStringLiteral("system"));
  CHECK_FALSE(QFile::exists(settings.path()));
}

TEST_CASE("Settings::validate: type, range and required keys", "[fast][gui][settings]") {
  QString reason;
  QJsonObject document = Settings::defaults();
  CHECK(Settings::validate(document, &reason));
  QJsonObject general = document.value(QStringLiteral("general")).toObject();
  general.insert(QStringLiteral("recent_files_count"), 1000);
  document.insert(QStringLiteral("general"), general);
  CHECK_FALSE(Settings::validate(document, &reason));
  CHECK(reason == QStringLiteral("/general/recent_files_count: above the maximum"));
  CHECK_FALSE(Settings::validate(QJsonObject(), &reason));
  CHECK(reason == QStringLiteral("/: missing \"settings_version\""));
}

TEST_CASE("Platform: OVT_CONFIG_DIR overrides the configuration directory", "[fast][gui][platform]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  qputenv("OVT_CONFIG_DIR", dir.path().toLocal8Bit());
  CHECK(Platform::configDir() == QDir::cleanPath(dir.path()));
  CHECK(Platform::settingsPath() == QDir::cleanPath(dir.path()) + QStringLiteral("/settings.json"));
  CHECK(Platform::layoutsPath().endsWith(QStringLiteral("/layouts.json")));
  CHECK(Platform::keymapPath().endsWith(QStringLiteral("/keymap.json")));
  qunsetenv("OVT_CONFIG_DIR");
  CHECK(Platform::configDir().endsWith(QStringLiteral("/ovt")));
}

TEST_CASE("Platform: RHI backend names", "[fast][gui][platform]") {
  QSGRendererInterface::GraphicsApi api = QSGRendererInterface::Unknown;
  CHECK(Platform::parseGraphicsApi(QStringLiteral("Metal"), &api));
  CHECK(api == QSGRendererInterface::Metal);
  CHECK(Platform::parseGraphicsApi(QStringLiteral("gles"), &api));
  CHECK(api == QSGRendererInterface::OpenGL);
  CHECK_FALSE(Platform::parseGraphicsApi(QStringLiteral("directx12"), &api));
#if defined(Q_OS_MACOS)
  CHECK(Platform::defaultGraphicsApi() == QSGRendererInterface::Metal);
#elif defined(Q_OS_ANDROID)
  CHECK(Platform::defaultGraphicsApi() == QSGRendererInterface::OpenGL);
#else
  CHECK(Platform::defaultGraphicsApi() == QSGRendererInterface::Vulkan);
#endif
}
