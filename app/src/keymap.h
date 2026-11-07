// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The keyboard map of chapter 11 §11.4.3 (WI-1.466): the action registry with its default bindings (the §11.4.3 table
// is the only source of defaults), the user's overrides from keymap.json (§11.2.5) and conflict detection
// (GUI-KEY-0001). An action id is `action.<menu id>`, so the "Next frame" entry `navigate.next` of the Navigate menu is
// `action.navigate.next`. Keys are Qt portable text ("Ctrl+Shift+O", "Right", "F5").
//
// SPEC-ASSUMPTION(#222): ids derive from the menu ids (the spec example `action.frame.next` is illustrative), and a
// conflict is judged on the whole file and only within one scope.
//
// Scope: a `window` action is a window-level shortcut (the shortcut layer of the shell registers it); any other
// scope names the panel whose focus it needs (`picture`, `hex`, `list`, `focus`). §11.4.3 says a panel-local binding
// wins over a window one while that panel has focus, so two actions conflict only when they share a scope.

#ifndef OVT_APP_KEYMAP_H
#define OVT_APP_KEYMAP_H

#include <QtQml/qqmlregistration.h>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QtGlobal>

#include <vector>

namespace ovt::app {

struct ActionDef {
  QString id;
  QString text;  // the label of the Keyboard map dialog (spec wording, untranslated like the menus)
  QString scope;
  QStringList keys;  // default bindings
};

// All actions of §11.4.3 in table order.
const std::vector<ActionDef>& actionRegistry();

class Keymap : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(QString path READ path CONSTANT)
  Q_PROPERTY(QStringList actionIds READ actionIds CONSTANT)
  // The ids of the window-scope actions, for the shortcut layer.
  Q_PROPERTY(QStringList windowActionIds READ windowActionIds CONSTANT)
  // Incremented whenever a binding changes, so bindings that read `keys()` re-evaluate.
  Q_PROPERTY(int revision READ revision NOTIFY changed)
  Q_PROPERTY(QString diagnosticCode READ diagnosticCode NOTIFY changed)
  // One text per rejected entry of keymap.json (GUI-KEY-0001 ...), empty when the file loaded cleanly.
  Q_PROPERTY(QStringList diagnostics READ diagnostics NOTIFY changed)

 public:
  Keymap();  // keymap.json of Platform::configDir()
  explicit Keymap(const QString& path, QObject* parent = nullptr);

  [[nodiscard]] QStringList actionIds() const;
  [[nodiscard]] QStringList windowActionIds() const;
  [[nodiscard]] int revision() const { return revision_; }
  [[nodiscard]] QString diagnosticCode() const { return diagnostics_.isEmpty() ? QString() : code_; }
  [[nodiscard]] QStringList diagnostics() const { return diagnostics_; }
  [[nodiscard]] QString path() const { return path_; }

  Q_INVOKABLE [[nodiscard]] bool has(const QString& id) const;
  Q_INVOKABLE [[nodiscard]] QString text(const QString& id) const;
  Q_INVOKABLE [[nodiscard]] QString scope(const QString& id) const;
  // The current bindings of an action ("" is never in the list); empty for an unknown id.
  Q_INVOKABLE [[nodiscard]] QStringList keys(const QString& id) const;
  Q_INVOKABLE [[nodiscard]] QStringList defaultKeys(const QString& id) const;
  // "Ctrl+O" or "Right / L", for menus and the Keyboard map dialog.
  Q_INVOKABLE [[nodiscard]] QString display(const QString& id) const;
  // The id of the action that already uses `key` in the scope of `id` (other than `id` itself), or "".
  Q_INVOKABLE [[nodiscard]] QString conflictOf(const QString& id, const QString& key) const;
  // Replaces the bindings of an action and saves keymap.json. Returns "" on success, otherwise the GUI-KEY-0001 text
  // and nothing changes.
  Q_INVOKABLE QString setKeys(const QString& id, const QStringList& keys);
  Q_INVOKABLE void resetAction(const QString& id);
  Q_INVOKABLE void resetAll();

  // Portable text of a key press (`Qt::Key` and `Qt::KeyboardModifiers` as integers), "" for a modifier alone.
  Q_INVOKABLE [[nodiscard]] QString keyText(int key, int modifiers) const;
  // The reverse for tests: [key, modifiers] of a portable single-key text.
  Q_INVOKABLE [[nodiscard]] QVariantList decompose(const QString& key) const;
  // True when a key press is bound to `id`, for panel-scope actions that handle Keys.onPressed themselves.
  Q_INVOKABLE [[nodiscard]] bool matches(const QString& id, int key, int modifiers) const;

  // Normalised portable text of a key ("ctrl+o" -> "Ctrl+O"), "" when it is not a single valid key sequence.
  static QString normalise(const QString& key);

 Q_SIGNALS:
  void changed();

 private:
  void load();
  bool save();
  [[nodiscard]] const ActionDef* find(const QString& id) const;

  QString path_;
  QString code_;
  QMap<QString, QStringList> bindings_;   // id -> current keys (defaults unless overridden)
  QMap<QString, QStringList> overrides_;  // id -> keys that differ from the defaults
  QJsonObject unknown_;                   // entries of unknown ids, kept when the file is written
  QStringList diagnostics_;
  int revision_ = 0;
};

}  // namespace ovt::app

#endif  // OVT_APP_KEYMAP_H
