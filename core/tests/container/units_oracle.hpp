// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers of test_container_{annexb,obu,ivf,y4m}.cpp (WI-1.570): the `<id>.ovt-units.json` goldens of
// tools/goldens/gen_units.py, a runner of that generator and a maker of small FFmpeg-encoded streams. Tests that need
// an external tool skip when it is missing.

#ifndef OVT_TESTS_CONTAINER_UNITS_ORACLE_HPP
#define OVT_TESTS_CONTAINER_UNITS_ORACLE_HPP

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ovt::test::units {

struct Packet {
  std::optional<uint64_t> pos;
  uint64_t size = 0;
  std::optional<std::vector<uint32_t>> types;  // NAL unit types (H.264/HEVC); absent for AV1 and Y4M
};

struct Golden {
  std::string id;
  std::string codec;
  std::string framing;
  std::vector<Packet> packets;
};

inline Golden parse_golden(const nlohmann::json& doc) {
  Golden g;
  g.id = doc.at("id");
  g.codec = doc.at("codec");
  g.framing = doc.at("framing");
  for (const auto& p : doc.at("packets")) {
    Packet pkt;
    if (p.contains("pos")) {
      pkt.pos = p.at("pos").get<uint64_t>();
    }
    pkt.size = p.at("size").get<uint64_t>();
    if (p.contains("types")) {
      pkt.types = p.at("types").get<std::vector<uint32_t>>();
    }
    g.packets.push_back(std::move(pkt));
  }
  return g;
}

/// Every committed golden of `framing`: tests/golden/<codec>/<id>.ovt-units.json.
inline std::vector<Golden> committed_goldens(const std::string& framing) {
  std::vector<Golden> out;
  const std::filesystem::path root = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tests" / "golden";
  std::vector<std::filesystem::path> files;
  for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {  // the golden tree always exists
    if (e.is_regular_file() && e.path().filename().string().ends_with(".ovt-units.json")) {
      files.push_back(e.path());
    }
  }
  std::ranges::sort(files);
  for (const auto& f : files) {
    std::ifstream in(f);
    Golden g = parse_golden(nlohmann::json::parse(in));
    if (g.framing == framing) {
      out.push_back(std::move(g));
    }
  }
  return out;
}

inline std::optional<std::string> capture(const std::string& cmd) {
  // NOLINTNEXTLINE(cert-env33-c): test-only; the command is fixed and the paths come from the build
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) {
    return std::nullopt;
  }
  std::string out;
  std::array<char, 4096> chunk{};
  while (fgets(chunk.data(), static_cast<int>(chunk.size()), pipe.get()) != nullptr) {
    out += chunk.data();
  }
  return out;
}

inline bool tool_on_path(const char* name) {
  const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
  // NOLINTNEXTLINE(cert-env33-c,concurrency-mt-unsafe): test-only probe
  return std::system(cmd.c_str()) == 0;
}

/// The golden of `path` as tools/goldens/gen_units.py writes it; nullopt when python3, ffprobe or ffmpeg is missing.
inline std::optional<Golden> generate(const std::filesystem::path& path, const std::string& codec,
                                      const std::string& framing) {
  if (!tool_on_path("python3") || !tool_on_path("ffprobe") || !tool_on_path("ffmpeg")) {
    return std::nullopt;
  }
  const auto script = std::filesystem::path(OVT_TEST_SOURCE_DIR) / "tools" / "goldens" / "gen_units.py";
  const auto out = capture("python3 -I '" + script.string() + "' '" + path.string() + "' --codec " + codec +
                           " --framing " + framing + " 2>/dev/null");
  if (!out || out->empty()) {
    return std::nullopt;
  }
  return parse_golden(nlohmann::json::parse(*out));
}

/// A stream the test makes with FFmpeg into the build directory, removed by the destructor.
class MadeStream {
 public:
  MadeStream(const std::string& name, const std::string& source, const std::string& encode)
      : path_(std::filesystem::path(OVT_TEST_BINARY_DIR) / (std::to_string(::getpid()) + "_" + name)) {
    if (!tool_on_path("ffmpeg")) {
      return;
    }
    const std::string cmd = "ffmpeg -v error -y -nostdin -f lavfi -i '" + source + "' " + encode + " '" +
                            path_.string() + "' >/dev/null 2>&1";
    // NOLINTNEXTLINE(cert-env33-c,concurrency-mt-unsafe): test-only
    ok_ = std::system(cmd.c_str()) == 0 && std::filesystem::exists(path_);
  }
  ~MadeStream() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }
  MadeStream(const MadeStream&) = delete;
  MadeStream& operator=(const MadeStream&) = delete;
  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  bool ok_ = false;
};

inline constexpr const char* kTestSource = "testsrc=size=128x96:rate=10";

}  // namespace ovt::test::units

#endif  // OVT_TESTS_CONTAINER_UNITS_ORACLE_HPP
