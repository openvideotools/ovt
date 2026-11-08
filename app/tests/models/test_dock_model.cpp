// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the dock model (WI-1.451; chapter 11 §11.2.2). Acceptance test 1: the Analyze preset serialised and
// restored gives identical JSON and equal groups. Acceptance test 2 (model half): a tab dropped on the right edge
// makes a new Right group with that panel; tst_dock.qml drives the same through a real drag.

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QSignalSpy>
#include <QString>
#include <QTemporaryDir>

#include "dock/dock_model.h"
#include "qt_test_app.h"

namespace {

using ovt::app::DockArea;
using ovt::app::DockGroup;
using ovt::app::DockModel;
using ovt::app::Layout;
namespace dock = ovt::app::dock;

Layout analyze() {
  return dock::preset(u"Analyze").value_or(Layout{});
}

int groupsIn(const Layout& layout, DockArea area) {
  int n = 0;
  for (const DockGroup& g : layout.groups) {
    n += g.area == area ? 1 : 0;
  }
  return n;
}

}  // namespace

TEST_CASE("Every preset is valid and round-trips byte-identically", "[fast][gui][dock]") {
  for (const QString& name : dock::presetNames()) {
    INFO(name.toStdString());
    const Layout layout = dock::preset(name).value_or(Layout{});
    QString reason;
    CHECK(dock::validate(layout, &reason));
    CHECK(reason.isEmpty());
    Layout normalised = layout;
    dock::normalise(normalised);
    CHECK(normalised == layout);  // presets are stored normalised
    const QByteArray json = dock::toJson(layout);
    const Layout restored = dock::fromJson(json, &reason).value_or(Layout{});
    CHECK(restored == layout);
    CHECK(dock::toJson(restored) == json);
  }
  CHECK_FALSE(dock::preset(u"Nonexistent"));
}

TEST_CASE("AT1: Analyze serialised and restored gives identical JSON and equal groups",
          "[fast][gui][dock][acceptance]") {
  const Layout layout = analyze();
  const QByteArray json = dock::toJson(layout);
  const Layout restored = dock::fromJson(json, nullptr).value_or(Layout{});
  CHECK(dock::toJson(restored) == json);
  CHECK(restored.groups == layout.groups);
  // sizeFraction is written with exactly 4 decimals.
  CHECK(json.contains("\"sizeFraction\":0.6000"));
  CHECK(json.startsWith("{\"preset\":\"Analyze\","));
}

TEST_CASE("AT2: a tab dropped on the right edge makes a new Right group with that panel",
          "[fast][gui][dock][acceptance]") {
  Layout layout = analyze();
  const int before = groupsIn(layout, DockArea::Right);
  REQUIRE(dock::movePanelToNewGroup(layout, QStringLiteral("hex"), DockArea::Right, -1));
  CHECK(groupsIn(layout, DockArea::Right) == before + 1);
  const int g = dock::groupOf(layout, u"hex");
  REQUIRE(g >= 0);
  CHECK(layout.groups[g].area == DockArea::Right);
  CHECK(layout.groups[g].panelIds == QStringList{QStringLiteral("hex")});
  // 1/(n+1) for the new group, the others scaled; the area still sums to 1 within rounding.
  CHECK(layout.groups[g].sizeFraction == 0.25);
  CHECK(dock::validate(layout, nullptr));
}

TEST_CASE("Moving tabs between groups", "[fast][gui][dock]") {
  Layout layout = analyze();
  const int syntax = dock::groupOf(layout, u"syntax");
  REQUIRE(dock::movePanelToGroup(layout, QStringLiteral("units"), syntax, 0));
  // The Units group became empty and is gone; Container now fills Left.
  CHECK(groupsIn(layout, DockArea::Left) == 1);
  const int target = dock::groupOf(layout, u"syntax");
  CHECK(dock::groupOf(layout, u"units") == target);
  CHECK(layout.groups[target].panelIds == QStringList{QStringLiteral("units"), QStringLiteral("syntax")});
  CHECK(layout.groups[target].currentIndex == 0);
  const int left = dock::groupOf(layout, u"container");
  CHECK(layout.groups[left].sizeFraction == 1.0);

  // Reorder within the same group: moving tab 0 to the end.
  REQUIRE(dock::movePanelToGroup(layout, QStringLiteral("units"), target, 2));
  CHECK(layout.groups[target].panelIds == QStringList{QStringLiteral("syntax"), QStringLiteral("units")});
}

TEST_CASE("The Picture never leaves Center and Center keeps one group", "[fast][gui][dock]") {
  Layout layout = analyze();
  const Layout before = layout;
  const int right = dock::groupOf(layout, u"syntax");
  CHECK_FALSE(dock::movePanelToGroup(layout, QStringLiteral("picture"), right, 0));
  CHECK_FALSE(dock::movePanelToNewGroup(layout, QStringLiteral("picture"), DockArea::Bottom, 0));
  CHECK_FALSE(dock::movePanelToNewGroup(layout, QStringLiteral("hex"), DockArea::Center, 0));
  CHECK_FALSE(dock::closePanel(layout, QStringLiteral("picture")));
  CHECK(layout == before);
  // Other panels may share the Picture group as tabs (§11.2.2: e.g. Ref graph).
  const int center = dock::groupOf(layout, u"picture");
  REQUIRE(dock::movePanelToGroup(layout, QStringLiteral("hex"), center, 1));
  CHECK(layout.groups[dock::groupOf(layout, u"hex")].area == DockArea::Center);
}

TEST_CASE("Close and re-open panels from the View menu", "[fast][gui][dock]") {
  Layout layout = analyze();
  REQUIRE(dock::closePanel(layout, QStringLiteral("hex")));
  CHECK(dock::groupOf(layout, u"hex") < 0);
  CHECK_FALSE(dock::closePanel(layout, QStringLiteral("hex")));
  CHECK_FALSE(dock::openPanels(layout).contains(QStringLiteral("hex")));
  // Re-opened as a tab of the first group of its home area, and made current.
  REQUIRE(dock::openPanel(layout, QStringLiteral("hex")));
  const int g = dock::groupOf(layout, u"hex");
  CHECK(layout.groups[g].area == DockArea::Bottom);
  CHECK(layout.groups[g].panelIds.indexOf(QStringLiteral("hex")) == layout.groups[g].currentIndex);
  CHECK_FALSE(dock::openPanel(layout, QStringLiteral("nonexistent")));

  // A panel whose home area is empty gets a new group there, and the area is shown.
  Layout syntax = dock::preset(u"Syntax").value_or(Layout{});
  CHECK_FALSE(syntax.leftVisible);
  REQUIRE(dock::openPanel(syntax, QStringLiteral("bookmarks")));
  CHECK(syntax.leftVisible);
}

TEST_CASE("Tab order for Ctrl+Shift+[1-9]", "[fast][gui][dock]") {
  const QStringList order = dock::tabOrder(analyze());
  CHECK(order ==
        QStringList{"container", "units", "picture", "syntax", "selection_info", "block_samples", "timeline", "hex"});
}

TEST_CASE("Floating groups exist only on the desktop", "[fast][gui][dock]") {
  Layout layout = analyze();
  const bool floated = dock::movePanelToNewGroup(layout, QStringLiteral("hex"), DockArea::Floating, -1);
  CHECK(floated == dock::floatingSupported());
  if (floated) {
    const QByteArray json = dock::toJson(layout);
    CHECK(json.contains("\"area\":\"floating\""));
    CHECK(dock::fromJson(json, nullptr) == layout);
  }
}

TEST_CASE("Invalid JSON is rejected with a reason; unknown panels are dropped", "[fast][gui][dock]") {
  QString error;
  CHECK_FALSE(dock::fromJson("not json", &error));
  CHECK_FALSE(error.isEmpty());
  CHECK_FALSE(dock::fromJson("[]", &error));
  CHECK_FALSE(dock::fromJson("{\"groups\":[]}", &error));  // no Center
  CHECK(error.contains(QStringLiteral("Center")));
  CHECK_FALSE(dock::fromJson(R"({"groups":[{"area":"middle","panelIds":["picture"]}]})", &error));
  CHECK_FALSE(dock::fromJson(
      R"({"groups":[{"area":"center","panelIds":["picture"]},{"area":"left","panelIds":["picture"]}]})", &error));
  CHECK(error.contains(QStringLiteral("twice")));

  const Layout layout =
      dock::fromJson(
          R"({"preset":"x","groups":[{"area":"center","panelIds":["picture","future_panel"],"currentIndex":7,)"
          R"("sizeFraction":-3},{"area":"right","panelIds":["future_panel"]},)"
          R"({"area":"bottom","panelIds":["hex"],"sizeFraction":3},{"area":"bottom","panelIds":["timeline"],)"
          R"("sizeFraction":1}],"leftSize":7})",
          &error)
          .value_or(Layout{});
  REQUIRE(layout.groups.size() == 3);  // the empty Right group is dropped
  CHECK(layout.groups[0].panelIds == QStringList{QStringLiteral("picture")});
  CHECK(layout.groups[0].currentIndex == 0);
  CHECK(layout.groups[0].sizeFraction == 1.0);
  CHECK(layout.groups[1].sizeFraction == 0.75);
  CHECK(layout.groups[2].sizeFraction == 0.25);
  CHECK(layout.leftSize == 0.9);  // clamped
}

TEST_CASE("DockModel persists the layout to layouts.json, debounced", "[fast][gui][dock]") {
  ovt::app::testing::qt_app();
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString path = dir.filePath(QStringLiteral("layouts.json"));
  {
    DockModel model(path, nullptr);
    CHECK(model.layoutName() == QStringLiteral("Analyze"));
    const QSignalSpy changed(&model, &DockModel::layoutChanged);
    REQUIRE(model.applyPreset(QStringLiteral("Syntax")));
    CHECK(changed.count() == 1);
    CHECK_FALSE(model.leftVisible());
    REQUIRE(model.movePanelToNewGroup(QStringLiteral("hex"), QStringLiteral("right"), 0));
    CHECK(model.groups(QStringLiteral("right")).size() == 2);
    CHECK(model.groups(QStringLiteral("bottom")).isEmpty());
    CHECK_FALSE(model.movePanelToNewGroup(QStringLiteral("hex"), QStringLiteral("sideways"), 0));
    // Not written yet; written once the 500 ms debounce has passed.
    CHECK_FALSE(QFile::exists(path));
    CHECK(ovt::app::testing::spin_until([&] { return QFile::exists(path); }, std::chrono::milliseconds(3000)));
    REQUIRE(
        model.setCurrentIndex(model.groups(QStringLiteral("right"))[1].toMap()[QStringLiteral("group")].toInt(), 0));
    CHECK(model.togglePanel(QStringLiteral("metrics")));
    CHECK(model.openPanels().contains(QStringLiteral("metrics")));
    model.setPictureOnly(true);
    CHECK(model.pictureOnly());
  }  // the destructor writes the pending change
  DockModel restored(path, nullptr);
  CHECK(restored.layoutName() == QStringLiteral("Syntax"));
  CHECK(restored.openPanels().contains(QStringLiteral("metrics")));
  CHECK_FALSE(restored.pictureOnly());  // not part of the layout
  QFile file(path);
  REQUIRE(file.open(QIODevice::ReadOnly));
  const QByteArray bytes = file.readAll();
  CHECK(bytes.startsWith("{\"layouts_version\":1,\"current\":{\"preset\":\"Syntax\""));
  CHECK(bytes.endsWith("}\n"));

  restored.resetLayout();
  CHECK(restored.layout() == analyze());
  CHECK(restored.restoreJson(QString::fromUtf8(dock::toJson(dock::preset(u"HDR").value_or(Layout{})))));
  CHECK(restored.layoutName() == QStringLiteral("HDR"));
  CHECK_FALSE(restored.restoreJson(QStringLiteral("{}")));
}

TEST_CASE("DockModel falls back to Analyze for a corrupt layouts.json", "[fast][gui][dock]") {
  const QTemporaryDir dir;
  const QString path = dir.filePath(QStringLiteral("layouts.json"));
  QFile file(path);
  REQUIRE(file.open(QIODevice::WriteOnly));
  file.write("{\"current\": 42");
  file.close();
  const DockModel model(path, nullptr);
  CHECK(model.layout() == analyze());
  CHECK(model.panelTitle(QStringLiteral("selection_info")) == QStringLiteral("Selection Info"));
  CHECK(model.allPanels().size() == 17);
}

TEST_CASE("DockModel helpers for QML: areas, cycleTab, resetSizes", "[fast][gui][dock]") {
  DockModel model(QString(), nullptr);  // in memory only
  const QVariantMap areas = model.areas();
  CHECK(areas.value(QStringLiteral("right")).toList().size() == 3);
  CHECK(areas.value(QStringLiteral("center")).toList().size() == 1);
  CHECK(areas.value(QStringLiteral("floating")).toList().isEmpty());

  REQUIRE(model.movePanelToGroup(QStringLiteral("units"), model.groupOf(QStringLiteral("syntax")), 1));
  CHECK(model.cycleTab(QStringLiteral("syntax"), 1) == QStringLiteral("units"));
  CHECK(model.cycleTab(QStringLiteral("units"), 1) == QStringLiteral("syntax"));
  CHECK(model.cycleTab(QStringLiteral("syntax"), -1) == QStringLiteral("units"));
  CHECK(model.cycleTab(QStringLiteral("hdr"), 1) == QStringLiteral("hdr"));  // not open

  model.setRightSize(0.5);
  model.setSizeFractions(QStringLiteral("right"), {QVariant(2.0), QVariant(1.0), QVariant(1.0)});
  CHECK(model.rightSize() == 0.5);
  CHECK(model.groups(QStringLiteral("right"))[0].toMap()[QStringLiteral("sizeFraction")].toDouble() == 0.5);
  model.resetSizes();
  CHECK(model.rightSize() == 0.24);
  CHECK(model.groups(QStringLiteral("right"))[0].toMap()[QStringLiteral("sizeFraction")].toDouble() == 0.3333);
  CHECK(model.save());  // no path: nothing to write
}

TEST_CASE("normalise() is idempotent", "[fast][gui][dock]") {
  // Shares that do not divide evenly into 4 decimals, a tiny share and an unnormalised area.
  Layout layout = analyze();
  for (const QString& id : {QStringLiteral("hdr"), QStringLiteral("metrics"), QStringLiteral("statistics")}) {
    REQUIRE(dock::openPanel(layout, id));
    REQUIRE(dock::movePanelToNewGroup(layout, id, DockArea::Right, -1));
  }
  const QList<qreal> sizes{0.7, 0.00001, 1.0 / 3.0, 2.0 / 7.0, 0.123456, 5.0};
  int k = 0;
  for (DockGroup& g : layout.groups) {
    if (g.area == DockArea::Right) {
      g.sizeFraction = sizes.value(k++, 1.0);
    }
  }
  dock::normalise(layout);
  const Layout once = layout;
  dock::normalise(layout);
  CHECK(layout == once);
  CHECK(dock::validate(layout, nullptr));
  for (const DockGroup& g : layout.groups) {
    CHECK(g.sizeFraction > 0);
  }
  // And for every preset after a round trip.
  for (const QString& name : dock::presetNames()) {
    Layout preset = dock::fromJson(dock::toJson(dock::preset(name).value_or(Layout{})), nullptr).value_or(Layout{});
    const Layout before = preset;
    dock::normalise(preset);
    CHECK(preset == before);
  }
}

TEST_CASE("foldFloating docks floating panels in their home areas", "[fast][gui][dock]") {
  Layout layout = analyze();
  layout.groups.append(DockGroup{.area = DockArea::Floating,
                                 .panelIds = {QStringLiteral("metrics"), QStringLiteral("hex")},
                                 .currentIndex = 0,
                                 .sizeFraction = 1.0});
  layout.groups.removeIf([](const DockGroup& g) { return g.panelIds == QStringList{QStringLiteral("hex")}; });
  dock::foldFloating(layout);
  dock::normalise(layout);
  CHECK(dock::validate(layout, nullptr));
  for (const DockGroup& g : layout.groups) {
    CHECK(g.area != DockArea::Floating);
  }
  CHECK(layout.groups[dock::groupOf(layout, u"metrics")].area == DockArea::Right);
  CHECK(layout.groups[dock::groupOf(layout, u"hex")].area == DockArea::Bottom);
}
