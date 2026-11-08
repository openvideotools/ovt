// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The project model of chapter 11 §11.2.6 (WI-1.454): the files of a session with their roles, the `.ovtproj`
// JSON document (schema docs/spec/schemas/ovtproj.schema.json), relative paths, autosave and recovery, the recent
// files list (FR-GUI-027). The Raw format dialog model and the dialogs follow in part b of the work item.
//
// SPEC-ASSUMPTION(#243): ovt_container_open and ovt_stream_build_index (WI-1.217, WI-1.121) are not merged yet.
// Project therefore records the opened file and emits fileOpened(id); the container -> track choice -> index job
// chain attaches to that signal when the core calls land (`track_index` and `source_hash` are already stored).

#ifndef OVT_APP_PROJECT_H
#define OVT_APP_PROJECT_H

#include <QtQml/qqmlregistration.h>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

#include <cstdint>
#include <optional>

namespace ovt::app {

enum class FileRole : std::uint8_t { Single, Reference, Distorted };

[[nodiscard]] QString roleName(FileRole role);
[[nodiscard]] std::optional<FileRole> roleFromName(const QString& name);

// The "raw_format" object of a file entry (only for raw YUV).
struct RawFormat {
  int width = 0;
  int height = 0;
  QString chroma_format = QStringLiteral("420");  // "400", "420", "422" or "444"
  int bit_depth = 8;
  double fps = 25.0;
  QJsonObject color;  // H.273 codes, passed through

  // Bytes of one frame, 0 when the format is unusable.
  [[nodiscard]] std::uint64_t frameSize() const;
  [[nodiscard]] QJsonObject toJson() const;
  static RawFormat fromJson(const QJsonObject& object);
  friend bool operator==(const RawFormat&, const RawFormat&) = default;
};

struct ProjectFile {
  QString id;    // "f<number>"
  QString path;  // absolute file path or content:// URI in memory; relative on disk when possible
  FileRole role = FileRole::Single;
  int track_index = 0;
  QString source_hash;
  std::optional<RawFormat> raw_format;
  QString error_code;  // "GUI-PROJ-0003" when the file is missing, else empty
  QString error_message;
  friend bool operator==(const ProjectFile&, const ProjectFile&) = default;
};

struct ProjectDiagnostic {
  QString code;
  QString severity;  // "Warning" or "Error"
  QString message;
};

// What the "Open as" dialog of §11.8 decided for the second file.
enum class OpenAs : std::uint8_t { Replace, Reference, Distorted };

class Project : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(QString path READ path NOTIFY pathChanged)
  Q_PROPERTY(QString name READ name NOTIFY pathChanged)
  Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
  Q_PROPERTY(int fileCount READ fileCount NOTIFY filesChanged)
  Q_PROPERTY(QString activeFile READ activeFile WRITE setActiveFile NOTIFY filesChanged)
  Q_PROPERTY(QVariantList files READ fileList NOTIFY filesChanged)
  Q_PROPERTY(QString diagnosticCode READ diagnosticCode NOTIFY diagnosticsChanged)
  Q_PROPERTY(QString diagnosticMessage READ diagnosticMessage NOTIFY diagnosticsChanged)

 public:
  enum Choice : std::uint8_t { Replace, Reference, Distorted };
  Q_ENUM(Choice)

  // For QML: the directory of Platform::configDir() (autosave of an unsaved project lives there).
  Project();
  explicit Project(QString config_dir, QObject* parent = nullptr);

  [[nodiscard]] QString path() const { return path_; }
  [[nodiscard]] QString name() const;
  [[nodiscard]] bool dirty() const { return dirty_; }
  [[nodiscard]] int fileCount() const { return static_cast<int>(files_.size()); }
  [[nodiscard]] QString activeFile() const { return active_file_; }
  void setActiveFile(const QString& id);
  [[nodiscard]] QVariantList fileList() const;
  [[nodiscard]] const QVector<ProjectFile>& files() const { return files_; }
  [[nodiscard]] const ProjectFile* file(const QString& id) const;
  [[nodiscard]] const QVector<ProjectDiagnostic>& diagnostics() const { return diagnostics_; }
  [[nodiscard]] QString diagnosticCode() const;
  [[nodiscard]] QString diagnosticMessage() const;

  // Adds a file and returns its id (the first becomes active). `path` is a file path or a content:// URI.
  QString addFile(const QString& path, FileRole role, int track_index = 0, std::optional<RawFormat> raw = {});
  Q_INVOKABLE bool removeFile(const QString& id);
  Q_INVOKABLE bool setRole(const QString& id, const QString& role);
  Q_INVOKABLE bool setTrackIndex(const QString& id, int track_index);
  // Records the StreamIndex.source_hash of a file; a stored hash that differs raises GUI-PROJ-0001.
  Q_INVOKABLE void noteSourceHash(const QString& id, const QString& hex);

  // The entry points of the open flow. openFile opens the first file; with a file already open it emits
  // secondFileRequested (the Open-as dialog) instead. openAs applies the dialog's choice (New window is not a
  // project operation: the caller starts another process). openReference is Ctrl+Shift+O (compare slot B).
  Q_INVOKABLE void openFile(const QString& path_or_url);
  Q_INVOKABLE void openReference(const QString& path_or_url);
  Q_INVOKABLE void openAs(const QString& path_or_url, int choice);
  // Adds a raw YUV file with the format chosen in the Raw format dialog (first file: single; else distorted).
  Q_INVOKABLE void addRaw(const QString& path, int width, int height, const QString& chroma, int bit_depth, double fps);
  // "New window": starts another instance of this program on `path` (desktop only; false elsewhere).
  Q_INVOKABLE static bool launchNewWindow(const QString& path_or_url);
  Q_INVOKABLE void closeAll();

  // The sections the panels own: "layout", "panel_state", "view" (objects), "bookmarks", "comments",
  // "metrics_jobs" (arrays). Any other name is kept as an unknown key.
  [[nodiscard]] QJsonValue section(const QString& name) const;
  void setSection(const QString& name, const QJsonValue& value);

  // The document as it will be written. `base_dir` is the directory paths are made relative to.
  [[nodiscard]] QJsonObject document(const QString& base_dir) const;
  Q_INVOKABLE bool save();
  Q_INVOKABLE bool saveAs(const QString& path);
  // Loads a project: schema failure -> GUI-PROJ-0002 and false; missing files open in error state (GUI-PROJ-0003).
  Q_INVOKABLE bool load(const QString& path);
  // True when the document passes ovtproj.schema.json; `reason` names the JSON pointer.
  static bool validate(const QJsonObject& document, QString* reason);

  // Autosave (§11.2.6): every `autosaveIntervalMs` when dirty, to <project>.ovtproj.autosave (or
  // <config>/untitled.ovtproj.autosave for an unsaved project), removed on a successful save.
  [[nodiscard]] QString autosavePath() const;
  Q_INVOKABLE bool autosaveNow();
  [[nodiscard]] bool hasRecovery() const;
  Q_INVOKABLE bool recover();
  void setAutosaveIntervalMs(int ms);

  static constexpr int kDefaultAutosaveMs = 60'000;

 Q_SIGNALS:
  void pathChanged();
  void dirtyChanged();
  void filesChanged();
  void diagnosticsChanged();
  void fileOpened(const QString& id);
  void secondFileRequested(const QString& path);

 private:
  void setDirty(bool dirty);
  void raise(const QString& code, const QString& severity, const QString& message);
  bool readDocument(const QString& path, const QString& base_dir, bool mark_clean);
  [[nodiscard]] bool write(const QString& path, const QString& base_dir) const;
  static QString normalise(const QString& path_or_url);

  QString config_dir_;
  QString path_;
  QString active_file_;
  QVector<ProjectFile> files_;
  QJsonObject sections_;  // everything except files/active_file/ovtproj_version
  QDateTime created_;
  QVector<ProjectDiagnostic> diagnostics_;
  int next_id_ = 1;
  bool dirty_ = false;
  QTimer autosave_timer_;
};

// The 20-entry recent files list of FR-GUI-027, kept in <config>/recent_files.json (pinned entries first).
class RecentFiles : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)

 public:
  static constexpr int kCapacity = 20;
  RecentFiles();
  explicit RecentFiles(const QString& config_dir, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const;
  Q_INVOKABLE void add(const QString& path, const QString& codec = {});
  Q_INVOKABLE void remove(const QString& path);
  Q_INVOKABLE void setPinned(const QString& path, bool pinned);
  Q_INVOKABLE void clear();

 Q_SIGNALS:
  void entriesChanged();

 private:
  struct Entry {
    QString path;
    QString codec;
    QDateTime last_opened;
    bool pinned = false;
  };
  void load();
  void save() const;
  void trim();

  QString file_;
  QVector<Entry> entries_;
};

}  // namespace ovt::app

#endif  // OVT_APP_PROJECT_H
