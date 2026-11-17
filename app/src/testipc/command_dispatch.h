// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command set of the test IPC (chapter 14 §14.10.3, WI-1.465). CommandDispatch turns one request object
// `{"id", "cmd", "args"}` into one response object `{"id", "ok", "value"}` or `{"id", "ok": false, "error"}` and runs
// on the GUI thread. It knows nothing about sockets (TestIpcServer owns those), so the tests drive it directly.
//
//   open <path>                       Project.openFile; value = {fileCount, activeFile}
//   click <objectName>                click the centre of the item
//   click <objectName> <row>          click row <row> of a list (ListView.itemAtIndex) or of the timeline (xOfFrame)
//   click <objectName> <x> <y>        click at item-local (x, y); mapped to scene coordinates before it is sent
//   select-syntax-node <path>         call selectByPath(path) of the object named "syntaxTree"
//   key <sequence>                    QKeySequence portable text ("J", "Ctrl+G", "Ctrl+K, Ctrl+C")
//   get <objectName>.<property>[.<key>...]   a property, then keys of maps, indexes of lists, properties of objects;
//                                     an upper-case first segment names a QML singleton (get Project.fileCount)
//   screenshot <path>                 QQuickWindow::grabWindow() as PNG; value = {path, width, height}
//   wait-idle [timeout_ms]            no running job (JobsModel.activeCount == 0) and no frame swapped for three
//                                     polls of 20 ms; error "timeout" after timeout_ms (default 10000)
//
// Items are found by objectName under the QML engine's root objects (the QObject tree first, then the item tree, so
// delegates are found too). Mouse and key events go through QTest, so they take the path of real input; coordinates
// handed to QTest are scene coordinates (QQuickItem::mapToScene), never item-local ones.

#ifndef OVT_APP_TESTIPC_COMMAND_DISPATCH_H
#define OVT_APP_TESTIPC_COMMAND_DISPATCH_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariant>

#include <functional>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;

namespace ovt::app::testipc {

using Reply = std::function<void(const QJsonObject&)>;

class CommandDispatch : public QObject {
  Q_OBJECT

 public:
  static constexpr int kDefaultWaitIdleMs = 10000;
  static constexpr int kIdlePollMs = 20;
  static constexpr int kQuietPolls = 3;
  static constexpr int kActivateWaitMs = 1000;

  CommandDispatch(QQmlApplicationEngine* engine, QQuickWindow* window, QObject* parent = nullptr);

  // Runs one request and calls `reply` exactly once: at once, except for wait-idle, which answers when the
  // application is idle or the timeout expires. A request without an integer id gets id -1.
  void execute(const QJsonObject& request, const Reply& reply);

  static QJsonObject okResponse(int id, const QJsonValue& value);
  static QJsonObject errorResponse(int id, const QString& message);

 private:
  struct Outcome {
    bool ok = true;
    QJsonValue value;
    QString error;
  };
  static Outcome fail(const QString& message);
  static Outcome succeed(const QJsonValue& value = QJsonValue());

  Outcome open(const QJsonArray& args);
  Outcome click(const QJsonArray& args);
  Outcome selectSyntaxNode(const QJsonArray& args);
  Outcome key(const QJsonArray& args);
  Outcome get(const QJsonArray& args);
  Outcome screenshot(const QJsonArray& args);
  void waitIdle(const QJsonArray& args, int id, const Reply& reply);

  [[nodiscard]] QObject* findObject(const QString& name) const;
  [[nodiscard]] QObject* singleton(const QString& type_name) const;
  [[nodiscard]] bool jobsIdle() const;

  QPointer<QQmlApplicationEngine> engine_;
  QPointer<QQuickWindow> window_;
  int frames_swapped_ = 0;
};

// QVariant → JSON for `get`. Fails (returns false, fills `error`) for objects and types without a JSON form.
bool variantToJson(const QVariant& value, QJsonValue* out, QString* error);

}  // namespace ovt::app::testipc

#endif  // OVT_APP_TESTIPC_COMMAND_DISPATCH_H
