// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared fixtures of the virtual-file tests (WI-4.210): synthesised DASH packages, byte helpers and a python3
// http.server child.

#ifndef OVT_TESTS_CONTAINER_VIRTUAL_FILE_FIXTURE_HPP
#define OVT_TESTS_CONTAINER_VIRTUAL_FILE_FIXTURE_HPP

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "container/virtual_file/segment_cache.hpp"
#include "container/virtual_file/virtual_file.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>

namespace ovt::test::vfile {

namespace fs = std::filesystem;
namespace vf = ovt::container::vfile;

using Bytes = std::vector<uint8_t>;

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& tag) {
    static int counter = 0;
    path = fs::temp_directory_path() /
           ("ovt_vfile_" + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

inline void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> s));
  }
}
inline void put64(Bytes& b, uint64_t v) {
  put32(b, static_cast<uint32_t>(v >> 32));
  put32(b, static_cast<uint32_t>(v));
}
inline void put_str(Bytes& b, const std::string& s) {
  b.insert(b.end(), s.begin(), s.end());
}

inline Bytes box(const std::string& type, const Bytes& payload) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  put_str(b, type);
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
inline Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const auto& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

inline Bytes init_segment() {
  Bytes ftyp;
  put_str(ftyp, "iso6");
  put32(ftyp, 0);
  put_str(ftyp, "iso6");
  put_str(ftyp, "dash");
  return cat({box("ftyp", ftyp), box("moov", box("mvhd", Bytes(100, 0)))});
}

// styp + moof(mfhd, traf(tfhd, tfdt)) + mdat; tfdt.baseMediaDecodeTime = (n - 1) * 1000.
inline Bytes media_segment(uint32_t n, size_t mdat_bytes) {
  Bytes styp;
  put_str(styp, "msdh");
  put32(styp, 0);
  put_str(styp, "msdh");
  Bytes mfhd;
  put32(mfhd, 0);
  put32(mfhd, n);
  Bytes tfhd;
  put32(tfhd, 0x20000);
  put32(tfhd, 1);
  Bytes tfdt;
  put32(tfdt, 0x01000000);
  put64(tfdt, uint64_t{n - 1} * 1000);
  Bytes mdat(mdat_bytes);
  for (size_t i = 0; i < mdat.size(); ++i) {
    mdat[i] = static_cast<uint8_t>((size_t{n} * 31) + i);
  }
  return cat({box("styp", styp),
              box("moof", cat({box("mfhd", mfhd), box("traf", cat({box("tfhd", tfhd), box("tfdt", tfdt)}))})),
              box("mdat", mdat)});
}

inline void write_file(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  REQUIRE(out.good());
}
inline void write_text(const fs::path& p, const std::string& s) {
  write_file(p, Bytes(s.begin(), s.end()));
}

constexpr uint32_t kSegments = 4;

// A DASH package: init.mp4, seg1..seg4.m4s and manifest.mpd under `dir`; returns the concatenation of the files.
inline Bytes write_dash_package(const fs::path& dir) {
  Bytes all = init_segment();
  write_file(dir / "init.mp4", all);
  for (uint32_t n = 1; n <= kSegments; ++n) {
    const Bytes seg = media_segment(n, size_t{100} * n);  // different sizes so a wrong offset cannot hide
    write_file(dir / ("seg" + std::to_string(n) + ".m4s"), seg);
    all.insert(all.end(), seg.begin(), seg.end());
  }
  write_text(dir / "manifest.mpd",
             R"(<?xml version="1.0"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static" mediaPresentationDuration="PT4S" minBufferTime="PT1S"
     profiles="urn:mpeg:dash:profile:isoff-live:2011">
  <Period id="p0">
    <AdaptationSet contentType="video" mimeType="video/mp4" segmentAlignment="true">
      <Representation id="v1" bandwidth="1000000" codecs="avc1.64001f" width="640" height="360">
        <SegmentTemplate timescale="1000" duration="1000" startNumber="1" initialization="init.mp4" media="seg$Number$.m4s"/>
      </Representation>
    </AdaptationSet>
  </Period>
</MPD>
)");
  return all;
}

inline Bytes read_all(const ovt::io::RandomAccessFile& f) {
  Bytes out(f.size());
  REQUIRE(f.read(0, out.size(), out.data()) == out.size());
  return out;
}

inline std::shared_ptr<vf::SegmentCache> fresh_cache(const TempDir& t) {
  return std::make_shared<vf::SegmentCache>(t.path / "cache" / "segments");
}

// A python3 http.server child serving `dir` on a free loopback port.
class HttpServer {
 public:
  explicit HttpServer(const fs::path& dir) {
    port_ = free_port();
    if (port_ == 0) {
      return;
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      if (const int null = ::open("/dev/null", O_WRONLY); null >= 0) {
        ::dup2(null, STDERR_FILENO);  // http.server logs every request to stderr
      }
      const std::string port = std::to_string(port_);
      ::execlp("python3", "python3", "-I", "-m", "http.server", port.c_str(), "--bind", "127.0.0.1", "--directory",
               dir.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    if (pid_ < 0) {
      return;
    }
    for (int i = 0; i < 100 && !up_; ++i) {
      up_ = connect_once();
      if (!up_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  ~HttpServer() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
  }
  [[nodiscard]] bool up() const { return up_; }
  [[nodiscard]] std::string base() const { return "http://127.0.0.1:" + std::to_string(port_) + "/"; }

 private:
  static uint16_t free_port() {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
      return 0;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    uint16_t port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) {
      port = ntohs(a.sin_port);
    }
    ::close(s);
    return port;
  }
  [[nodiscard]] bool connect_once() const {
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    ::close(s);
    return ok;
  }

  uint16_t port_ = 0;
  pid_t pid_ = -1;
  bool up_ = false;
};

}  // namespace ovt::test::vfile

#endif  // OVT_TESTS_CONTAINER_VIRTUAL_FILE_FIXTURE_HPP
