// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the HDR panel models (WI-1.462; chapter 11 §11.3.14, §11.3.15): the Metadata rows and provenance from an
// HdrMetadata FlatBuffer, the ⚠ row of the core's DEC-HDR-0001 (AT1, model half), the Dolby label, and the
// DisplayCaps table and status text of an SDR display (AT2, model half). The QML half is
// app/tests/qml/tst_HdrMetadataModel.qml.

#include <catch2/catch_test_macros.hpp>

#include <QSignalSpy>
#include <QVariantMap>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "models/hdr_metadata_model.h"
#include "models/qt_test_app.h"
#include "ovt/color.h"
#include "ovt_container_generated.h"

using ovt::app::DisplayCapsInfo;
using ovt::app::DisplayCapsModel;
using ovt::app::HdrMetadataModel;

namespace {

// syn_h265_main10_pq_hdr_mismatch (chapter 14 §14.x row 23): `colr` nclc 1/1/1 (BT.709) wins for the colour fields,
// the SPS VUI says BT.2020/PQ, MDCV and CLL come from SEI, and the core attached DEC-HDR-0001.
QByteArray mismatch_buffer() {
  flatbuffers::FlatBufferBuilder b;
  ovt_ColorDescription c{};
  c.struct_size = sizeof c;
  c.primaries = 1;
  c.transfer = 1;
  c.matrix = 1;
  c.mdcv_primaries[0][0] = 34000;  // R
  c.mdcv_primaries[0][1] = 16000;
  c.mdcv_primaries[1][0] = 13250;  // G
  c.mdcv_primaries[1][1] = 34500;
  c.mdcv_primaries[2][0] = 7500;  // B
  c.mdcv_primaries[2][1] = 3000;
  c.mdcv_white_point[0] = 15635;
  c.mdcv_white_point[1] = 16450;
  c.mdcv_max_luminance = 10000000;
  c.mdcv_min_luminance = 1;
  c.max_cll = 1000;
  c.max_fall = 400;
  std::vector<std::uint8_t> bytes(sizeof c);
  std::memcpy(bytes.data(), &c, sizeof c);
  const auto color = b.CreateVector(bytes);
  const std::vector<flatbuffers::Offset<ovt::fb::HdrSignal>> signal_list{
      ovt::fb::CreateHdrSignalDirect(b, "colr", 1234, -1, -1, true, "nclc 1/1/1"),
      ovt::fb::CreateHdrSignalDirect(b, "sps_vui", -1, 0, 1, true, "BT.2020 / PQ / BT.2020-NCL"),
      ovt::fb::CreateHdrSignalDirect(b, "sei_137", -1, 0, 3, true, "MDCV"),
      ovt::fb::CreateHdrSignalDirect(b, "sei_144", -1, 0, 4, true, "CLL 1000/400"),
      ovt::fb::CreateHdrSignalDirect(b, "mdcv", -1, -1, -1, false, ""),
  };
  const auto sig = b.CreateVector(signal_list);
  const std::vector<flatbuffers::Offset<ovt::fb::Diagnostic>> diags{ovt::fb::CreateDiagnosticDirect(
      b, ovt::fb::Severity::Warning, "DEC-HDR-0001",
      "colour signalling differs between container (BT.709/BT.709) and bitstream (BT.2020/PQ)")};
  const auto dv = b.CreateVector(diags);
  ovt::fb::HdrMetadataBuilder m(b);
  m.add_track_id(1);
  m.add_kind(ovt::fb::HdrKind::HDR10);
  m.add_color(color);
  m.add_signals(sig);
  m.add_box_vs_bitstream_mismatch(true);
  m.add_diagnostics(dv);
  b.Finish(m.Finish());
  return {reinterpret_cast<const char*>(b.GetBufferPointer()),  // NOLINT(*-reinterpret-cast)
          static_cast<qsizetype>(b.GetSize())};
}

}  // namespace

TEST_CASE("HDR metadata rows carry value, provenance and the core's DEC-HDR-0001", "[fast][gui][hdr]") {
  ovt::app::testing::qt_app();
  HdrMetadataModel model;
  const QSignalSpy changed(&model, &HdrMetadataModel::metadataChanged);
  REQUIRE(model.setHdrMetadata(mismatch_buffer()));
  CHECK(changed.count() == 1);
  CHECK(model.has_metadata());
  CHECK(model.kind_text() == QStringLiteral("HDR10"));
  CHECK(model.declared_max_cll() == 1000);
  CHECK(model.declared_max_fall() == 400);

  // AT1 (model half): the ⚠ row with the core's diagnostic, verbatim.
  const int diag = model.findCode(QStringLiteral("DEC-HDR-0001"));
  REQUIRE(diag >= 0);
  const QVariantMap row = model.row(diag);
  CHECK(row.value(QStringLiteral("warning")).toBool());
  CHECK(row.value(QStringLiteral("item")).toString() == QStringLiteral("DEC-HDR-0001"));
  CHECK(row.value(QStringLiteral("value")).toString().startsWith(QStringLiteral("colour signalling differs")));

  // The colour rows show the container's values (boxes win) with the ⚠ and the bitstream's own signal next to them.
  const QVariantMap primaries = model.row(1);
  CHECK(primaries.value(QStringLiteral("item")).toString() == QStringLiteral("Colour primaries"));
  CHECK(primaries.value(QStringLiteral("value")).toString() == QStringLiteral("BT.709 (1)"));
  CHECK(primaries.value(QStringLiteral("provenance")).toString() == QStringLiteral("container colr"));
  CHECK(primaries.value(QStringLiteral("warning")).toBool());
  CHECK(primaries.value(QStringLiteral("code")).toString() == QStringLiteral("DEC-HDR-0001"));
  bool bitstream_row = false;
  bool mdcv_row = false;
  bool cll_row = false;
  for (int i = 0; i < model.rowCount(); ++i) {
    const QVariantMap r = model.row(i);
    const QString item = r.value(QStringLiteral("item")).toString();
    if (item == QStringLiteral("Bitstream colour")) {
      bitstream_row = true;
      CHECK(r.value(QStringLiteral("provenance")).toString() == QStringLiteral("SPS VUI #1"));
    } else if (item == QStringLiteral("Mastering luminance")) {
      mdcv_row = true;
      CHECK(r.value(QStringLiteral("value")).toString() == QStringLiteral("0.0001 – 1000 cd/m²"));
      CHECK(r.value(QStringLiteral("provenance")).toString() == QStringLiteral("SEI #3"));
    } else if (item == QStringLiteral("Mastering display primaries")) {
      CHECK(r.value(QStringLiteral("value")).toString() ==
            QStringLiteral("R (0.6800, 0.3200)  G (0.2650, 0.6900)  B (0.1500, 0.0600)"));
    } else if (item == QStringLiteral("MaxCLL")) {
      cll_row = true;
      CHECK(r.value(QStringLiteral("value")).toString() == QStringLiteral("1000 cd/m²"));
      CHECK(r.value(QStringLiteral("provenance")).toString() == QStringLiteral("SEI #4"));
    }
  }
  CHECK(bitstream_row);
  CHECK(mdcv_row);
  CHECK(cll_row);
  CHECK(model.warning_count() >= 4);  // three colour rows, the bitstream row and the diagnostic
}

TEST_CASE("a buffer that does not verify clears the HDR metadata", "[fast][gui][hdr]") {
  ovt::app::testing::qt_app();
  HdrMetadataModel model;
  REQUIRE(model.setHdrMetadata(mismatch_buffer()));
  CHECK_FALSE(model.setHdrMetadata(QByteArray("not a flatbuffer")));
  CHECK_FALSE(model.has_metadata());
  CHECK(model.rowCount() == 0);
  CHECK_FALSE(model.setHdrMetadata(QByteArray()));
}

TEST_CASE("SDR content reads SDR (BT.709) and Dolby Vision gets its label", "[fast][gui][hdr]") {
  ovt::app::testing::qt_app();
  HdrMetadataModel model;
  REQUIRE(model.setFromJson({{QStringLiteral("kind"), QStringLiteral("SDR")},
                             {QStringLiteral("color"), QVariantMap{{QStringLiteral("primaries"), 1},
                                                                   {QStringLiteral("transfer"), 1},
                                                                   {QStringLiteral("matrix"), 1}}}}));
  CHECK(model.is_sdr());
  CHECK(model.kind_text() == QStringLiteral("SDR (BT.709)"));
  CHECK(model.dolby_label().isEmpty());
  CHECK(model.findCode(QStringLiteral("DEC-HDR-0001")) == -1);

  REQUIRE(model.setFromJson(
      {{QStringLiteral("kind"), 5},
       {QStringLiteral("dv_profile"), 8},
       {QStringLiteral("dv_bl_compat_id"), 1},
       {QStringLiteral("dv_level"), 6},
       {QStringLiteral("dv_rpu_present"), true},
       {QStringLiteral("signals"), QVariantList{QVariantMap{{QStringLiteral("source"), QStringLiteral("dvvC")},
                                                            {QStringLiteral("box_offset"), 900},
                                                            {QStringLiteral("present"), true}}}}}));
  CHECK(model.kind_text() == QStringLiteral("Dolby Vision"));
  CHECK(model.dolby_label() == QStringLiteral("Dolby Vision profile 8.1, level 6"));
  bool dv = false;
  for (int i = 0; i < model.rowCount(); ++i) {
    const QVariantMap r = model.row(i);
    if (r.value(QStringLiteral("item")).toString() == QStringLiteral("Dolby Vision")) {
      dv = true;
      CHECK(r.value(QStringLiteral("provenance")).toString() == QStringLiteral("container dvvC"));
    }
  }
  CHECK(dv);
  CHECK_FALSE(model.setFromJson({}));
  CHECK_FALSE(model.has_metadata());
}

TEST_CASE("an SDR display: not capable, the reason, SDR status and unknown rows", "[fast][gui][hdr]") {
  ovt::app::testing::qt_app();
  DisplayCapsModel caps;
  DisplayCapsInfo sdr;
  sdr.reason = QStringLiteral("display has no EDR headroom");
  sdr.source = QStringLiteral("NSScreen");
  sdr.primaries_name = QStringLiteral("BT.709");
  sdr.platform_detail = QStringLiteral("macOS 15.5");
  caps.setProber([sdr] { return sdr; });
  caps.probeNow();
  CHECK_FALSE(caps.hdr_capable());
  CHECK(caps.reason() == QStringLiteral("display has no EDR headroom"));
  CHECK(caps.status_text() == QStringLiteral("SDR"));
  REQUIRE(caps.rowCount() == 13);  // one row per §10.4.5 "Panel contents" field
  CHECK(caps.row(1).value(QStringLiteral("value")).toString() == QStringLiteral("no (display has no EDR headroom)"));
  // Luminance unknown: "—" with the reason.
  CHECK(caps.row(6).value(QStringLiteral("unknown")).toBool());
  CHECK(caps.row(6).value(QStringLiteral("value")).toString() == QStringLiteral("— (display has no EDR headroom)"));
  CHECK(caps.reportText().contains(QStringLiteral("Source API: NSScreen")));
}

TEST_CASE("an XDR display: capable, headroom status, PQ pass-through and re-probe", "[fast][gui][hdr]") {
  ovt::app::testing::qt_app();
  DisplayCapsModel caps;
  DisplayCapsInfo xdr;
  xdr.hdr_capable = true;
  xdr.potential_headroom = 16.0F;
  xdr.sdr_white_nits = std::numeric_limits<float>::quiet_NaN();
  xdr.supports_pq = true;
  xdr.supports_scrgb = true;
  xdr.source = QStringLiteral("NSScreen");
  int probes = 0;
  caps.setProber([&probes, xdr] {
    ++probes;
    return xdr;
  });
  const QSignalSpy probing(&caps, &DisplayCapsModel::probingChanged);
  caps.reprobe();
  CHECK(caps.probing());
  REQUIRE(ovt::app::testing::spin_until([&caps] { return !caps.probing(); }, std::chrono::seconds(5)));
  CHECK(probes == 1);
  CHECK(probing.count() == 2);
  CHECK(caps.hdr_capable());
  CHECK(caps.reason().isEmpty());
  CHECK(caps.status_text() == QStringLiteral("HDR headroom 16.0×"));
  CHECK(caps.row(5).value(QStringLiteral("value")).toString() == QStringLiteral("relative"));
  CHECK(caps.row(7).value(QStringLiteral("value")).toString() == QStringLiteral("PQ, scRGB"));
  caps.set_output_encoding(QStringLiteral("Pq10"));
  CHECK(caps.status_text() == QStringLiteral("PQ pass-through"));
  CHECK(caps.row(3).value(QStringLiteral("value")).toString() == QStringLiteral("Pq10"));
}
