// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <cstdio>

#include "platform/platform.h"
#include "settings.h"

namespace ovt::app {

namespace {

constexpr int kProjectVersion = 1;
constexpr std::uint64_t kMaxPreviewPixels = 64ULL * 1024 * 1024;

QString key(const char* name) {
  return QString::fromLatin1(name);
}

bool is_uri(const QString& path) {
  return path.startsWith(QLatin1String("content://"));
}

QJsonObject load_schema(QString* error) {
  QFile file(QStringLiteral(":/qt/qml/OvtStudio/ovtproj.schema.json"));
  if (!file.open(QIODevice::ReadOnly)) {
    *error = QStringLiteral("the bundled ovtproj schema is missing");
    return {};
  }
  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (!doc.isObject()) {
    *error = QStringLiteral("the bundled ovtproj schema is not JSON: %1").arg(parse_error.errorString());
    return {};
  }
  return doc.object();
}

bool write_atomic(const QString& path, const QByteArray& bytes) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(bytes);
  return file.commit();
}

QString iso(const QDateTime& when) {
  return when.toUTC().toString(Qt::ISODateWithMs);
}

void to_stderr(const QString& line) {
  static_cast<void>(std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData()));
}

// Chroma samples per luma sample as numerator / 2: 4:0:0 -> 0, 4:2:0 -> 1, 4:2:2 -> 2, 4:4:4 -> 4.
int chroma_halves(const QString& chroma) {
  if (chroma == QLatin1String("400")) {
    return 0;
  }
  if (chroma == QLatin1String("420")) {
    return 1;
  }
  if (chroma == QLatin1String("422")) {
    return 2;
  }
  if (chroma == QLatin1String("444")) {
    return 4;
  }
  return -1;
}

QMutex g_preview_mutex;
QImage g_preview_image;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): guarded by g_preview_mutex

}  // namespace

QString roleName(FileRole role) {
  switch (role) {
    case FileRole::Single:
      return QStringLiteral("single");
    case FileRole::Reference:
      return QStringLiteral("reference");
    case FileRole::Distorted:
      return QStringLiteral("distorted");
  }
  return QStringLiteral("single");
}

std::optional<FileRole> roleFromName(const QString& name) {
  if (name == QLatin1String("single")) {
    return FileRole::Single;
  }
  if (name == QLatin1String("reference")) {
    return FileRole::Reference;
  }
  if (name == QLatin1String("distorted")) {
    return FileRole::Distorted;
  }
  return std::nullopt;
}

// ---- RawFormat
// ---------------------------------------------------------------------------------------------------------

std::uint64_t RawFormat::frameSize() const {
  const int halves = chroma_halves(chroma_format);
  if (width <= 0 || height <= 0 || halves < 0 || bit_depth < 8 || bit_depth > 16) {
    return 0;
  }
  const std::uint64_t luma = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  const std::uint64_t bytes_per_sample = bit_depth > 8 ? 2 : 1;
  // Chroma planes are subsampled by rounding up (odd sizes).
  std::uint64_t chroma = 0;
  if (halves > 0) {
    const auto cw = halves == 4 ? static_cast<std::uint64_t>(width) : (static_cast<std::uint64_t>(width) + 1) / 2;
    const auto ch = halves == 1 ? (static_cast<std::uint64_t>(height) + 1) / 2 : static_cast<std::uint64_t>(height);
    chroma = 2 * cw * ch;
  }
  return (luma + chroma) * bytes_per_sample;
}

QJsonObject RawFormat::toJson() const {
  QJsonObject object = color.isEmpty() ? QJsonObject{} : QJsonObject{{key("color"), color}};
  object.insert(key("width"), width);
  object.insert(key("height"), height);
  object.insert(key("chroma_format"), chroma_format);
  object.insert(key("bit_depth"), bit_depth);
  object.insert(key("fps"), fps);
  return object;
}

RawFormat RawFormat::fromJson(const QJsonObject& object) {
  RawFormat format;
  format.width = object.value(key("width")).toInt(0);
  format.height = object.value(key("height")).toInt(0);
  format.chroma_format = object.value(key("chroma_format")).toString(QStringLiteral("420"));
  format.bit_depth = object.value(key("bit_depth")).toInt(8);
  format.fps = object.value(key("fps")).toDouble(25.0);
  format.color = object.value(key("color")).toObject();
  return format;
}

// ---- Project
// -----------------------------------------------------------------------------------------------------------

Project::Project() : Project(Platform::configDir()) {}

Project::Project(const QString& config_dir, QObject* parent)
    : QObject(parent), config_dir_(config_dir), created_(QDateTime::currentDateTimeUtc()) {
  autosave_timer_.setInterval(kDefaultAutosaveMs);
  connect(&autosave_timer_, &QTimer::timeout, this, [this] {
    if (dirty_) {
      autosaveNow();
    }
  });
  autosave_timer_.start();
}

QString Project::name() const {
  return path_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(path_).completeBaseName();
}

const ProjectFile* Project::file(const QString& id) const {
  const auto it = std::ranges::find(files_, id, &ProjectFile::id);
  return it == files_.end() ? nullptr : &*it;
}

QString Project::diagnosticCode() const {
  return diagnostics_.isEmpty() ? QString() : diagnostics_.constLast().code;
}

QString Project::diagnosticMessage() const {
  return diagnostics_.isEmpty() ? QString() : diagnostics_.constLast().message;
}

void Project::raise(const QString& code, const QString& severity, const QString& message) {
  diagnostics_.push_back({code, severity, message});
  // No logger in the GUI yet (WI-1.512); the Diagnostics panel is WI-1.453.
  to_stderr(code + QStringLiteral(": ") + message);
  Q_EMIT diagnosticsChanged();
}

void Project::setDirty(bool dirty) {
  if (dirty_ != dirty) {
    dirty_ = dirty;
    Q_EMIT dirtyChanged();
  }
}

QString Project::normalise(const QString& path_or_url) {
  if (is_uri(path_or_url)) {
    return path_or_url;  // Android: stored verbatim
  }
  const QUrl url(path_or_url);
  if (url.isLocalFile()) {
    return QDir::cleanPath(url.toLocalFile());
  }
  return QDir::cleanPath(QFileInfo(path_or_url).absoluteFilePath());
}

QVariantList Project::fileList() const {
  QVariantList list;
  for (const ProjectFile& f : files_) {
    list.push_back(QVariantMap{{key("id"), f.id},
                               {key("path"), f.path},
                               {key("name"), is_uri(f.path) ? f.path : QFileInfo(f.path).fileName()},
                               {key("role"), roleName(f.role)},
                               {key("trackIndex"), f.track_index},
                               {key("errorCode"), f.error_code},
                               {key("errorMessage"), f.error_message},
                               {key("raw"), f.raw_format.has_value()}});
  }
  return list;
}

void Project::setActiveFile(const QString& id) {
  if (id != active_file_ && file(id) != nullptr) {
    active_file_ = id;
    setDirty(true);
    Q_EMIT filesChanged();
  }
}

QString Project::addFile(const QString& path, FileRole role, int track_index, std::optional<RawFormat> raw) {
  ProjectFile entry;
  entry.id = QStringLiteral("f%1").arg(next_id_++);
  entry.path = normalise(path);
  entry.role = role;
  entry.track_index = std::max(0, track_index);
  entry.raw_format = std::move(raw);
  if (!is_uri(entry.path) && !QFileInfo::exists(entry.path)) {
    entry.error_code = QStringLiteral("GUI-PROJ-0003");
    entry.error_message = QStringLiteral("file not found: %1").arg(entry.path);
    raise(entry.error_code, QStringLiteral("Error"), entry.error_message);
  }
  const QString id = entry.id;
  files_.push_back(std::move(entry));
  if (active_file_.isEmpty()) {
    active_file_ = id;
  }
  setDirty(true);
  Q_EMIT filesChanged();
  return id;
}

bool Project::removeFile(const QString& id) {
  const auto it = std::ranges::find(files_, id, &ProjectFile::id);
  if (it == files_.end()) {
    return false;
  }
  files_.erase(it);
  if (active_file_ == id) {
    active_file_ = files_.isEmpty() ? QString() : files_.constFirst().id;
  }
  setDirty(true);
  Q_EMIT filesChanged();
  return true;
}

bool Project::setRole(const QString& id, const QString& role) {
  const auto parsed = roleFromName(role);
  const auto it = std::ranges::find(files_, id, &ProjectFile::id);
  if (!parsed || it == files_.end()) {
    return false;
  }
  it->role = *parsed;
  setDirty(true);
  Q_EMIT filesChanged();
  return true;
}

bool Project::setTrackIndex(const QString& id, int track_index) {
  const auto it = std::ranges::find(files_, id, &ProjectFile::id);
  if (it == files_.end() || track_index < 0) {
    return false;
  }
  it->track_index = track_index;
  setDirty(true);
  Q_EMIT filesChanged();
  return true;
}

void Project::noteSourceHash(const QString& id, const QString& hex) {
  const auto it = std::ranges::find(files_, id, &ProjectFile::id);
  if (it == files_.end()) {
    return;
  }
  if (!it->source_hash.isEmpty() && it->source_hash != hex) {
    raise(QStringLiteral("GUI-PROJ-0001"), QStringLiteral("Warning"),
          QStringLiteral("file changed since project was saved (%1)").arg(it->path));
  }
  if (it->source_hash != hex) {
    it->source_hash = hex;
    setDirty(true);
  }
}

void Project::openFile(const QString& path_or_url) {
  const QString path = normalise(path_or_url);
  if (path.endsWith(QLatin1String(".ovtproj"), Qt::CaseInsensitive)) {
    load(path);
    return;
  }
  if (files_.isEmpty()) {
    Q_EMIT fileOpened(addFile(path_or_url, FileRole::Single));
  } else {
    Q_EMIT secondFileRequested(normalise(path_or_url));
  }
}

void Project::openReference(const QString& path_or_url) {
  if (files_.isEmpty()) {
    openFile(path_or_url);
    return;
  }
  openAs(path_or_url, Reference);
}

// SPEC-ASSUMPTION(#243): §11.8 names "Reference (B)" and "Distorted (A), current becomes reference". Reference (B)
// makes the new file the reference and the open one the distorted file; Distorted (A) is the mirror case.
void Project::openAs(const QString& path_or_url, int choice) {
  if (files_.isEmpty()) {
    openFile(path_or_url);
    return;
  }
  if (choice == Replace) {
    files_.clear();
    active_file_.clear();
    Q_EMIT fileOpened(addFile(path_or_url, FileRole::Single));
    return;
  }
  const bool new_is_reference = choice == Reference;
  for (ProjectFile& f : files_) {
    if (f.id == active_file_) {
      f.role = new_is_reference ? FileRole::Distorted : FileRole::Reference;
    }
  }
  Q_EMIT fileOpened(addFile(path_or_url, new_is_reference ? FileRole::Reference : FileRole::Distorted));
}

void Project::addRaw(const QString& path, int width, int height, const QString& chroma, int bit_depth, double fps) {
  RawFormat format;
  format.width = width;
  format.height = height;
  format.chroma_format = chroma;
  format.bit_depth = bit_depth;
  format.fps = fps;
  Q_EMIT fileOpened(addFile(path, files_.isEmpty() ? FileRole::Single : FileRole::Distorted, 0, format));
}

bool Project::launchNewWindow(const QString& path_or_url) {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
  Q_UNUSED(path_or_url)
  return false;
#else
  return QProcess::startDetached(QCoreApplication::applicationFilePath(), {normalise(path_or_url)});
#endif
}

void Project::closeAll() {
  files_.clear();
  active_file_.clear();
  path_.clear();
  sections_ = {};
  setDirty(false);
  Q_EMIT pathChanged();
  Q_EMIT filesChanged();
}

QJsonValue Project::section(const QString& name) const {
  return sections_.value(name);
}

void Project::setSection(const QString& name, const QJsonValue& value) {
  if (sections_.value(name) == value) {
    return;
  }
  sections_.insert(name, value);
  setDirty(true);
}

QJsonObject Project::document(const QString& base_dir) const {
  QJsonObject doc = sections_;
  doc.insert(key("ovtproj_version"), kProjectVersion);
  doc.insert(key("created"), iso(created_));
  doc.insert(key("modified"), iso(QDateTime::currentDateTimeUtc()));
  doc.insert(key("app_version"), QCoreApplication::applicationVersion());
  QJsonArray files;
  const QDir base(base_dir);
  for (const ProjectFile& f : files_) {
    QString stored = f.path;
    // Relative only when the file lies under the project directory; content:// URIs verbatim.
    if (!is_uri(f.path) && !base_dir.isEmpty()) {
      const QString rel = base.relativeFilePath(f.path);
      if (!rel.startsWith(QLatin1String("../")) && rel != QLatin1String("..") && !QDir::isAbsolutePath(rel)) {
        stored = rel;
      }
    }
    QJsonObject entry{
        {key("id"), f.id}, {key("path"), stored}, {key("role"), roleName(f.role)}, {key("track_index"), f.track_index}};
    if (!f.source_hash.isEmpty()) {
      entry.insert(key("source_hash"), f.source_hash);
    }
    if (f.raw_format) {
      entry.insert(key("raw_format"), f.raw_format->toJson());
    }
    files.push_back(entry);
  }
  doc.insert(key("files"), files);
  if (!active_file_.isEmpty()) {
    doc.insert(key("active_file"), active_file_);
  }
  return doc;
}

bool Project::validate(const QJsonObject& document, QString* reason) {
  QString error;
  const QJsonObject schema = load_schema(&error);
  if (!error.isEmpty()) {
    if (reason != nullptr) {
      *reason = error;
    }
    return false;
  }
  return Settings::validateAgainst(document, schema, reason);
}

bool Project::write(const QString& path, const QString& base_dir) {
  const QByteArray bytes = QJsonDocument(document(base_dir)).toJson(QJsonDocument::Indented);
  return write_atomic(path, bytes);
}

bool Project::save() {
  return !path_.isEmpty() && saveAs(path_);
}

bool Project::saveAs(const QString& path) {
  const QString target = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  if (!write(target, QFileInfo(target).absolutePath())) {
    return false;
  }
  if (path_ != target) {
    // The autosave of the old (or unsaved) name is obsolete.
    QFile::remove(autosavePath());
    path_ = target;
    Q_EMIT pathChanged();
  }
  QFile::remove(autosavePath());
  setDirty(false);
  return true;
}

bool Project::readDocument(const QString& path, const QString& base_dir, bool mark_clean) {
  QFile input(path);
  QJsonParseError parse_error;
  QJsonDocument doc;
  if (input.open(QIODevice::ReadOnly)) {
    doc = QJsonDocument::fromJson(input.readAll(), &parse_error);
  } else {
    parse_error.error = QJsonParseError::IllegalValue;
    parse_error.offset = 0;
  }
  if (!doc.isObject()) {
    raise(QStringLiteral("GUI-PROJ-0002"), QStringLiteral("Error"),
          QStringLiteral("project file invalid: / %1")
              .arg(input.isOpen() ? parse_error.errorString() : input.errorString()));
    return false;
  }
  QString reason;
  if (!validate(doc.object(), &reason)) {
    // `reason` is "<json pointer>: <what is wrong>".
    const qsizetype colon = reason.indexOf(QLatin1Char(':'));
    raise(QStringLiteral("GUI-PROJ-0002"), QStringLiteral("Error"),
          QStringLiteral("project file invalid: %1 %2")
              .arg(reason.left(colon), colon < 0 ? QString() : reason.mid(colon + 2)));
    return false;
  }
  const QJsonObject obj = doc.object();
  files_.clear();
  next_id_ = 1;
  const QDir base(base_dir);
  for (const QJsonValue value : obj.value(key("files")).toArray()) {
    const QJsonObject entry = value.toObject();
    ProjectFile f;
    f.id = entry.value(key("id")).toString();
    const QString stored = entry.value(key("path")).toString();
    f.path = is_uri(stored) || QDir::isAbsolutePath(stored) ? stored : QDir::cleanPath(base.absoluteFilePath(stored));
    f.role = roleFromName(entry.value(key("role")).toString()).value_or(FileRole::Single);
    f.track_index = entry.value(key("track_index")).toInt(0);
    f.source_hash = entry.value(key("source_hash")).toString();
    if (entry.contains(key("raw_format"))) {
      f.raw_format = RawFormat::fromJson(entry.value(key("raw_format")).toObject());
    }
    if (!is_uri(f.path) && !QFileInfo::exists(f.path)) {
      f.error_code = QStringLiteral("GUI-PROJ-0003");
      f.error_message = QStringLiteral("file not found: %1").arg(f.path);
      raise(f.error_code, QStringLiteral("Error"), f.error_message);
    }
    next_id_ = std::max(next_id_, f.id.mid(1).toInt() + 1);
    files_.push_back(std::move(f));
  }
  active_file_ = obj.value(key("active_file")).toString();
  if (file(active_file_) == nullptr) {
    active_file_ = files_.isEmpty() ? QString() : files_.constFirst().id;
  }
  created_ = QDateTime::fromString(obj.value(key("created")).toString(), Qt::ISODate);
  if (!created_.isValid()) {
    created_ = QDateTime::currentDateTimeUtc();
  }
  sections_ = obj;  // unknown keys survive; the keys written by document() are replaced on save
  for (const char* owned : {"ovtproj_version", "files", "active_file", "created", "modified", "app_version"}) {
    sections_.remove(key(owned));
  }
  setDirty(!mark_clean);
  Q_EMIT filesChanged();
  return true;
}

bool Project::load(const QString& path) {
  const QString source = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  if (!readDocument(source, QFileInfo(source).absolutePath(), true)) {
    return false;
  }
  path_ = source;
  Q_EMIT pathChanged();
  return true;
}

QString Project::autosavePath() const {
  return (path_.isEmpty() ? QDir(config_dir_).filePath(QStringLiteral("untitled.ovtproj")) : path_) +
         QStringLiteral(".autosave");
}

bool Project::autosaveNow() {
  return write(autosavePath(), path_.isEmpty() ? config_dir_ : QFileInfo(path_).absolutePath());
}

bool Project::hasRecovery() const {
  return QFileInfo::exists(autosavePath());
}

bool Project::recover() {
  if (!hasRecovery()) {
    return false;
  }
  const QString base = path_.isEmpty() ? config_dir_ : QFileInfo(path_).absolutePath();
  return readDocument(autosavePath(), base, false);
}

void Project::setAutosaveIntervalMs(int ms) {
  autosave_timer_.setInterval(std::max(1, ms));
}

// ---- RecentFiles
// -------------------------------------------------------------------------------------------------------

RecentFiles::RecentFiles() : RecentFiles(Platform::configDir()) {}

RecentFiles::RecentFiles(const QString& config_dir, QObject* parent)
    : QObject(parent), file_(QDir::cleanPath(config_dir) + QStringLiteral("/recent_files.json")) {
  load();
}

QVariantList RecentFiles::entries() const {
  QVariantList list;
  for (const Entry& e : entries_) {
    list.push_back(QVariantMap{{key("path"), e.path},
                               {key("name"), is_uri(e.path) ? e.path : QFileInfo(e.path).fileName()},
                               {key("codec"), e.codec},
                               {key("lastOpened"), e.last_opened},
                               {key("pinned"), e.pinned}});
  }
  return list;
}

void RecentFiles::trim() {
  // Pinned entries first, then most recent first; the list never exceeds kCapacity.
  std::ranges::stable_sort(entries_, [](const Entry& a, const Entry& b) {
    return a.pinned != b.pinned ? a.pinned : a.last_opened > b.last_opened;
  });
  if (entries_.size() > kCapacity) {
    entries_.resize(kCapacity);
  }
}

void RecentFiles::add(const QString& path, const QString& codec) {
  const QString stored = is_uri(path) ? path : QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  bool pinned = false;
  QString known_codec = codec;
  const auto it = std::ranges::find(entries_, stored, &Entry::path);
  if (it != entries_.end()) {
    pinned = it->pinned;
    if (known_codec.isEmpty()) {
      known_codec = it->codec;
    }
    entries_.erase(it);
  }
  entries_.prepend({stored, known_codec, QDateTime::currentDateTimeUtc(), pinned});  // ties: newest first
  trim();
  save();
  Q_EMIT entriesChanged();
}

void RecentFiles::remove(const QString& path) {
  const auto it = std::ranges::find(entries_, path, &Entry::path);
  if (it != entries_.end()) {
    entries_.erase(it);
    save();
    Q_EMIT entriesChanged();
  }
}

void RecentFiles::setPinned(const QString& path, bool pinned) {
  const auto it = std::ranges::find(entries_, path, &Entry::path);
  if (it != entries_.end() && it->pinned != pinned) {
    it->pinned = pinned;
    trim();
    save();
    Q_EMIT entriesChanged();
  }
}

void RecentFiles::clear() {
  entries_.clear();
  save();
  Q_EMIT entriesChanged();
}

void RecentFiles::load() {
  QFile file(file_);
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  for (const QJsonValue value : doc.object().value(key("recent")).toArray()) {
    const QJsonObject o = value.toObject();
    if (o.value(key("path")).toString().isEmpty()) {
      continue;
    }
    entries_.push_back({o.value(key("path")).toString(), o.value(key("codec")).toString(),
                        QDateTime::fromString(o.value(key("last_opened")).toString(), Qt::ISODate),
                        o.value(key("pinned")).toBool()});
  }
  trim();
}

void RecentFiles::save() const {
  QJsonArray array;
  for (const Entry& e : entries_) {
    array.push_back(QJsonObject{{key("path"), e.path},
                                {key("codec"), e.codec},
                                {key("last_opened"), iso(e.last_opened)},
                                {key("pinned"), e.pinned}});
  }
  const QByteArray bytes = QJsonDocument(QJsonObject{{key("recent"), array}}).toJson(QJsonDocument::Indented);
  if (!write_atomic(file_, bytes)) {
    to_stderr(QStringLiteral("recent files: could not write %1").arg(file_));
  }
}

// ---- RawFormatModel
// ----------------------------------------------------------------------------------------------------

RawFormatModel::RawFormatModel(QObject* parent) : QObject(parent) {
  format_.width = 1920;
  format_.height = 1080;
}

void RawFormatModel::setPath(const QString& path) {
  path_ = path;
  update();
}

void RawFormatModel::setWidth(int width) {
  format_.width = width;
  update();
}

void RawFormatModel::setHeight(int height) {
  format_.height = height;
  update();
}

void RawFormatModel::setChromaFormat(const QString& chroma) {
  format_.chroma_format = chroma;
  update();
}

void RawFormatModel::setBitDepth(int bit_depth) {
  format_.bit_depth = bit_depth;
  update();
}

void RawFormatModel::setFps(double fps) {
  format_.fps = fps;
  update();
}

QString RawFormatModel::previewSource() const {
  return QStringLiteral("image://rawpreview/%1").arg(serial_);
}

// SPEC-ASSUMPTION(#243): "expected" in GUI-RAW-0001 is the size of the whole frames that fit the file (at least one
// frame); "actual" is the file size. A file whose size is not a multiple of the frame size raises the diagnostic.
QString RawFormatModel::checkSize(const RawFormat& format, std::uint64_t file_size, std::uint64_t* expected) {
  const std::uint64_t frame = format.frameSize();
  if (frame == 0) {
    *expected = 0;
    return QStringLiteral("GUI-RAW-0001");
  }
  const std::uint64_t whole = std::max<std::uint64_t>(1, file_size / frame);
  *expected = whole * frame;
  return file_size % frame == 0 && file_size > 0 ? QString() : QStringLiteral("GUI-RAW-0001");
}

QImage RawFormatModel::renderPreview(const QString& path, const RawFormat& format) {
  const std::uint64_t frame = format.frameSize();
  const auto pixels =
      static_cast<std::uint64_t>(std::max(0, format.width)) * static_cast<std::uint64_t>(std::max(0, format.height));
  if (frame == 0 || pixels == 0 || pixels > kMaxPreviewPixels) {
    return {};
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  const bool wide = format.bit_depth > 8;
  const QByteArray luma = file.read(static_cast<qint64>(pixels * (wide ? 2 : 1)));
  if (static_cast<std::uint64_t>(luma.size()) < pixels * (wide ? 2 : 1)) {
    return {};  // shorter than one luma plane
  }
  QImage image(format.width, format.height, QImage::Format_Grayscale8);
  const auto* src = reinterpret_cast<const unsigned char*>(luma.constData());
  const int shift = format.bit_depth - 8;
  for (int y = 0; y < format.height; ++y) {
    uchar* dst = image.scanLine(y);
    for (int x = 0; x < format.width; ++x) {
      const std::size_t i =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(format.width) + static_cast<std::size_t>(x);
      dst[x] = wide ? static_cast<uchar>(std::min(255, (src[2 * i] | (src[2 * i + 1] << 8)) >> shift)) : src[i];
    }
  }
  return image;
}

void RawFormatModel::update() {
  diagnostic_code_.clear();
  diagnostic_message_.clear();
  frame_count_ = 0;
  preview_ = {};
  if (!path_.isEmpty() && QFileInfo::exists(path_)) {
    const auto size = static_cast<std::uint64_t>(QFileInfo(path_).size());
    std::uint64_t expected = 0;
    diagnostic_code_ = checkSize(format_, size, &expected);
    if (!diagnostic_code_.isEmpty()) {
      diagnostic_message_ = QStringLiteral("raw format does not match the file size (%1 ≠ %2)").arg(expected).arg(size);
    }
    if (format_.frameSize() != 0) {
      frame_count_ = static_cast<int>(size / format_.frameSize());
    }
    preview_ = renderPreview(path_, format_);
  }
  ++serial_;
  RawPreviewProvider::publish(preview_);
  Q_EMIT changed();
}

QImage RawPreviewProvider::requestImage(const QString& /*id*/, QSize* size, const QSize& requested_size) {
  const QMutexLocker lock(&g_preview_mutex);
  QImage image = g_preview_image;
  if (image.isNull()) {
    image = QImage(1, 1, QImage::Format_Grayscale8);
    image.fill(0);
  }
  if (requested_size.isValid() && !requested_size.isEmpty()) {
    image = image.scaled(requested_size, Qt::KeepAspectRatio, Qt::FastTransformation);
  }
  if (size != nullptr) {
    *size = image.size();
  }
  return image;
}

void RawPreviewProvider::publish(const QImage& image) {
  const QMutexLocker lock(&g_preview_mutex);
  g_preview_image = image;
}

}  // namespace ovt::app
