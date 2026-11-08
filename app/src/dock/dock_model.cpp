// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dock/dock_model.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "platform/platform.h"

namespace ovt::app {

namespace dock {

namespace {

constexpr std::array kPanels{
    PanelInfo{"timeline", QT_TRANSLATE_NOOP("Dock", "Timeline"), DockArea::Bottom},
    PanelInfo{"picture", QT_TRANSLATE_NOOP("Dock", "Picture"), DockArea::Center},
    PanelInfo{"overlay_legend", QT_TRANSLATE_NOOP("Dock", "Overlay Legend"), DockArea::Right},
    PanelInfo{"syntax", QT_TRANSLATE_NOOP("Dock", "Syntax"), DockArea::Right},
    PanelInfo{"hex", QT_TRANSLATE_NOOP("Dock", "Hex"), DockArea::Bottom},
    PanelInfo{"units", QT_TRANSLATE_NOOP("Dock", "Units"), DockArea::Left},
    PanelInfo{"container", QT_TRANSLATE_NOOP("Dock", "Container"), DockArea::Left},
    PanelInfo{"stream_info", QT_TRANSLATE_NOOP("Dock", "Stream Info"), DockArea::Left},
    PanelInfo{"selection_info", QT_TRANSLATE_NOOP("Dock", "Selection Info"), DockArea::Right},
    PanelInfo{"block_samples", QT_TRANSLATE_NOOP("Dock", "Block Samples"), DockArea::Right},
    PanelInfo{"ref_graph", QT_TRANSLATE_NOOP("Dock", "Reference Graph"), DockArea::Right},
    PanelInfo{"statistics", QT_TRANSLATE_NOOP("Dock", "Statistics"), DockArea::Right},
    PanelInfo{"metrics", QT_TRANSLATE_NOOP("Dock", "Metrics"), DockArea::Right},
    PanelInfo{"hdr", QT_TRANSLATE_NOOP("Dock", "HDR"), DockArea::Right},
    PanelInfo{"display_caps", QT_TRANSLATE_NOOP("Dock", "Display Capabilities"), DockArea::Right},
    PanelInfo{"diagnostics", QT_TRANSLATE_NOOP("Dock", "Diagnostics"), DockArea::Bottom},
    PanelInfo{"bookmarks", QT_TRANSLATE_NOOP("Dock", "Bookmarks"), DockArea::Left},
};

constexpr std::array kAreaNames{"left", "center", "right", "bottom", "floating"};
constexpr std::array kAreaOrder{DockArea::Left, DockArea::Center, DockArea::Right, DockArea::Bottom,
                                DockArea::Floating};

constexpr qreal kMinAreaSize = 0.05;
constexpr qreal kMaxAreaSize = 0.9;

DockGroup group(DockArea area, QStringList panels, qreal fraction) {
  return DockGroup{.area = area, .panelIds = std::move(panels), .currentIndex = 0, .sizeFraction = fraction};
}

DockGroup center() {
  return group(DockArea::Center, {QString(kPicture)}, 1.0);
}

// The preset table of §11.2.2. A slash in the specification ("Left = Container/Units") is one group per panel,
// stacked along the area's axis; areas a preset does not name keep the Analyze contents.
// SPEC-ASSUMPTION(#229): the group split, the fractions and the area sizes are not specified.
Layout analyze() {
  Layout layout;
  layout.name = QStringLiteral("Analyze");
  layout.groups = {group(DockArea::Left, {QStringLiteral("container")}, 0.6),
                   group(DockArea::Left, {QStringLiteral("units")}, 0.4),
                   center(),
                   group(DockArea::Right, {QStringLiteral("syntax")}, 0.5),
                   group(DockArea::Right, {QStringLiteral("selection_info")}, 0.25),
                   group(DockArea::Right, {QStringLiteral("block_samples")}, 0.25),
                   group(DockArea::Bottom, {QStringLiteral("timeline")}, 0.6),
                   group(DockArea::Bottom, {QStringLiteral("hex")}, 0.4)};
  return layout;
}

Layout withRightAndBottom(const QString& name, const QList<DockGroup>& right, const QList<DockGroup>& bottom) {
  Layout layout = analyze();
  layout.name = name;
  layout.groups.erase(
      std::remove_if(layout.groups.begin(), layout.groups.end(),
                     [](const DockGroup& g) { return g.area == DockArea::Right || g.area == DockArea::Bottom; }),
      layout.groups.end());
  layout.groups.append(right);
  layout.groups.append(bottom);
  return layout;
}

// Positions in `groups` of the groups of `area`, in order.
QList<int> groupsIn(const Layout& layout, DockArea area) {
  QList<int> result;
  for (int i = 0; i < layout.groups.size(); ++i) {
    if (layout.groups[i].area == area) {
      result.append(i);
    }
  }
  return result;
}

// Removes `panelId` from its group (the group may become empty; normalise() drops it).
// Returns {group, tab} or {-1, -1}.
std::pair<int, int> take(Layout& layout, const QString& panelId) {
  const int g = groupOf(layout, panelId);
  if (g < 0) {
    return {-1, -1};
  }
  DockGroup& source = layout.groups[g];
  const auto tab = static_cast<int>(source.panelIds.indexOf(panelId));
  source.panelIds.removeAt(tab);
  if (source.currentIndex > tab || source.currentIndex >= source.panelIds.size()) {
    source.currentIndex = std::max(0, source.currentIndex - 1);
  }
  return {g, tab};
}

void showArea(Layout& layout, DockArea area) {
  switch (area) {
    case DockArea::Left:
      layout.leftVisible = true;
      break;
    case DockArea::Right:
      layout.rightVisible = true;
      break;
    case DockArea::Bottom:
      layout.bottomVisible = true;
      break;
    case DockArea::Center:
    case DockArea::Floating:
      break;
  }
}

// Accepts `next` only if it is valid after normalising.
bool accept(Layout& layout, Layout next) {
  normalise(next);
  if (!validate(next, nullptr)) {
    return false;
  }
  layout = std::move(next);
  return true;
}

QByteArray quoted(const QString& text) {
  // QJsonDocument does the escaping; strip the surrounding brackets of the one-element array.
  const QByteArray array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
  return array.mid(1, array.size() - 2);
}

QByteArray number4(qreal value) {
  return QByteArray::number(round4(value), 'f', 4);
}

QByteArray boolean(bool value) {
  return value ? QByteArrayLiteral("true") : QByteArrayLiteral("false");
}

bool fail(QString* error, const QString& reason) {
  if (error != nullptr) {
    *error = reason;
  }
  return false;
}

}  // namespace

std::span<const PanelInfo> panels() {
  return kPanels;
}

const PanelInfo* findPanel(QStringView id) {
  const auto* it =
      std::find_if(kPanels.begin(), kPanels.end(), [id](const PanelInfo& p) { return id == QLatin1StringView(p.id); });
  return it == kPanels.end() ? nullptr : &*it;
}

QString areaName(DockArea area) {
  return QString::fromLatin1(kAreaNames.at(static_cast<std::size_t>(area)));
}

std::optional<DockArea> parseArea(QStringView name) {
  for (std::size_t i = 0; i < kAreaNames.size(); ++i) {
    if (name == QLatin1StringView(kAreaNames.at(i))) {
      return static_cast<DockArea>(i);
    }
  }
  return std::nullopt;
}

QStringList presetNames() {
  return {QStringLiteral("Analyze"), QStringLiteral("Syntax"), QStringLiteral("Quality"), QStringLiteral("HDR"),
          QStringLiteral("Compare")};
}

std::optional<Layout> preset(QStringView name) {
  if (name == u"Analyze") {
    return analyze();
  }
  if (name == u"Syntax") {
    // Left hidden, Right = Syntax wide, Bottom = Hex tall.
    Layout layout =
        withRightAndBottom(QStringLiteral("Syntax"), {group(DockArea::Right, {QStringLiteral("syntax")}, 1)},
                           {group(DockArea::Bottom, {QStringLiteral("hex")}, 1)});
    layout.leftVisible = false;
    layout.rightSize = 0.36;
    layout.bottomSize = 0.4;
    return layout;
  }
  if (name == u"Quality") {
    // The metric series in the timeline is panel state (WI-1.455), not dock state.
    return withRightAndBottom(QStringLiteral("Quality"),
                              {group(DockArea::Right, {QStringLiteral("metrics")}, 0.5),
                               group(DockArea::Right, {QStringLiteral("statistics")}, 0.5)},
                              {group(DockArea::Bottom, {QStringLiteral("timeline")}, 1)});
  }
  if (name == u"HDR") {
    return withRightAndBottom(QStringLiteral("HDR"),
                              {group(DockArea::Right, {QStringLiteral("hdr")}, 0.6),
                               group(DockArea::Right, {QStringLiteral("display_caps")}, 0.4)},
                              {group(DockArea::Bottom, {QStringLiteral("timeline")}, 1)});
  }
  if (name == u"Compare") {
    // Split mode is Picture state (WI-1.458); Selection Info shows A and B itself.
    return withRightAndBottom(QStringLiteral("Compare"),
                              {group(DockArea::Right, {QStringLiteral("selection_info")}, 1)},
                              {group(DockArea::Bottom, {QStringLiteral("timeline")}, 1)});
  }
  return std::nullopt;
}

qreal round4(qreal value) {
  return std::round(value * 10000.0) / 10000.0;
}

void normalise(Layout& layout) {
  layout.groups.erase(std::remove_if(layout.groups.begin(), layout.groups.end(),
                                     [](const DockGroup& g) { return g.panelIds.isEmpty(); }),
                      layout.groups.end());
  for (const DockArea area : kAreaOrder) {
    const QList<int> members = groupsIn(layout, area);
    qreal sum = 0;
    bool positive = true;
    for (const int i : members) {
      const qreal f = layout.groups[i].sizeFraction;
      positive = positive && std::isfinite(f) && f > 0;
      sum += f;
    }
    for (const int i : members) {
      DockGroup& g = layout.groups[i];
      g.currentIndex = std::clamp(g.currentIndex, 0, static_cast<int>(g.panelIds.size()) - 1);
      // Floating groups are alone in their window.
      const bool alone = area == DockArea::Floating || area == DockArea::Center;
      g.sizeFraction =
          alone ? 1.0 : round4(positive && sum > 0 ? g.sizeFraction / sum : 1.0 / static_cast<qreal>(members.size()));
    }
  }
  const auto clampSize = [](qreal size) {
    return round4(std::isfinite(size) ? std::clamp(size, kMinAreaSize, kMaxAreaSize) : kMinAreaSize);
  };
  layout.leftSize = clampSize(layout.leftSize);
  layout.rightSize = clampSize(layout.rightSize);
  layout.bottomSize = clampSize(layout.bottomSize);
}

namespace {

bool validateGroup(const DockGroup& g, QStringList& seen, QString* reason) {
  if (g.panelIds.isEmpty()) {
    return fail(reason, QStringLiteral("empty group"));
  }
  if (g.area == DockArea::Floating && !floatingSupported()) {
    return fail(reason, QStringLiteral("floating groups are not supported on this platform"));
  }
  if (g.currentIndex < 0 || g.currentIndex >= g.panelIds.size()) {
    return fail(reason, QStringLiteral("currentIndex out of range"));
  }
  if (!std::isfinite(g.sizeFraction) || g.sizeFraction <= 0 || g.sizeFraction > 1) {
    return fail(reason, QStringLiteral("sizeFraction out of range"));
  }
  if (g.area == DockArea::Center && !g.panelIds.contains(kPicture)) {
    return fail(reason, QStringLiteral("the Center group must hold the Picture panel"));
  }
  for (const QString& id : g.panelIds) {
    if (findPanel(id) == nullptr) {
      return fail(reason, QStringLiteral("unknown panel '%1'").arg(id));
    }
    if (seen.contains(id)) {
      return fail(reason, QStringLiteral("panel '%1' is open twice").arg(id));
    }
    seen.append(id);
  }
  return true;
}

}  // namespace

bool validate(const Layout& layout, QString* reason) {
  QStringList seen;
  int centers = 0;
  for (const DockGroup& g : layout.groups) {
    if (!validateGroup(g, seen, reason)) {
      return false;
    }
    centers += g.area == DockArea::Center ? 1 : 0;
  }
  if (centers != 1) {
    return fail(reason, QStringLiteral("exactly one Center group is required"));
  }
  return true;
}

QByteArray toJson(const Layout& layout) {
  QByteArray out = "{\"preset\":" + quoted(layout.name);
  out += ",\"leftVisible\":" + boolean(layout.leftVisible);
  out += ",\"rightVisible\":" + boolean(layout.rightVisible);
  out += ",\"bottomVisible\":" + boolean(layout.bottomVisible);
  out += ",\"leftSize\":" + number4(layout.leftSize);
  out += ",\"rightSize\":" + number4(layout.rightSize);
  out += ",\"bottomSize\":" + number4(layout.bottomSize);
  out += ",\"groups\":[";
  for (int i = 0; i < layout.groups.size(); ++i) {
    const DockGroup& g = layout.groups[i];
    out += (i == 0 ? "" : ",") + QByteArray("{\"area\":") + quoted(areaName(g.area)) + ",\"panelIds\":[";
    for (int p = 0; p < g.panelIds.size(); ++p) {
      out += (p == 0 ? "" : ",") + quoted(g.panelIds[p]);
    }
    out += "],\"currentIndex\":" + QByteArray::number(g.currentIndex);
    out += ",\"sizeFraction\":" + number4(g.sizeFraction) + "}";
  }
  out += "]}";
  return out;
}

std::optional<Layout> fromJson(const QByteArray& json, QString* error) {
  QJsonParseError parse{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject()) {
    fail(error,
         parse.error != QJsonParseError::NoError ? parse.errorString() : QStringLiteral("a layout is a JSON object"));
    return std::nullopt;
  }
  return fromJsonObject(document.object(), error);
}

std::optional<Layout> fromJsonObject(const QJsonObject& object, QString* error) {
  Layout layout;
  const auto boolField = [&object](const char* key, bool fallback) {
    const QJsonValue v = object.value(QLatin1StringView(key));
    return v.isBool() ? v.toBool() : fallback;
  };
  const auto sizeField = [&object](const char* key, qreal fallback) {
    const QJsonValue v = object.value(QLatin1StringView(key));
    return v.isDouble() ? v.toDouble() : fallback;
  };
  layout.name = object.value(QLatin1StringView("preset")).toString();
  layout.leftVisible = boolField("leftVisible", true);
  layout.rightVisible = boolField("rightVisible", true);
  layout.bottomVisible = boolField("bottomVisible", true);
  layout.leftSize = sizeField("leftSize", layout.leftSize);
  layout.rightSize = sizeField("rightSize", layout.rightSize);
  layout.bottomSize = sizeField("bottomSize", layout.bottomSize);
  const QJsonValue groups = object.value(QLatin1StringView("groups"));
  if (!groups.isArray()) {
    fail(error, QStringLiteral("'groups' must be an array"));
    return std::nullopt;
  }
  // By value: QJsonArray's iterators yield temporaries (-Wrange-loop-bind-reference on Apple clang).
  const QJsonArray groupArray = groups.toArray();
  for (const QJsonValue value : groupArray) {
    const QJsonObject g = value.toObject();
    const std::optional<DockArea> area = parseArea(g.value(QLatin1StringView("area")).toString());
    const QJsonValue ids = g.value(QLatin1StringView("panelIds"));
    if (!value.isObject() || !area || !ids.isArray()) {
      fail(error, QStringLiteral("a group needs 'area' and 'panelIds'"));
      return std::nullopt;
    }
    DockGroup group{.area = *area,
                    .panelIds = {},
                    .currentIndex = g.value(QLatin1StringView("currentIndex")).toInt(0),
                    .sizeFraction = g.value(QLatin1StringView("sizeFraction")).toDouble(1.0)};
    const QJsonArray idArray = ids.toArray();
    for (const QJsonValue idValue : idArray) {
      const QString id = idValue.toString();
      if (findPanel(id) != nullptr) {  // unknown ids (a newer version's panels) are dropped
        group.panelIds.append(id);
      }
    }
    layout.groups.append(group);
  }
  normalise(layout);
  QString reason;
  if (!validate(layout, &reason)) {
    fail(error, reason);
    return std::nullopt;
  }
  return layout;
}

int groupOf(const Layout& layout, QStringView panelId) {
  for (int i = 0; i < layout.groups.size(); ++i) {
    if (layout.groups[i].panelIds.contains(panelId)) {
      return i;
    }
  }
  return -1;
}

QStringList tabOrder(const Layout& layout) {
  QStringList order;
  for (const DockArea area : kAreaOrder) {
    for (const int i : groupsIn(layout, area)) {
      order.append(layout.groups[i].panelIds);
    }
  }
  return order;
}

QStringList openPanels(const Layout& layout) {
  return tabOrder(layout);
}

bool movePanelToGroup(Layout& layout, const QString& panelId, int group, int tabIndex) {
  if (group < 0 || group >= layout.groups.size() || groupOf(layout, panelId) < 0) {
    return false;
  }
  if (panelId == kPicture && layout.groups[group].area != DockArea::Center) {
    return false;
  }
  Layout next = layout;
  const auto [source, sourceTab] = take(next, panelId);
  QStringList& ids = next.groups[group].panelIds;
  if (source == group && sourceTab < tabIndex) {
    --tabIndex;
  }
  tabIndex = std::clamp(tabIndex, 0, static_cast<int>(ids.size()));
  ids.insert(tabIndex, panelId);
  next.groups[group].currentIndex = tabIndex;
  return accept(layout, std::move(next));
}

bool movePanelToNewGroup(Layout& layout, const QString& panelId, DockArea area, int position) {
  if (area == DockArea::Center || panelId == kPicture || groupOf(layout, panelId) < 0) {
    return false;
  }
  if (area == DockArea::Floating && !floatingSupported()) {
    return false;
  }
  Layout next = layout;
  take(next, panelId);
  const QList<int> members = groupsIn(next, area);
  qreal sum = 0;
  int others = 0;
  for (const int i : members) {
    if (!next.groups[i].panelIds.isEmpty()) {
      sum += next.groups[i].sizeFraction;
      ++others;
    }
  }
  // The average share of the others, so the new group gets 1/(n+1) once normalised.
  const qreal fraction = others == 0 || sum <= 0 ? 1.0 : sum / others;
  int at = static_cast<int>(next.groups.size());
  if (position >= 0 && position < members.size()) {
    at = members[position];
  } else if (!members.isEmpty()) {
    at = members.back() + 1;
  }
  next.groups.insert(at, group(area, {panelId}, fraction));
  showArea(next, area);
  return accept(layout, std::move(next));
}

bool closePanel(Layout& layout, const QString& panelId) {
  if (panelId == kPicture || groupOf(layout, panelId) < 0) {
    return false;
  }
  Layout next = layout;
  take(next, panelId);
  return accept(layout, std::move(next));
}

bool openPanel(Layout& layout, const QString& panelId) {
  const PanelInfo* info = findPanel(panelId);
  if (info == nullptr) {
    return false;
  }
  if (groupOf(layout, panelId) >= 0) {
    return setCurrentPanel(layout, panelId);
  }
  Layout next = layout;
  const QList<int> members = groupsIn(next, info->home);
  if (members.isEmpty()) {
    next.groups.append(group(info->home, {panelId}, 1.0));
  } else {
    DockGroup& target = next.groups[members.front()];
    target.panelIds.append(panelId);
    target.currentIndex = static_cast<int>(target.panelIds.size()) - 1;
  }
  showArea(next, info->home);
  return accept(layout, std::move(next));
}

bool setCurrentPanel(Layout& layout, const QString& panelId) {
  const int g = groupOf(layout, panelId);
  if (g < 0) {
    return false;
  }
  layout.groups[g].currentIndex = static_cast<int>(layout.groups[g].panelIds.indexOf(panelId));
  return true;
}

}  // namespace dock

// ---- DockModel --------------------------------------------------------------------------------------------------

namespace {
constexpr int kSaveDebounceMs = 500;  // §11.2.2
constexpr int kLayoutsVersion = 1;
}  // namespace

DockModel::DockModel(QObject* parent) : DockModel(Platform::layoutsPath(), parent) {}

DockModel::DockModel(QString storagePath, QObject* parent)
    : QObject(parent), layout_(dock::preset(u"Analyze").value_or(Layout{})), storage_path_(std::move(storagePath)) {
  save_timer_.setSingleShot(true);
  save_timer_.setInterval(kSaveDebounceMs);
  connect(&save_timer_, &QTimer::timeout, this, &DockModel::save);
  load();
}

DockModel::~DockModel() {
  if (save_timer_.isActive()) {
    save();
  }
}

void DockModel::load() {
  if (storage_path_.isEmpty()) {
    return;
  }
  QFile file(storage_path_);
  if (!file.open(QIODevice::ReadOnly)) {
    return;  // first start: the default layout
  }
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  const QJsonValue current = document.object().value(QLatin1StringView("current"));
  if (std::optional<Layout> restored = dock::fromJsonObject(current.toObject(), nullptr); restored) {
    layout_ = std::move(*restored);
  }
}

bool DockModel::save() {
  save_timer_.stop();
  if (storage_path_.isEmpty()) {
    return true;
  }
  // Keep the other keys of layouts.json (the custom layouts of a later work item).
  QJsonObject root;
  if (QFile existing(storage_path_); existing.open(QIODevice::ReadOnly)) {
    root = QJsonDocument::fromJson(existing.readAll()).object();
  }
  root.insert(QStringLiteral("layouts_version"), kLayoutsVersion);
  root.remove(QStringLiteral("current"));
  // The current layout goes in verbatim so that its sizes keep their 4 decimals.
  QByteArray rest = QJsonDocument(root).toJson(QJsonDocument::Compact);
  rest.chop(1);  // the closing brace
  const QByteArray bytes = rest + ",\"current\":" + dock::toJson(layout_) + "}\n";
  QDir().mkpath(QFileInfo(storage_path_).absolutePath());
  QSaveFile file(storage_path_);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
    return false;
  }
  return file.commit();
}

bool DockModel::commit(Layout layout) {
  dock::normalise(layout);
  if (!dock::validate(layout, nullptr)) {
    return false;
  }
  if (layout == layout_) {
    return true;
  }
  layout_ = std::move(layout);
  ++revision_;
  save_timer_.start();
  Q_EMIT layoutChanged();
  return true;
}

bool DockModel::setLayout(Layout layout) {
  return commit(std::move(layout));
}

void DockModel::setLeftVisible(bool visible) {
  Layout next = layout_;
  next.leftVisible = visible;
  commit(std::move(next));
}

void DockModel::setRightVisible(bool visible) {
  Layout next = layout_;
  next.rightVisible = visible;
  commit(std::move(next));
}

void DockModel::setBottomVisible(bool visible) {
  Layout next = layout_;
  next.bottomVisible = visible;
  commit(std::move(next));
}

void DockModel::setLeftSize(qreal size) {
  Layout next = layout_;
  next.leftSize = size;
  commit(std::move(next));
}

void DockModel::setRightSize(qreal size) {
  Layout next = layout_;
  next.rightSize = size;
  commit(std::move(next));
}

void DockModel::setBottomSize(qreal size) {
  Layout next = layout_;
  next.bottomSize = size;
  commit(std::move(next));
}

void DockModel::setPictureOnly(bool on) {
  if (on != picture_only_) {
    picture_only_ = on;
    Q_EMIT pictureOnlyChanged();
  }
}

QVariantList DockModel::groups(const QString& area) const {
  QVariantList result;
  const std::optional<DockArea> wanted = dock::parseArea(area);
  for (int i = 0; wanted && i < layout_.groups.size(); ++i) {
    const DockGroup& g = layout_.groups[i];
    if (g.area == *wanted) {
      result.append(QVariantMap{{QStringLiteral("group"), i},
                                {QStringLiteral("panelIds"), g.panelIds},
                                {QStringLiteral("currentIndex"), g.currentIndex},
                                {QStringLiteral("sizeFraction"), g.sizeFraction}});
    }
  }
  return result;
}

QVariantMap DockModel::areas() const {
  QVariantMap result;
  for (const char* name : {"left", "center", "right", "bottom", "floating"}) {
    result.insert(QString::fromLatin1(name), groups(QString::fromLatin1(name)));
  }
  return result;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a QML method of the singleton
QStringList DockModel::allPanels() const {
  QStringList ids;
  for (const dock::PanelInfo& info : dock::panels()) {
    ids.append(QString::fromLatin1(info.id));
  }
  return ids;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): a QML method of the singleton
QString DockModel::panelTitle(const QString& panelId) const {
  const dock::PanelInfo* info = dock::findPanel(panelId);
  return info == nullptr ? panelId : QCoreApplication::translate("Dock", info->title);
}

bool DockModel::applyPreset(const QString& name) {
  std::optional<Layout> layout = dock::preset(name);
  return layout && commit(std::move(*layout));
}

void DockModel::resetLayout() {
  applyPreset(QStringLiteral("Analyze"));
  setPictureOnly(false);
}

bool DockModel::movePanelToGroup(const QString& panelId, int group, int tabIndex) {
  Layout next = layout_;
  return dock::movePanelToGroup(next, panelId, group, tabIndex) && commit(std::move(next));
}

bool DockModel::movePanelToNewGroup(const QString& panelId, const QString& area, int position) {
  const std::optional<DockArea> parsed = dock::parseArea(area);
  Layout next = layout_;
  return parsed && dock::movePanelToNewGroup(next, panelId, *parsed, position) && commit(std::move(next));
}

bool DockModel::floatPanel(const QString& panelId) {
  Layout next = layout_;
  return dock::movePanelToNewGroup(next, panelId, DockArea::Floating, -1) && commit(std::move(next));
}

bool DockModel::closePanel(const QString& panelId) {
  Layout next = layout_;
  return dock::closePanel(next, panelId) && commit(std::move(next));
}

bool DockModel::openPanel(const QString& panelId) {
  Layout next = layout_;
  return dock::openPanel(next, panelId) && commit(std::move(next));
}

bool DockModel::togglePanel(const QString& panelId) {
  return dock::groupOf(layout_, panelId) >= 0 ? closePanel(panelId) : openPanel(panelId);
}

bool DockModel::setCurrentPanel(const QString& panelId) {
  Layout next = layout_;
  return dock::setCurrentPanel(next, panelId) && commit(std::move(next));
}

bool DockModel::setCurrentIndex(int group, int index) {
  if (group < 0 || group >= layout_.groups.size() || index < 0 || index >= layout_.groups[group].panelIds.size()) {
    return false;
  }
  Layout next = layout_;
  next.groups[group].currentIndex = index;
  return commit(std::move(next));
}

QString DockModel::cycleTab(const QString& panelId, int delta) {
  const int g = dock::groupOf(layout_, panelId);
  if (g < 0) {
    return panelId;
  }
  const QStringList ids = layout_.groups[g].panelIds;  // a copy: setCurrentIndex replaces layout_
  const auto n = static_cast<int>(ids.size());
  const int next = (((static_cast<int>(ids.indexOf(panelId)) + delta) % n) + n) % n;
  setCurrentIndex(g, next);
  return ids[next];
}

void DockModel::resetSizes() {
  const Layout defaults = dock::preset(layout_.name).value_or(Layout{});
  Layout next = layout_;
  next.leftSize = defaults.leftSize;
  next.rightSize = defaults.rightSize;
  next.bottomSize = defaults.bottomSize;
  for (DockGroup& g : next.groups) {
    g.sizeFraction = 1.0;  // normalise() makes them equal per area
  }
  commit(std::move(next));
}

void DockModel::setSizeFractions(const QString& area, const QVariantList& sizes) {
  const std::optional<DockArea> parsed = dock::parseArea(area);
  if (!parsed) {
    return;
  }
  Layout next = layout_;
  int k = 0;
  for (DockGroup& g : next.groups) {
    if (g.area == *parsed && k < sizes.size()) {
      g.sizeFraction = sizes[k++].toDouble();
    }
  }
  commit(std::move(next));
}

bool DockModel::restoreJson(const QString& json) {
  std::optional<Layout> layout = dock::fromJson(json.toUtf8(), nullptr);
  return layout && commit(std::move(*layout));
}

}  // namespace ovt::app
