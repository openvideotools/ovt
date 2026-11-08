// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of Project, RecentFiles and RawFormatModel (WI-1.454; chapter 11 §11.2.6, §11.8).
//   AT1: a saved project with two files and bookmarks restores files, roles, layout, view and bookmarks.
//   AT2: a project whose file no longer exists opens with the file in error state (GUI-PROJ-0003).
//   AT3: changing the raw width/height/bit depth updates the preview within 200 ms; a size mismatch gives GUI-RAW-0001.
//   AT4 (the model part): the second file opened while one is open asks for the Open-as choice.

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include "project.h"
#include "qt_test_app.h"

namespace {

using ovt::app::FileRole;
using ovt::app::Project;
using ovt::app::RawFormat;
using ovt::app::RawFormatModel;
using ovt::app::RecentFiles;

void touch(const QString& path, const QByteArray& bytes = QByteArray(16, 'x')) {
  REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly));
  file.write(bytes);
}

QJsonObject object_of(const QString& json) {
  return QJsonDocument::fromJson(json.toUtf8()).object();
}

}  // namespace

TEST_CASE("Project: AT1 a saved project restores files, roles, layout, view and bookmarks", "[fast][gui][project]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  touch(dir.filePath(QStringLiteral("media/ref.mp4")));
  touch(dir.filePath(QStringLiteral("dist.mp4")));
  const QString project_path = dir.filePath(QStringLiteral("work/session.ovtproj"));

  Project first(dir.filePath(QStringLiteral("cfg")));
  const QString ref = first.addFile(dir.filePath(QStringLiteral("media/ref.mp4")), FileRole::Reference, 1);
  const QString dist = first.addFile(dir.filePath(QStringLiteral("dist.mp4")), FileRole::Distorted);
  first.noteSourceHash(ref, QString(64, QLatin1Char('a')));
  first.setActiveFile(dist);
  first.setSection(QStringLiteral("layout"), object_of(R"({"preset":"analysis","leftVisible":true})"));
  first.setSection(QStringLiteral("view"), object_of(R"({"order":"display","zoom":2.5,"pan":[10,20],"plane_mode":1})"));
  first.setSection(
      QStringLiteral("bookmarks"),
      QJsonDocument::fromJson(R"([{"id":"b1","file":"f1","decode_index":42,"name":"scene cut","color":"#FF0000"}])")
          .array());
  first.setSection(QStringLiteral("x_future"), QJsonObject{{QStringLiteral("keep"), true}});
  REQUIRE(first.dirty());
  REQUIRE(first.saveAs(project_path));
  CHECK_FALSE(first.dirty());

  // The file validates, the file under the project directory's parent keeps "..", and the file below is relative.
  QFile written(project_path);
  REQUIRE(written.open(QIODevice::ReadOnly));
  const QJsonObject doc = QJsonDocument::fromJson(written.readAll()).object();
  QString reason;
  CHECK(Project::validate(doc, &reason));
  CHECK(reason.isEmpty());

  Project second(dir.filePath(QStringLiteral("cfg")));
  REQUIRE(second.load(project_path));
  CHECK_FALSE(second.dirty());
  REQUIRE(second.files().size() == 2);
  CHECK(second.files() == first.files());  // ids, absolute paths, roles, track indices and hashes
  CHECK(second.activeFile() == dist);
  CHECK(second.section(QStringLiteral("layout")) == first.section(QStringLiteral("layout")));
  CHECK(second.section(QStringLiteral("view")) == first.section(QStringLiteral("view")));
  CHECK(second.section(QStringLiteral("bookmarks")) == first.section(QStringLiteral("bookmarks")));
  CHECK(second.section(QStringLiteral("x_future")).toObject().value(QStringLiteral("keep")).toBool());
  CHECK(second.diagnostics().isEmpty());
}

TEST_CASE("Project: paths are relative only under the project directory; content URIs are verbatim",
          "[fast][gui][project]") {
  const QTemporaryDir dir;
  const QTemporaryDir elsewhere;
  REQUIRE(dir.isValid());
  REQUIRE(elsewhere.isValid());
  touch(dir.filePath(QStringLiteral("proj/media/a.mp4")));
  touch(elsewhere.filePath(QStringLiteral("b.mp4")));
  Project project(dir.filePath(QStringLiteral("cfg")));
  project.addFile(dir.filePath(QStringLiteral("proj/media/a.mp4")), FileRole::Single);
  project.addFile(elsewhere.filePath(QStringLiteral("b.mp4")), FileRole::Reference);
  project.addFile(QStringLiteral("content://media/external/video/7"), FileRole::Distorted);
  const QJsonArray files =
      project.document(dir.filePath(QStringLiteral("proj"))).value(QStringLiteral("files")).toArray();
  REQUIRE(files.size() == 3);
  CHECK(files.at(0).toObject().value(QStringLiteral("path")).toString() == QStringLiteral("media/a.mp4"));
  CHECK(files.at(1).toObject().value(QStringLiteral("path")).toString() ==
        QDir::cleanPath(elsewhere.filePath(QStringLiteral("b.mp4"))));
  CHECK(files.at(2).toObject().value(QStringLiteral("path")).toString() ==
        QStringLiteral("content://media/external/video/7"));
  CHECK(project.diagnostics().isEmpty());  // a content:// URI is never checked for existence
}

TEST_CASE("Project: AT2 a missing file opens in error state with GUI-PROJ-0003", "[fast][gui][project]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  touch(dir.filePath(QStringLiteral("clip.mp4")));
  const QString project_path = dir.filePath(QStringLiteral("p.ovtproj"));
  {
    Project project(dir.filePath(QStringLiteral("cfg")));
    project.addFile(dir.filePath(QStringLiteral("clip.mp4")), FileRole::Single);
    REQUIRE(project.saveAs(project_path));
  }
  REQUIRE(QFile::remove(dir.filePath(QStringLiteral("clip.mp4"))));
  Project reopened(dir.filePath(QStringLiteral("cfg")));
  REQUIRE(reopened.load(project_path));  // the project still opens
  REQUIRE(reopened.files().size() == 1);
  CHECK(reopened.files().constFirst().error_code == QStringLiteral("GUI-PROJ-0003"));
  CHECK(reopened.diagnosticCode() == QStringLiteral("GUI-PROJ-0003"));
  CHECK(reopened.diagnosticMessage().contains(QStringLiteral("clip.mp4")));
}

TEST_CASE("Project: a file that fails the schema raises GUI-PROJ-0002 with the JSON pointer", "[fast][gui][project]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString bad = dir.filePath(QStringLiteral("bad.ovtproj"));
  touch(bad, R"({"ovtproj_version":1,"files":[{"id":"x1","path":"a.mp4","role":"single"}]})");
  Project project(dir.filePath(QStringLiteral("cfg")));
  CHECK_FALSE(project.load(bad));
  CHECK(project.diagnosticCode() == QStringLiteral("GUI-PROJ-0002"));
  CHECK(project.diagnosticMessage().contains(QStringLiteral("/files/0/id")));

  touch(bad, R"({"ovtproj_version":2,"files":[]})");
  CHECK_FALSE(project.load(bad));
  CHECK(project.diagnosticMessage().contains(QStringLiteral("/ovtproj_version")));

  touch(bad, "not json");
  CHECK_FALSE(project.load(bad));
  CHECK(project.diagnostics().size() == 3);
}

TEST_CASE("Project: a changed source hash warns with GUI-PROJ-0001", "[fast][gui][project]") {
  const QTemporaryDir dir;
  touch(dir.filePath(QStringLiteral("a.mp4")));
  Project project(dir.filePath(QStringLiteral("cfg")));
  const QString id = project.addFile(dir.filePath(QStringLiteral("a.mp4")), FileRole::Single);
  project.noteSourceHash(id, QString(64, QLatin1Char('1')));
  CHECK(project.diagnostics().isEmpty());
  project.noteSourceHash(id, QString(64, QLatin1Char('1')));
  CHECK(project.diagnostics().isEmpty());
  project.noteSourceHash(id, QString(64, QLatin1Char('2')));
  CHECK(project.diagnosticCode() == QStringLiteral("GUI-PROJ-0001"));
}

TEST_CASE("Project: autosave writes when dirty, is offered for recovery and removed by a save",
          "[fast][gui][project]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  touch(dir.filePath(QStringLiteral("a.mp4")));
  const QString project_path = dir.filePath(QStringLiteral("p.ovtproj"));
  {
    Project crashed(dir.filePath(QStringLiteral("cfg")));
    crashed.addFile(dir.filePath(QStringLiteral("a.mp4")), FileRole::Single);
    REQUIRE(crashed.saveAs(project_path));
    crashed.setSection(QStringLiteral("view"), object_of(R"({"zoom":4})"));
    crashed.setAutosaveIntervalMs(20);
    CHECK_FALSE(crashed.hasRecovery());
    REQUIRE(ovt::app::testing::spin_until([&crashed] { return crashed.hasRecovery(); }, std::chrono::seconds(3)));
    CHECK(crashed.autosavePath() == project_path + QStringLiteral(".autosave"));
  }  // no save: "the application crashed"
  Project next(dir.filePath(QStringLiteral("cfg")));
  REQUIRE(next.load(project_path));
  CHECK(next.section(QStringLiteral("view")).isUndefined());  // the saved file lacks the change
  REQUIRE(next.hasRecovery());
  REQUIRE(next.recover());
  CHECK(next.section(QStringLiteral("view")).toObject().value(QStringLiteral("zoom")).toInt() == 4);
  CHECK(next.dirty());
  REQUIRE(next.save());
  CHECK_FALSE(next.hasRecovery());
}

TEST_CASE("Project: the open flow asks for the Open-as choice from the second file on", "[fast][gui][project]") {
  const QTemporaryDir dir;
  touch(dir.filePath(QStringLiteral("a.mp4")));
  touch(dir.filePath(QStringLiteral("b.mp4")));
  const QString a = dir.filePath(QStringLiteral("a.mp4"));
  const QString b = dir.filePath(QStringLiteral("b.mp4"));
  Project project(dir.filePath(QStringLiteral("cfg")));
  QStringList opened;
  QStringList asked;
  QObject::connect(&project, &Project::fileOpened, &project, [&opened](const QString& id) { opened << id; });
  QObject::connect(&project, &Project::secondFileRequested, &project, [&asked](const QString& p) { asked << p; });

  project.openFile(a);
  CHECK(opened == QStringList{QStringLiteral("f1")});
  CHECK(project.files().constFirst().role == FileRole::Single);
  project.openFile(b);  // dropped on the window
  CHECK(asked == QStringList{QDir::cleanPath(b)});
  CHECK(project.fileCount() == 1);

  project.openAs(b, Project::Reference);  // new = reference (B), current = distorted
  REQUIRE(project.fileCount() == 2);
  CHECK(project.files().at(0).role == FileRole::Distorted);
  CHECK(project.files().at(1).role == FileRole::Reference);

  project.closeAll();
  project.openFile(a);
  project.openAs(b, Project::Distorted);  // new = distorted (A), current becomes reference
  CHECK(project.files().at(0).role == FileRole::Reference);
  CHECK(project.files().at(1).role == FileRole::Distorted);

  project.openAs(b, Project::Replace);
  REQUIRE(project.fileCount() == 1);
  CHECK(project.files().constFirst().role == FileRole::Single);
  CHECK(project.files().constFirst().path == QDir::cleanPath(b));
}

TEST_CASE("Project: a .ovtproj given to openFile is loaded, not added", "[fast][gui][project]") {
  const QTemporaryDir dir;
  touch(dir.filePath(QStringLiteral("a.mp4")));
  const QString project_path = dir.filePath(QStringLiteral("p.ovtproj"));
  {
    Project project(dir.filePath(QStringLiteral("cfg")));
    project.addFile(dir.filePath(QStringLiteral("a.mp4")), FileRole::Single);
    REQUIRE(project.saveAs(project_path));
  }
  Project project(dir.filePath(QStringLiteral("cfg")));
  project.openFile(project_path);
  CHECK(project.path() == QDir::cleanPath(project_path));
  CHECK(project.fileCount() == 1);
}

TEST_CASE("RecentFiles: 20 entries, newest first, pinned first, persisted", "[fast][gui][project]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  {
    RecentFiles recent(dir.path());
    for (int i = 0; i < 25; ++i) {
      recent.add(QStringLiteral("/media/clip%1.mp4").arg(i), QStringLiteral("h264"));
    }
    REQUIRE(recent.entries().size() == RecentFiles::kCapacity);
    CHECK(recent.entries().constFirst().toMap().value(QStringLiteral("path")).toString() ==
          QStringLiteral("/media/clip24.mp4"));
    recent.add(QStringLiteral("/media/clip10.mp4"));  // moves to the front, keeps its codec
    CHECK(recent.entries().size() == RecentFiles::kCapacity);
    CHECK(recent.entries().constFirst().toMap().value(QStringLiteral("codec")).toString() == QStringLiteral("h264"));
    recent.setPinned(QStringLiteral("/media/clip6.mp4"), true);
    CHECK(recent.entries().constFirst().toMap().value(QStringLiteral("path")).toString() ==
          QStringLiteral("/media/clip6.mp4"));
    recent.remove(QStringLiteral("/media/clip24.mp4"));
    CHECK(recent.entries().size() == RecentFiles::kCapacity - 1);
  }
  const RecentFiles again(dir.path());
  CHECK(again.entries().size() == RecentFiles::kCapacity - 1);
  CHECK(again.entries().constFirst().toMap().value(QStringLiteral("pinned")).toBool());
}

TEST_CASE("RawFormat: frame sizes per chroma format and bit depth", "[fast][gui][project]") {
  RawFormat f;
  f.width = 64;
  f.height = 32;
  CHECK(f.frameSize() == 64ULL * 32ULL * 3 / 2);  // 4:2:0, 8-bit
  f.chroma_format = QStringLiteral("422");
  CHECK(f.frameSize() == 64ULL * 32ULL * 2);
  f.chroma_format = QStringLiteral("444");
  f.bit_depth = 10;
  CHECK(f.frameSize() == 64ULL * 32ULL * 3 * 2);
  f.chroma_format = QStringLiteral("400");
  CHECK(f.frameSize() == 64ULL * 32ULL * 2);
  f.chroma_format = QStringLiteral("420");
  f.width = 63;  // odd sizes round the chroma up
  f.height = 31;
  f.bit_depth = 8;
  CHECK(f.frameSize() == 63U * 31U + 2U * 32U * 16U);
  f.chroma_format = QStringLiteral("411");
  CHECK(f.frameSize() == 0);
  RawFormat small;
  small.width = 8;
  small.height = 4;
  CHECK(RawFormat::fromJson(small.toJson()) == small);
}

TEST_CASE("RawFormatModel: AT3 the preview follows the format within 200 ms; a mismatch gives GUI-RAW-0001",
          "[fast][gui][project]") {
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  // Two 64x32 4:2:0 8-bit frames: luma ramps with x, chroma 128.
  QByteArray bytes;
  for (int frame = 0; frame < 2; ++frame) {
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 64; ++x) {
        bytes.append(static_cast<char>(x * 4));
      }
    }
    bytes.append(QByteArray(64 * 32 / 2, static_cast<char>(128)));
  }
  const QString path = dir.filePath(QStringLiteral("clip.yuv"));
  touch(path, bytes);

  RawFormatModel model;
  model.setPath(path);  // default 1920x1080: far too big for the file
  CHECK(model.diagnosticCode() == QStringLiteral("GUI-RAW-0001"));
  CHECK(model.diagnosticMessage().contains(QStringLiteral("does not match the file size")));
  CHECK(model.preview().isNull());

  QElapsedTimer clock;
  clock.start();
  model.setWidth(64);
  model.setHeight(32);
  CHECK(clock.elapsed() < 200);
  CHECK(model.diagnosticCode().isEmpty());
  CHECK(model.frameCount() == 2);
  REQUIRE_FALSE(model.preview().isNull());
  CHECK(model.preview().size() == QSize(64, 32));
  CHECK(qGray(model.preview().pixel(10, 5)) == 40);

  const QString first_source = model.previewSource();
  model.setBitDepth(10);  // 2 bytes per sample: one 6144-byte frame fits the 6144-byte file exactly
  CHECK(model.previewSource() != first_source);
  CHECK(model.diagnosticCode().isEmpty());
  CHECK(model.frameCount() == 1);
  model.setBitDepth(8);
  model.setHeight(31);  // 64x31 frames are 2976 bytes: the file has 6144
  CHECK(model.diagnosticCode() == QStringLiteral("GUI-RAW-0001"));
  CHECK(model.diagnosticMessage().contains(QStringLiteral("6144")));
}
