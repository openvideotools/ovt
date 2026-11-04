// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Tests of ovt::test::corpus_path (chapter 14 §14.2.4, WI-0.550).

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "test_support/corpus.h"

namespace {

// A scratch corpus cache below the build tree, selected through OVT_CORPORA_DIR for one test case.
class ScratchCorpora {
 public:
  ScratchCorpora() : dir_(std::filesystem::path(OVT_TEST_BINARY_DIR) / "scratch_corpora") {
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    if (const char* old = std::getenv("OVT_CORPORA_DIR")) {  // NOLINT(concurrency-mt-unsafe)
      old_ = old;
      had_old_ = true;
    }
    ::setenv("OVT_CORPORA_DIR", dir_.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
  }
  ~ScratchCorpora() {
    if (had_old_) {
      ::setenv("OVT_CORPORA_DIR", old_.c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    } else {
      ::unsetenv("OVT_CORPORA_DIR");  // NOLINT(concurrency-mt-unsafe)
    }
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  ScratchCorpora(const ScratchCorpora&) = delete;
  ScratchCorpora& operator=(const ScratchCorpora&) = delete;

  void add(const std::string& id, const std::string& file, bool verified) const {
    const auto entry = dir_ / id;
    std::filesystem::create_directories(entry);
    std::ofstream(entry / file) << "payload";
    if (verified) {
      std::ofstream(entry / ".ok") << "";
    }
  }
  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

 private:
  std::filesystem::path dir_;
  std::string old_;
  bool had_old_ = false;
};

}  // namespace

TEST_CASE("test_support/corpus: finds verified entries only", "[test_support][fast]") {
  const ScratchCorpora corpora;
  corpora.add("syn_present", "present.mp4", true);
  corpora.add("syn_unverified", "unverified.mp4", false);

  CHECK(ovt::test::corpora_dir() == corpora.dir());

  const auto present = ovt::test::corpus_path("syn_present");
  REQUIRE(present.has_value());
  CHECK(present->filename() == "present.mp4");

  CHECK_FALSE(ovt::test::corpus_path("syn_unverified").has_value());  // no .ok marker
  CHECK_FALSE(ovt::test::corpus_path("syn_absent").has_value());
}

TEST_CASE("test_support/corpus: rejects ids that are not plain names", "[test_support][fast]") {
  CHECK_FALSE(ovt::test::corpus_path("").has_value());
  CHECK_FALSE(ovt::test::corpus_path("..").has_value());
  CHECK_FALSE(ovt::test::corpus_path("../etc").has_value());
  CHECK_FALSE(ovt::test::corpus_path("a/b").has_value());
}

// Acceptance test 2 of WI-0.550. Hidden ([.]) so the suite never depends on a corpus that is deliberately
// absent; run it explicitly with `ovt_core_tests "[needs-corpus-demo]"`. It is skipped without
// OVT_REQUIRE_CORPORA and fails with "corpus file missing: syn_wi0550_absent" when OVT_REQUIRE_CORPORA=1.
TEST_CASE("test_support/corpus: a missing corpus file skips or fails", "[.][needs-corpus][needs-corpus-demo]") {
  const auto path = ovt::test::corpus_or_skip("syn_wi0550_absent");
  CHECK(path.empty());  // not reached
}
