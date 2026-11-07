// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WI-1.466: the Go to grammar of FR-GUI-006 (d123, 123, p123, poc45, 12.5s, 00:01:30.250, @0x1A2B3C, @1715004, unit:N).

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QVariantMap>

#include "goto_parser.h"

namespace {

using ovt::app::GotoParser;
using ovt::app::GotoTarget;
using ovt::app::parseGoto;
using Kind = GotoTarget::Kind;

GotoTarget parsed(const char* text) {
  QString error;
  const auto target = parseGoto(QString::fromLatin1(text), &error);
  INFO(text << ": " << error.toStdString());
  REQUIRE(target.has_value());
  return target.value_or(GotoTarget{});
}

}  // namespace

TEST_CASE("Go to: indices, POC, offsets and units", "[fast][gui][goto]") {
  CHECK(parsed("d123") == GotoTarget{Kind::DecodeIndex, 123, 0});
  CHECK(parsed("123") == GotoTarget{Kind::DisplayIndex, 123, 0});
  CHECK(parsed("p123") == GotoTarget{Kind::DisplayIndex, 123, 0});
  CHECK(parsed(" P7 ") == GotoTarget{Kind::DisplayIndex, 7, 0});
  CHECK(parsed("poc45") == GotoTarget{Kind::Poc, 45, 0});
  CHECK(parsed("poc-4") == GotoTarget{Kind::Poc, -4, 0});
  CHECK(parsed("@0x1A2B3C") == GotoTarget{Kind::ByteOffset, 0x1A2B3C, 0});
  CHECK(parsed("@0x1A3F40") == GotoTarget{Kind::ByteOffset, 0x1A3F40, 0});
  CHECK(parsed("@1715004") == GotoTarget{Kind::ByteOffset, 1715004, 0});
  CHECK(parsed("@010") == GotoTarget{Kind::ByteOffset, 10, 0});  // decimal, never octal
  CHECK(parsed("unit:12") == GotoTarget{Kind::Unit, 12, 0});
}

TEST_CASE("Go to: times", "[fast][gui][goto]") {
  CHECK(parsed("12.5s") == GotoTarget{Kind::Time, 0, 12'500'000});
  CHECK(parsed("3s") == GotoTarget{Kind::Time, 0, 3'000'000});
  CHECK(parsed("00:01:30.250") == GotoTarget{Kind::Time, 0, 90'250'000});
  CHECK(parsed("1:30") == GotoTarget{Kind::Time, 0, 90'000'000});
  CHECK(parsed("2:00:00") == GotoTarget{Kind::Time, 0, 7'200'000'000});
  CHECK(parsed("75:00") == GotoTarget{Kind::Time, 0, 4'500'000'000});  // the leading field may exceed 59
}

TEST_CASE("Go to: text that is not the grammar is refused with a reason", "[fast][gui][goto]") {
  for (const char* text :
       {"",      "   ",     "abc",  "d",    "p",      "dx1",     "poc", "poc4.5", "@",   "@0x",        "@0xZZ",
        "unit:", "unit:-1", "12.5", "1:75", "1:2:75", "0:61:00", "-3",  "1e3s",   ".5s", "12.1234567s"}) {
    QString error;
    CHECK_FALSE(parseGoto(QString::fromLatin1(text), &error).has_value());
    CHECK_FALSE(error.isEmpty());
  }
}

TEST_CASE("Go to: the QML view", "[fast][gui][goto]") {
  QVariantMap result = GotoParser::parse(QStringLiteral("@0x1A3F40"));
  CHECK(result.value(QStringLiteral("ok")).toBool());
  CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("byte"));
  CHECK(result.value(QStringLiteral("value")).toLongLong() == 0x1A3F40);
  result = GotoParser::parse(QStringLiteral("00:01:30.250"));
  CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("time"));
  CHECK(result.value(QStringLiteral("value")).toDouble() == 90.25);
  result = GotoParser::parse(QStringLiteral("poc9999"));
  CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("poc"));
  result = GotoParser::parse(QStringLiteral("nonsense"));
  CHECK_FALSE(result.value(QStringLiteral("ok")).toBool());
  CHECK_FALSE(result.value(QStringLiteral("error")).toString().isEmpty());
}
