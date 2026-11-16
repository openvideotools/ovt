// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AV1 read_tx_mode() (chapter 7 §7.3.6 row 23; WI-1.118).

#include <string_view>

#include "ovt/cxx/syntax/av1/frame_header.hpp"

namespace ovt::syntax::av1 {

namespace {

constexpr std::string_view kSpecTxMode = "AV1 5.9.21";

}  // namespace

std::string_view tx_mode_name(TxModeType m) noexcept {
  switch (m) {
    case TxModeType::Only4x4:
      return "ONLY_4X4";
    case TxModeType::Largest:
      return "TX_MODE_LARGEST";
    case TxModeType::Select:
      return "TX_MODE_SELECT";
  }
  return "";
}

TxModeParams parse_tx_mode(BitReader& br, SyntaxBuilder& b, bool CodedLossless) {
  // AV1 §5.9.21
  TxModeParams t;
  const auto start = br.mark();
  auto s = b.scoped("tx_mode", kSpecTxMode);
  if (CodedLossless) {
    t.TxMode = TxModeType::Only4x4;
    b.derived("TxMode", "TxMode = ONLY_4X4 (CodedLossless)");
  } else {
    t.tx_mode_select = b.read_flag("tx_mode_select");
    t.TxMode = t.tx_mode_select ? TxModeType::Select : TxModeType::Largest;
    b.derived("TxMode", t.tx_mode_select ? "TxMode = TX_MODE_SELECT" : "TxMode = TX_MODE_LARGEST");
  }
  t.span = br.span_since(start);
  return t;
}

}  // namespace ovt::syntax::av1
