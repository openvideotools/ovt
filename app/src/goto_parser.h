// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Go to grammar of FR-GUI-006 (chapter 11 §11.4.3 `Ctrl+G`, WI-1.466): decode index `d123`, display index
// `123` or `p123`, POC / order hint `poc45`, time `12.5s` or `00:01:30.250`, file byte offset `@0x1A2B3C` or
// `@1715004`, and the unit index `unit:N` of the Hex panel. The parser only reads the text; finding the frame, the
// byte or the unit is up to the panel that handles the request (a target that matches nothing is "no frame").

#ifndef OVT_APP_GOTO_PARSER_H
#define OVT_APP_GOTO_PARSER_H

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

#include <optional>

namespace ovt::app {

struct GotoTarget {
  enum class Kind { DecodeIndex, DisplayIndex, Poc, Time, ByteOffset, Unit };

  Kind kind = Kind::DisplayIndex;
  qint64 integer = 0;  // index, POC, byte offset or unit index
  qint64 time_us = 0;  // Kind::Time: microseconds (exact for the decimal places the grammar allows)

  bool operator==(const GotoTarget&) const = default;
};

// nullopt and `error` (a short sentence) when the text matches no form of the grammar.
std::optional<GotoTarget> parseGoto(const QString& text, QString* error = nullptr);

// QML view: Goto.parse("poc45") -> {ok, kind: "poc", value: 45, error}. `kind` is one of "decode", "display", "poc",
// "time" (value in seconds), "byte" and "unit".
class GotoParser : public QObject {
  Q_OBJECT
  QML_NAMED_ELEMENT(Goto)
  QML_SINGLETON

 public:
  Q_INVOKABLE [[nodiscard]] static QVariantMap parse(const QString& text);
};

}  // namespace ovt::app

#endif  // OVT_APP_GOTO_PARSER_H
