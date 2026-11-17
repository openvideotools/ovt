// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testipc/test_ipc_server.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalSocket>
#include <QPointer>

#include <memory>

namespace ovt::app::testipc {

TestIpcServer::TestIpcServer(QQmlApplicationEngine* engine, QQuickWindow* window, QObject* parent)
    : QObject(parent), server_(this), dispatch_(engine, window, this) {
  server_.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&server_, &QLocalServer::newConnection, this, &TestIpcServer::onNewConnection);
}

TestIpcServer::~TestIpcServer() {
  // The sockets are children and die after the members: their disconnected lambdas must not reach connections_ then.
  for (auto it = connections_.cbegin(); it != connections_.cend(); ++it) {
    it.key()->disconnect(this);
  }
}

bool TestIpcServer::listen(const QString& socket_path) {
  // removeServer() unlinks whatever is at the path: only a stale socket of an earlier run may go.
  const QFileInfo existing(socket_path);
  if (existing.isSymLink() || existing.isFile() || existing.isDir()) {
    error_ = QStringLiteral("%1 exists and is not a socket").arg(socket_path);
    return false;
  }
  error_.clear();
  QLocalServer::removeServer(socket_path);
  return server_.listen(socket_path);
}

void TestIpcServer::onNewConnection() {
  while (QLocalSocket* socket = server_.nextPendingConnection()) {
    connections_.insert(socket, Connection{});
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
      connections_.remove(socket);
      socket->deleteLater();
    });
  }
}

void TestIpcServer::onReadyRead(QLocalSocket* socket) {
  auto it = connections_.find(socket);
  if (it == connections_.end()) {
    return;
  }
  it->buffer.append(socket->readAll());
  qsizetype newline = 0;
  while ((newline = it->buffer.indexOf('\n')) >= 0) {
    QByteArray line = it->buffer.left(newline).trimmed();
    it->buffer.remove(0, newline + 1);
    if (!line.isEmpty()) {
      it->pending.append(std::move(line));
    }
  }
  if (it->buffer.size() > kMaxLineBytes) {
    socket->disconnectFromServer();
    return;
  }
  pump(socket);
}

// Executes the queued lines of one connection one after the other: the next starts when the previous has replied,
// which keeps the responses in request order even for wait-idle.
void TestIpcServer::pump(QLocalSocket* socket) {
  const QPointer<QLocalSocket> guard(socket);
  while (guard != nullptr) {
    auto it = connections_.find(socket);
    if (it == connections_.end() || it->busy || it->pending.isEmpty()) {
      return;
    }
    const QByteArray line = it->pending.takeFirst();
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      write(socket, CommandDispatch::errorResponse(-1, QStringLiteral("not a JSON object: %1")
                                                           .arg(parse_error.error != QJsonParseError::NoError
                                                                    ? parse_error.errorString()
                                                                    : QStringLiteral("a JSON object is required"))));
      continue;
    }
    it->busy = true;
    struct Flags {  // shared: a late reply outlives this loop iteration
      bool in_execute = true;
      bool replied_inline = false;
    };
    const auto flags = std::make_shared<Flags>();
    dispatch_.execute(document.object(), [this, guard, flags](const QJsonObject& response) {
      if (guard == nullptr) {
        return;
      }
      write(guard, response);
      auto found = connections_.find(guard.data());
      if (found != connections_.end()) {
        found->busy = false;
      }
      if (flags->in_execute) {
        flags->replied_inline = true;  // the loop continues with the next line
      } else {
        pump(guard);  // a late reply (wait-idle): carry on with the queue
      }
    });
    flags->in_execute = false;
    if (!flags->replied_inline) {
      return;  // waiting for a late reply
    }
  }
}

void TestIpcServer::write(QLocalSocket* socket, const QJsonObject& response) {
  QByteArray line = QJsonDocument(response).toJson(QJsonDocument::Compact);
  line.append('\n');
  socket->write(line);
  socket->flush();
}

}  // namespace ovt::app::testipc
