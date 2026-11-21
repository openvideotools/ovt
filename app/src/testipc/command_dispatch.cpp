// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "testipc/command_dispatch.h"

#include <QtQml/qqml.h>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QJSValue>
#include <QKeySequence>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QPoint>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtTest/QTest>

#include <memory>

namespace ovt::app::testipc {

namespace {

constexpr const char* kModuleUri = "OvtStudio";

QString typeName(const QVariant& value) {
  return QString::fromUtf8(value.typeName() != nullptr ? value.typeName() : "invalid");
}

bool isNumber(const QJsonValue& value) {
  return value.isDouble();
}

// The elements of `args` as strings, or false when one is not a string.
bool stringArg(const QJsonArray& args, qsizetype index, QString* out) {
  if (index >= args.size() || !args.at(index).isString()) {
    return false;
  }
  *out = args.at(index).toString();
  return true;
}

// One step of the `get` path below the first property: a key of a map, an index of a list, a property of an object.
bool step(const QVariant& current, const QString& token, QVariant* next, QString* error) {
  QVariant value = current;
  if (value.metaType() == QMetaType::fromType<QJSValue>()) {
    value = value.value<QJSValue>().toVariant();
  }
  if (value.canConvert<QObject*>() && value.metaType().flags().testFlag(QMetaType::PointerToQObject)) {
    auto* object = value.value<QObject*>();
    if (object == nullptr) {
      *error = QStringLiteral("'%1': the object is null").arg(token);
      return false;
    }
    const int index = object->metaObject()->indexOfProperty(token.toUtf8().constData());
    if (index < 0) {
      *error =
          QStringLiteral("no property '%1' on %2").arg(token, QString::fromUtf8(object->metaObject()->className()));
      return false;
    }
    *next = object->metaObject()->property(index).read(object);
    return true;
  }
  if (value.metaType() == QMetaType::fromType<QVariantMap>()) {
    const QVariantMap map = value.toMap();
    if (!map.contains(token)) {
      *error = QStringLiteral("no key '%1'").arg(token);
      return false;
    }
    *next = map.value(token);
    return true;
  }
  if (value.metaType() == QMetaType::fromType<QVariantList>()) {
    bool is_index = false;
    const qsizetype index = token.toLongLong(&is_index);
    const QVariantList list = value.toList();
    if (!is_index || index < 0 || index >= list.size()) {
      *error = QStringLiteral("'%1' is not an index of a list of %2").arg(token).arg(list.size());
      return false;
    }
    *next = list.at(index);
    return true;
  }
  *error = QStringLiteral("cannot read '%1' of a value of type %2").arg(token, typeName(value));
  return false;
}

}  // namespace

namespace {

// The geometry types of Qt (rect, point, size) as JSON objects; false when `value` is none of them.
bool geometryToJson(const QVariant& value, QJsonValue* out) {
  const QMetaType type = value.metaType();
  if (type == QMetaType::fromType<QRect>() || type == QMetaType::fromType<QRectF>()) {
    const QRectF rect = value.toRectF();
    *out = QJsonObject{{"x", rect.x()}, {"y", rect.y()}, {"width", rect.width()}, {"height", rect.height()}};
    return true;
  }
  if (type == QMetaType::fromType<QPoint>() || type == QMetaType::fromType<QPointF>()) {
    const QPointF point = value.toPointF();
    *out = QJsonObject{{"x", point.x()}, {"y", point.y()}};
    return true;
  }
  if (type == QMetaType::fromType<QSize>() || type == QMetaType::fromType<QSizeF>()) {
    const QSizeF size = value.toSizeF();
    *out = QJsonObject{{"width", size.width()}, {"height", size.height()}};
    return true;
  }
  return false;
}

bool isPlainNumberOrString(const QMetaType& type) {
  switch (type.id()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Long:
    case QMetaType::ULong:
    case QMetaType::Short:
    case QMetaType::UShort:
    case QMetaType::Char:
    case QMetaType::UChar:
    case QMetaType::SChar:
    case QMetaType::Float:
    case QMetaType::Double:
    case QMetaType::QString:
      return true;
    default:
      return false;
  }
}

}  // namespace

// NOLINTNEXTLINE(misc-no-recursion): the depth is that of the value read, which is a QML property
bool variantToJson(const QVariant& value, QJsonValue* out, QString* error) {
  if (!value.isValid() || value.isNull()) {
    *out = QJsonValue(QJsonValue::Null);
    return true;
  }
  const QMetaType type = value.metaType();
  if (type == QMetaType::fromType<QJSValue>()) {
    return variantToJson(value.value<QJSValue>().toVariant(), out, error);
  }
  if (type.flags().testFlag(QMetaType::PointerToQObject)) {
    *error = QStringLiteral("the value is an object (%1); read one of its properties").arg(typeName(value));
    return false;
  }
  if (geometryToJson(value, out)) {
    return true;
  }
  if (type == QMetaType::fromType<QUrl>() || type == QMetaType::fromType<QColor>()) {
    *out = value.toString();
    return true;
  }
  if (type == QMetaType::fromType<QVariantList>() || type == QMetaType::fromType<QStringList>()) {
    QJsonArray array;
    for (const QVariant& element : value.toList()) {
      QJsonValue converted;
      if (!variantToJson(element, &converted, error)) {
        return false;
      }
      array.append(converted);
    }
    *out = array;
    return true;
  }
  if (type == QMetaType::fromType<QVariantMap>()) {
    QJsonObject object;
    const QVariantMap map = value.toMap();
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
      QJsonValue converted;
      if (!variantToJson(it.value(), &converted, error)) {
        return false;
      }
      object.insert(it.key(), converted);
    }
    *out = object;
    return true;
  }
  if (isPlainNumberOrString(type)) {
    *out = QJsonValue::fromVariant(value);
    return true;
  }
  if (value.canConvert<QString>()) {  // enums and string-like types
    *out = value.toString();
    return true;
  }
  *error = QStringLiteral("a value of type %1 has no JSON form").arg(typeName(value));
  return false;
}

CommandDispatch::CommandDispatch(QQmlApplicationEngine* engine, QQuickWindow* window, QObject* parent)
    : QObject(parent), engine_(engine), window_(window) {
  if (window_ != nullptr) {
    connect(window_, &QQuickWindow::frameSwapped, this, [this] { ++frames_swapped_; });
  }
}

QJsonObject CommandDispatch::okResponse(int id, const QJsonValue& value) {
  return QJsonObject{{"id", id}, {"ok", true}, {"value", value}};
}

QJsonObject CommandDispatch::errorResponse(int id, const QString& message) {
  return QJsonObject{{"id", id}, {"ok", false}, {"error", message}};
}

CommandDispatch::Outcome CommandDispatch::fail(const QString& message) {
  Outcome outcome;
  outcome.ok = false;
  outcome.error = message;
  return outcome;
}

CommandDispatch::Outcome CommandDispatch::succeed(const QJsonValue& value) {
  Outcome outcome;
  outcome.value = value;
  return outcome;
}

void CommandDispatch::execute(const QJsonObject& request, const Reply& reply) {
  const QJsonValue id_value = request.value(QStringLiteral("id"));
  const int id = id_value.isDouble() ? id_value.toInt(-1) : -1;
  const QJsonValue cmd_value = request.value(QStringLiteral("cmd"));
  if (!cmd_value.isString()) {
    reply(errorResponse(id, QStringLiteral("the request needs a string \"cmd\"")));
    return;
  }
  const QJsonValue args_value = request.value(QStringLiteral("args"));
  if (!args_value.isUndefined() && !args_value.isArray()) {
    reply(errorResponse(id, QStringLiteral("\"args\" must be an array")));
    return;
  }
  const QJsonArray args = args_value.toArray();
  const QString cmd = cmd_value.toString();
  if (window_ == nullptr || engine_ == nullptr) {
    reply(errorResponse(id, QStringLiteral("the application window is gone")));
    return;
  }
  if (cmd == QLatin1String("wait-idle")) {
    waitIdle(args, id, reply);
    return;
  }
  Outcome outcome;
  if (cmd == QLatin1String("open")) {
    outcome = open(args);
  } else if (cmd == QLatin1String("click")) {
    outcome = click(args);
  } else if (cmd == QLatin1String("select-syntax-node")) {
    outcome = selectSyntaxNode(args);
  } else if (cmd == QLatin1String("key")) {
    outcome = key(args);
  } else if (cmd == QLatin1String("get")) {
    outcome = get(args);
  } else if (cmd == QLatin1String("screenshot")) {
    outcome = screenshot(args);
  } else {
    outcome = fail(QStringLiteral("unknown command '%1'").arg(cmd));
  }
  reply(outcome.ok ? okResponse(id, outcome.value)
                   : errorResponse(id, QStringLiteral("%1: %2").arg(cmd, outcome.error)));
}

QObject* CommandDispatch::singleton(const QString& type_name) const {
  const int type_id = qmlTypeId(kModuleUri, 1, 0, type_name.toUtf8().constData());
  return type_id < 0 ? nullptr : engine_->singletonInstance<QObject*>(type_id);
}

QObject* CommandDispatch::findObject(const QString& name) const {
  if (name.isEmpty()) {
    return nullptr;
  }
  if (name.at(0).isUpper()) {
    if (QObject* found = singleton(name); found != nullptr) {
      return found;
    }
  }
  for (QObject* root : engine_->rootObjects()) {
    if (root->objectName() == name) {
      return root;
    }
    if (auto* found = root->findChild<QObject*>(name); found != nullptr) {
      return found;
    }
  }
  // Delegates and Loader contents are visual children only.
  QList<QQuickItem*> queue{window_->contentItem()};
  while (!queue.isEmpty()) {
    QQuickItem* item = queue.takeFirst();
    if (item->objectName() == name) {
      return item;
    }
    queue.append(item->childItems());
  }
  return nullptr;
}

CommandDispatch::Outcome CommandDispatch::open(const QJsonArray& args) {
  QString path;
  if (args.size() != 1 || !stringArg(args, 0, &path)) {
    return fail(QStringLiteral("usage: open <path>"));
  }
  QObject* project = singleton(QStringLiteral("Project"));
  if (project == nullptr) {
    return fail(QStringLiteral("the Project singleton is not available"));
  }
  const QString before = project->property("diagnosticCode").toString();
  if (!QMetaObject::invokeMethod(project, "openFile", Q_ARG(QString, path))) {
    return fail(QStringLiteral("Project.openFile is not callable"));
  }
  QCoreApplication::processEvents();
  const QString code = project->property("diagnosticCode").toString();
  if (!code.isEmpty() && code != before) {
    return fail(QStringLiteral("%1 %2").arg(code, project->property("diagnosticMessage").toString()));
  }
  return succeed(QJsonObject{{"fileCount", project->property("fileCount").toInt()},
                             {"activeFile", project->property("activeFile").toString()}});
}

CommandDispatch::Outcome CommandDispatch::click(const QJsonArray& args) {
  QString name;
  const bool bad_shape = args.isEmpty() || args.size() > 3 || !stringArg(args, 0, &name) ||
                         (args.size() == 3 && !(isNumber(args.at(1)) && isNumber(args.at(2)))) ||
                         (args.size() == 2 && !isNumber(args.at(1)));
  if (bad_shape) {
    return fail(QStringLiteral("usage: click <objectName> [row] | click <objectName> <x> <y>"));
  }
  QObject* object = findObject(name);
  if (object == nullptr) {
    return fail(QStringLiteral("no object named '%1'").arg(name));
  }
  auto* item = qobject_cast<QQuickItem*>(object);
  if (item == nullptr) {
    return fail(QStringLiteral("'%1' is not an item").arg(name));
  }
  if (item->window() != window_.data() || !item->isVisible()) {
    return fail(QStringLiteral("'%1' is not visible in the main window").arg(name));
  }
  QQuickItem* target = item;
  QPointF local(item->width() / 2.0, item->height() / 2.0);
  if (args.size() == 3) {
    local = QPointF(args.at(1).toDouble(), args.at(2).toDouble());
  } else if (args.size() == 2) {
    const int row = args.at(1).toInt(-1);
    if (row < 0) {
      return fail(QStringLiteral("row must be >= 0"));
    }
    // A ListView-like item: bring the row into view, then click the centre of its delegate.
    if (item->metaObject()->indexOfMethod("itemAtIndex(int)") >= 0) {
      QMetaObject::invokeMethod(item, "positionViewAtIndex", Q_ARG(int, row), Q_ARG(int, 3 /* ListView.Contain */));
      QCoreApplication::processEvents();
      QQuickItem* delegate = nullptr;
      QMetaObject::invokeMethod(item, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate), Q_ARG(int, row));
      if (delegate == nullptr) {
        return fail(QStringLiteral("'%1' has no row %2").arg(name).arg(row));
      }
      target = delegate;
      local = QPointF(delegate->width() / 2.0, delegate->height() / 2.0);
    } else if (item->metaObject()->indexOfMethod("xOfFrame(int)") >= 0) {  // the timeline chart
      double x = 0.0;
      QMetaObject::invokeMethod(item, "xOfFrame", Q_RETURN_ARG(double, x), Q_ARG(int, row));
      local = QPointF(x, item->height() / 2.0);
    } else {
      return fail(QStringLiteral("'%1' has no rows (needs itemAtIndex(int) or xOfFrame(int))").arg(name));
    }
  }
  // QTest takes window coordinates, which for a window's content item are scene coordinates: item-local ones would
  // land on the wrong item.
  const QPoint scene = target->mapToScene(local).toPoint();
  if (!QRect(QPoint(0, 0), window_->size()).contains(scene)) {
    return fail(
        QStringLiteral("the point (%1, %2) of '%3' is outside the window").arg(scene.x()).arg(scene.y()).arg(name));
  }
  QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, scene);
  return succeed(QJsonObject{{"x", scene.x()}, {"y", scene.y()}});
}

CommandDispatch::Outcome CommandDispatch::selectSyntaxNode(const QJsonArray& args) {
  QString path;
  if (args.size() != 1 || !stringArg(args, 0, &path)) {
    return fail(QStringLiteral("usage: select-syntax-node <path>"));
  }
  QObject* tree = findObject(QStringLiteral("syntaxTree"));
  if (tree == nullptr || tree->metaObject()->indexOfMethod("selectByPath(QString)") < 0) {
    return fail(QStringLiteral("this build has no object named 'syntaxTree' with selectByPath(QString)"));
  }
  bool found = true;
  const QMetaObject* meta = tree->metaObject();
  const QMetaMethod method = meta->method(meta->indexOfMethod("selectByPath(QString)"));
  if (method.returnType() == QMetaType::Bool) {
    method.invoke(tree, Q_RETURN_ARG(bool, found), Q_ARG(QString, path));
  } else {
    method.invoke(tree, Q_ARG(QString, path));
  }
  QCoreApplication::processEvents();
  return found ? succeed() : fail(QStringLiteral("no syntax node '%1'").arg(path));
}

CommandDispatch::Outcome CommandDispatch::key(const QJsonArray& args) {
  QString text;
  if (args.size() != 1 || !stringArg(args, 0, &text)) {
    return fail(QStringLiteral("usage: key <sequence>"));
  }
  const QKeySequence sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
  if (sequence.isEmpty()) {
    return fail(QStringLiteral("'%1' is not a key sequence").arg(text));
  }
  if (!window_->isActive()) {  // offscreen windows are activated asynchronously
    window_->requestActivate();
    QElapsedTimer clock;
    clock.start();
    while (!window_->isActive() && clock.elapsed() < kActivateWaitMs) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, kIdlePollMs);
    }
  }
  for (int i = 0; i < sequence.count(); ++i) {
    const QKeyCombination combination = sequence[static_cast<uint>(i)];
    QTest::keyClick(window_, combination.key(), combination.keyboardModifiers());
  }
  return succeed();
}

CommandDispatch::Outcome CommandDispatch::get(const QJsonArray& args) {
  QString path;
  if (args.size() != 1 || !stringArg(args, 0, &path)) {
    return fail(QStringLiteral("usage: get <objectName>.<property>"));
  }
  const QStringList tokens = path.split(QLatin1Char('.'));
  if (tokens.size() < 2 || tokens.constFirst().isEmpty() || tokens.at(1).isEmpty()) {
    return fail(QStringLiteral("'%1' is not <objectName>.<property>").arg(path));
  }
  QObject* object = findObject(tokens.constFirst());
  if (object == nullptr) {
    return fail(QStringLiteral("no object named '%1'").arg(tokens.constFirst()));
  }
  QVariant value = QVariant::fromValue(object);
  QString error;
  for (qsizetype i = 1; i < tokens.size(); ++i) {
    QVariant next;
    if (!step(value, tokens.at(i), &next, &error)) {
      return fail(error);
    }
    value = next;
  }
  QJsonValue json;
  if (!variantToJson(value, &json, &error)) {
    return fail(error);
  }
  return succeed(json);
}

CommandDispatch::Outcome CommandDispatch::screenshot(const QJsonArray& args) {
  QString path;
  if (args.size() != 1 || !stringArg(args, 0, &path)) {
    return fail(QStringLiteral("usage: screenshot <path>"));
  }
  const QImage image = window_->grabWindow();
  if (image.isNull()) {
    return fail(QStringLiteral("the window could not be grabbed"));
  }
  if (!image.save(path, "PNG")) {
    return fail(QStringLiteral("cannot write '%1'").arg(path));
  }
  return succeed(QJsonObject{{"path", path}, {"width", image.width()}, {"height", image.height()}});
}

bool CommandDispatch::jobsIdle() const {
  QObject* jobs = singleton(QStringLiteral("JobsModel"));
  return jobs == nullptr || jobs->property("activeCount").toInt() == 0;
}

void CommandDispatch::waitIdle(const QJsonArray& args, int id, const Reply& reply) {
  int timeout_ms = kDefaultWaitIdleMs;
  if (!args.isEmpty()) {
    if (args.size() > 1 || !isNumber(args.at(0)) || args.at(0).toInt(-1) < 0) {
      reply(errorResponse(id, QStringLiteral("wait-idle: usage: wait-idle [timeout_ms]")));
      return;
    }
    timeout_ms = args.at(0).toInt();
  }
  struct State {
    QElapsedTimer clock;
    int quiet = 0;
    int last_frames = -1;
  };
  auto state = std::make_shared<State>();
  state->clock.start();
  auto* timer = new QTimer(this);  // NOLINT(cppcoreguidelines-owning-memory): parented; deleted when the wait ends
  timer->setInterval(kIdlePollMs);
  connect(timer, &QTimer::timeout, this, [this, state, timer, timeout_ms, id, reply] {
    const bool jobs_idle = jobsIdle();
    state->quiet = (jobs_idle && frames_swapped_ == state->last_frames) ? state->quiet + 1 : 0;
    state->last_frames = frames_swapped_;
    if (state->quiet >= kQuietPolls) {
      timer->deleteLater();
      reply(okResponse(id, QJsonValue(QJsonValue::Null)));
    } else if (state->clock.elapsed() >= timeout_ms) {
      timer->deleteLater();
      reply(errorResponse(id, QStringLiteral("wait-idle: timeout after %1 ms (%2)")
                                  .arg(timeout_ms)
                                  .arg(jobs_idle ? QStringLiteral("frames still being rendered")
                                                 : QStringLiteral("jobs still running"))));
    }
  });
  timer->start();
}

}  // namespace ovt::app::testipc
