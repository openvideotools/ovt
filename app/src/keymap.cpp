// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "keymap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QKeySequence>
#include <QSaveFile>

#include <algorithm>
#include <cstdio>

#include "platform/platform.h"

namespace ovt::app {

namespace {

constexpr auto kWindow = "window";
constexpr auto kPicture = "picture";
constexpr auto kList = "list";
constexpr auto kFocus = "focus";
constexpr auto kHex = "hex";

QString qs(const char* text) {
  return QString::fromUtf8(text);
}

ActionDef def(const char* menu_id, const char* text, const char* scope, std::initializer_list<const char*> keys) {
  ActionDef action;
  action.id = QStringLiteral("action.") + qs(menu_id);
  action.text = qs(text);
  action.scope = qs(scope);
  for (const char* key : keys) {
    action.keys.append(qs(key));
  }
  return action;
}

ActionDef win(const char* menu_id, const char* text, std::initializer_list<const char*> keys) {
  return def(menu_id, text, kWindow, keys);
}

// The §11.4.3 table, row by row. Chapter 3/4 bindings are not consulted (WI-1.466 notes).
std::vector<ActionDef> build_registry() {
  std::vector<ActionDef> r = {
      win("file.open", "Open", {"Ctrl+O"}),
      win("file.openReference", "Open reference", {"Ctrl+Shift+O"}),
      win("file.save", "Save project", {"Ctrl+S"}),
      win("file.saveAs", "Save project as", {"Ctrl+Shift+S"}),
      win("file.close", "Close file", {"Ctrl+W"}),
      win("file.quit", "Quit", {"Ctrl+Q"}),
      win("file.newWindow", "New window", {"Ctrl+N"}),
      win("edit.preferences", "Preferences", {"Ctrl+,"}),
      win("edit.find", "Find in focused panel", {"Ctrl+F"}),
      win("edit.goTo", "Go to (frame/time/offset/unit)", {"Ctrl+G"}),
      win("edit.copy", "Copy", {"Ctrl+C"}),
      win("edit.copyJson", "Copy as JSON", {"Ctrl+Shift+C"}),
      win("file.export.menu", "Export menu", {"Ctrl+E"}),
      win("analysis.metrics", "New metrics job", {"Ctrl+M"}),
      win("view.hdrOutput", "Toggle HDR output", {"Ctrl+H"}),
      win("view.pictureOnly", "Picture only", {"Ctrl+Shift+F"}),
      win("view.area.left", "Toggle left area", {"Ctrl+Shift+L"}),
      win("view.area.right", "Toggle right area", {"Ctrl+Shift+R"}),
      win("view.area.bottom", "Toggle bottom area", {"Ctrl+Shift+B"}),
  };
  static const char* const kFocusText[] = {"Focus panel 1", "Focus panel 2", "Focus panel 3",
                                           "Focus panel 4", "Focus panel 5", "Focus panel 6",
                                           "Focus panel 7", "Focus panel 8", "Focus panel 9"};
  for (int i = 1; i <= 9; ++i) {
    ActionDef a;
    a.id = QStringLiteral("action.view.focusPanel%1").arg(i);
    a.text = qs(kFocusText[i - 1]);
    a.scope = qs(kWindow);
    a.keys = {QStringLiteral("Ctrl+Shift+%1").arg(i)};
    r.push_back(a);
  }
  static const char* const kModeText[] = {
      "Coding flow / partitions", "Prediction",    "Motion vectors",
      "Transform / residual",     "QP map",        "Bits heatmap",
      "Reference indices",        "Loop filter",   "In-loop filter params (SAO / CDEF / LR)",
      "Metrics heatmap",          "Plain picture", "HDR false colour"};
  for (int i = 1; i <= 12; ++i) {
    ActionDef a;
    a.id = QStringLiteral("action.overlays.mode.f%1").arg(i);
    a.text = QStringLiteral("Overlay mode F%1: ").arg(i) + qs(kModeText[i - 1]);
    a.scope = qs(kWindow);
    a.keys = {QStringLiteral("F%1").arg(i)};
    r.push_back(a);
  }
  const std::vector<ActionDef> rest = {
      win("overlays.picture", "Toggle picture", {"P"}),
      win("overlays.info", "Toggle info labels", {"I"}),
      win("overlays.grid", "Toggle grid", {"G"}),
      win("overlays.motionVectors", "Toggle motion vectors", {"M"}),
      win("overlays.heatmap", "Toggle heatmap", {"T"}),
      win("overlays.legend", "Toggle legend", {"L"}),
      win("overlays.hex", "Hexadecimal values", {"H"}),
      win("overlays.dec", "Decimal values", {"D"}),
      win("overlays.colourBlindSafe", "Colour-blind-safe palette", {"K"}),
      win("overlays.deblockEdges", "Toggle deblocking edges", {"E"}),
      win("overlays.skipHatch", "Toggle skip hatch", {"S"}),
      win("view.plane.yuv", "Plane YUV", {"1"}),
      win("view.plane.y", "Plane Y", {"2"}),
      win("view.plane.u", "Plane U", {"3"}),
      win("view.plane.v", "Plane V", {"4"}),
      win("view.plane.gray", "Plane gray", {"5"}),
      win("view.zoom.in", "Zoom in", {"+"}),
      win("view.zoom.out", "Zoom out", {"-"}),
      win("view.zoom.fit", "Zoom to fit", {"R"}),
      win("view.zoom.100", "Zoom 100 %", {"Ctrl+1"}),
      win("view.zoom.200", "Zoom 200 %", {"Ctrl+2"}),
      win("view.zoom.400", "Zoom 400 %", {"Ctrl+4"}),
      win("view.dualView", "Toggle Dual view", {"Ctrl+D"}),
      win("view.fullScreen", "Full screen", {"F"}),
      win("view.order", "Toggle decode / display order", {"O"}),
      win("view.resetLayout", "Reset layout to preset", {"Ctrl+0"}),
      win("view.toggleTheme", "Toggle theme", {"Ctrl+Shift+T"}),
      win("view.nextTab", "Next tab in group", {"Ctrl+Tab"}),
      win("view.previousTab", "Previous tab in group", {"Ctrl+Shift+Tab"}),
      win("view.jobs", "Jobs popover", {"Ctrl+J"}),
      win("navigate.playPause", "Play / pause", {"Space"}),
      win("navigate.next", "Next frame (current order)", {"Right"}),
      win("navigate.previous", "Previous frame (current order)", {"Left"}),
      win("navigate.next10", "Forward 10 frames", {"Shift+Right"}),
      win("navigate.previous10", "Back 10 frames", {"Shift+Left"}),
      win("navigate.nextKey", "Next RAP (I/Key)", {"Ctrl+Right"}),
      win("navigate.previousKey", "Previous RAP (I/Key)", {"Ctrl+Left"}),
      win("navigate.nextSameType", "Next frame of the same type", {"Alt+Right"}),
      win("navigate.previousSameType", "Previous frame of the same type", {"Alt+Left"}),
      win("navigate.nextDecode", "Next frame in decode order", {"."}),
      win("navigate.previousDecode", "Previous frame in decode order", {","}),
      win("navigate.first", "First frame", {"Home"}),
      win("navigate.last", "Last frame", {"End"}),
      win("navigate.nextGop", "Forward 1 GOP", {"PgDown"}),
      win("navigate.previousGop", "Back 1 GOP", {"PgUp"}),
      win("navigate.forwardSecond", "Forward 1 second", {"Shift+PgDown"}),
      win("navigate.backSecond", "Back 1 second", {"Shift+PgUp"}),
      win("navigate.nextError", "Next error (Diagnostics)", {"Ctrl+Down"}),
      win("navigate.previousError", "Previous error (Diagnostics)", {"Ctrl+Up"}),
      win("navigate.addBookmark", "Add bookmark", {"B"}),
      win("navigate.addComment", "Add comment", {"Shift+B"}),
      win("navigate.nextBookmark", "Next bookmark", {"N"}),
      win("navigate.previousBookmark", "Previous bookmark", {"Shift+N"}),
      win("analysis.frame", "Analyse frame", {"A"}),
      win("analysis.gop", "Analyse GOP", {"Shift+A"}),
      win("analysis.cancelAll", "Cancel all jobs", {"Ctrl+."}),
      win("compare.nextMode", "Next compare mode", {"C"}),
      win("compare.previousMode", "Previous compare mode", {"Shift+C"}),
      win("compare.swap", "Swap A/B", {"X"}),
      win("compare.gainDown", "Difference gain -", {"["}),
      win("compare.gainUp", "Difference gain +", {"]"}),
      win("compare.falseColour", "Toggle false colour (= F12)", {"V"}),
      win("help.keyboardMap", "Keyboard map", {"?"}),
      // Panel-local keys: handled by the panel that has focus (Keymap.matches), never registered window-wide.
      def("picture.levelDown", "Cycle block level down", kPicture, {"Tab"}),
      def("picture.levelUp", "Cycle block level up", kPicture, {"Shift+Tab"}),
      def("picture.selectLeft", "Select the block to the left", kPicture, {"Shift+Left"}),
      def("picture.selectRight", "Select the block to the right", kPicture, {"Shift+Right"}),
      def("picture.selectUp", "Select the block above", kPicture, {"Shift+Up"}),
      def("picture.selectDown", "Select the block below", kPicture, {"Shift+Down"}),
      def("picture.probeLeft", "Move the probe one sample left", kPicture, {"Ctrl+Shift+Left"}),
      def("picture.probeRight", "Move the probe one sample right", kPicture, {"Ctrl+Shift+Right"}),
      def("picture.probeUp", "Move the probe one sample up", kPicture, {"Ctrl+Shift+Up"}),
      def("picture.probeDown", "Move the probe one sample down", kPicture, {"Ctrl+Shift+Down"}),
      def("hex.binary", "Toggle binary ribbon (Hex focused)", kHex, {"Ctrl+B"}),
      def("list.activate", "Activate row (trees and tables)", kList, {"Enter"}),
      def("list.activateNewWindow", "Activate row in a new window", kList, {"Ctrl+Enter"}),
      def("focus.escape", "Clear block/probe/rect, close popups, leave full screen", kFocus, {"Esc"}),
  };
  r.insert(r.end(), rest.begin(), rest.end());
  return r;
}

QStringList keys_of(const QJsonValue& value, bool* ok) {
  QStringList keys;
  *ok = value.isArray();
  if (!*ok) {
    return keys;
  }
  for (const QJsonValue entry : value.toArray()) {
    const QString key = entry.isString() ? Keymap::normalise(entry.toString()) : QString();
    if (key.isEmpty()) {
      *ok = false;
      return {};
    }
    if (!keys.contains(key)) {
      keys.append(key);
    }
  }
  return keys;
}

}  // namespace

const std::vector<ActionDef>& actionRegistry() {
  static const std::vector<ActionDef> registry = build_registry();
  return registry;
}

QString Keymap::normalise(const QString& key) {
  const QKeySequence sequence(key.trimmed(), QKeySequence::PortableText);
  if (sequence.count() != 1 || sequence.isEmpty()) {
    return {};
  }
  return sequence.toString(QKeySequence::PortableText);
}

Keymap::Keymap() : Keymap(Platform::keymapPath()) {}

Keymap::Keymap(const QString& path, QObject* parent)
    : QObject(parent), path_(path), code_(QStringLiteral("GUI-KEY-0001")) {
  for (const ActionDef& action : actionRegistry()) {
    bindings_.insert(action.id, action.keys);
  }
  load();
}

const ActionDef* Keymap::find(const QString& id) const {
  const auto& registry = actionRegistry();
  const auto it = std::ranges::find(registry, id, &ActionDef::id);
  return it == registry.end() ? nullptr : &*it;
}

QStringList Keymap::actionIds() const {
  QStringList ids;
  for (const ActionDef& action : actionRegistry()) {
    ids.append(action.id);
  }
  return ids;
}

QStringList Keymap::windowActionIds() const {
  QStringList ids;
  for (const ActionDef& action : actionRegistry()) {
    if (action.scope == QLatin1String(kWindow)) {
      ids.append(action.id);
    }
  }
  return ids;
}

bool Keymap::has(const QString& id) const {
  return find(id) != nullptr;
}

QString Keymap::text(const QString& id) const {
  const ActionDef* action = find(id);
  return action != nullptr ? action->text : QString();
}

QString Keymap::scope(const QString& id) const {
  const ActionDef* action = find(id);
  return action != nullptr ? action->scope : QString();
}

QStringList Keymap::keys(const QString& id) const {
  return bindings_.value(id);
}

QStringList Keymap::defaultKeys(const QString& id) const {
  const ActionDef* action = find(id);
  return action != nullptr ? action->keys : QStringList();
}

QString Keymap::display(const QString& id) const {
  return bindings_.value(id).join(QStringLiteral(" / "));
}

QString Keymap::conflictOf(const QString& id, const QString& key) const {
  const ActionDef* self = find(id);
  if (self == nullptr) {
    return {};
  }
  for (const ActionDef& other : actionRegistry()) {
    if (other.id != id && other.scope == self->scope && bindings_.value(other.id).contains(key)) {
      return other.id;
    }
  }
  return {};
}

void Keymap::load() {
  QFile file(path_);
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
    return;
  }
  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
    diagnostics_.append(QStringLiteral("GUI-KEY-0001: keymap.json is not a JSON object, the default map is used"));
    std::fprintf(stderr, "%s: %s\n", diagnostics_.back().toLocal8Bit().constData(), path_.toLocal8Bit().constData());
    return;
  }
  // Phase 1: the proposed map, every well-formed override applied to the defaults.
  QMap<QString, QStringList> proposed = bindings_;
  const QJsonObject object = doc.object();
  QStringList applied;
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!has(it.key())) {
      unknown_.insert(it.key(), it.value());
      continue;
    }
    bool ok = false;
    const QStringList keys = keys_of(it.value(), &ok);
    if (!ok) {
      diagnostics_.append(QStringLiteral("GUI-KEY-0001: %1 is not a list of key names, entry rejected").arg(it.key()));
      continue;
    }
    proposed.insert(it.key(), keys);
    applied.append(it.key());
  }
  // Phase 2: a key shared by two actions of one scope rejects every overriding entry that holds it; the defaults of
  // the other action stay. Repeat because rejecting an entry restores its defaults, which may conflict in turn.
  bool again = true;
  while (again) {
    again = false;
    for (const QString& id : std::as_const(applied)) {
      const ActionDef* self = find(id);
      for (const QString& key : proposed.value(id)) {
        QString other_id;
        for (const ActionDef& other : actionRegistry()) {
          if (other.id != id && other.scope == self->scope && proposed.value(other.id).contains(key)) {
            other_id = other.id;
            break;
          }
        }
        if (other_id.isEmpty()) {
          continue;
        }
        diagnostics_.append(QStringLiteral("GUI-KEY-0001: shortcut %1 is bound to %2 and %3, entry %3 rejected")
                                .arg(key, other_id, id));
        std::fprintf(stderr, "%s\n", diagnostics_.back().toLocal8Bit().constData());
        proposed.insert(id, self->keys);
        applied.removeAll(id);
        again = true;
        break;
      }
      if (again) {
        break;
      }
    }
  }
  bindings_ = proposed;
  for (const QString& id : std::as_const(applied)) {
    if (bindings_.value(id) != find(id)->keys) {
      overrides_.insert(id, bindings_.value(id));
    }
  }
}

bool Keymap::save() {
  if (!QDir().mkpath(QFileInfo(path_).absolutePath())) {
    return false;
  }
  QJsonObject object = unknown_;
  for (auto it = overrides_.begin(); it != overrides_.end(); ++it) {
    object.insert(it.key(), QJsonArray::fromStringList(it.value()));
  }
  QSaveFile file(path_);
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
  return file.commit();
}

QString Keymap::setKeys(const QString& id, const QStringList& keys) {
  if (!has(id)) {
    return QStringLiteral("unknown action %1").arg(id);
  }
  QStringList normal;
  for (const QString& key : keys) {
    const QString n = normalise(key);
    if (n.isEmpty()) {
      return QStringLiteral("\"%1\" is not a key").arg(key);
    }
    if (!normal.contains(n)) {
      normal.append(n);
    }
  }
  for (const QString& key : std::as_const(normal)) {
    const QString other = conflictOf(id, key);
    if (!other.isEmpty()) {
      return QStringLiteral("GUI-KEY-0001: shortcut %1 is bound to %2 and %3").arg(key, other, id);
    }
  }
  bindings_.insert(id, normal);
  if (normal == find(id)->keys) {
    overrides_.remove(id);
  } else {
    overrides_.insert(id, normal);
  }
  ++revision_;
  if (!save()) {
    std::fprintf(stderr, "keymap: could not write %s\n", path_.toLocal8Bit().constData());
  }
  Q_EMIT changed();
  return {};
}

void Keymap::resetAction(const QString& id) {
  const ActionDef* action = find(id);
  if (action == nullptr || !overrides_.contains(id)) {
    return;
  }
  // The defaults may clash with a rebinding made since: those actions lose the shared key.
  for (const QString& key : action->keys) {
    const QString other = conflictOf(id, key);
    if (!other.isEmpty()) {
      QStringList others = bindings_.value(other);
      others.removeAll(key);
      bindings_.insert(other, others);
      overrides_.insert(other, others);
    }
  }
  bindings_.insert(id, action->keys);
  overrides_.remove(id);
  ++revision_;
  static_cast<void>(save());
  Q_EMIT changed();
}

void Keymap::resetAll() {
  for (const ActionDef& action : actionRegistry()) {
    bindings_.insert(action.id, action.keys);
  }
  overrides_.clear();
  ++revision_;
  static_cast<void>(save());
  Q_EMIT changed();
}

QString Keymap::keyText(int key, int modifiers) const {
  if (key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta ||
      key == Qt::Key_AltGr || key == Qt::Key_unknown || key == 0) {
    return {};
  }
  const QKeySequence sequence(
      QKeyCombination((Qt::KeyboardModifiers(static_cast<Qt::KeyboardModifier>(modifiers)) & Qt::KeyboardModifierMask),
                      static_cast<Qt::Key>(key)));
  return sequence.toString(QKeySequence::PortableText);
}

QVariantList Keymap::decompose(const QString& key) const {
  const QKeySequence sequence(key, QKeySequence::PortableText);
  if (sequence.count() != 1) {
    return {};
  }
  const QKeyCombination combination = sequence[0];
  return {static_cast<int>(combination.key()), static_cast<int>(combination.keyboardModifiers().toInt())};
}

bool Keymap::matches(const QString& id, int key, int modifiers) const {
  return bindings_.value(id).contains(keyText(key, modifiers));
}

}  // namespace ovt::app
