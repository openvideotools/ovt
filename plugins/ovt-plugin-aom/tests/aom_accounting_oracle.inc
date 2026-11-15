// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
//
// The oracle decoder of the ovt-plugin-aom accounting tests (WI-2.157, WI-2.581): libaom's accounting records of every
// frame, read through the inspection API the way examples/inspect.c's read_frame() loop does. Not a .hpp on purpose: it
// needs Catch2 and libaom's semi-public headers (tp::aom_inspection), like aom_test_util.inc. Included inside an
// anonymous namespace by the test file, after aom_test_util.inc and the libaom headers.
#ifndef OVT_PLUGIN_AOM_TESTS_AOM_ACCOUNTING_ORACLE_INC
#define OVT_PLUGIN_AOM_TESTS_AOM_ACCOUNTING_ORACLE_INC

// ---- oracle: libaom's accounting records, as examples/inspect reads them ----------------------------------------

struct RawRecord {
  int x;
  int y;
  uint32_t id;
  uint32_t bits;
  uint32_t samples;
};
struct RawFrame {
  std::vector<std::string> names;
  std::vector<RawRecord> records;
};
struct RawRun {
  insp_frame_data fd{};
  std::vector<RawFrame> frames;
};

void on_raw_frame(void* pbi, void* data) {
  auto* run = static_cast<RawRun*>(data);
  ifd_inspect(&run->fd, pbi, 0);
  const Accounting* acc = run->fd.accounting;
  REQUIRE(acc != nullptr);
  RawFrame f;
  for (const char* s : std::span(acc->syms.dictionary.strs).first(static_cast<size_t>(acc->syms.dictionary.num_strs))) {
    f.names.emplace_back(s);
  }
  for (const AccountingSymbol& s : std::span(acc->syms.syms, static_cast<size_t>(acc->syms.num_syms))) {
    f.records.push_back(RawRecord{s.context.x, s.context.y, s.id, s.bits, s.samples});
  }
  run->frames.push_back(std::move(f));
}

// The inspect.c read_frame() loop over every temporal unit: one aom_codec_decode() call per frame, user_priv set.
std::vector<RawFrame> raw_accounting(const std::vector<ovt::aom::test::TemporalUnit>& tus, size_t max_frames) {
  RawRun run;
  aom_codec_ctx_t ctx{};
  aom_codec_dec_cfg_t cfg{};
  cfg.threads = 1;
  cfg.allow_lowbitdepth = 1;
  REQUIRE(aom_codec_dec_init(&ctx, aom_codec_av1_dx(), &cfg, 0) == AOM_CODEC_OK);
  ifd_init(&run.fd, 8, 8);
  aom_inspect_init ii{};
  ii.inspect_cb = &on_raw_frame;
  ii.inspect_ctx = &run;
  REQUIRE(aom_codec_control(&ctx, AV1_SET_INSPECTION_CALLBACK, &ii) == AOM_CODEC_OK);
  for (const auto& tu : tus) {
    std::vector<uint8_t> bytes;
    for (const auto& o : tu) {
      bytes.insert(bytes.end(), o.bytes.begin(), o.bytes.end());
    }
    const uint8_t* p = bytes.data();
    size_t len = bytes.size();
    while (len > 0) {
      Av1DecodeReturn ret{};
      REQUIRE(aom_codec_decode(&ctx, p, len, &ret) == AOM_CODEC_OK);
      const auto consumed = static_cast<size_t>(ret.buf - p);
      if (consumed == 0) {
        break;
      }
      p = ret.buf;
      len -= consumed;
      aom_codec_iter_t iter = nullptr;
      while (aom_codec_get_frame(&ctx, &iter) != nullptr) {
      }
    }
    if (run.frames.size() >= max_frames) {
      break;
    }
  }
  aom_codec_destroy(&ctx);
  ifd_clear(&run.fd);
  if (run.frames.size() > max_frames) {
    run.frames.resize(max_frames);
  }
  return run.frames;
}

#endif  // OVT_PLUGIN_AOM_TESTS_AOM_ACCOUNTING_ORACLE_INC
