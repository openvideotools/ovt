// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.465: the test IPC of chapter 14 §14.10.3 against a small QML scene. CommandDispatch is driven directly; the
// socket server through a QLocalSocket of this process. The real application is covered by
// app/tests/scripts/test_test_ipc.py. Offscreen platform, software scene graph, so no GPU and no display.

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTemporaryDir>
#include <QUrl>
#include <QtGlobal>
#include <QtTest/QTest>

#include <array>
#include <memory>
#include <optional>

#include "testipc/command_dispatch.h"
#include "testipc/test_ipc_server.h"

using ovt::app::testipc::CommandDispatch;
using ovt::app::testipc::TestIpcServer;

namespace {

constexpr const char* kScene = R"(
import QtQuick
import QtQuick.Window

Window {
    id: win
    objectName: "win"
    width: 240
    height: 200
    visible: true
    property var info: ({"a": {"b": 3}, "list": [10, 20]})
    property int rowClicks: 0
    property int lastRow: -1
    property string lastKey: ""
    property rect box: Qt.rect(1, 2, 3, 4)

    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: event => { win.lastKey = event.key + ":" + event.modifiers }

        Rectangle {
            objectName: "box"
            x: 100; y: 100; width: 100; height: 50
            color: "red"
            property int clicks: 0
            property point lastPos: Qt.point(-1, -1)
            MouseArea {
                anchors.fill: parent
                onClicked: mouse => { parent.clicks++; parent.lastPos = Qt.point(mouse.x, mouse.y) }
            }
        }

        ListView {
            objectName: "list"
            x: 0; y: 0; width: 90; height: 100
            model: 5
            delegate: Rectangle {
                width: 90; height: 20
                color: index % 2 ? "grey" : "white"
                MouseArea {
                    anchors.fill: parent
                    onClicked: { win.rowClicks++; win.lastRow = index }
                }
            }
        }
    }
}
)";

// The application lives until the process ends and is never destroyed: Qt Quick's own statics are gone by the time
// static destruction would reach a function-local QGuiApplication, and that crashes on exit (also of --list-tests).
QGuiApplication& gui_app() {
  static int argc = 1;
  static std::array<char, 20> name{"ovt_tests_testipc"};
  static std::array<char*, 2> argv{name.data(), nullptr};
  static QGuiApplication* app = [] {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    return new QGuiApplication(argc, argv.data());  // NOLINT(cppcoreguidelines-owning-memory)
  }();
  return *app;
}

// Created before main(): the application must exist before any QQuickWindow.
[[maybe_unused]] QGuiApplication& const_app = gui_app();

struct Scene {
  QQmlApplicationEngine engine;
  QQuickWindow* window = nullptr;
  std::unique_ptr<CommandDispatch> dispatch;

  Scene() {
    engine.loadData(QByteArray(kScene), QUrl(QStringLiteral("inline:scene.qml")));
    // loadData() completes from the event loop (Qt 6).
    QElapsedTimer load_clock;
    load_clock.start();
    while (engine.rootObjects().isEmpty() && load_clock.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    REQUIRE(!engine.rootObjects().isEmpty());
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    REQUIRE(window != nullptr);
    REQUIRE(QTest::qWaitForWindowExposed(window));
    dispatch = std::make_unique<CommandDispatch>(&engine, window);
  }

  // Sends one request and returns the response (a late reply, wait-idle, is awaited for up to 5 s).
  [[nodiscard]] QJsonObject call(const QString& cmd, const QJsonArray& args = {}, int id = 1) const {
    std::optional<QJsonObject> response;
    dispatch->execute(QJsonObject{{"id", id}, {"cmd", cmd}, {"args", args}},
                      [&response](const QJsonObject& r) { response = r; });
    QElapsedTimer clock;
    clock.start();
    while (!response && clock.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    REQUIRE(response.has_value());
    const QJsonObject result = response.value_or(QJsonObject());
    CHECK(result.value("id").toInt() == id);
    return result;
  }

  [[nodiscard]] QJsonValue value(const QString& cmd, const QJsonArray& args = {}) const {
    const QJsonObject r = call(cmd, args);
    INFO(QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString());
    REQUIRE(r.value("ok").toBool());
    return r.value("value");
  }

  // A command whose value is not of interest; it must succeed.
  void run(const QString& cmd, const QJsonArray& args = {}) const {
    const QJsonObject r = call(cmd, args);
    INFO(QJsonDocument(r).toJson(QJsonDocument::Compact).toStdString());
    REQUIRE(r.value("ok").toBool());
  }

  [[nodiscard]] QString error(const QString& cmd, const QJsonArray& args = {}) const {
    const QJsonObject r = call(cmd, args);
    REQUIRE_FALSE(r.value("ok").toBool());
    return r.value("error").toString();
  }
};

// Reads response lines from `client` until `count` have arrived or `limit_ms` passed.
QList<QJsonObject> readReplies(QLocalSocket& client, qsizetype count, qint64 limit_ms) {
  QList<QJsonObject> replies;
  QByteArray buffer;
  QElapsedTimer clock;
  clock.start();
  while (replies.size() < count && clock.elapsed() < limit_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    buffer.append(client.readAll());
    qsizetype newline = 0;
    while ((newline = buffer.indexOf('\n')) >= 0) {
      replies.append(QJsonDocument::fromJson(buffer.left(newline)).object());
      buffer.remove(0, newline + 1);
    }
  }
  return replies;
}

}  // namespace

TEST_CASE("get reads properties, nested keys, list indexes and geometry types", "[fast][gui][testipc]") {
  const Scene scene;
  CHECK(scene.value("get", {"box.width"}).toInt() == 100);
  CHECK(scene.value("get", {"win.info.a.b"}).toInt() == 3);
  CHECK(scene.value("get", {"win.info.list.1"}).toInt() == 20);
  const QJsonObject rect = scene.value("get", {"win.box"}).toObject();
  CHECK(rect.value("width").toInt() == 3);
  CHECK(rect.value("y").toInt() == 2);
  CHECK(scene.value("get", {"box.color"}).toString() == "#ff0000");
}

TEST_CASE("get fails with a message for what it cannot read", "[fast][gui][testipc]") {
  const Scene scene;
  CHECK(scene.error("get", {"nothing.width"}).contains("no object named 'nothing'"));
  CHECK(scene.error("get", {"box.nothing"}).contains("no property 'nothing'"));
  CHECK(scene.error("get", {"win.info.zzz"}).contains("no key 'zzz'"));
  CHECK(scene.error("get", {"win.info.list.7"}).contains("not an index"));
  CHECK(scene.error("get", {"box"}).contains("<objectName>.<property>"));
  CHECK(scene.error("get", {"box.parent"}).contains("is an object"));
}

TEST_CASE("click sends scene coordinates: item-local x, y are mapped before they reach the window",
          "[fast][gui][testipc]") {
  const Scene scene;
  // box sits at (100, 100): item-local (10, 5) is scene (110, 105). Sent as local coordinates it would hit nothing.
  scene.run("click", {"box", 10, 5});
  CHECK(scene.value("get", {"box.clicks"}).toInt() == 1);
  const QJsonObject pos = scene.value("get", {"box.lastPos"}).toObject();
  CHECK(pos.value("x").toInt() == 10);
  CHECK(pos.value("y").toInt() == 5);
  scene.run("click", {"box"});  // the centre
  CHECK(scene.value("get", {"box.clicks"}).toInt() == 2);
  CHECK(scene.value("get", {"box.lastPos"}).toObject().value("x").toInt() == 50);
}

TEST_CASE("click <list> <row> clicks that delegate, also one outside the visible rows", "[fast][gui][testipc]") {
  const Scene scene;
  scene.run("click", {"list", 2});
  CHECK(scene.value("get", {"win.lastRow"}).toInt() == 2);
  scene.run("click", {"list", 4});
  CHECK(scene.value("get", {"win.lastRow"}).toInt() == 4);
  CHECK(scene.value("get", {"win.rowClicks"}).toInt() == 2);
  CHECK(scene.error("click", {"list", 40}).contains("no row 40"));
}

TEST_CASE("click rejects bad arguments and unreachable points", "[fast][gui][testipc]") {
  const Scene scene;
  CHECK(scene.error("click", {}).contains("usage"));
  CHECK(scene.error("click", {"box", "x"}).contains("usage"));
  CHECK(scene.error("click", {"missing"}).contains("no object named"));
  CHECK(scene.error("click", {"box", 500, 5}).contains("outside the window"));
  CHECK(scene.error("click", {"box", 5}).contains("has no rows"));
}

TEST_CASE("key delivers a key sequence to the focused item", "[fast][gui][testipc]") {
  const Scene scene;
  scene.run("key", {"J"});
  CHECK(scene.value("get", {"win.lastKey"}).toString().startsWith(QString::number(Qt::Key_J) + ":"));
  scene.run("key", {"Ctrl+G"});
  CHECK(scene.value("get", {"win.lastKey"}).toString() ==
        QString::number(Qt::Key_G) + ":" + QString::number(static_cast<int>(Qt::ControlModifier)));
  CHECK(scene.error("key", {""}).contains("not a key sequence"));
}

TEST_CASE("screenshot writes a PNG of the window", "[fast][gui][testipc]") {
  const Scene scene;
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString path = dir.filePath("shot.png");
  const QJsonObject v = scene.value("screenshot", {path}).toObject();
  const QImage image(path);
  REQUIRE_FALSE(image.isNull());
  CHECK(image.width() == v.value("width").toInt());
  CHECK(image.width() >= 240);
  CHECK(scene.error("screenshot", {dir.filePath("missing/shot.png")}).contains("cannot write"));
}

TEST_CASE("wait-idle answers when no frame is swapped any more, and validates its argument", "[fast][gui][testipc]") {
  const Scene scene;
  CHECK(scene.call("wait-idle", {5000}).value("ok").toBool());
  CHECK(scene.call("wait-idle").value("ok").toBool());
  CHECK(scene.error("wait-idle", {"soon"}).contains("usage"));
}

TEST_CASE("commands that need the application report what is missing", "[fast][gui][testipc]") {
  const Scene scene;
  CHECK(scene.error("open", {"x.264"}).contains("Project singleton is not available"));
  CHECK(scene.error("select-syntax-node", {"units[0]"}).contains("syntaxTree"));
  CHECK(scene.error("dance").contains("unknown command 'dance'"));
  const QJsonObject r = scene.call("get", {});
  CHECK_FALSE(r.value("ok").toBool());
}

TEST_CASE("the server answers JSON lines in request order over the socket", "[fast][gui][testipc]") {
  Scene scene;
  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString path = dir.filePath("ipc.sock");
  TestIpcServer server(&scene.engine, scene.window);
  CHECK_FALSE(QFileInfo::exists(path));  // not listening: no socket
  REQUIRE(server.listen(path));
  CHECK(QFileInfo::exists(path));

  QLocalSocket client;
  client.connectToServer(path);
  REQUIRE(client.waitForConnected(2000));
  // Three requests in one write; wait-idle answers late but the replies keep the request order. The garbage line
  // between them gets an error with id -1.
  client.write(
      "{\"id\":1,\"cmd\":\"wait-idle\",\"args\":[3000]}\n"
      "this is not json\n"
      "{\"id\":3,\"cmd\":\"get\",\"args\":[\"box.width\"]}\n");
  const QList<QJsonObject> replies = readReplies(client, 3, 8000);
  REQUIRE(replies.size() == 3);
  CHECK(replies[0].value("id").toInt() == 1);
  CHECK(replies[0].value("ok").toBool());
  CHECK(replies[1].value("id").toInt() == -1);
  CHECK_FALSE(replies[1].value("ok").toBool());
  CHECK(replies[2].value("id").toInt() == 3);
  CHECK(replies[2].value("value").toInt() == 100);

  client.disconnectFromServer();
}
