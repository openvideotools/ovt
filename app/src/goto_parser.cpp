// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "goto_parser.h"

#include <QRegularExpression>
#include <QStringList>

namespace ovt::app {

namespace {

constexpr qint64 kMicrosPerSecond = 1'000'000;
// The leading hours or minutes field: far beyond any video, and keeps the microsecond sum inside qint64.
constexpr qint64 kMaxLeadingField = 99'999;

std::optional<GotoTarget> fail(QString* error, const QString& message) {
  if (error != nullptr) {
    *error = message;
  }
  return std::nullopt;
}

// Decimal digits only (no sign, no octal prefix) as an index.
std::optional<qint64> parse_digits(const QString& text) {
  static const QRegularExpression digits(QStringLiteral("^[0-9]{1,15}$"));
  if (!digits.match(text).hasMatch()) {
    return std::nullopt;
  }
  return text.toLongLong();
}

// "90", "90.5", "5.250" -> microseconds; at most six decimal places.
std::optional<qint64> parse_seconds(const QString& text) {
  static const QRegularExpression pattern(QStringLiteral("^([0-9]{1,12})(?:\\.([0-9]{1,6}))?$"));
  const QRegularExpressionMatch match = pattern.match(text);
  if (!match.hasMatch()) {
    return std::nullopt;
  }
  qint64 micros = match.captured(1).toLongLong() * kMicrosPerSecond;
  QString fraction = match.captured(2);
  if (!fraction.isEmpty()) {
    fraction = fraction.leftJustified(6, QLatin1Char('0'));
    micros += fraction.toLongLong();
  }
  return micros;
}

// "H:MM:SS.fff" or "M:SS.fff".
std::optional<qint64> parse_clock(const QString& text) {
  const QStringList parts = text.split(QLatin1Char(':'));
  if (parts.size() != 2 && parts.size() != 3) {
    return std::nullopt;
  }
  const std::optional<qint64> seconds = parse_seconds(parts.back());
  if (!seconds || *seconds >= 60 * kMicrosPerSecond) {
    return std::nullopt;
  }
  qint64 total = *seconds;
  qint64 unit = 60;  // seconds per step of the next field to the left
  for (qsizetype i = parts.size() - 2; i >= 0; --i) {
    const std::optional<qint64> value = parse_digits(parts.at(i));
    // Only the leading field may exceed 59.
    if (!value || (i > 0 && *value >= 60) || *value > kMaxLeadingField) {
      return std::nullopt;
    }
    total += *value * unit * kMicrosPerSecond;
    unit *= 60;
  }
  return total;
}

std::optional<GotoTarget> make(GotoTarget::Kind kind, std::optional<qint64> value, QString* error,
                               const QString& what) {
  if (!value) {
    return fail(error, what);
  }
  GotoTarget target;
  target.kind = kind;
  target.integer = *value;
  return target;
}

std::optional<GotoTarget> parse_offset(const QString& rest, QString* error) {
  static const QRegularExpression hex(QStringLiteral("^0x([0-9a-f]{1,15})$"));
  const QRegularExpressionMatch match = hex.match(rest);
  if (match.hasMatch()) {
    return make(GotoTarget::Kind::ByteOffset, match.captured(1).toLongLong(nullptr, 16), error, {});
  }
  return make(GotoTarget::Kind::ByteOffset, parse_digits(rest), error,
              QStringLiteral("a byte offset is @0x1A2B3C or @1715004"));
}

std::optional<GotoTarget> parse_poc(const QString& rest, QString* error) {
  static const QRegularExpression poc(QStringLiteral("^(-?[0-9]{1,15})$"));
  const QRegularExpressionMatch match = poc.match(rest);
  return make(GotoTarget::Kind::Poc, match.hasMatch() ? std::optional<qint64>(rest.toLongLong()) : std::nullopt, error,
              QStringLiteral("a POC is poc45"));
}

}  // namespace

std::optional<GotoTarget> parseGoto(const QString& text, QString* error) {
  const QString t = text.trimmed().toLower();
  if (t.isEmpty()) {
    return fail(error, QStringLiteral("nothing to go to"));
  }
  if (t.startsWith(QLatin1Char('@'))) {
    return parse_offset(t.mid(1), error);
  }
  if (t.startsWith(QLatin1String("unit:"))) {
    return make(GotoTarget::Kind::Unit, parse_digits(t.mid(5)), error, QStringLiteral("a unit is unit:12"));
  }
  if (t.startsWith(QLatin1String("poc"))) {
    return parse_poc(t.mid(3), error);
  }
  if (t.startsWith(QLatin1Char('d'))) {
    return make(GotoTarget::Kind::DecodeIndex, parse_digits(t.mid(1)), error, QStringLiteral("a decode index is d123"));
  }
  if (t.startsWith(QLatin1Char('p'))) {
    return make(GotoTarget::Kind::DisplayIndex, parse_digits(t.mid(1)), error,
                QStringLiteral("a display index is 123 or p123"));
  }
  std::optional<qint64> micros;
  if (t.contains(QLatin1Char(':'))) {
    micros = parse_clock(t);
  } else if (t.endsWith(QLatin1Char('s'))) {
    micros = parse_seconds(t.chopped(1));
  } else {
    return make(GotoTarget::Kind::DisplayIndex, parse_digits(t), error,
                QStringLiteral("not a frame, time, offset or unit"));
  }
  if (!micros) {
    return fail(error, QStringLiteral("a time is 12.5s or 00:01:30.250"));
  }
  GotoTarget target;
  target.kind = GotoTarget::Kind::Time;
  target.time_us = *micros;
  return target;
}

QVariantMap GotoParser::parse(const QString& text) {
  QString error;
  const std::optional<GotoTarget> target = parseGoto(text, &error);
  QVariantMap result;
  result.insert(QStringLiteral("ok"), target.has_value());
  result.insert(QStringLiteral("error"), error);
  if (!target) {
    return result;
  }
  QString kind;
  QVariant value = target->integer;
  switch (target->kind) {
    case GotoTarget::Kind::DecodeIndex:
      kind = QStringLiteral("decode");
      break;
    case GotoTarget::Kind::DisplayIndex:
      kind = QStringLiteral("display");
      break;
    case GotoTarget::Kind::Poc:
      kind = QStringLiteral("poc");
      break;
    case GotoTarget::Kind::Time:
      kind = QStringLiteral("time");
      value = static_cast<double>(target->time_us) / static_cast<double>(kMicrosPerSecond);
      break;
    case GotoTarget::Kind::ByteOffset:
      kind = QStringLiteral("byte");
      break;
    case GotoTarget::Kind::Unit:
      kind = QStringLiteral("unit");
      break;
  }
  result.insert(QStringLiteral("kind"), kind);
  result.insert(QStringLiteral("value"), value);
  return result;
}

}  // namespace ovt::app
