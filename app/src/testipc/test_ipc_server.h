// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The socket side of the test IPC (chapter 14 §14.10.3, WI-1.465): a QLocalServer on the path given with
// `--test-ipc <socket>`, JSON lines in both directions, one response per request in request order. The server is
// compiled only into builds configured with -DOVT_ENABLE_TEST_IPC=ON (default OFF; app/CMakeLists.txt), and
// main.cpp creates it only when the flag is on the command line, so a release build has no such socket and a debug
// build has one only on request. The socket is private to the user (QLocalServer::UserAccessOption).

#ifndef OVT_APP_TESTIPC_TEST_IPC_SERVER_H
#define OVT_APP_TESTIPC_TEST_IPC_SERVER_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QObject>
#include <QString>

#include "testipc/command_dispatch.h"

class QLocalSocket;
class QQmlApplicationEngine;
class QQuickWindow;

namespace ovt::app::testipc {

class TestIpcServer : public QObject {
  Q_OBJECT

 public:
  // A request line longer than this closes the connection (a missing newline must not grow the buffer forever).
  static constexpr qsizetype kMaxLineBytes = 1 << 20;

  TestIpcServer(QQmlApplicationEngine* engine, QQuickWindow* window, QObject* parent = nullptr);
  ~TestIpcServer() override;
  TestIpcServer(const TestIpcServer&) = delete;
  TestIpcServer& operator=(const TestIpcServer&) = delete;

  // Starts listening on `socket_path`, replacing a stale socket file of an earlier run. False with errorString().
  bool listen(const QString& socket_path);
  [[nodiscard]] QString socketPath() const { return server_.fullServerName(); }
  [[nodiscard]] QString errorString() const { return error_.isEmpty() ? server_.errorString() : error_; }

 private:
  struct Connection {
    QByteArray buffer;
    QList<QByteArray> pending;  // complete lines not yet executed
    bool busy = false;          // a request of this connection is waiting for its reply
  };

  void onNewConnection();
  void onReadyRead(QLocalSocket* socket);
  void pump(QLocalSocket* socket);
  static void write(QLocalSocket* socket, const QJsonObject& response);

  QLocalServer server_;
  QString error_;
  CommandDispatch dispatch_;
  QHash<QLocalSocket*, Connection> connections_;
};

}  // namespace ovt::app::testipc

#endif  // OVT_APP_TESTIPC_TEST_IPC_SERVER_H
