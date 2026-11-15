// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/bookmarks_model.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRect>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace ovt::app {

namespace {

const QString kBookmarks = QStringLiteral("bookmarks");
const QString kComments = QStringLiteral("comments");
constexpr int kExcerptChars = 60;

// A fixed palette so that new bookmarks are told apart on the timeline.
constexpr std::array kPalette{"#e5484d", "#f5a623", "#46a758", "#3e9bff", "#a07cf0", "#e54d9e"};

QString iso_now() {
  return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QString excerpt_of(const QString& text) {
  QString line = text.section(QLatin1Char('\n'), 0, 0).simplified();
  if (line.size() > kExcerptChars) {
    line = line.left(kExcerptChars - 1) + QStringLiteral("…");
  }
  return line;
}

QString anchor_kind(const QJsonObject& anchor) {
  for (const QString& kind : {QStringLiteral("field"), QStringLiteral("bytes"), QStringLiteral("block"),
                              QStringLiteral("frame"), QStringLiteral("box")}) {
    if (anchor.value(kind).isObject()) {
      return kind;
    }
  }
  return {};
}

int anchor_decode_index(const QJsonObject& anchor) {
  for (const QString& kind : {QStringLiteral("block"), QStringLiteral("frame")}) {
    if (anchor.value(kind).isObject()) {
      return anchor.value(kind).toObject().value(QStringLiteral("decode_index")).toInt(-1);
    }
  }
  return -1;
}

int index_of(const QJsonArray& items, const QString& id) {
  for (int i = 0; i < items.size(); ++i) {
    if (items.at(i).toObject().value(QStringLiteral("id")).toString() == id) {
      return i;
    }
  }
  return -1;
}

QString csv_cell(const QString& value) {
  if (!value.contains(QLatin1Char(',')) && !value.contains(QLatin1Char('"')) && !value.contains(QLatin1Char('\n'))) {
    return value;
  }
  QString quoted = value;
  quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
  return QLatin1Char('"') + quoted + QLatin1Char('"');
}

QString csv_row(const QStringList& cells) {
  QStringList out;
  for (const QString& cell : cells) {
    out.append(csv_cell(cell));
  }
  return out.join(QLatin1Char(',')) + QLatin1Char('\n');
}

bool write_file(const QString& path, const QByteArray& bytes) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(bytes);
  return file.commit();
}

}  // namespace

// ---- BookmarkRows ---------------------------------------------------------------------------------------------

BookmarkRows::BookmarkRows(Kind kind, QObject* parent) : QAbstractListModel(parent), kind_(kind) {}

int BookmarkRows::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

QVariant BookmarkRows::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) {
    return {};
  }
  const QJsonObject item = items_.at(index.row()).toObject();
  const QJsonObject anchor = item.value(QStringLiteral("anchor")).toObject();
  switch (role) {
    case IdRole:
      return item.value(QStringLiteral("id")).toString();
    case NameRole:
      return kind_ == Bookmarks ? item.value(QStringLiteral("name")).toString()
                                : excerpt_of(item.value(QStringLiteral("text")).toString());
    case ColorRole:
      return item.value(QStringLiteral("color")).toString(QStringLiteral("#3e9bff"));
    case DecodeIndexRole:
      return kind_ == Bookmarks ? item.value(QStringLiteral("decode_index")).toInt(-1) : anchor_decode_index(anchor);
    case AnchorKindRole:
      return anchor_kind(anchor);
    case AuthorRole:
      return item.value(QStringLiteral("author")).toString();
    case CreatedRole:
      return item.value(QStringLiteral("created")).toString();
    case TextRole:
      return item.value(QStringLiteral("text")).toString();
    case FileRole:
      return item.value(QStringLiteral("file")).toString();
    default:
      return {};
  }
}

QHash<int, QByteArray> BookmarkRows::roleNames() const {
  return {{IdRole, "id"},
          {NameRole, "name"},
          {ColorRole, "color"},
          {DecodeIndexRole, "decodeIndex"},
          {AnchorKindRole, "anchorKind"},
          {AuthorRole, "author"},
          {CreatedRole, "created"},
          {TextRole, "text"},
          {FileRole, "file"}};
}

void BookmarkRows::reset(QJsonArray items) {
  beginResetModel();
  items_ = std::move(items);
  endResetModel();
}

// ---- BookmarksModel -------------------------------------------------------------------------------------------

BookmarksModel::BookmarksModel() : BookmarksModel(nullptr) {}

BookmarksModel::BookmarksModel(QObject* parent)
    : QObject(parent),
      bookmarks_(new BookmarkRows(BookmarkRows::Bookmarks, this)),
      comments_(new BookmarkRows(BookmarkRows::Comments, this)) {}

BookmarksModel::~BookmarksModel() = default;

QObject* BookmarksModel::bookmarks() const {
  return bookmarks_;
}

QObject* BookmarksModel::comments() const {
  return comments_;
}

const QJsonArray& BookmarksModel::bookmark_items() const {
  return bookmarks_->items();
}

const QJsonArray& BookmarksModel::comment_items() const {
  return comments_->items();
}

void BookmarksModel::attach(Project* project, SelectionModel* selection) {
  if (project_ != nullptr) {
    project_->disconnect(this);
  }
  project_ = project;
  selection_ = selection;
  if (project_ != nullptr) {
    connect(project_, &Project::filesChanged, this, &BookmarksModel::reload);
  }
  reload();
}

void BookmarksModel::reload() {
  QJsonArray marks;
  QJsonArray notes;
  if (project_ != nullptr) {
    marks = project_->section(kBookmarks).toArray();
    notes = project_->section(kComments).toArray();
  }
  bookmarks_->reset(std::move(marks));
  comments_->reset(std::move(notes));
  Q_EMIT changed();
  Q_EMIT markersChanged();
}

QString BookmarksModel::current_file() const {
  if (selection_ != nullptr && !selection_->file_id().isEmpty()) {
    return selection_->file_id();
  }
  return project_ != nullptr ? project_->activeFile() : QString();
}

QString BookmarksModel::next_id(const QString& prefix) const {
  const QJsonArray& items = prefix == QLatin1String("b") ? bookmark_items() : comment_items();
  int highest = 0;
  for (const QJsonValue value : items) {
    const QString id = value.toObject().value(QStringLiteral("id")).toString();
    if (id.startsWith(prefix)) {
      highest = std::max(highest, id.mid(prefix.size()).toInt());
    }
  }
  return prefix + QString::number(highest + 1);
}

void BookmarksModel::store() {
  if (project_ != nullptr) {
    project_->setSection(kBookmarks, bookmark_items());
    project_->setSection(kComments, comment_items());
  }
  Q_EMIT changed();
  Q_EMIT markersChanged();
}

QVariantList BookmarksModel::markers() const {
  struct Marker {
    int decode_index;
    QString color;
    QString name;
  };
  std::vector<Marker> found;
  const QString file = current_file();
  for (const QJsonValue value : bookmark_items()) {
    const QJsonObject item = value.toObject();
    if (item.value(QStringLiteral("file")).toString() == file) {
      found.push_back({item.value(QStringLiteral("decode_index")).toInt(-1),
                       item.value(QStringLiteral("color")).toString(QStringLiteral("#3e9bff")),
                       item.value(QStringLiteral("name")).toString()});
    }
  }
  std::ranges::stable_sort(found, {}, &Marker::decode_index);
  QVariantList out;
  for (const Marker& marker : found) {
    out.append(QVariantMap{{QStringLiteral("decodeIndex"), marker.decode_index},
                           {QStringLiteral("color"), marker.color},
                           {QStringLiteral("name"), marker.name}});
  }
  return out;
}

QString BookmarksModel::addBookmark() {
  if (selection_ == nullptr || selection_->frame_decode_index() < 0) {
    return {};
  }
  const int decode = selection_->frame_decode_index();
  const QString file = current_file();
  QJsonArray items = bookmark_items();
  for (const QJsonValue value : items) {
    const QJsonObject item = value.toObject();
    if (item.value(QStringLiteral("file")).toString() == file &&
        item.value(QStringLiteral("decode_index")).toInt(-1) == decode) {
      return item.value(QStringLiteral("id")).toString();
    }
  }
  const QString id = next_id(QStringLiteral("b"));
  const int display = selection_->frame_display_index();
  QJsonObject item;
  item.insert(QStringLiteral("id"), id);
  item.insert(QStringLiteral("file"), file);
  item.insert(QStringLiteral("decode_index"), decode);
  item.insert(QStringLiteral("name"), tr("Frame %1").arg(display >= 0 ? display : decode));
  item.insert(QStringLiteral("color"),
              QString::fromLatin1(kPalette.at(static_cast<std::size_t>(items.size()) % kPalette.size())));
  item.insert(QStringLiteral("created"), iso_now());
  items.append(item);
  bookmarks_->reset(std::move(items));
  store();
  return id;
}

QString BookmarksModel::add_comment(const QString& text, const QJsonObject& anchor) {
  if (text.trimmed().isEmpty() || anchor.isEmpty()) {
    return {};
  }
  const QString id = next_id(QStringLiteral("c"));
  QJsonObject item;
  item.insert(QStringLiteral("id"), id);
  item.insert(QStringLiteral("file"), current_file());
  item.insert(QStringLiteral("text"), text);
  item.insert(QStringLiteral("created"), iso_now());
  item.insert(QStringLiteral("anchor"), anchor);
  QJsonArray items = comment_items();
  items.append(item);
  comments_->reset(std::move(items));
  store();
  return id;
}

QString BookmarksModel::addComment(const QString& text) {
  if (selection_ == nullptr) {
    return {};
  }
  QJsonObject anchor;
  if (!selection_->field_path().isEmpty() && selection_->unit_index() >= 0) {
    anchor.insert(QStringLiteral("field"), QJsonObject{{QStringLiteral("unit_index"), selection_->unit_index()},
                                                       {QStringLiteral("path"), selection_->field_path()},
                                                       {QStringLiteral("bit_offset"), selection_->bit_offset()},
                                                       {QStringLiteral("bit_length"), selection_->bit_length()}});
  } else if (selection_->byte_length() > 0) {
    anchor.insert(QStringLiteral("bytes"), QJsonObject{{QStringLiteral("offset"), selection_->byte_offset()},
                                                       {QStringLiteral("length"), selection_->byte_length()}});
  } else if (selection_->frame_decode_index() >= 0 && !selection_->selection_rect().isEmpty()) {
    const QRect rect = selection_->selection_rect();
    anchor.insert(QStringLiteral("block"),
                  QJsonObject{{QStringLiteral("decode_index"), selection_->frame_decode_index()},
                              {QStringLiteral("rect"), QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()}}});
  } else if (selection_->frame_decode_index() >= 0) {
    anchor.insert(QStringLiteral("frame"),
                  QJsonObject{{QStringLiteral("decode_index"), selection_->frame_decode_index()}});
  }
  return add_comment(text, anchor);
}

QString BookmarksModel::addBoxComment(const QString& box_path, const QString& text) {
  if (box_path.isEmpty()) {
    return {};
  }
  return add_comment(text, QJsonObject{{QStringLiteral("box"), QJsonObject{{QStringLiteral("path"), box_path}}}});
}

bool BookmarksModel::removeBookmark(const QString& id) {
  QJsonArray items = bookmark_items();
  const int row = index_of(items, id);
  if (row < 0) {
    return false;
  }
  items.removeAt(row);
  bookmarks_->reset(std::move(items));
  store();
  return true;
}

bool BookmarksModel::removeComment(const QString& id) {
  QJsonArray items = comment_items();
  const int row = index_of(items, id);
  if (row < 0) {
    return false;
  }
  items.removeAt(row);
  comments_->reset(std::move(items));
  store();
  return true;
}

bool BookmarksModel::renameBookmark(const QString& id, const QString& name) {
  QJsonArray items = bookmark_items();
  const int row = index_of(items, id);
  if (row < 0) {
    return false;
  }
  QJsonObject item = items.at(row).toObject();
  item.insert(QStringLiteral("name"), name);
  items.replace(row, item);
  bookmarks_->reset(std::move(items));
  store();
  return true;
}

bool BookmarksModel::setBookmarkColor(const QString& id, const QString& color) {
  static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));  // the schema's pattern
  QJsonArray items = bookmark_items();
  const int row = index_of(items, id);
  if (row < 0 || !hex.match(color).hasMatch()) {
    return false;
  }
  QJsonObject item = items.at(row).toObject();
  item.insert(QStringLiteral("color"), color);
  items.replace(row, item);
  bookmarks_->reset(std::move(items));
  store();
  return true;
}

bool BookmarksModel::moveBookmark(int from, int to) {
  QJsonArray items = bookmark_items();
  if (from < 0 || from >= items.size() || to < 0 || to >= items.size()) {
    return false;
  }
  if (from != to) {
    const QJsonValue moved = items.at(from);
    items.removeAt(from);
    items.insert(to, moved);
    bookmarks_->reset(std::move(items));
    store();
  }
  return true;
}

bool BookmarksModel::step(int direction) {
  if (selection_ == nullptr) {
    return false;
  }
  std::vector<int> frames;
  const QString file = current_file();
  for (const QJsonValue value : bookmark_items()) {
    const QJsonObject item = value.toObject();
    if (item.value(QStringLiteral("file")).toString() == file) {
      frames.push_back(item.value(QStringLiteral("decode_index")).toInt(-1));
    }
  }
  if (frames.empty()) {
    return false;
  }
  std::ranges::sort(frames);
  frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
  const int current = selection_->frame_decode_index();
  int target = 0;
  if (direction > 0) {
    const auto next = std::ranges::upper_bound(frames, current);
    target = next == frames.end() ? frames.front() : *next;
  } else {
    const auto next = std::ranges::lower_bound(frames, current);
    target = next == frames.begin() ? frames.back() : *(next - 1);
  }
  selection_->selectFrame(target, -1, QString::fromLatin1(kOrigin));
  return true;
}

bool BookmarksModel::nextBookmark() {
  return step(1);
}

bool BookmarksModel::previousBookmark() {
  return step(-1);
}

bool BookmarksModel::goToBookmark(const QString& id) {
  const QJsonArray& items = bookmark_items();
  const int row = index_of(items, id);
  if (row < 0 || selection_ == nullptr) {
    return false;
  }
  selection_->selectFrame(items.at(row).toObject().value(QStringLiteral("decode_index")).toInt(-1), -1,
                          QString::fromLatin1(kOrigin));
  return true;
}

bool BookmarksModel::goToComment(const QString& id) {
  const QJsonArray& items = comment_items();
  const int row = index_of(items, id);
  return row >= 0 && navigate(items.at(row).toObject().value(QStringLiteral("anchor")).toObject());
}

bool BookmarksModel::navigate(const QJsonObject& anchor) {
  if (selection_ == nullptr) {
    return false;
  }
  const QString origin = QString::fromLatin1(kOrigin);
  const QString kind = anchor_kind(anchor);
  const QJsonObject body = anchor.value(kind).toObject();
  if (kind == QLatin1String("frame")) {
    selection_->selectFrame(body.value(QStringLiteral("decode_index")).toInt(-1), -1, origin);
  } else if (kind == QLatin1String("block")) {
    selection_->selectFrame(body.value(QStringLiteral("decode_index")).toInt(-1), -1, origin);
    const QJsonArray rect = body.value(QStringLiteral("rect")).toArray();
    if (rect.size() == 4) {
      selection_->selectRect(QRect(rect.at(0).toInt(), rect.at(1).toInt(), rect.at(2).toInt(), rect.at(3).toInt()),
                             origin);
    }
  } else if (kind == QLatin1String("bytes")) {
    selection_->selectBytes(static_cast<qlonglong>(body.value(QStringLiteral("offset")).toDouble()),
                            static_cast<qlonglong>(body.value(QStringLiteral("length")).toDouble()), origin);
  } else if (kind == QLatin1String("field")) {
    selection_->selectField(body.value(QStringLiteral("unit_index")).toInt(-1),
                            body.value(QStringLiteral("path")).toString(),
                            static_cast<qlonglong>(body.value(QStringLiteral("bit_offset")).toDouble()),
                            body.value(QStringLiteral("bit_length")).toInt(0), origin);
  } else if (kind == QLatin1String("box")) {
    Q_EMIT boxRequested(body.value(QStringLiteral("path")).toString());
  } else {
    return false;
  }
  return true;
}

bool BookmarksModel::exportJson(const QString& path) const {
  const QJsonObject doc{{kBookmarks, bookmark_items()}, {kComments, comment_items()}};
  return write_file(path, QJsonDocument(doc).toJson(QJsonDocument::Indented));
}

bool BookmarksModel::exportCsv(const QString& path) const {
  QString out = csv_row({QStringLiteral("id"), QStringLiteral("file"), QStringLiteral("decode_index"),
                         QStringLiteral("name"), QStringLiteral("color"), QStringLiteral("created")});
  for (const QJsonValue value : bookmark_items()) {
    const QJsonObject item = value.toObject();
    out += csv_row({item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("file")).toString(),
                    QString::number(item.value(QStringLiteral("decode_index")).toInt(-1)),
                    item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("color")).toString(),
                    item.value(QStringLiteral("created")).toString()});
  }
  out += QLatin1Char('\n');
  out +=
      csv_row({QStringLiteral("id"), QStringLiteral("file"), QStringLiteral("anchor"), QStringLiteral("decode_index"),
               QStringLiteral("author"), QStringLiteral("created"), QStringLiteral("text")});
  for (const QJsonValue value : comment_items()) {
    const QJsonObject item = value.toObject();
    const QJsonObject anchor = item.value(QStringLiteral("anchor")).toObject();
    out += csv_row({item.value(QStringLiteral("id")).toString(), item.value(QStringLiteral("file")).toString(),
                    anchor_kind(anchor), QString::number(anchor_decode_index(anchor)),
                    item.value(QStringLiteral("author")).toString(), item.value(QStringLiteral("created")).toString(),
                    item.value(QStringLiteral("text")).toString()});
  }
  return write_file(path, out.toUtf8());
}

int BookmarksModel::importJson(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return -1;
  }
  QJsonParseError error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    return -1;
  }
  int added = 0;
  const auto merge = [&](const QString& section, BookmarkRows* rows, const QString& prefix,
                         const QStringList& required) {
    QJsonArray items = rows->items();
    for (const QJsonValue value : doc.object().value(section).toArray()) {
      QJsonObject item = value.toObject();
      const bool complete = std::ranges::all_of(required, [&item](const QString& key) { return item.contains(key); });
      if (!complete) {
        continue;
      }
      const QString id = item.value(QStringLiteral("id")).toString();
      if (id.isEmpty() || index_of(items, id) >= 0) {
        // A fresh id that is unique in this array, including entries added by this import.
        int highest = 0;
        for (const QJsonValue existing : items) {
          highest =
              std::max(highest, existing.toObject().value(QStringLiteral("id")).toString().mid(prefix.size()).toInt());
        }
        item.insert(QStringLiteral("id"), prefix + QString::number(highest + 1));
      }
      items.append(item);
      ++added;
    }
    rows->reset(std::move(items));
  };
  merge(kBookmarks, bookmarks_, QStringLiteral("b"), {QStringLiteral("file"), QStringLiteral("decode_index")});
  merge(kComments, comments_, QStringLiteral("c"),
        {QStringLiteral("file"), QStringLiteral("text"), QStringLiteral("created")});
  store();
  return added;
}

}  // namespace ovt::app
