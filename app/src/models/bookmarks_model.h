// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// BookmarksModel (chapter 11 §11.3.17; WI-2.487): named frame bookmarks and anchored comments, kept in the project's
// `bookmarks[]` and `comments[]` sections (§11.2.6). Anchors are navigated through the SelectionModel, so Syntax,
// Hex, Picture and Timeline follow. Bookmarks refer to frames by `decode_index`, which is stable across order toggles.
//
// SPEC-ASSUMPTION(#421): (1) `N` / `Shift+N` visit the bookmarks of the active file in ascending decode_index order
// and wrap; (2) a `field` anchor also stores `bit_offset` / `bit_length` (extra keys, allowed by ovtproj.schema.json)
// so that selecting it can mark the bytes in Hex; (3) the schema has no display index, type or time for a bookmark, so
// the list shows the decode index only until the index lookups of WI-2.480 exist; (4) a `box` anchor has no
// SelectionModel setter yet, so navigating to one emits boxRequested() for the Container panel to follow.

#ifndef OVT_APP_MODELS_BOOKMARKS_MODEL_H
#define OVT_APP_MODELS_BOOKMARKS_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>

#include "models/selection_model.h"
#include "project.h"

namespace ovt::app {

// Rows over one project array (bookmarks or comments). The array is the storage; BookmarksModel is the only writer.
class BookmarkRows : public QAbstractListModel {
  Q_OBJECT

 public:
  enum Kind { Bookmarks, Comments };
  enum Role {
    IdRole = Qt::UserRole + 1,
    NameRole,         // bookmark name, or the comment's excerpt
    ColorRole,        // bookmarks
    DecodeIndexRole,  // bookmarks; the anchor frame of a comment (-1: none)
    AnchorKindRole,   // comments: "frame" | "block" | "bytes" | "field" | "box"
    AuthorRole,
    CreatedRole,
    TextRole,
    FileRole,
  };
  Q_ENUM(Role)

  BookmarkRows(Kind kind, QObject* parent);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  [[nodiscard]] const QJsonArray& items() const { return items_; }
  void reset(QJsonArray items);

 private:
  Kind kind_;
  QJsonArray items_;
};

class BookmarksModel : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(Bookmarks)
  QML_SINGLETON
  Q_PROPERTY(QObject* bookmarks READ bookmarks CONSTANT)
  Q_PROPERTY(QObject* comments READ comments CONSTANT)
  // Timeline markers of the active file: [{decodeIndex, color, name}], in decode order.
  Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
  Q_PROPERTY(int bookmarkCount READ bookmark_count NOTIFY changed)
  Q_PROPERTY(int commentCount READ comment_count NOTIFY changed)

 public:
  static constexpr const char* kOrigin = "bookmarks";

  BookmarksModel();
  explicit BookmarksModel(QObject* parent);
  ~BookmarksModel() override;
  BookmarksModel(const BookmarksModel&) = delete;
  BookmarksModel& operator=(const BookmarksModel&) = delete;
  BookmarksModel(BookmarksModel&&) = delete;
  BookmarksModel& operator=(BookmarksModel&&) = delete;

  // Binds the project that stores the arrays and the selection that anchors navigate. Reloads from the project.
  Q_INVOKABLE void attach(Project* project, SelectionModel* selection);
  // Re-reads both arrays from the project (after a load or close).
  Q_INVOKABLE void reload();

  [[nodiscard]] QObject* bookmarks() const;
  [[nodiscard]] QObject* comments() const;
  [[nodiscard]] QVariantList markers() const;
  [[nodiscard]] int bookmark_count() const {
    return bookmarks_ == nullptr ? 0 : static_cast<int>(bookmark_items().size());
  }
  [[nodiscard]] int comment_count() const {
    return comments_ == nullptr ? 0 : static_cast<int>(comment_items().size());
  }

  // `B`: bookmarks the selected frame. Returns the new id, or "" when no frame is selected. A frame that is already
  // bookmarked is not duplicated; its id is returned.
  Q_INVOKABLE QString addBookmark();
  // `Shift+B`: a comment anchored to the current selection (field, else bytes, else block rectangle, else frame).
  // "" when nothing is selected or the text is empty.
  Q_INVOKABLE QString addComment(const QString& text);
  Q_INVOKABLE QString addBoxComment(const QString& box_path, const QString& text);
  Q_INVOKABLE bool removeBookmark(const QString& id);
  Q_INVOKABLE bool removeComment(const QString& id);
  Q_INVOKABLE bool renameBookmark(const QString& id, const QString& name);
  Q_INVOKABLE bool setBookmarkColor(const QString& id, const QString& color);
  // Drag to reorder: moves the row `from` so that it ends up at `to`.
  Q_INVOKABLE bool moveBookmark(int from, int to);

  // `Shift+B`: asks the panel to open its inline comment editor (the text is typed there, then addComment()).
  Q_INVOKABLE void requestComment() { Q_EMIT commentRequested(); }

  // `N` / `Shift+N`. False when the active file has no bookmark.
  Q_INVOKABLE bool nextBookmark();
  Q_INVOKABLE bool previousBookmark();
  Q_INVOKABLE bool goToBookmark(const QString& id);
  Q_INVOKABLE bool goToComment(const QString& id);

  // Import / export (JSON: {"bookmarks": [...], "comments": [...]}; CSV: bookmarks, then a blank line, then comments).
  // Import merges: an entry whose id exists already gets a fresh id. Returns the number of entries added, -1 on error.
  Q_INVOKABLE [[nodiscard]] bool exportJson(const QString& path) const;
  Q_INVOKABLE [[nodiscard]] bool exportCsv(const QString& path) const;
  Q_INVOKABLE int importJson(const QString& path);

 Q_SIGNALS:
  void changed();
  void markersChanged();
  void commentRequested();
  // Navigating to a comment anchored to a container box: the Container panel selects `path`.
  void boxRequested(const QString& path);

 private:
  [[nodiscard]] const QJsonArray& bookmark_items() const;
  [[nodiscard]] const QJsonArray& comment_items() const;
  [[nodiscard]] QString current_file() const;
  [[nodiscard]] QString next_id(const QString& prefix) const;
  void store();  // writes both arrays back to the project and signals
  bool step(int direction);
  bool navigate(const QJsonObject& anchor);
  QString add_comment(const QString& text, const QJsonObject& anchor);

  BookmarkRows* bookmarks_ = nullptr;
  BookmarkRows* comments_ = nullptr;
  QPointer<Project> project_;
  QPointer<SelectionModel> selection_;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_BOOKMARKS_MODEL_H
