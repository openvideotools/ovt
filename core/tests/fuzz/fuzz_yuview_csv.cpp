// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Fuzz target of the YUView statistics CSV importer (chapter 06 §6.8.2, WI-0.089; chapter 14 §14.7.1). The
// importer must never throw on bad text: it reports Diagnostics instead. Its result must always be a
// FrameAnalysis buffer that passes the FlatBuffers verifier and carries a grid of cells_w * cells_h cells.
// WI-0.094 lists this target among its own; WI-0.089 adds it with the importer (Definition of Done 3).

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "ovt/cxx/model/yuview_csv.hpp"
#include "ovt_model_generated.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Byte 0 picks the POC (0..3, so rows of the seeds match); the rest is the CSV text.
  const int poc = size == 0 ? 0 : data[0] % 4;
  const std::string text = size == 0 ? std::string{} : std::string(data + 1, data + size);
  std::istringstream in(text);
  const std::vector<std::uint8_t> buffer = ovt::model::read_yuview_csv(in, poc);

  flatbuffers::Verifier verifier(buffer.data(), buffer.size());
  if (!ovt::fb::VerifyFrameAnalysisBuffer(verifier)) {
    std::abort();
  }
  const auto* fa = ovt::fb::GetFrameAnalysis(buffer.data());
  if (const auto* grid = fa->grid(); grid != nullptr) {
    const auto cells = static_cast<std::size_t>(grid->cells_w()) * grid->cells_h();
    if (grid->qp() != nullptr && grid->qp()->size() != cells) {
      std::abort();
    }
  } else if (fa->diagnostics() == nullptr || fa->diagnostics()->size() == 0) {
    std::abort();  // no grid must come with the MDL-CSV-0004 diagnostic
  }
  return 0;
}
