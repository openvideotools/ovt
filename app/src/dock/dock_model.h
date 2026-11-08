// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dock model of OVT Studio (chapter 11 §11.2.2; WI-1.451): panels are tabs inside groups, groups sit in the
// Left, Center, Right and Bottom areas (and in floating windows on the desktop). The plain structs and the free
// functions in ovt::app::dock hold every rule (presets, moves, validation, JSON); DockModel is the QML singleton
// that owns the current Layout, tells QML when it changes and persists it.
//
// SPEC-ASSUMPTION(#229): §11.2.2 names no panel ids, no area sizes and no storage for the current layout.
// Panel ids are the snake_case names of §11.3 (panels() below). Layout gains leftSize/rightSize/bottomSize (the
// area's share of the window), so that the "Syntax wide" and "Bottom = Hex tall" presets can be expressed. The
// current layout is persisted in layouts.json next to settings.json (§11.2.5) as {"layouts_version": 1,
// "current": <layout>}, not in settings.json (FR-GUI-002), whose schema has no layout key.

#ifndef OVT_APP_DOCK_DOCK_MODEL_H
#define OVT_APP_DOCK_DOCK_MODEL_H

#include <QtQml/qqmlregistration.h>
#include <QByteArray>
#include <QJsonObject>
#include <QLatin1StringView>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

#include <cstdint>
#include <optional>
#include <span>

namespace ovt::app {

enum class DockArea : std::uint8_t { Left, Center, Right, Bottom, Floating /* desktop only */ };

struct DockGroup {
  DockArea area = DockArea::Right;
  QStringList panelIds;
  int currentIndex = 0;
  qreal sizeFraction = 1.0;  // of the area's split axis; 4 decimals

  friend bool operator==(const DockGroup&, const DockGroup&) = default;
};

struct Layout {
  QString name;  // the preset this layout started from, or the custom layout's name
  QList<DockGroup> groups;
  bool leftVisible = true;
  bool rightVisible = true;
  bool bottomVisible = true;
  // Share of the window width (Left, Right) or of the centre column height (Bottom); 4 decimals.
  qreal leftSize = 0.18;
  qreal rightSize = 0.24;
  qreal bottomSize = 0.28;

  friend bool operator==(const Layout&, const Layout&) = default;
};

namespace dock {

inline constexpr QLatin1StringView kPicture{"picture"};

struct PanelInfo {
  const char* id;
  const char* title;  // untranslated; QML translates
  DockArea home;      // where View ▸ Panels re-opens it
};

// The dock panels of §11.3.1–§11.3.17 (the phase-4 Buffer panel registers itself with WI-4.xxx).
std::span<const PanelInfo> panels();
const PanelInfo* findPanel(QStringView id);

QString areaName(DockArea area);  // "left", "center", "right", "bottom", "floating"
std::optional<DockArea> parseArea(QStringView name);

// Floating windows exist on the desktop only; on Android the code path is compiled out (§11.5.2).
constexpr bool floatingSupported() {
#ifdef Q_OS_ANDROID
  return false;
#else
  return true;
#endif
}

// Named presets of §11.2.2 in View ▸ Layout order: Analyze (the default), Syntax, Quality, HDR, Compare.
QStringList presetNames();
std::optional<Layout> preset(QStringView name);

// Rounds to the 4 decimals that are serialised (WI-1.451 note), so that round trips are byte-identical.
qreal round4(qreal value);

// Drops empty groups, clamps currentIndex, renormalises sizeFraction per area (sum 1) and rounds sizes.
void normalise(Layout& layout);
// Every panel known and open at most once, exactly one Center group and it holds the Picture, no floating group
// where floating is not supported, indices and sizes in range.
bool validate(const Layout& layout, QString* reason);

// Deterministic JSON: fixed key order, sizes with exactly 4 decimals, no whitespace.
QByteArray toJson(const Layout& layout);
// Parses, drops unknown panel ids (a layout from a newer version), normalises and validates.
std::optional<Layout> fromJson(const QByteArray& json, QString* error);
std::optional<Layout> fromJsonObject(const QJsonObject& object, QString* error);

int groupOf(const Layout& layout, QStringView panelId);  // index into groups, -1 when closed
QStringList openPanels(const Layout& layout);            // in tab order
// Left, Center, Right, Bottom, Floating; groups in list order, tabs in order (Ctrl+Shift+[1-9], §11.2.2).
QStringList tabOrder(const Layout& layout);

// The operations. Each returns false and leaves `layout` unchanged when the move breaks a rule (the Picture never
// leaves Center, Center has one group, no floating where unsupported).
bool movePanelToGroup(Layout& layout, const QString& panelId, int group, int tabIndex);
bool movePanelToNewGroup(Layout& layout, const QString& panelId, DockArea area, int position);
bool closePanel(Layout& layout, const QString& panelId);
bool openPanel(Layout& layout, const QString& panelId);
bool setCurrentPanel(Layout& layout, const QString& panelId);

}  // namespace dock

class DockModel : public QObject {
  Q_OBJECT
  QML_ELEMENT
  QML_SINGLETON
  Q_PROPERTY(int revision READ revision NOTIFY layoutChanged)
  Q_PROPERTY(QString layoutName READ layoutName NOTIFY layoutChanged)
  Q_PROPERTY(bool leftVisible READ leftVisible WRITE setLeftVisible NOTIFY layoutChanged)
  Q_PROPERTY(bool rightVisible READ rightVisible WRITE setRightVisible NOTIFY layoutChanged)
  Q_PROPERTY(bool bottomVisible READ bottomVisible WRITE setBottomVisible NOTIFY layoutChanged)
  Q_PROPERTY(qreal leftSize READ leftSize WRITE setLeftSize NOTIFY layoutChanged)
  Q_PROPERTY(qreal rightSize READ rightSize WRITE setRightSize NOTIFY layoutChanged)
  Q_PROPERTY(qreal bottomSize READ bottomSize WRITE setBottomSize NOTIFY layoutChanged)
  Q_PROPERTY(QStringList openPanels READ openPanels NOTIFY layoutChanged)
  // groups(area) for every area name, as a property so that QML bindings follow layout changes.
  Q_PROPERTY(QVariantMap areas READ areas NOTIFY layoutChanged)
  // "Picture only" (Ctrl+Shift+F) hides Left, Right and Bottom without changing the layout.
  Q_PROPERTY(bool pictureOnly READ pictureOnly WRITE setPictureOnly NOTIFY pictureOnlyChanged)
  Q_PROPERTY(QStringList presetNames READ presetNames CONSTANT)
  Q_PROPERTY(bool floatingSupported READ floatingSupported CONSTANT)

 public:
  // Restores from Platform::layoutsPath(); falls back to Analyze when the file is missing or invalid.
  explicit DockModel(QObject* parent = nullptr);
  // An empty path keeps the model in memory only.
  DockModel(QString storagePath, QObject* parent);
  ~DockModel() override;
  DockModel(const DockModel&) = delete;
  DockModel& operator=(const DockModel&) = delete;
  DockModel(DockModel&&) = delete;
  DockModel& operator=(DockModel&&) = delete;

  [[nodiscard]] const Layout& layout() const { return layout_; }
  bool setLayout(Layout layout);

  [[nodiscard]] int revision() const { return revision_; }
  [[nodiscard]] QString layoutName() const { return layout_.name; }
  [[nodiscard]] bool leftVisible() const { return layout_.leftVisible; }
  [[nodiscard]] bool rightVisible() const { return layout_.rightVisible; }
  [[nodiscard]] bool bottomVisible() const { return layout_.bottomVisible; }
  [[nodiscard]] qreal leftSize() const { return layout_.leftSize; }
  [[nodiscard]] qreal rightSize() const { return layout_.rightSize; }
  [[nodiscard]] qreal bottomSize() const { return layout_.bottomSize; }
  void setLeftVisible(bool visible);
  void setRightVisible(bool visible);
  void setBottomVisible(bool visible);
  void setLeftSize(qreal size);
  void setRightSize(qreal size);
  void setBottomSize(qreal size);
  [[nodiscard]] QStringList openPanels() const { return dock::openPanels(layout_); }
  [[nodiscard]] QVariantMap areas() const;
  [[nodiscard]] bool pictureOnly() const { return picture_only_; }
  void setPictureOnly(bool on);
  // Members, not static: QML reads them as properties of the singleton.
  // NOLINTBEGIN(readability-convert-member-functions-to-static)
  [[nodiscard]] QStringList presetNames() const { return dock::presetNames(); }
  [[nodiscard]] bool floatingSupported() const { return dock::floatingSupported(); }

  // The groups of one area for QML: [{group, panelIds, currentIndex, sizeFraction}], `group` indexes the layout.
  Q_INVOKABLE [[nodiscard]] QVariantList groups(const QString& area) const;
  Q_INVOKABLE [[nodiscard]] QStringList allPanels() const;
  Q_INVOKABLE [[nodiscard]] QString panelTitle(const QString& panelId) const;
  // NOLINTEND(readability-convert-member-functions-to-static)
  Q_INVOKABLE [[nodiscard]] QStringList tabOrder() const { return dock::tabOrder(layout_); }
  Q_INVOKABLE [[nodiscard]] int groupOf(const QString& panelId) const { return dock::groupOf(layout_, panelId); }

  Q_INVOKABLE bool applyPreset(const QString& name);
  Q_INVOKABLE void resetLayout();  // View ▸ Reset Layout (FR-GUI-002): the Analyze preset
  Q_INVOKABLE bool movePanelToGroup(const QString& panelId, int group, int tabIndex);
  Q_INVOKABLE bool movePanelToNewGroup(const QString& panelId, const QString& area, int position);
  Q_INVOKABLE bool floatPanel(const QString& panelId);
  Q_INVOKABLE bool closePanel(const QString& panelId);
  Q_INVOKABLE bool openPanel(const QString& panelId);
  Q_INVOKABLE bool togglePanel(const QString& panelId);
  Q_INVOKABLE bool setCurrentPanel(const QString& panelId);
  Q_INVOKABLE bool setCurrentIndex(int group, int index);
  // Ctrl+Tab / Ctrl+Shift+Tab: makes the next (delta 1) or previous (-1) tab of the panel's group current and
  // returns its id; returns `panelId` unchanged when it is not open.
  Q_INVOKABLE QString cycleTab(const QString& panelId, int delta);
  // Double-click on a splitter handle: the area sizes of the layout's preset and equal groups per area.
  Q_INVOKABLE void resetSizes();
  // After a splitter drag: the new sizes of the area's groups in order (any positive scale).
  Q_INVOKABLE void setSizeFractions(const QString& area, const QVariantList& sizes);

  Q_INVOKABLE [[nodiscard]] QString toJson() const { return QString::fromUtf8(dock::toJson(layout_)); }
  Q_INVOKABLE bool restoreJson(const QString& json);

  // Writes layouts.json now (otherwise 500 ms after the last change, §11.2.2). False on an I/O error.
  bool save();

 Q_SIGNALS:
  void layoutChanged();
  void pictureOnlyChanged();

 private:
  bool commit(Layout layout);
  void load();

  Layout layout_;
  QString storage_path_;
  QTimer save_timer_;
  int revision_ = 0;
  bool picture_only_ = false;
};

}  // namespace ovt::app

#endif  // OVT_APP_DOCK_DOCK_MODEL_H
