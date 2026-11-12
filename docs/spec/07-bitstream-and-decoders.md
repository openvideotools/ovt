# 7 — Bitstream parsing, stream indexing, and decoders

> **Scope.** This chapter specifies the `io`, `bitreader`, `syntax`, `index`, and `analysis` modules of `libovt-core` (§0.3), the analysis-decoder plugins (`ovt-plugin-aom`, `ovt-plugin-de265`, `ovt-plugin-ffmpeg`, `ovt-plugin-hm`) and the playback decoders. It conforms to ADR-001 (C++20, C ABI), ADR-007 (own position-recording header parsers), ADR-008 (block data from instrumented decoders behind `ovt_analysis_decoder_v1`), ADR-009 (two decode paths), ADR-013 (in-process plugins), ADR-015 (runtime-loaded decoder plugins) and the rules of §0.7. Data-model names are those of §0.5; the plugin ABI is §0.6.2 verbatim.
>
> **Inputs.** Research reports 02 (FOSS decoders and parsers) and 06 (codec internals). Spec references: *H.264* = ITU-T H.264 (08/2021); *H.265* = ITU-T H.265 (09/2023); *AV1* = AV1 Bitstream & Decoding Process Specification v1.0.0 with Errata 1.
>
> **Reading rule for implementers.** Every algorithm below is written so that it can be implemented line by line without consulting the codec specification; the spec section is cited so a reviewer can check it. Where this chapter says "MUST", a unit test or golden test named in §7.7/§7.8 checks it.

Section numbering inside this chapter: §7.1 `io`, §7.2 `bitreader`, §7.3 `syntax`, §7.4 `index`, §7.5 `analysis` host, §7.6 plugins, §7.7 test oracles, §7.8 work items, §7.9 glossary additions.

---

## 7.1 Module `io` — `ovt::io::RandomAccessFile`

### 7.1.1 Purpose and rules

`RandomAccessFile` is the **only** class in `libovt-core` that touches the operating system's file APIs for input data (§0.7.1). Every other module receives `std::span<const uint8_t>` views or copies obtained from it. It provides positional reads at arbitrary byte offsets, never a "current position", so that container, syntax and decoder code can run concurrently on different offsets of the same file (one `RandomAccessFile` instance MAY be shared by several threads for `read()`; see 7.1.4).

### 7.1.2 C++ interface (`core/include/ovt/cxx/io/random_access_file.hpp`)

```cpp
namespace ovt::io {

enum class OpenMode { Auto, Mmap, Pread };   // Auto = Mmap on desktop, Pread on Android (see 7.1.3)

struct FileInfo {
  uint64_t size;            // bytes
  int64_t  mtime_ns;        // modification time, ns since epoch; 0 if unknown (fd without stat)
  std::string display_name; // path, or "fd:<n>" / content URI display name supplied by the caller
};

class RandomAccessFile {
public:
  // Desktop: open by path (UTF-8). Throws ovt::IoError on failure.
  static std::unique_ptr<RandomAccessFile> open(std::string_view path, OpenMode mode = OpenMode::Auto);
  // Android / any OS: adopt an already-open descriptor. `size` MUST be the total length
  // (Android: ParcelFileDescriptor.getStatSize(); if < 0, the caller passes UINT64_MAX and
  // open_fd() uses fstat(2); if that fails too, IoError{OVT_E_IO}).
  // The RandomAccessFile takes ownership of fd (closes it in the destructor) when take_ownership == true.
  static std::unique_ptr<RandomAccessFile> open_fd(int fd, uint64_t size, bool take_ownership,
                                                  std::string display_name, OpenMode mode = OpenMode::Auto);
  virtual ~RandomAccessFile();

  const FileInfo& info() const noexcept;
  uint64_t size() const noexcept { return info().size; }

  // Copies min(len, size-offset) bytes into dst. Returns bytes copied (0 at/after EOF).
  // offset > size() → returns 0 (never throws for a short read). I/O failure → throws IoError.
  // Thread-safe: may be called concurrently from any number of threads.
  virtual size_t read(uint64_t offset, size_t len, uint8_t* dst) const = 0;

  // Zero-copy view when the backend is mmap; otherwise a copy into an internal, reference-counted
  // buffer. The returned span is valid as long as the returned Holder is alive.
  struct Holder { std::shared_ptr<const void> keepalive; std::span<const uint8_t> bytes; };
  virtual Holder view(uint64_t offset, size_t len) const = 0;

  // Hint only. mmap backend: madvise(MADV_WILLNEED / MADV_SEQUENTIAL / MADV_RANDOM). pread backend: no-op.
  enum class Access { Normal, Sequential, Random, WillNeed, DontNeed };
  virtual void advise(uint64_t offset, uint64_t len, Access) const noexcept = 0;

  // True when `view()` is zero-copy (mmap backend).
  virtual bool is_mapped() const noexcept = 0;
};

// Error type for this module (caught at the C API boundary → OVT_E_IO).
struct IoError : std::runtime_error {
  ovt_status code;       // OVT_E_IO or OVT_E_INVALID_ARG
  int os_errno;          // errno value or 0
  IoError(ovt_status c, int e, std::string msg);
};

} // namespace ovt::io
```

### 7.1.3 Backends and platform rules

| Platform | Default backend | Rules |
|---|---|---|
| macOS, Linux | `Mmap` | `mmap(PROT_READ, MAP_PRIVATE)` of the whole file in one mapping (files > 2^47 bytes are not supported; return `IoError{OVT_E_UNSUPPORTED}`). Mapping failure (ENOMEM, ENODEV for FUSE/network filesystems) → fall back to `Pread` transparently and log at level 2 (`io: mmap failed (errno), using pread`). `view()` returns the mapped range; `read()` is `memcpy` from the mapping. A `SIGBUS` caused by file truncation while mapped is **not** handled; the GUI chapter requires re-opening on size change (the index stores `source_hash` incl. size, §0.5.3). |
| Android | `Pread` | Files arrive as `content://` URIs; the Kotlin shell resolves them with `ContentResolver.openFileDescriptor(uri, "r")` and passes `ParcelFileDescriptor.detachFd()` plus `getStatSize()` to `ovt_file_open_fd` (§0.6.1). The C++ side uses `pread(2)` with retry on `EINTR`. `view()` copies into a 64 KiB-aligned buffer cache: an LRU of 64 blocks of 1 MiB, keyed by `offset / 1 MiB`, protected by a mutex (`read()` for lengths > 1 MiB bypasses the cache). `Mmap` MAY be used on Android when `fstat` reports a regular file **and** the caller passes `OpenMode::Mmap`; it is never the default because `content://` descriptors backed by `DocumentsProvider` pipes are not mappable. |
| Any | `Pread` | Also used for files on which `mmap` fails; identical semantics. |

`ovt_file_open` and `ovt_file_open_fd` (§0.6.1) are thin wrappers: `OVT_E_INVALID_ARG` for null/empty path or fd < 0; `OVT_E_IO` for every `IoError`; the `ovt_File` handle owns the `RandomAccessFile`.

### 7.1.4 Error behaviour (normative)

1. `read()`/`view()` with `offset >= size()` return 0 bytes / an empty span. They never throw for a short read; callers (container, unit scanners) decide whether a short read is a corruption (`Diagnostic` with code `IO-0001 "unexpected end of file"`).
2. OS-level failures (`EIO`, `EBADF`, `EACCES`) throw `IoError{OVT_E_IO, errno, strerror}`; callers do not retry.
3. `len > 2^31` in a single `read()` → `IoError{OVT_E_INVALID_ARG}`.
4. `open()` of a directory, socket or a file of size 0 → `IoError{OVT_E_INVALID_ARG, 0, "not a regular file / empty"}`.
5. Thread-safety contract: `read()`, `view()`, `advise()`, `info()` are safe to call concurrently; construction/destruction are not.

### 7.1.5 Unit tests (`core/tests/io/`)

- `io_open_missing_path_throws`, `io_open_empty_file_throws`, `io_read_past_eof_returns_zero`, `io_read_tail_short`, `io_view_equals_read` (random offsets/lengths vs `std::ifstream`), `io_pread_backend_matches_mmap` (same file both modes, 10 000 random reads), `io_fd_ownership` (fd closed on destruction when `take_ownership`), `io_concurrent_reads` (8 threads × 1 000 reads, compare checksums).

---

### 7.1.6 Shared infrastructure used by every module: `ovt::Result<T>`, `ovt::jobs`, `ovt::log`

These three small APIs are referenced by chapters 7–12 (`ovt::jobs::Cancel&` parameters in chapter 8, `jobs::Pool&` in chapter 9, `ovt::jobs::Pool` in chapter 5 §5.5). They are **implemented by WI-1.512** (chapter 12 §12.6; also owns the C headers `ovt/jobs.h`, `ovt/log.h`, `ovt/status.h`) but their C++ shape is fixed here so that every chapter means the same thing. C++ headers (D22, chapter 5 §5.2.1 header rule — never included from a C header, not installed): `core/include/ovt/cxx/result.hpp` (`ovt::Result<T>`), `core/include/ovt/cxx/cancel.hpp` (`ovt::Cancelled`), `core/include/ovt/cxx/jobs.hpp` (`ovt::jobs::*`; includes `cancel.hpp`), `core/include/ovt/cxx/log.hpp` (`ovt::log`). Every other C++ header of this chapter also lives under `core/include/ovt/cxx/` (`bitreader.hpp`, `rbsp.hpp`, `io/`, `syntax/`, `index/`, `analysis/`).

```cpp
namespace ovt {

// cxx/cancel.hpp — the one exception type besides ParseError (chapter 5 §5.7). Thrown only by Cancel::throw_if_requested();
// caught at the job boundary (Pool::submit wrapper, C API entry points) and mapped to OVT_E_CANCELLED. Never crosses the C ABI.
struct Cancelled : std::runtime_error {
  Cancelled() : std::runtime_error("cancelled") {}
};

// cxx/result.hpp — status + value without exceptions at module boundaries (§0.1.7: exceptions stay inside a module).
template <class T> class Result {
public:
  static Result ok(T v)                                { return Result(std::move(v)); }
  static Result error(ovt_status s, std::string msg)   { return Result(s, std::move(msg)); }
  bool        has_value() const noexcept               { return status_ == OVT_OK; }
  explicit    operator bool() const noexcept           { return has_value(); }
  ovt_status  status() const noexcept                  { return status_; }
  const std::string& message() const noexcept          { return message_; }   // empty when ok
  T&          value() &;      const T& value() const &;  T&& value() &&;        // precondition: has_value()
  // ...
};
template <> class Result<void> { /* status()/message()/has_value() only */ };

namespace jobs {   // cxx/jobs.hpp

// Cancellation token — checked cooperatively (every ≤ 50 ms of work, §0.7.2) by index, decode, container and metrics loops.
class Cancel {
public:
  bool requested() const noexcept;                     // true once request() was called (acquire load)
  void request() noexcept;                             // idempotent, thread-safe
  void throw_if_requested() const;                     // throws ovt::Cancelled (caught at the job boundary → OVT_E_CANCELLED)
  static Cancel& never();                              // shared token that is never requested (for synchronous CLI paths)
};
using CancelToken = Cancel;                            // alias used in some chapters

struct Progress { uint64_t done = 0, total = 0; std::string phase; };  // total = 0 → indeterminate

struct JobHandle {
  uint64_t id;
  Cancel&  cancel;                                     // request() cancels the job
  std::function<Progress()> progress;                  // snapshot, callable from any thread
  void wait();                                         // blocks until finished or cancelled
  bool done() const noexcept;
};

class Pool {
public:
  explicit Pool(unsigned threads = 0);                 // 0 = hardware_concurrency() capped at 16 (chapter 12 §12.1.2 --threads)
  ~Pool();                                             // requests cancel on every running job, then joins
  // The function receives the job's Cancel token and a progress sink; it MUST check cancel at least every 50 ms.
  JobHandle submit(std::string name, std::function<ovt_status(Cancel&, std::function<void(const Progress&)>)> fn);
  unsigned  threads() const noexcept;
  static Pool& global();                               // the process-wide pool used by the C API (ovt_Job) and the GUI
};

}  // namespace jobs

namespace log {   // cxx/log.hpp
enum class Level : uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };   // chapter 7 texts "level 1/3" refer to Debug/Warn
void   set_level(Level);                     Level level() noexcept;
void   set_sink(std::function<void(Level, std::string_view module, std::string_view code /* may be empty */, std::string_view message)>);
// Format of the default sink (stderr, and the --log-file of chapter 12 §12.1.2):
//   <ISO-8601 UTC time with ms> <LEVEL 5-wide> <module> [<diagnostic code>] <message>
//   e.g. 2026-11-03T10:15:42.117Z WARN  index [SYN-H264-0103] frame_num gap at unit 1234
// Logging is never used for data output (stdout is data only, chapter 12 §12.1.3) and never for Diagnostics that belong in a record.
#define OVT_LOG(level, module, ...)  /* fmt-style; compiled out below the build's minimum level */
}  // namespace log

}  // namespace ovt
```

Rules: (1) `Cancel` is passed **by reference** down every long-running call chain; functions MUST NOT store it beyond the call. (2) `Pool::submit` never throws; a job function that throws `ovt::Cancelled` ends the job with `OVT_E_CANCELLED`; one that throws anything else ends the job with `OVT_E_INTERNAL` and a level-4 log line (the exception never reaches the caller). (3) The GUI thread never calls `wait()` (chapter 4 FR-GUI-022). (4) Log lines are not deterministic output and therefore never appear on stdout.

## 7.2 Module `bitreader` — `ovt::BitReader`

### 7.2.1 Purpose

`BitReader` is the mandatory, bounds-checked reader for every parser (§0.7.1). It works on a contiguous byte buffer that is already free of emulation-prevention bytes (H.26x RBSP, 7.2.5) or is an OBU (AV1). It reads the descriptors defined in H.264 §7.2 / H.265 §7.2 (`u(n)`, `f(n)`, `b(8)`, `i(n)`, `ue(v)`, `se(v)`, `te(v)`, `me(v)`, `st(v)`, `ae(v)`/`ce(v)` excluded) and AV1 §4.10 (`f(n)`, `uvlc()`, `le(n)`, `leb128()`, `su(n)`, `ns(n)`, `L(n)`, `S()`, `NS(n)` excluded), plus the helper functions `more_rbsp_data()`, `byte_aligned()`, `rbsp_trailing_bits()`, `trailing_bits()`, `byte_alignment()`.

Bit offsets are 0-based from the start of the buffer handed to the constructor (§0.1.9). For an H.26x NAL unit this buffer is the RBSP **including the NAL unit header bytes** (so `forbidden_zero_bit` is at bit 0, exactly like FFmpeg `trace_headers`, §7.7.1). For an AV1 OBU it is the OBU bytes of `Unit.file_span` starting at `obu_header` bit 0 (an OBU contains no emulation prevention, so raw == RBSP; the phrase "OBU payload" in §0.5.1 is to be read as these bytes).

### 7.2.2 Class declaration (`core/include/ovt/cxx/bitreader.hpp`)

```cpp
namespace ovt {

struct ParseError : std::runtime_error {
  std::string code;        // stable code, e.g. "BR-0001" (table 7.2.4)
  uint64_t    bit_offset;  // position at which the failing read started
  ParseError(std::string code, std::string message, uint64_t bit_offset);
};

struct BitSpan { uint64_t bit_offset; uint32_t bit_length; };   // same layout as ovt.fb.BitSpan

class BitReader {
public:
  using Mark = uint64_t;                                    // an absolute bit position

  BitReader(std::span<const uint8_t> data) noexcept;        // data must outlive the reader
  BitReader(const uint8_t* data, size_t size) noexcept;

  // ---- position --------------------------------------------------------------------------
  uint64_t bit_pos()  const noexcept;                       // bits consumed so far
  uint64_t bits_left() const noexcept;                      // size*8 - bit_pos()
  uint64_t size_bits() const noexcept;
  bool     byte_aligned() const noexcept;                   // bit_pos() % 8 == 0           (H.26x byte_aligned())
  void     seek_bits(uint64_t abs_bit_pos);                 // BR-0002 if > size_bits()
  void     skip_bits(uint64_t n);                           // BR-0001 if n > bits_left()
  void     skip_bytes(uint64_t n);                          // requires byte_aligned(), else BR-0004
  Mark     mark() const noexcept { return bit_pos(); }
  BitSpan  span_since(Mark m) const noexcept;               // {m, bit_pos()-m}; length saturates at UINT32_MAX
  std::span<const uint8_t> remaining_bytes() const;         // requires byte_aligned(); view of unread bytes
  std::span<const uint8_t> bytes(size_t byte_off, size_t n) const; // bounds-checked raw access (BR-0001)

  // ---- peek (never advance) ------------------------------------------------------------------
  uint64_t peek_bits(unsigned n) const;                     // n <= 64; BR-0001 if n > bits_left()
  bool     peek_bit() const;

  // ---- H.264 / H.265 descriptors (§7.2 of either spec) -------------------------------------
  uint64_t u(unsigned n);          // u(n): unsigned, MSB first. n in [0,64]. n == 0 returns 0 and reads nothing.
  uint64_t f(unsigned n);          // f(n): same as u(n); the *caller* checks the fixed value (7.2.3 helper expect_f)
  uint8_t  b8();                   // b(8): one byte, any value
  int64_t  i(unsigned n);          // i(n): two's complement, MSB first, n in [1,64]
  uint32_t ue();                   // ue(v): Exp-Golomb order 0, §9.1. Result <= 2^32-2. BR-0005 if leadingZeroBits > 31.
  int32_t  se();                   // se(v): §9.1.1: k = ue(); value = (k & 1) ? (k+1)/2 : -(k/2)
  uint32_t te(uint32_t cmax);      // te(v): §9.1.2: cmax > 1 → ue(); cmax == 1 → !u(1); cmax == 0 → BR-0006
  // me(v): §9.1.3 maps codeNum=ue() to coded_block_pattern through Table 9-4; it appears only in
  // macroblock_layer() (slice data) which OVT never parses (ADR-008). Provided for completeness:
  uint32_t me(uint32_t chroma_array_type, bool is_intra);   // returns coded_block_pattern via Table 9-4 (both columns)
  std::string st();                // st(v): UTF-8 bytes up to and excluding a 0x00 byte; requires byte_aligned() (BR-0004);
                                   // BR-0001 if no terminator before the end; max 65535 bytes (BR-0007)

  // helpers
  void   expect_f(unsigned n, uint64_t value, const char* name);    // reads f(n); ParseError SYN-GEN-0003 "forbidden/reserved value" if != value
  bool   more_rbsp_data() const;   // H.264 §7.2 / H.265 §7.2 definition, algorithm 7.2.3
  void   rbsp_trailing_bits();     // §7.3.2.11: rbsp_stop_one_bit (=1) then rbsp_alignment_zero_bit (=0) until byte_aligned(); BR-0008 on wrong values
  bool   more_data_in_byte_stream() const noexcept { return bits_left() > 0; }

  // ---- AV1 descriptors (§4.10) ---------------------------------------------------------------
  // f(n) is shared with the u(n) above (AV1 §4.10.2 == same bit order).
  uint64_t leb128();               // AV1 §4.10.5: up to 8 bytes; result < 2^56. If the 8th byte still has its continuation bit set (conformance violation, not a read error) the bit is ignored, BR-0009 is reported as a Warning to the attached DiagnosticSink (see "diagnostic sink" below; dropped when none is attached) and reading continues
  uint32_t uvlc();                 // AV1 §4.10.3: leadingZeros counted until a 1; if leadingZeros >= 32 return (1<<32)-1; else f(leadingZeros) + (1<<leadingZeros) - 1
  int64_t  su(unsigned n);         // AV1 §4.10.6: value = f(n); signMask = 1 << (n-1); if (value & signMask) value -= 2*signMask
  uint32_t ns(uint32_t n);         // AV1 §4.10.7: w = FloorLog2(n)+1; m = (1<<w)-n; v = f(w-1); if v < m return v; extra = f(1); return (v<<1) - m + extra.   n == 0 → BR-0006
  uint64_t le(unsigned n);         // AV1 §4.10.4: n bytes little-endian, requires byte_aligned() (BR-0004), n in [1,8]
  int32_t  delta_q();              // AV1 §5.9.13 read_delta_q(): delta_coded = f(1); return delta_coded ? su(1+6) : 0
  // L(n) (AV1 §4.10.8) and S()/NS(n) are symbol-decoder (arithmetic) descriptors used only inside tile data,
  // which OVT does not parse. Declared so parsers can `static_assert` they are never called on a BitReader:
  [[noreturn]] uint64_t L(unsigned n);        // always throws BR-0010 "L(n) requires the AV1 symbol decoder"
  void     trailing_bits(uint64_t nb_bits);   // AV1 §5.3.4: trailing_one_bit f(1) == 1, then nb_bits-1 zero bits == 0; BR-0008 on mismatch
  void     byte_alignment();                  // AV1 §5.3.5: zero bits until byte_aligned(); BR-0008 if a one bit is seen

  // ---- diagnostic sink (non-fatal reader findings, e.g. BR-0009) -------------------------------
  // Fatal problems are thrown as ParseError. Non-fatal conformance findings are reported to an attached sink.
  // SyntaxBuilder implements the sink and attaches itself in its constructor (§7.3.2), so a parser never calls this.
  struct DiagnosticSink {
    virtual void reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) = 0;
  protected:
    ~DiagnosticSink() = default;
  };
  void            set_diagnostic_sink(DiagnosticSink* sink) noexcept;   // non-owning; nullptr detaches; returns nothing
  DiagnosticSink* diagnostic_sink() const noexcept;                     // currently attached sink or nullptr

  // ---- diagnostics ---------------------------------------------------------------------------
  // Reads are counted; parsers call this at the end of a unit to flag unread trailing bytes
  // (Diagnostic Info "N bytes of trailing data", not an error — H.264 §7.4.1 allows cabac_zero_words etc.).
  uint64_t unread_bytes_after_trailing() const noexcept;
};

} // namespace ovt
```

Implementation requirements: `u(n)` MUST be implemented with a 64-bit cache refilled bytewise (no unaligned loads of the input); every public read method first checks `n <= bits_left()` and throws `ParseError{"BR-0001", ..., bit_pos()}` **before** advancing; no method may read outside `[data, data+size)` under any input (checked by the fuzz target 7.2.7). `ue()` counts leading zeros bit by bit with a cap of 32 (BR-0005), so a stream of zeros cannot loop.

### 7.2.3 `more_rbsp_data()` algorithm (H.264 §7.2 / H.265 §7.2, "more_rbsp_data")

```
1. if bits_left() == 0: return false
2. Find the last byte of the buffer that is != 0x00; call its index L. If none exists: return false
   (malformed: no rbsp_stop_one_bit; the parser will hit BR-0008 in rbsp_trailing_bits()).
3. Let T = (L*8) + (7 - index_of_lowest_set_bit(data[L]))   -- absolute bit position of rbsp_stop_one_bit
4. return bit_pos() < T
```
The computation of `L`/`T` is done once in the constructor (`stop_bit_pos_`), so `more_rbsp_data()` is O(1). Note: for H.26x `cabac_zero_words` (0x0000) after `rbsp_slice_trailing_bits` are zero bytes and are skipped correctly by step 2.

### 7.2.4 Error codes thrown by `BitReader`

| Code | Condition | Message template |
|---|---|---|
| `BR-0001` | read of n bits with n > bits_left() | `"read of %u bits at bit %llu exceeds unit size (%llu bits)"` |
| `BR-0002` | `seek_bits` beyond end | `"seek to bit %llu beyond end"` |
| `BR-0003` | `u(n)` with n > 64, `i(n)` with n == 0 or n > 64 | `"invalid descriptor width %u"` |
| `BR-0004` | byte-aligned operation while not aligned | `"operation requires byte alignment at bit %llu"` |
| `BR-0005` | `ue()` with ≥ 32 leading zero bits | `"Exp-Golomb code too long at bit %llu"` |
| `BR-0006` | `te(0)`, `ns(0)` | `"invalid range argument for %s"` |
| `BR-0007` | `st()` longer than 65535 bytes | `"string too long"` |
| `BR-0008` | wrong trailing/alignment bit value | `"expected %s at bit %llu, got %u"` |
| `BR-0009` | `leb128` whose 8th byte has the continuation bit set — **Warning**, not thrown: recorded via the attached `SyntaxBuilder` and parsing continues with the 56-bit value | `"leb128 8th byte has continuation bit at bit %llu"` |
| `BR-0010` | `L(n)` called | `"L(n) requires the AV1 symbol decoder"` |

At the module boundary (`ovt_stream_build_index`, `ovt_stream_unit_syntax`) a `ParseError` becomes `OVT_E_CORRUPT` and a `Diagnostic{Severity::Error, code, message, span{bit_offset, 0}, spec_ref}` attached to the unit's `SyntaxNode.diagnostics`; indexing continues with the next unit (§0.7.1, 7.4.6).

### 7.2.5 Emulation-prevention removal and offset mapping (`core/include/ovt/cxx/rbsp.hpp`)

```cpp
namespace ovt {
struct Rbsp {
  std::vector<uint8_t>  rbsp;            // NAL header bytes + payload with EPBs removed
  std::vector<uint32_t> epb_positions;   // RAW byte offsets (within the NAL unit, header byte 0 = 0) of each removed 0x03, ascending
};
// H.264 §7.3.1 / H.265 §7.3.1.1 nal_unit(): removes emulation_prevention_three_byte.
Rbsp rbsp_from_nal(std::span<const uint8_t> nal_bytes);

// Mapping helpers used by the hex view (both O(log n) by binary search over epb_positions):
uint64_t rbsp_bit_to_raw_bit(uint64_t rbsp_bit, std::span<const uint32_t> epb_positions);
// raw_byte_is_epb → true when the raw byte is a removed 0x03 (the hex view paints it grey; it has no RBSP position).
std::pair<uint64_t,bool> raw_byte_to_rbsp_byte(uint64_t raw_byte, std::span<const uint32_t> epb_positions);
}
```

**`rbsp_from_nal` algorithm** (exact; nal_bytes excludes start code / length prefix):

```
zeros = 0; out.reserve(n)
for i in 0 .. n-1:
    b = nal_bytes[i]
    if zeros >= 2 and b == 0x03:
        # H.264 §7.4.1: within the NAL, 0x000003 → the 0x03 is emulation_prevention_three_byte
        # (regardless of the following byte; a following byte > 0x03 is a stream error reported as
        #  Diagnostic SYN-GEN-0002 "emulation prevention byte followed by 0x%02X" but the byte is still removed)
        epb_positions.push_back(i)
        zeros = 0
        continue
    out.push_back(b)
    zeros = (b == 0x00) ? zeros + 1 : 0
```
The first two bytes (H.264: one header byte; H.265: two header bytes) are copied by the same loop; the spec forbids EPB there and none will match since `zeros < 2`.

**`rbsp_bit_to_raw_bit`**: `rbsp_byte = rbsp_bit / 8; raw_byte = rbsp_byte; for p in epb_positions (ascending): if p <= raw_byte: raw_byte += 1 else break; return raw_byte*8 + (rbsp_bit % 8)`. Implemented as: `k = number of epb positions p with p <= rbsp_byte + k` found by iterating (each EPB shifts the frontier by one), equivalently `std::upper_bound` repeated at most |epb| times; a 64-entry stride table makes it O(log n) for large slices.

**`raw_byte_to_rbsp_byte`**: `k = count(epb_positions < raw_byte)` (binary search); `if raw_byte ∈ epb_positions: return {raw_byte - k, true}` (the position the *next* RBSP byte will have) `else return {raw_byte - k, false}`.

`Unit.epb_positions` (§0.5.3) stores exactly `Rbsp.epb_positions`. For AV1 units the vector is empty and both mapping functions are the identity.

### 7.2.6 Worked example (used as a golden test vector)

Raw NAL `67 42 00 1E 00 00 03 01 E9` → `rbsp = 67 42 00 1E 00 00 01 E9`, `epb_positions = {6}`. `rbsp_bit_to_raw_bit(6*8+3) = 7*8+3 = 59`. `raw_byte_to_rbsp_byte(6) = {6, true}`; `raw_byte_to_rbsp_byte(7) = {6, false}`.

### 7.2.7 Unit tests (`core/tests/bitreader/`) and fuzz target

| Test name | Checks |
|---|---|
| `br_u_all_widths` | `u(n)` for n = 0..64 over a 1 KiB random buffer vs a naive bit-by-bit reference implementation |
| `br_i_signed` | `i(n)` sign extension for n = 1..64, e.g. `i(3)` of `111` = −1 |
| `br_ue_table` | `ue()` of the §9.1 Table 9-2 examples: `1`→0, `010`→1, `011`→2, `00100`→3, `00101`→4, `00110`→5, `00111`→6, `0001000`→7 |
| `br_se_table` | `se()` Table 9-3: 0→0, 1→1, 2→−1, 3→2, 4→−2 |
| `br_te` | `te(1)` with bit 0 → 1, bit 1 → 0; `te(5)` behaves as `ue()`; `te(0)` throws BR-0006 |
| `br_ue_too_long` | 32 zero bits → BR-0005 |
| `br_bounds` | every read method at `bits_left()-1` throws BR-0001 and leaves `bit_pos()` unchanged |
| `br_more_rbsp_data` | buffers `80`, `00 80`, `C0`, `C0 00 00` (cabac_zero_words), `00` (no stop bit) |
| `br_rbsp_trailing_bits` | valid `80`, invalid `40` (BR-0008), unaligned start |
| `br_leb128` | AV1 §4.10.5 vectors: `00`→0, `7F`→127, `80 01`→128, `FF FF FF FF FF FF FF 7F`→2^56−1, `80×8` → value 0, 8 bytes consumed, Warning BR-0009 recorded (no throw) |
| `br_uvlc` | `1`→0, `010`→1, `0000…(32 zeros)`→4294967295 |
| `br_su_ns` | `su(7)` of `1000000`= −64; `ns(5)`: inputs `00`→0, `01`→1, `10`→2, `110`→3, `111`→4 (w=3, m=3) |
| `br_le` | `le(4)` of `01 02 03 04` = 0x04030201 |
| `br_delta_q` | `0`→0; `1 1000000`→−64 |
| `br_trailing_bits` | `trailing_bits(8)` of `80` ok, of `C0` → BR-0008 |
| `br_st` | `"abc\0"`→"abc"; missing terminator → BR-0001 |
| `rbsp_golden` | the 7.2.6 vector plus 20 generated NALs with EPBs at random positions: `rbsp_from_nal` round-trip with a reference encoder (insert 0x03 after `00 00` followed by `00..03`) |
| `rbsp_mapping_bijection` | for every RBSP byte, `raw_byte_to_rbsp_byte(rbsp_bit_to_raw_bit(b*8)/8) == {b,false}` |
| fuzz `fuzz_bitreader` | libFuzzer target: random descriptor sequence driven by the fuzz input; asserts no read outside the buffer (ASan) and that only `ParseError` escapes |

---

## 7.3 Module `syntax` — header-level parsers with position recording

### 7.3.1 Design

`core/src/syntax/` contains one sub-directory per codec (`h264/`, `h265/`, `av1/`) plus `common/` (SyntaxBuilder, T.35 registry, SEI framing shared by H.264/H.265). A parser is a free function that takes a `BitReader` and a `SyntaxBuilder` and mirrors the spec's syntax table **line by line**: one `read_*` call per syntax element in spec order, with the exact spec element name as the node name. Nothing is reordered or skipped, so a reviewer can put the spec table and the function side by side. Derived variables (spec "semantics" values) are emitted as `derived` text on the owning node and stored in typed C++ structs that the `index` module uses.

Rules:
1. Every parser catches nothing; `ParseError` propagates to the caller (`index` or the C API), which attaches the diagnostic and keeps the partial tree built so far (SyntaxBuilder is exception-safe: `finish()` returns the nodes created before the throw).
2. A parser MUST NOT interpret slice data / tile data. Those bytes are the opaque remainder of the unit (`SyntaxNode{name:"slice_data", kind:Bytes, span}` with `bytes` left empty; the hex view reads the file directly).
3. Numbers of array elements are bounded by the spec's maxima (e.g. `num_ref_idx_l0_active_minus1 ≤ 31`, `num_short_term_ref_pic_sets ≤ 64`, `vps_max_sub_layers_minus1 ≤ 6`, AV1 `operating_points_cnt_minus_1 ≤ 31`, `TileCols ≤ 64`). Exceeding a maximum throws `ParseError{"SYN-<CODEC>-<nnnn>", "... out of range [a,b]"}` **before** any loop is entered. Reserved/forbidden values are reported as `Diagnostic` Warnings but parsing continues when the spec allows a decoder to ignore them (e.g. `profile_idc` unknown).
4. Each parser has a header (e.g. `core/include/ovt/cxx/syntax/h264/sps.hpp`) with a typed struct (`ovt::syntax::h264::Sps`) holding every syntax element and every derived variable listed in the tables below; the struct is what other modules consume; the `SyntaxNode` tree is for display.

### 7.3.2 `SyntaxBuilder` (`core/include/ovt/cxx/syntax/syntax_builder.hpp`)

```cpp
namespace ovt::syntax {

class SyntaxBuilder : public BitReader::DiagnosticSink {
public:
  // Attaches itself to `br` (br.set_diagnostic_sink(this)) and restores the previous sink in the destructor.
  // A reader warning (e.g. BR-0009) becomes diag(Severity::Warning, code, message) on the current struct.
  explicit SyntaxBuilder(BitReader& br, std::string_view default_spec_ref = {});
  ~SyntaxBuilder();
  SyntaxBuilder(const SyntaxBuilder&) = delete; SyntaxBuilder& operator=(const SyntaxBuilder&) = delete;
  void reader_warning(std::string_view code, std::string_view message, uint64_t bit_offset) override;

  // Structure nesting. begin_struct() creates a node with kind None whose span is closed by end_struct().
  void begin_struct(std::string_view name, std::string_view spec_ref = {});
  void end_struct();                       // sets span = {start, bit_pos()-start}
  // RAII convenience
  struct Scope { SyntaxBuilder& b; ~Scope() { b.end_struct(); } };
  [[nodiscard]] Scope scoped(std::string_view name, std::string_view spec_ref = {});

  // Array element nesting: every read_* inside gets array_index = idx (nodes named like "offset_for_ref_frame").
  void begin_array_element(std::string_view name, int idx, std::string_view spec_ref = {});
  void end_array_element();

  // Reads. Each one: m = br.mark(); v = br.<descriptor>(...); emit node {name, descriptor text, kind, value, span_since(m)}; return v.
  uint64_t read_u (std::string_view name, unsigned n);                       // "u(n)"  kind UInt
  uint64_t read_f (std::string_view name, unsigned n, uint64_t expected);    // "f(n)"; Diagnostic Error SYN-GEN-0001 if != expected, continue
  bool     read_flag(std::string_view name);                                 // "u(1)"  kind Bool
  int64_t  read_i (std::string_view name, unsigned n);                       // "i(n)"  kind Int
  uint32_t read_ue(std::string_view name);                                   // "ue(v)" kind UInt
  int32_t  read_se(std::string_view name);                                   // "se(v)" kind Int
  uint32_t read_te(std::string_view name, uint32_t cmax);                    // "te(v)"
  std::string read_st(std::string_view name);                                // "st(v)" kind String
  std::span<const uint8_t> read_bytes(std::string_view name, size_t n);      // "b(8)*n" kind Bytes (copied into node.bytes when n <= 256, else only span)
  // AV1
  uint64_t read_leb128(std::string_view name);                               // "leb128()"
  uint32_t read_uvlc(std::string_view name);                                 // "uvlc()"
  int64_t  read_su(std::string_view name, unsigned n);                       // "su(n)" (descriptor text "su(1+6)" when the caller passes a label, see below)
  uint32_t read_ns(std::string_view name, uint32_t n);                       // "ns(n)"
  uint64_t read_le(std::string_view name, unsigned n);                       // "le(n)"
  int32_t  read_delta_q(std::string_view name);                              // "delta_q" → two child nodes delta_coded f(1), delta_q su(1+6)
  // Enum decoration: sets enum_name of the LAST emitted node (e.g. after read_ue("slice_type"), set_enum("P")).
  void set_enum(std::string_view enum_name);
  // Overrides the descriptor text of the LAST emitted node (e.g. "u(v)" after read_u(name, bits)).
  void set_descriptor_text(std::string_view descriptor);
  // Derived values: appended as a child node {name, kind None, derived:text, span:{bit_pos,0}} of the current struct.
  void derived(std::string_view name, std::string_view text);
  template<class T> void derived(std::string_view name, T value) { derived(name, fmt::format("{} = {}", name, value)); }
  // Diagnostics attached to the current struct (or the last node if attach_to_last).
  void diag(Severity, std::string_view code, std::string_view message, std::string_view spec_ref = {}, bool attach_to_last = false);
  // Opaque remainder (slice_data / tile data): node kind Bytes with span [bit_pos, end) and no copy.
  void opaque_rest(std::string_view name);

  // Result: the root node (the unit) and its children; call exactly once. Partial trees are returned after a ParseError.
  SyntaxNodeBuffer finish();           // SyntaxNodeBuffer = in-memory tree convertible to the ovt.fb.SyntaxNode FlatBuffer (model module)
  // Flag for performance mode (7.4.5): when false, read_* records nothing and only returns values.
  void set_recording(bool on) noexcept;
};
}
```

Descriptor text rule: the `descriptor` string of a node is exactly the spec's descriptor with the literal bit count substituted (`"u(8)"`, `"u(v)"` for variable-length `u(v)` elements such as `slice_pic_order_cnt_lsb`, `"su(1+6)"`, `"ns(8)"`, `"f(1)"`, `"le(2)"`, `"leb128()"`). For `u(v)` the caller passes `read_u(name, bits)` and then `set_descriptor_text("u(v)")`. Example of parser style (H.265 §7.3.2.3.1 excerpt):

```cpp
auto s = b.scoped("pic_parameter_set_rbsp", "H.265 7.3.2.3.1");
pps.pps_pic_parameter_set_id = b.read_ue("pps_pic_parameter_set_id");          // 0..63
if (pps.pps_pic_parameter_set_id > 63) throw ParseError{"SYN-H265-0010", "pps_pic_parameter_set_id out of range [0,63]", br.bit_pos()};
pps.pps_seq_parameter_set_id = b.read_ue("pps_seq_parameter_set_id");          // 0..15
pps.dependent_slice_segments_enabled_flag = b.read_flag("dependent_slice_segments_enabled_flag");
...
pps.init_qp_minus26 = b.read_se("init_qp_minus26");
b.derived("SliceQpY(default)", 26 + pps.init_qp_minus26);
```

### 7.3.3 Unit splitting (shared by `container` and `index`)

`core/include/ovt/cxx/syntax/unit_scan.hpp` provides the scanners that produce `Unit.file_span`/`prefix_span`:

| Function | Input | Rule |
|---|---|---|
| `scan_annexb(RandomAccessFile&, callback)` | `.h264/.264/.h265/.hevc` byte streams, MPEG-TS PES payloads | H.264 Annex B / H.265 Annex B: a start code is `00 00 01`; a preceding `00` (zero_byte) and trailing_zero_8bits belong to `prefix_span`. Uses `memchr`-based search on 1 MiB `view()` windows with 4-byte overlap. A NAL unit ends at the next start code or EOF; trailing zero bytes before a start code are stripped from the NAL (they are `trailing_zero_8bits`). |
| `split_length_prefixed(span, nal_length_size)` | ISOBMFF samples (`avcC.lengthSizeMinusOne+1`, `hvcC`), MPEG-4 sample payloads | Loop: read N-byte big-endian length (N ∈ {1,2,4}); `prefix_span` = length field; `Diagnostic SYN-GEN-0105 "length-prefixed unit overruns its sample"` if length overruns the sample (unit truncated to the sample end). |
| `scan_obu_low_overhead(span, cb)` | AV1 Section 5 streams (`.obu`, IVF frames, ISOBMFF `av01` samples, `av1C.configOBUs`) | Parse `obu_header` (1 byte + optional extension byte), `obu_has_size_field == 0` is legal only for the **last** OBU of a length-delimited unit (AV1 Annex B `obu_length`, an ISOBMFF sample or an IVF frame; AV1 §5.3.1 / Annex B) — such an OBU extends to the end of the span; a size-less OBU that is not last in its span, or in a Section 5 low-overhead stream, is a conformance violation → `Diagnostic Error SYN-AV1-0001 "obu_has_size_field = 0 in a position that requires a size"` (the OBU still extends to the end of the span); otherwise `obu_size = leb128()`; `file_span` = whole OBU incl. header and size field; `prefix_span` = empty. |
| `scan_obu_annexb(span, cb)` | AV1 Annex B (`.obu` detected by the container sniffer) | `temporal_unit_size leb128(); frame_unit_size leb128(); obu_length leb128(); open_bitstream_unit(obu_length)` nested loops (AV1 Annex B.2); the three length fields are `prefix_span`s of the first OBU of each unit; inside, `obu_has_size_field` may be 0. |

Each callback receives `(file_offset_of_unit, unit_bytes_view, prefix_span, container_sample_index)`.

### 7.3.4 H.264 syntax structures to implement

All structures below live in `core/src/syntax/h264/`. "Node" is the root `SyntaxNode.name`. "Derived" lists variables that MUST be computed, stored in the typed struct and emitted with `derived()`; variable names are the spec's.

| # | Structure (spec §) | Node name | Notes / derived values |
|---|---|---|---|
| 1 | `nal_unit()` header, §7.3.1 | `nal_unit_header` | `forbidden_zero_bit f(1)=0`, `nal_ref_idc u(2)`, `nal_unit_type u(5)`; for types 14, 20, 21: `svc_extension_flag`/`avc_3d_extension_flag u(1)` and `nal_unit_header_svc_extension()` (G.7.3.1.1: idr_flag, priority_id, no_inter_layer_pred_flag, dependency_id, quality_id, temporal_id, use_ref_base_pic_flag, discardable_flag, output_flag, reserved_three_2bits) or `nal_unit_header_mvc_extension()` (H.7.3.1.1: non_idr_flag, priority_id, view_id, temporal_id, anchor_pic_flag, inter_view_flag, reserved_one_bit) or `nal_unit_header_3davc_extension()` (I.7.3.1.1). Derived: `IdrPicFlag = (nal_unit_type == 5)`, `type_name` (Table 7-1), `Unit.temporal_id` (from SVC/MVC ext, else 0), `Unit.layer_id` (dependency_id or view order index, else 0). |
| 2 | `seq_parameter_set_data()`, §7.3.2.1.1 | `seq_parameter_set_rbsp` | `profile_idc u(8)`, `constraint_set0..5_flag`, `reserved_zero_2bits`, `level_idc`, `seq_parameter_set_id ue ≤ 31`; if profile_idc ∈ {100,110,122,244,44,83,86,118,128,138,139,134,135}: `chroma_format_idc ue ≤ 3`, `separate_colour_plane_flag` (if 3), `bit_depth_luma_minus8 ue ≤ 6`, `bit_depth_chroma_minus8`, `qpprime_y_zero_transform_bypass_flag`, `seq_scaling_matrix_present_flag` + 8 or 12 `scaling_list()` (#3); `log2_max_frame_num_minus4 ue ≤ 12`, `pic_order_cnt_type ue ≤ 2`; type 0: `log2_max_pic_order_cnt_lsb_minus4 ≤ 12`; type 1: `delta_pic_order_always_zero_flag`, `offset_for_non_ref_pic se`, `offset_for_top_to_bottom_field se`, `num_ref_frames_in_pic_order_cnt_cycle ue ≤ 255`, `offset_for_ref_frame[i] se`; `max_num_ref_frames ue ≤ 16`, `gaps_in_frame_num_value_allowed_flag`, `pic_width_in_mbs_minus1`, `pic_height_in_map_units_minus1`, `frame_mbs_only_flag`, `mb_adaptive_frame_field_flag` (if !frame_mbs_only), `direct_8x8_inference_flag`, `frame_cropping_flag` + 4 offsets, `vui_parameters_present_flag` + `vui_parameters()` (#4). **Derived:** `ChromaArrayType = separate_colour_plane_flag ? 0 : chroma_format_idc` (default chroma_format_idc = 1 when absent); `SubWidthC/SubHeightC` (Table 6-1: 420→2,2; 422→2,1; 444→1,1; 400→n/a); `BitDepthY = 8 + bit_depth_luma_minus8`, `BitDepthC`, `QpBdOffsetY = 6*bit_depth_luma_minus8`, `QpBdOffsetC`; `MaxFrameNum = 2^(log2_max_frame_num_minus4+4)`; `MaxPicOrderCntLsb = 2^(log2_max_pic_order_cnt_lsb_minus4+4)`; `ExpectedDeltaPerPicOrderCntCycle = Σ offset_for_ref_frame[i]`; `PicWidthInMbs = pic_width_in_mbs_minus1+1`; `PicWidthInSamplesL = PicWidthInMbs*16`; `PicHeightInMapUnits`; `FrameHeightInMbs = (2 − frame_mbs_only_flag) * PicHeightInMapUnits`; `CropUnitX/CropUnitY` (§7.4.2.1.1: ChromaArrayType 0 → 1, 2−frame_mbs_only_flag; else SubWidthC, SubHeightC*(2−frame_mbs_only_flag)); crop rect `x = CropUnitX*frame_crop_left_offset, w = PicWidthInSamplesL − CropUnitX*(left+right)`, `y`, `h` likewise with `FrameHeightInMbs*16`; `MaxDpbMbs` from Table A-1 by level_idc (+ `constraint_set3_flag` for level 1b) and `MaxDpbFrames = Min(MaxDpbMbs / (PicWidthInMbs*FrameHeightInMbs), 16)`. |
| 3 | `scaling_list()`, §7.3.2.1.1.1 | `scaling_list` | `delta_scale se` loop with `useDefaultScalingMatrixFlag`; store the 16/64 entries and the flag; derived `UseDefaultScalingMatrixFlag`. Fall-back rules A/B (Table 7-2) are applied when PPS is activated (7.3.8). |
| 4 | `vui_parameters()`, Annex E.1.1 | `vui_parameters` | aspect_ratio (`aspect_ratio_idc`, Extended_SAR 255 → sar_width/height u(16)), overscan, video_signal_type (`video_format u(3)`, `video_full_range_flag`, `colour_description_present_flag` → `colour_primaries u(8)`, `transfer_characteristics u(8)`, `matrix_coefficients u(8)`), chroma_loc (`chroma_sample_loc_type_top_field/bottom_field ue ≤ 5`), timing (`num_units_in_tick u(32)`, `time_scale u(32)`, `fixed_frame_rate_flag`), `nal_hrd_parameters_present_flag` → `hrd_parameters()` (#5), `vcl_hrd_...`, `low_delay_hrd_flag`, `pic_struct_present_flag`, `bitstream_restriction_flag` → `motion_vectors_over_pic_boundaries_flag`, `max_bytes_per_pic_denom`, `max_bits_per_mb_denom`, `log2_max_mv_length_horizontal/vertical`, `max_num_reorder_frames`, `max_dec_frame_buffering`. Derived: `frame_rate = time_scale / (2*num_units_in_tick)` (E.2.1, field-based tick), `ovt_ColorDescription` candidate (primaries, transfer, matrix, full_range, chroma_siting: `chroma_sample_loc_type_top_field` 0→1 (left), 1→2 (centre), 2→3 (top-left), 3..5→0 (no OVT siting code; Figure E-1)). Note VUI `enum_name` values from Tables E-1..E-5 for primaries/transfer/matrix. |
| 5 | `hrd_parameters()`, E.1.2 | `hrd_parameters` | `cpb_cnt_minus1 ue ≤ 31`, `bit_rate_scale u(4)`, `cpb_size_scale u(4)`, per CPB `bit_rate_value_minus1 ue`, `cpb_size_value_minus1 ue`, `cbr_flag`, then `initial_cpb_removal_delay_length_minus1 u(5)`, `cpb_removal_delay_length_minus1 u(5)`, `dpb_output_delay_length_minus1 u(5)`, `time_offset_length u(5)`. Derived: `BitRate[i] = (bit_rate_value_minus1+1) * 2^(6+bit_rate_scale)`, `CpbSize[i] = (cpb_size_value_minus1+1) * 2^(4+cpb_size_scale)` (E.2.2). The lengths are needed to parse SEI #8/#9. |
| 6 | `pic_parameter_set_rbsp()`, §7.3.2.2 | `pic_parameter_set_rbsp` | `pic_parameter_set_id ue ≤ 255`, `seq_parameter_set_id ue ≤ 31`, `entropy_coding_mode_flag`, `bottom_field_pic_order_in_frame_present_flag`, `num_slice_groups_minus1 ue ≤ 7` → `slice_group_map_type ue ≤ 6` and its per-type fields (`run_length_minus1[]`, `top_left[]/bottom_right[]`, `slice_group_change_direction_flag`, `slice_group_change_rate_minus1`, `pic_size_in_map_units_minus1` + `slice_group_id[] u(v)` with `v = Ceil(Log2(num_slice_groups_minus1+1))`), `num_ref_idx_l0/l1_default_active_minus1 ue ≤ 31`, `weighted_pred_flag`, `weighted_bipred_idc u(2)`, `pic_init_qp_minus26 se` (range −(26+QpBdOffsetY)..25), `pic_init_qs_minus26 se`, `chroma_qp_index_offset se [−12,12]`, `deblocking_filter_control_present_flag`, `constrained_intra_pred_flag`, `redundant_pic_cnt_present_flag`; **if `more_rbsp_data()`**: `transform_8x8_mode_flag`, `pic_scaling_matrix_present_flag` + `6 + ((chroma_format_idc != 3) ? 2 : 6) * transform_8x8_mode_flag` scaling lists, `second_chroma_qp_index_offset se`; `rbsp_trailing_bits()`. Derived: `second_chroma_qp_index_offset` defaults to `chroma_qp_index_offset` when absent; `transform_8x8_mode_flag` default 0. **Pitfall:** the PPS needs its SPS's `chroma_format_idc` for the scaling-list count; if the SPS is unknown at parse time, parse with the count for chroma_format_idc != 3 and set `Diagnostic SYN-H264-0020 "PPS parsed before its SPS; scaling list count assumed"`; `index` re-parses PPSs once the SPS arrives (7.3.8). |
| 7 | `sei_rbsp()` / `sei_message()`, §7.3.2.3, §7.3.2.3.1 | `sei_rbsp` / `sei_message[i]` | `payloadType` = Σ `ff_byte` (0xFF) + `last_payload_type_byte`; `payloadSize` likewise; each payload is parsed with a **sub-BitReader** bounded to `payloadSize` bytes (so a malformed payload cannot overrun the next message); unknown payload types → node `reserved_sei_message` kind Bytes. After the payload: if not byte aligned or `more_rbsp_data()` within the payload sub-reader: `reserved_payload_extension_data`, `payload_bit_equal_to_one`, `payload_bit_equal_to_zero` (names per H.264 (08/2021) §7.3.2.3.1). Payload parsers #8–#18. |
| 8 | `buffering_period(payloadSize)` D.1.2 (type 0) | `buffering_period` | `seq_parameter_set_id ue`; per CPB (nal then vcl, counts from the active SPS HRD): `initial_cpb_removal_delay u(v)`, `initial_cpb_removal_delay_offset u(v)` with v = `initial_cpb_removal_delay_length_minus1+1`. Requires the SPS: if unavailable → `Diagnostic SYN-H264-0030` and node kind Bytes. |
| 9 | `pic_timing(payloadSize)` D.1.3 (type 1) | `pic_timing` | if `CpbDpbDelaysPresentFlag` (nal or vcl HRD present): `cpb_removal_delay u(v)`, `dpb_output_delay u(v)`; if `pic_struct_present_flag`: `pic_struct u(4)` (enum Table D-1: 0 frame … 8 frame tripling), `NumClockTS` (Table D-1), per TS `clock_timestamp_flag`, `ct_type u(2)`, `nuit_field_based_flag`, `counting_type u(5)`, `full_timestamp_flag`, `discontinuity_flag`, `cnt_dropped_flag`, `n_frames u(8)`, seconds/minutes/hours (`u(6)/u(6)/u(5)` with their flags), `time_offset i(v)` (v = `time_offset_length`). Derived: `clockTimestamp` (D.2.3 formula) as text `hh:mm:ss:ff`. |
| 10 | `recovery_point()` D.1.8 (type 6) | `recovery_point` | `recovery_frame_cnt ue`, `exact_match_flag`, `broken_link_flag`, `changing_slice_group_idc u(2)`. Used by `index` for "recovery RAP" (7.4.4). |
| 11 | `user_data_registered_itu_t_t35()` D.1.6 (type 4) | `user_data_registered_itu_t_t35` | `itu_t_t35_country_code b(8)`; if 0xFF: `itu_t_t35_country_code_extension_byte b(8)`; then `itu_t_t35_payload_byte[]` to payload end. The common T.35 dispatcher (7.3.11) parses `itu_t_t35_terminal_provider_code u(16)`, `itu_t_t35_terminal_provider_oriented_code u(16)` and the known payloads. |
| 12 | `user_data_unregistered()` D.1.7 (type 5) | `user_data_unregistered` | `uuid_iso_iec_11578` 16 bytes (kind Bytes, `svalue` = canonical UUID text), `user_data_payload_byte[]`; registry 7.3.11 names it (`enum_name` "x264 options" etc.) and the payload is shown as string when all bytes are printable ASCII. |
| 13 | `frame_packing_arrangement()` (type 45) | `frame_packing_arrangement` | `frame_packing_arrangement_id ue`, `..._cancel_flag`, `frame_packing_arrangement_type u(7)` (enum Table D-8), `quincunx_sampling_flag`, `content_interpretation_type u(6)`, `spatial_flipping_flag`, `frame0_flipped_flag`, `field_views_flag`, `current_frame_is_frame0_flag`, `frame0/1_self_contained_flag`, grid positions `u(4)×4` (when !quincunx and type != 5), `frame_packing_arrangement_reserved_byte u(8)`, `frame_packing_arrangement_repetition_period ue`, `frame_packing_arrangement_extension_flag`. |
| 14 | `display_orientation()` (type 47) | `display_orientation` | `display_orientation_cancel_flag`, `hor_flip`, `ver_flip`, `anticlockwise_rotation u(16)` (derived degrees = value × 360 / 65536), `display_orientation_repetition_period ue`, `display_orientation_extension_flag`. |
| 15 | `mastering_display_colour_volume()` (type 137) | `mastering_display_colour_volume` | `display_primaries_x[c] u(16)`, `display_primaries_y[c] u(16)` for c = 0..2, `white_point_x/y u(16)`, `max_display_mastering_luminance u(32)`, `min_display_mastering_luminance u(32)`. Units: primaries 0.00002, luminance 0.0001 cd/m². **Order:** the SEI carries primaries in **G, B, R** order (c = 0 green, 1 blue, 2 red by H.265 D.3.28 convention, identical here); OVT stores `ovt_ColorDescription.mdcv_primaries` in **R, G, B** order → `mdcv_primaries[0] = c2, [1] = c0, [2] = c1`. Derived text: `"R (0.708, 0.292) G (0.170, 0.797) B (0.131, 0.046) WP (0.3127, 0.3290) max 1000 cd/m² min 0.0001 cd/m²"`. |
| 16 | `content_light_level_info()` (type 144) | `content_light_level_info` | `max_content_light_level u(16)`, `max_pic_average_light_level u(16)` (cd/m²) → `max_cll`, `max_fall`. |
| 17 | `alternative_transfer_characteristics()` (type 147) | `alternative_transfer_characteristics` | `preferred_transfer_characteristics u(8)` (HLG signalling: 18 inside a BT.2020/BT.709 stream). |
| 18 | Other SEI types | `<spec name>` or `reserved_sei_message` | Pan-scan rect (2), filler (3), dec_ref_pic_marking_repetition (7), spare_pic (8), scene_info (9), sub_seq (10–12), full_frame_freeze (13/14), full_frame_snapshot (15), progressive refinement (16/17), motion_constrained_slice_group_set (18), film_grain_characteristics (19), deblocking_filter_display_preference (20), stereo_video_info (21), post_filter_hint (22), tone_mapping_info (23), SVC/MVC SEI (24–37), green_metadata (56): phase 1 parses 19 (film grain, D.1.20) and 23 (tone mapping); all others are kind Bytes with the spec name as node name (so the tree still shows what is present). |
| 19 | `access_unit_delimiter_rbsp()` §7.3.2.4 | `access_unit_delimiter_rbsp` | `primary_pic_type u(3)` (enum Table 7-5), `rbsp_trailing_bits`. |
| 20 | `end_of_seq_rbsp()` / `end_of_stream_rbsp()` / `filler_data_rbsp()` §7.3.2.5–7 | by spec name | Empty / `ff_byte` run (count as derived). |
| 21 | `seq_parameter_set_extension_rbsp()` §7.3.2.1.2 (type 13) | `seq_parameter_set_extension_rbsp` | `seq_parameter_set_id`, `aux_format_idc ue`, (if != 0) `bit_depth_aux_minus8`, `alpha_incr_flag`, `alpha_opaque_value u(v)`, `alpha_transparent_value u(v)` (v = bit_depth_aux_minus8+9), `additional_extension_flag`. |
| 22 | `subset_seq_parameter_set_rbsp()` §7.3.2.1.3 (type 15) | `subset_seq_parameter_set_rbsp` | `seq_parameter_set_data()` (#2) then, by profile_idc: 83/86 → `seq_parameter_set_svc_extension()` (G.7.3.2.1.4) + `svc_vui_parameters_present_flag` + `svc_vui_parameters_extension()`; 118/128/134 → `bit_equal_to_one f(1)`, `seq_parameter_set_mvc_extension()` (H.7.3.2.1.4: `num_views_minus1`, `view_id[]`, anchor/non-anchor ref lists, level values and applicable ops) + `mvc_vui_parameters_present_flag` + `mvc_vui_parameters_extension()`; 135/138/139 → MVCD/3D ext (I.7.3.2.1.5); then `additional_extension2_flag` loop. **Headers-only:** OVT parses these so SVC/MVC streams show a complete tree; slice extension NAL (20) is parsed as #1 header + `slice_header_in_scalable_extension()` only when `profile_idc` is SVC (G.7.3.3.4), else the slice header of #24 with the subset SPS as active SPS (MVC, H.7.3.3.4); block analysis of enhancement layers is out of scope. |
| 23 | `prefix_nal_unit_rbsp()` §7.3.2.12 (type 14) | `prefix_nal_unit_rbsp` | if `svc_extension_flag`: `prefix_nal_unit_svc()` (G.7.3.2.12.1: `store_ref_base_pic_flag`, `dec_ref_base_pic_marking()`, `additional_prefix_nal_unit_extension_flag`); else empty. |
| 24 | `slice_header()` §7.3.3 | `slice_header` | `first_mb_in_slice ue`, `slice_type ue ≤ 9` (enum Table 7-6: 0/5 P, 1/6 B, 2/7 I, 3/8 SP, 4/9 SI), `pic_parameter_set_id ue`; **activate PPS → SPS (7.3.8) here**; if `separate_colour_plane_flag`: `colour_plane_id u(2)`; `frame_num u(v)` (v = log2_max_frame_num_minus4+4); if !frame_mbs_only_flag: `field_pic_flag`, (if set) `bottom_field_flag`; if IdrPicFlag: `idr_pic_id ue ≤ 65535`; if pic_order_cnt_type == 0: `pic_order_cnt_lsb u(v)`, (if `bottom_field_pic_order_in_frame_present_flag && !field_pic_flag`) `delta_pic_order_cnt_bottom se`; if type 1 && !delta_pic_order_always_zero_flag: `delta_pic_order_cnt[0] se`, (same condition as above) `delta_pic_order_cnt[1] se`; if `redundant_pic_cnt_present_flag`: `redundant_pic_cnt ue`; if B: `direct_spatial_mv_pred_flag`; if P/SP/B: `num_ref_idx_active_override_flag` → `num_ref_idx_l0_active_minus1 ue ≤ 31` (≤ 15 for frames: Diagnostic when > 15 and !field_pic_flag), (B) `num_ref_idx_l1_active_minus1`; if nal_unit_type ∈ {20,21}: `ref_pic_list_mvc_modification()` (H.7.3.3.1.1) else `ref_pic_list_modification()` (#25); if `(weighted_pred_flag && (P||SP)) \|\| (weighted_bipred_idc == 1 && B)`: `pred_weight_table()` (#26); if nal_ref_idc != 0: `dec_ref_pic_marking()` (#27); if `entropy_coding_mode_flag && !I && !SI`: `cabac_init_idc ue ≤ 2`; `slice_qp_delta se`; if SP/SI: (SP) `sp_for_switch_flag`, `slice_qs_delta se`; if `deblocking_filter_control_present_flag`: `disable_deblocking_filter_idc ue ≤ 2`, (if != 1) `slice_alpha_c0_offset_div2 se [−6,6]`, `slice_beta_offset_div2 se`; if `num_slice_groups_minus1 > 0 && slice_group_map_type ∈ [3,5]`: `slice_group_change_cycle u(v)` with v = `Ceil(Log2(PicSizeInMapUnits ÷ SliceGroupChangeRate + 1))`. Then `opaque_rest("slice_data")`. **Derived:** `MbaffFrameFlag = mb_adaptive_frame_field_flag && !field_pic_flag`; `PicHeightInMbs = FrameHeightInMbs / (1 + field_pic_flag)`; `PicSizeInMbs`; `MaxPicNum = field_pic_flag ? 2*MaxFrameNum : MaxFrameNum`; `CurrPicNum = field_pic_flag ? 2*frame_num+1 : frame_num`; `SliceQPY = 26 + pic_init_qp_minus26 + slice_qp_delta` (→ `FrameInfo.qp_base` for the first slice); `QSY`; `FilterOffsetA = slice_alpha_c0_offset_div2 << 1`, `FilterOffsetB`; `num_ref_idx_l0_active_minus1` default from PPS; `PictureStructure` (Frame / TopField / BottomField / Mbaff); `slice_type_name`. |
| 25 | `ref_pic_list_modification()` §7.3.3.1 | `ref_pic_list_modification` | For L0 (P/SP/B) and L1 (B): `ref_pic_list_modification_flag_lX`; loop `modification_of_pic_nums_idc ue ≤ 3` until 3: idc 0/1 → `abs_diff_pic_num_minus1 ue`, idc 2 → `long_term_pic_num ue`. Max 33 iterations (`num_ref_idx_lX_active_minus1+1` + the terminator; Diagnostic beyond). Stored as an ordered op list consumed by 7.4.4 (ref list modification §8.2.4.3). |
| 26 | `pred_weight_table()` §7.3.3.2 | `pred_weight_table` | `luma_log2_weight_denom ue ≤ 7`, (ChromaArrayType != 0) `chroma_log2_weight_denom`; for each i ≤ num_ref_idx_l0_active_minus1: `luma_weight_l0_flag` → `luma_weight_l0[i] se [−128,127]`, `luma_offset_l0[i] se`; `chroma_weight_l0_flag` → 2 × (`chroma_weight_l0[i][j]`, `chroma_offset_l0[i][j]`); same for L1 when B. Derived defaults when flags are 0: weight = 2^denom, offset = 0. |
| 27 | `dec_ref_pic_marking()` §7.3.3.3 | `dec_ref_pic_marking` | if IdrPicFlag: `no_output_of_prior_pics_flag`, `long_term_reference_flag`; else `adaptive_ref_pic_marking_mode_flag` → loop `memory_management_control_operation ue ≤ 6` until 0: op 1/3 → `difference_of_pic_nums_minus1 ue`; op 2 → `long_term_pic_num ue`; op 3/6 → `long_term_frame_idx ue`; op 4 → `max_long_term_frame_idx_plus1 ue`. Stored as op list (MMCO) for 7.4.4. Max 66 ops (Diagnostic beyond). |
| 28 | `slice_data_partition_a/b/c_layer_rbsp()` §7.3.2.9 (types 2–4) | by spec name | A: `slice_header()` + `slice_id ue` + opaque; B/C: `slice_id ue`, (if separate_colour_plane) `colour_plane_id u(2)`, (if redundant_pic_cnt_present) `redundant_pic_cnt ue`, opaque. |

### 7.3.5 H.265 syntax structures to implement

| # | Structure (spec §) | Node name | Notes / derived values |
|---|---|---|---|
| 1 | `nal_unit_header()` §7.3.1.2 | `nal_unit_header` | `forbidden_zero_bit f(1)`, `nal_unit_type u(6)` (enum Table 7-1), `nuh_layer_id u(6)`, `nuh_temporal_id_plus1 u(3) ≥ 1` (0 → ParseError SYN-H265-0001). Derived: `TemporalId = nuh_temporal_id_plus1 − 1`, `Unit.layer_id = nuh_layer_id`, `is_vcl = nal_unit_type < 32`, `is_irap = 16..23`, `is_idr = 19/20`, `is_bla = 16..18`, `is_cra = 21`, `is_rasl = 8/9`, `is_radl = 6/7`, `is_sub_layer_non_ref = nal_unit_type ≤ 14 && even` (TRAIL_N, TSA_N, STSA_N, RADL_N, RASL_N, RSV_VCL_N10/12/14). |
| 2 | `video_parameter_set_rbsp()` §7.3.2.1 | `video_parameter_set_rbsp` | `vps_video_parameter_set_id u(4)`, `vps_base_layer_internal_flag`, `vps_base_layer_available_flag`, `vps_max_layers_minus1 u(6)`, `vps_max_sub_layers_minus1 u(3) ≤ 6`, `vps_temporal_id_nesting_flag`, `vps_reserved_0xffff_16bits f(16)=0xFFFF`, `profile_tier_level(1, vps_max_sub_layers_minus1)` (#3), `vps_sub_layer_ordering_info_present_flag`, per sub-layer `vps_max_dec_pic_buffering_minus1[i] ue`, `vps_max_num_reorder_pics[i]`, `vps_max_latency_increase_plus1[i]`, `vps_max_layer_id u(6)`, `vps_num_layer_sets_minus1 ue ≤ 1023`, `layer_id_included_flag[i][j]`, `vps_timing_info_present_flag` → `vps_num_units_in_tick u(32)`, `vps_time_scale u(32)`, `vps_poc_proportional_to_timing_flag` → `vps_num_ticks_poc_diff_one_minus1 ue`, `vps_num_hrd_parameters ue`, per HRD `hrd_layer_set_idx[i] ue`, `cprms_present_flag[i]`, `hrd_parameters(cprms_present_flag[i], vps_max_sub_layers_minus1)` (#6); `vps_extension_flag` → `vps_extension()` (F.7.3.2.1.1, parsed as opaque Bytes in phase 1 with node name `vps_extension`), `rbsp_trailing_bits`. Derived: `frame_rate` hint. |
| 3 | `profile_tier_level(profilePresentFlag, maxNumSubLayersMinus1)` §7.3.3 | `profile_tier_level` | `general_profile_space u(2)`, `general_tier_flag`, `general_profile_idc u(5)`, `general_profile_compatibility_flag[32]`, `general_progressive_source_flag`, `general_interlaced_source_flag`, `general_non_packed_constraint_flag`, `general_frame_only_constraint_flag`, then the 43 + 1 constraint bits decoded **per profile** exactly as the table (RExt/SCC: `general_max_12bit_constraint_flag` … `general_lower_bit_rate_constraint_flag`, `general_max_14bit_constraint_flag`, reserved zero bits; otherwise `general_reserved_zero_43bits`), `general_inbld_flag`/`general_reserved_zero_bit`, `general_level_idc u(8)`; `sub_layer_profile_present_flag[i]`, `sub_layer_level_present_flag[i]` for i < maxNumSubLayersMinus1; if maxNumSubLayersMinus1 > 0: `reserved_zero_2bits[i]` for i = maxNumSubLayersMinus1..7; per sub-layer the same block. Derived: `profile_name` (Main, Main 10, Main Still Picture, RExt (Main 4:4:4 10 …), SCC, …) and `level = general_level_idc / 30.0`. |
| 4 | `seq_parameter_set_rbsp()` §7.3.2.2.1 | `seq_parameter_set_rbsp` | `sps_video_parameter_set_id u(4)`, `sps_max_sub_layers_minus1 u(3) ≤ 6` (when nuh_layer_id > 0: `sps_ext_or_max_sub_layers_minus1`, F.7.3.2.2.1 — phase 1: treat nuh_layer_id > 0 SPS as opaque with Diagnostic), `sps_temporal_id_nesting_flag`, `profile_tier_level(1, …)`, `sps_seq_parameter_set_id ue ≤ 15`, `chroma_format_idc ue ≤ 3`, (if 3) `separate_colour_plane_flag`, `pic_width_in_luma_samples ue`, `pic_height_in_luma_samples ue`, `conformance_window_flag` → `conf_win_left/right/top/bottom_offset ue`, `bit_depth_luma_minus8 ue ≤ 8`, `bit_depth_chroma_minus8`, `log2_max_pic_order_cnt_lsb_minus4 ue ≤ 12`, `sps_sub_layer_ordering_info_present_flag`, per sub-layer `sps_max_dec_pic_buffering_minus1[i]`, `sps_max_num_reorder_pics[i]`, `sps_max_latency_increase_plus1[i]`, `log2_min_luma_coding_block_size_minus3 ue`, `log2_diff_max_min_luma_coding_block_size ue`, `log2_min_luma_transform_block_size_minus2 ue`, `log2_diff_max_min_luma_transform_block_size ue`, `max_transform_hierarchy_depth_inter ue`, `max_transform_hierarchy_depth_intra ue`, `scaling_list_enabled_flag` → `sps_scaling_list_data_present_flag` → `scaling_list_data()` (#5), `amp_enabled_flag`, `sample_adaptive_offset_enabled_flag`, `pcm_enabled_flag` → `pcm_sample_bit_depth_luma_minus1 u(4)`, `pcm_sample_bit_depth_chroma_minus1 u(4)`, `log2_min_pcm_luma_coding_block_size_minus3 ue`, `log2_diff_max_min_pcm_luma_coding_block_size ue`, `pcm_loop_filter_disabled_flag`; `num_short_term_ref_pic_sets ue ≤ 64`, `st_ref_pic_set(i)` × n (#9), `long_term_ref_pics_present_flag` → `num_long_term_ref_pics_sps ue ≤ 32`, `lt_ref_pic_poc_lsb_sps[i] u(v)`, `used_by_curr_pic_lt_sps_flag[i]`; `sps_temporal_mvp_enabled_flag`, `strong_intra_smoothing_enabled_flag`, `vui_parameters_present_flag` → `vui_parameters()` (#7), `sps_extension_present_flag` → `sps_range_extension_flag`, `sps_multilayer_extension_flag`, `sps_3d_extension_flag`, `sps_scc_extension_flag`, `sps_extension_4bits`; `sps_range_extension()` (§7.3.2.2.2: `transform_skip_rotation_enabled_flag`, `transform_skip_context_enabled_flag`, `implicit_rdpcm_enabled_flag`, `explicit_rdpcm_enabled_flag`, `extended_precision_processing_flag`, `intra_smoothing_disabled_flag`, `high_precision_offsets_enabled_flag`, `persistent_rice_adaptation_enabled_flag`, `cabac_bypass_alignment_enabled_flag`), `sps_multilayer_extension()` (`inter_view_mv_vert_constraint_flag`), `sps_3d_extension()` (opaque, Diagnostic), `sps_scc_extension()` (§7.3.2.2.3: `sps_curr_pic_ref_enabled_flag`, `palette_mode_enabled_flag` → `palette_max_size ue`, `delta_palette_max_predictor_size ue`, `sps_palette_predictor_initializers_present_flag` → `sps_num_palette_predictor_initializers_minus1 ue`, `sps_palette_predictor_initializer[comp][i] u(v)`; `motion_vector_resolution_control_idc u(2)`, `intra_boundary_filtering_disabled_flag`), `sps_extension_4bits` → `sps_extension_data_flag` opaque. **Derived (§7.4.3.2.1):** `ChromaArrayType`, `SubWidthC/SubHeightC`, `MinCbLog2SizeY = log2_min_luma_coding_block_size_minus3 + 3`, `CtbLog2SizeY = MinCbLog2SizeY + log2_diff_max_min_luma_coding_block_size` (must be 4..6 → else Diagnostic Error), `MinCbSizeY`, `CtbSizeY`, `PicWidthInMinCbsY`, `PicWidthInCtbsY = Ceil(pic_width_in_luma_samples ÷ CtbSizeY)`, `PicHeightInMinCbsY`, `PicHeightInCtbsY`, `PicSizeInMinCbsY`, `PicSizeInCtbsY`, `PicSizeInSamplesY`, `PicWidthInSamplesC`, `PicHeightInSamplesC`, `MinTbLog2SizeY = log2_min_luma_transform_block_size_minus2 + 2`, `MaxTbLog2SizeY`, `BitDepthY/C`, `QpBdOffsetY = 6*bit_depth_luma_minus8`, `QpBdOffsetC`, `MaxPicOrderCntLsb`, `PcmBitDepthY/C`, `Log2MinIpcmCbSizeY`, `Log2MaxIpcmCbSizeY`, `CoeffMinY/C`, `CoeffMaxY/C` (RExt: extended precision), `WpOffsetBdShiftY/C`, `WpOffsetHalfRangeY/C`; conformance crop rect in luma samples `x = SubWidthC*conf_win_left_offset`, `y = SubHeightC*conf_win_top_offset`, `w = pic_width − SubWidthC*(left+right)`, `h`; `sps_max_dec_pic_buffering_minus1[HighestTid]` for DPB size; `PaletteMaxPredictorSize`. |
| 5 | `scaling_list_data()` §7.3.4 | `scaling_list_data` | sizeId 0..3, matrixId 0..5 (step 3 for sizeId 3): `scaling_list_pred_mode_flag` → `scaling_list_pred_matrix_id_delta ue` or (`scaling_list_dc_coef_minus8 se` for sizeId > 1) + `scaling_list_delta_coef se` loop with `nextCoef` wrap; derived `ScalingFactor` arrays (§7.4.5) stored for the plugin cross-check. |
| 6 | `hrd_parameters(commonInfPresentFlag, maxNumSubLayersMinus1)` E.2.2 + `sub_layer_hrd_parameters()` E.2.3 | `hrd_parameters` | `nal_hrd_parameters_present_flag`, `vcl_hrd_parameters_present_flag`, `sub_pic_hrd_params_present_flag` → `tick_divisor_minus2 u(8)`, `du_cpb_removal_delay_increment_length_minus1 u(5)`, `sub_pic_cpb_params_in_pic_timing_sei_flag`, `dpb_output_delay_du_length_minus1 u(5)`; `bit_rate_scale u(4)`, `cpb_size_scale u(4)`, (sub-pic) `cpb_size_du_scale u(4)`, `initial_cpb_removal_delay_length_minus1 u(5)`, `au_cpb_removal_delay_length_minus1 u(5)`, `dpb_output_delay_length_minus1 u(5)`; per sub-layer: `fixed_pic_rate_general_flag`, `fixed_pic_rate_within_cvs_flag`, `elemental_duration_in_tc_minus1 ue` or `low_delay_hrd_flag`, `cpb_cnt_minus1 ue ≤ 31`, `sub_layer_hrd_parameters()` for nal and/or vcl: `bit_rate_value_minus1`, `cpb_size_value_minus1`, (sub-pic) `cpb_size_du_value_minus1`, `bit_rate_du_value_minus1`, `cbr_flag`. Derived `BitRate`, `CpbSize` (E.3.2). |
| 7 | `vui_parameters()` E.2.1 | `vui_parameters` | As H.264 #4 with HEVC additions: `neutral_chroma_indication_flag`, `field_seq_flag`, `frame_field_info_present_flag`, `default_display_window_flag` + 4 offsets, `vui_timing_info_present_flag` → `vui_num_units_in_tick`, `vui_time_scale`, `vui_poc_proportional_to_timing_flag` → `vui_num_ticks_poc_diff_one_minus1`, `vui_hrd_parameters_present_flag` → `hrd_parameters(1, sps_max_sub_layers_minus1)`; `bitstream_restriction_flag` → `tiles_fixed_structure_flag`, `motion_vectors_over_pic_boundaries_flag`, `restricted_ref_pic_lists_flag`, `min_spatial_segmentation_idc ue`, `max_bytes_per_pic_denom`, `max_bits_per_min_cu_denom`, `log2_max_mv_length_horizontal`, `log2_max_mv_length_vertical`. Derived: `frame_rate = vui_time_scale / vui_num_units_in_tick`, `ovt_ColorDescription` candidate (chroma_siting: HEVC default colocated top-left → 3 when `chroma_loc_info_present_flag` = 0 and ChromaArrayType = 1; `chroma_sample_loc_type_top_field` 0→1, 1→2, 2→3 (top-left), else 0). |
| 8 | `pic_parameter_set_rbsp()` §7.3.2.3.1 | `pic_parameter_set_rbsp` | `pps_pic_parameter_set_id ue ≤ 63`, `pps_seq_parameter_set_id ue ≤ 15`, `dependent_slice_segments_enabled_flag`, `output_flag_present_flag`, `num_extra_slice_header_bits u(3)`, `sign_data_hiding_enabled_flag`, `cabac_init_present_flag`, `num_ref_idx_l0/l1_default_active_minus1 ue ≤ 14`, `init_qp_minus26 se`, `constrained_intra_pred_flag`, `transform_skip_enabled_flag`, `cu_qp_delta_enabled_flag` → `diff_cu_qp_delta_depth ue`, `pps_cb_qp_offset se [−12,12]`, `pps_cr_qp_offset`, `pps_slice_chroma_qp_offsets_present_flag`, `weighted_pred_flag`, `weighted_bipred_flag`, `transquant_bypass_enabled_flag`, `tiles_enabled_flag`, `entropy_coding_sync_enabled_flag`; if tiles: `num_tile_columns_minus1 ue`, `num_tile_rows_minus1 ue`, `uniform_spacing_flag`, (if 0) `column_width_minus1[i]`, `row_height_minus1[i]`, `loop_filter_across_tiles_enabled_flag`; `pps_loop_filter_across_slices_enabled_flag`, `deblocking_filter_control_present_flag` → `deblocking_filter_override_enabled_flag`, `pps_deblocking_filter_disabled_flag`, (if 0) `pps_beta_offset_div2 se [−6,6]`, `pps_tc_offset_div2`; `pps_scaling_list_data_present_flag` → `scaling_list_data()`; `lists_modification_present_flag`, `log2_parallel_merge_level_minus2 ue`, `slice_segment_header_extension_present_flag`, `pps_extension_present_flag` → `pps_range_extension_flag`, `pps_multilayer_extension_flag`, `pps_3d_extension_flag`, `pps_scc_extension_flag`, `pps_extension_4bits`; `pps_range_extension()` (§7.3.2.3.2: `log2_max_transform_skip_block_size_minus2 ue` (if transform_skip), `cross_component_prediction_enabled_flag`, `chroma_qp_offset_list_enabled_flag` → `diff_cu_chroma_qp_offset_depth ue`, `chroma_qp_offset_list_len_minus1 ue ≤ 5`, `cb_qp_offset_list[i] se`, `cr_qp_offset_list[i] se`; `log2_sao_offset_scale_luma ue`, `log2_sao_offset_scale_chroma ue`), `pps_multilayer_extension()`/`pps_3d_extension()` opaque with Diagnostic, `pps_scc_extension()` (§7.3.2.3.3: `pps_curr_pic_ref_enabled_flag`, `residual_adaptive_colour_transform_enabled_flag` → `pps_slice_act_qp_offsets_present_flag`, `pps_act_y/cb/cr_qp_offset_plus5/plus3 se`; `pps_palette_predictor_initializers_present_flag` → `pps_num_palette_predictor_initializers ue`, (if > 0) `monochrome_palette_flag`, `luma_bit_depth_entry_minus8 ue`, `chroma_bit_depth_entry_minus8 ue`, `pps_palette_predictor_initializer[comp][i] u(v)`). **Derived (H.265 §6.5.1, §7.4.3.3):** `colBd[]`, `rowBd[]` (uniform: `((i+1)*PicWidthInCtbsY)/(num_tile_columns_minus1+1) − (i*PicWidthInCtbsY)/(…)`), `ColumnWidthInLumaSamples`, `RowHeightInLumaSamples`, `CtbAddrRsToTs[]`, `CtbAddrTsToRs[]`, `TileId[]`, `Log2MinCuQpDeltaSize = CtbLog2SizeY − diff_cu_qp_delta_depth`, `Log2MinCuChromaQpOffsetSize`, `Log2ParMrgLevel`, `Log2MaxTransformSkipSize`, default `SliceQpY`. These depend on the SPS → computed at activation (7.3.8). |
| 9 | `st_ref_pic_set(stRpsIdx)` §7.3.7 | `st_ref_pic_set[stRpsIdx]` | if stRpsIdx != 0: `inter_ref_pic_set_prediction_flag`; if set: (stRpsIdx == num_short_term_ref_pic_sets) `delta_idx_minus1 ue`, `delta_rps_sign u(1)`, `abs_delta_rps_minus1 ue`, for j = 0..NumDeltaPocs[RefRpsIdx]: `used_by_curr_pic_flag[j]`, (if 0) `use_delta_flag[j]`; else `num_negative_pics ue ≤ sps_max_dec_pic_buffering_minus1[…]`, `num_positive_pics ue`, `delta_poc_s0_minus1[i] ue`, `used_by_curr_pic_s0_flag[i]`, `delta_poc_s1_minus1[i]`, `used_by_curr_pic_s1_flag[i]`. **Derived (§7.4.8, equations 7-59..7-62 — implement exactly as in 7.3.10.3):** `NumNegativePics`, `NumPositivePics`, `NumDeltaPocs`, `DeltaPocS0[i]`, `UsedByCurrPicS0[i]`, `DeltaPocS1[i]`, `UsedByCurrPicS1[i]`; `derived` text `"S0: -1(used) -2(used) -4 | S1: +1(used)"`. |
| 10 | `slice_segment_header()` §7.3.6.1 | `slice_segment_header` | `first_slice_segment_in_pic_flag`; if IRAP (16..23): `no_output_of_prior_pics_flag`; `slice_pic_parameter_set_id ue ≤ 63` → **activate (7.3.8)**; if !first: (dependent_slice_segments_enabled_flag) `dependent_slice_segment_flag`, `slice_segment_address u(v)` with v = `Ceil(Log2(PicSizeInCtbsY))`; if !dependent_slice_segment_flag: `slice_reserved_flag[i]` × num_extra_slice_header_bits, `slice_type ue ≤ 2` (enum: 0 B, 1 P, 2 I), (output_flag_present_flag) `pic_output_flag`, (separate_colour_plane_flag) `colour_plane_id u(2)`; if not IDR: `slice_pic_order_cnt_lsb u(v)` (v = log2_max_pic_order_cnt_lsb_minus4+4), `short_term_ref_pic_set_sps_flag`; if 0: `st_ref_pic_set(num_short_term_ref_pic_sets)` (#9, inline; its BitSpan is recorded as `NumBitsForShortTermRPSInSlice`); else if num_short_term_ref_pic_sets > 1: `short_term_ref_pic_set_idx u(v)` v = `Ceil(Log2(num_short_term_ref_pic_sets))`; if long_term_ref_pics_present_flag: (num_long_term_ref_pics_sps > 0) `num_long_term_sps ue`, `num_long_term_pics ue`, per entry: (i < num_long_term_sps && num_long_term_ref_pics_sps > 1) `lt_idx_sps[i] u(v)` else `poc_lsb_lt[i] u(v)`, `used_by_curr_pic_lt_flag[i]`; `delta_poc_msb_present_flag[i]` → `delta_poc_msb_cycle_lt[i] ue`; (sps_temporal_mvp_enabled_flag) `slice_temporal_mvp_enabled_flag`; if SAO enabled: `slice_sao_luma_flag`, (ChromaArrayType != 0) `slice_sao_chroma_flag`; if P/B: `num_ref_idx_active_override_flag` → `num_ref_idx_l0_active_minus1 ue ≤ 14`, (B) `num_ref_idx_l1_active_minus1`; (lists_modification_present_flag && NumPicTotalCurr > 1) `ref_pic_lists_modification()` (#11); (B) `mvd_l1_zero_flag`; (cabac_init_present_flag) `cabac_init_flag`; if slice_temporal_mvp_enabled_flag: (B) `collocated_from_l0_flag` (default 1), (num_ref_idx > 0 in that list) `collocated_ref_idx ue`; if `(weighted_pred_flag && P) \|\| (weighted_bipred_flag && B)`: `pred_weight_table()` (#12); `five_minus_max_num_merge_cand ue ≤ 4`; (motion_vector_resolution_control_idc == 2) `use_integer_mv_flag`; `slice_qp_delta se`; (pps_slice_chroma_qp_offsets_present_flag) `slice_cb_qp_offset se`, `slice_cr_qp_offset se`; (pps_slice_act_qp_offsets_present_flag) `slice_act_y/cb/cr_qp_offset se`; (chroma_qp_offset_list_enabled_flag) `cu_chroma_qp_offset_enabled_flag`; (deblocking_filter_override_enabled_flag) `deblocking_filter_override_flag` → `slice_deblocking_filter_disabled_flag`, (if 0) `slice_beta_offset_div2 se`, `slice_tc_offset_div2 se`; (pps_loop_filter_across_slices_enabled_flag && (sao or !deblock disabled)) `slice_loop_filter_across_slices_enabled_flag`. Then if tiles_enabled_flag \|\| entropy_coding_sync_enabled_flag: `num_entry_point_offsets ue` (≤ PicHeightInCtbsY*… bounded per §7.4.7.1), (if > 0) `offset_len_minus1 ue ≤ 31`, `entry_point_offset_minus1[i] u(v)`; (slice_segment_header_extension_present_flag) `slice_segment_header_extension_length ue ≤ 256`, `slice_segment_header_extension_data_byte[i] u(8)`; `byte_alignment()` (§7.3.2.12: `alignment_bit_equal_to_one f(1)` then zeros); `opaque_rest("slice_segment_data")`. **Derived:** `SliceQpY = 26 + init_qp_minus26 + slice_qp_delta`; `MaxNumMergeCand = 5 − five_minus_max_num_merge_cand`; `SliceAddrRs` (= slice_segment_address of the preceding independent slice segment for dependent segments); `CurrRpsIdx`; `NumPicTotalCurr` (§7.4.7.2: count of UsedByCurrPicS0/S1/Lt plus 1 if pps_curr_pic_ref_enabled_flag); `PocLsbLt[i]`, `UsedByCurrPicLt[i]`, `DeltaPocMsbCycleLt[i]` (cumulative, eq. 7-52); substream byte ranges: `firstByte[k] = Σ_{n<k} (entry_point_offset_minus1[n]+1)` relative to the first byte of slice_segment_data **in raw (EPB-included) bytes** (§7.4.7.1 — the offsets count emulation prevention bytes: map with 7.2.5); `slice_type_name`; `inferred collocated_from_l0_flag = 1`. |
| 11 | `ref_pic_lists_modification()` §7.3.6.2 | `ref_pic_lists_modification` | `ref_pic_list_modification_flag_l0` → `list_entry_l0[i] u(v)` for i ≤ num_ref_idx_l0_active_minus1 with v = `Ceil(Log2(NumPicTotalCurr))`; (B) `ref_pic_list_modification_flag_l1` → `list_entry_l1[i]`. |
| 12 | `pred_weight_table()` §7.3.6.3 | `pred_weight_table` | `luma_log2_weight_denom ue ≤ 7`, (ChromaArrayType != 0) `delta_chroma_log2_weight_denom se`; for each i in L0: `luma_weight_l0_flag[i]` (skipped when the ref is the current picture under pps_curr_pic_ref: pic is RefPicList0[i] with same POC and layer), then `chroma_weight_l0_flag[i]`, then per i with flags: `delta_luma_weight_l0[i] se [−128,127]`, `luma_offset_l0[i] se`, per j: `delta_chroma_weight_l0[i][j] se`, `delta_chroma_offset_l0[i][j] se`; L1 for B. Derived: `ChromaLog2WeightDenom`, `LumaWeightL0[i] = (1 << luma_log2_weight_denom) + delta`, `ChromaWeightL0[i][j]`, `ChromaOffsetL0[i][j]` (eq. 7-56 with `WpOffsetHalfRangeC`). **Pitfall:** the "ref is current picture" skip requires knowing RefPicList0 (7.4.4) at parse time; when unavailable (SCC streams only) parse as if no entry is the current picture and emit Diagnostic SYN-H265-0040. |
| 13 | `sei_rbsp()`/`sei_message()` §7.3.2.4, §7.3.5 | `sei_rbsp`/`sei_message[i]` | As H.264 #7 (same framing); prefix (39) vs suffix (40) determines the allowed payload table (D.2.1); OVT parses by payloadType regardless and adds Diagnostic Warning `SYN-H265-0050 "payload type %d not allowed in %s SEI"` when misplaced. |
| 14 | SEI payloads (Annex D.2/D.3) | spec name | `buffering_period` (0; `bp_seq_parameter_set_id ue`, `irap_cpb_params_present_flag`, `cpb_delay_offset u(v)`, `dpb_delay_offset u(v)`, `concatenation_flag`, `au_cpb_removal_delay_delta_minus1 u(v)`, per CPB nal/vcl `nal_initial_cpb_removal_delay[i] u(v)`, `nal_initial_cpb_removal_offset[i] u(v)`, (sub_pic or irap) `nal_initial_alt_cpb_removal_delay/offset` — lengths from the active SPS HRD), `pic_timing` (1; `pic_struct u(4)` Table D-2, `source_scan_type u(2)`, `duplicate_flag`, `au_cpb_removal_delay_minus1 u(v)`, `pic_dpb_output_delay u(v)`, (sub_pic) `pic_dpb_output_du_delay u(v)`, `num_decoding_units_minus1 ue`, `du_common_cpb_removal_delay_flag`, `du_common_cpb_removal_delay_increment_minus1 u(v)`, per DU `num_nalus_in_du_minus1[i] ue`, `du_cpb_removal_delay_increment_minus1[i] u(v)`), `pan_scan_rect` (2), `filler_payload` (3), `user_data_registered_itu_t_t35` (4, 7.3.11), `user_data_unregistered` (5, 7.3.11), `recovery_point` (6; `recovery_poc_cnt se`, `exact_match_flag`, `broken_link_flag`), `scene_info` (9), `picture_snapshot` (15), progressive refinement (16/17), `film_grain_characteristics` (19), `post_filter_hint` (22), `tone_mapping_info` (23), `frame_packing_arrangement` (45, as H.264 #13 plus `upsampled_aspect_ratio_flag`), `display_orientation` (47), `structure_of_pictures_info` (128), `active_parameter_sets` (129; `active_video_parameter_set_id u(4)`, `self_contained_cvs_flag`, `no_parameter_set_update_flag`, `num_sps_ids_minus1 ue`, `active_seq_parameter_set_id[i] ue`, `layer_sps_idx[i]`), `decoding_unit_info` (130), `temporal_sub_layer_zero_idx` (131), **`decoded_picture_hash` (132, suffix; `hash_type u(8)` 0 MD5 → `picture_md5[cIdx][16] b(8)`, 1 CRC → `picture_crc[cIdx] u(16)`, 2 checksum → `picture_checksum[cIdx] u(32)`, for cIdx < (chroma_format_idc == 0 ? 1 : 3); stored for the verify job 7.6.4)**, `scalable_nesting` (133; recursive sei_message list, nesting depth ≤ 1), `region_refresh_info` (134), `no_display` (135), `time_code` (136; `num_clock_ts u(2)`, per TS `clock_timestamp_flag`, `units_field_based_flag`, `counting_type u(5)`, `full_timestamp_flag`, `discontinuity_flag`, `cnt_dropped_flag`, `n_frames u(9)`, `seconds_value u(6)` etc. with flags, `time_offset_length u(5)`, `time_offset_value i(v)`), `mastering_display_colour_volume` (137; as H.264 #15), `segmented_rect_frame_packing_arrangement` (138), `temporal_motion_constrained_tile_sets` (139), `chroma_resampling_filter_hint` (140), `knee_function_info` (141), `colour_remapping_info` (142), `deinterlaced_field_identification` (143), `content_light_level_info` (144), `dependent_rap_indication` (145), `coded_region_completion` (146), `alternative_transfer_characteristics` (147), `ambient_viewing_environment` (148; `ambient_illuminance u(32)`, `ambient_light_x u(16)`, `ambient_light_y u(16)`), `content_colour_volume` (149), `equirectangular_projection` (150), `cubemap_projection` (151), `sphere_rotation` (154), `regionwise_packing` (155), `omni_viewport` (156), `regional_nesting` (157), `mcts_extraction_info_sets` (158), `mcts_extraction_info_nesting` (159), `layers_not_present` (160), `inter_layer_constrained_tile_sets` (161), `bsp_nesting` (162) … `sei_manifest` (200), `sei_prefix_indication` (201), `annotated_regions` (202), `shutter_interval_info` (205). Phase 1 MUST fully parse: 0, 1, 4, 5, 6, 45, 47, 129, 132, 136, 137, 144, 147, 148; all others are kind Bytes with the spec name. |
| 15 | `access_unit_delimiter_rbsp()` §7.3.2.5, `end_of_seq_rbsp` §7.3.2.6, `end_of_bitstream_rbsp` §7.3.2.7, `filler_data_rbsp` §7.3.2.8 | spec name | `pic_type u(3)` (Table 7-2) for AUD. |
| 16 | Unspecified NAL types 48–63 | `unspecified_nal_unit` | Type 62 with `nuh_layer_id == 0` → `enum_name "Dolby Vision RPU"`, type 63 → `"Dolby Vision EL"`; payload opaque (7.3.11). |

### 7.3.6 AV1 syntax structures to implement

Terminology: a "frame header" means `uncompressed_header()` (AV1 §5.9.2) inside `OBU_FRAME_HEADER`, `OBU_REDUNDANT_FRAME_HEADER` or `OBU_FRAME`. The parser keeps a `SequenceState` (last sequence header) and a `DecoderState` (the 8 reference slots and the variables AV1 §7.20 saves) — see 7.3.8 and 7.3.10.4.

| # | Structure (spec §) | Node name | Notes / derived values |
|---|---|---|---|
| 1 | `obu_header()` AV1 §5.3.2, `obu_extension_header()` AV1 §5.3.3, `obu_size leb128()` AV1 §5.3.1 | `obu_header` | `obu_forbidden_bit f(1)=0`, `obu_type f(4)` (enum: 1 OBU_SEQUENCE_HEADER, 2 OBU_TEMPORAL_DELIMITER, 3 OBU_FRAME_HEADER, 4 OBU_TILE_GROUP, 5 OBU_METADATA, 6 OBU_FRAME, 7 OBU_REDUNDANT_FRAME_HEADER, 8 OBU_TILE_LIST, 15 OBU_PADDING, others "Reserved"), `obu_extension_flag f(1)`, `obu_has_size_field f(1)`, `obu_reserved_1bit f(1)`; if extension: `temporal_id f(3)`, `spatial_id f(2)`, `extension_header_reserved_3bits f(3)`; if has_size: `obu_size leb128()`. Derived: `Unit.temporal_id`, `Unit.layer_id = spatial_id`, `obu_size` (or remaining bytes), `type_name`. **Operating point drop rule (AV1 §7.5 / AV1 §6.2):** if `obu_type` ∉ {sequence header, temporal delimiter, padding} and extension present and the selected operating point's `OperatingPointIdc` does not include the OBU's `temporal_id`/`spatial_id` (`inTemporalLayer = (OperatingPointIdc >> temporal_id) & 1`, `inSpatialLayer = (OperatingPointIdc >> (spatial_id+8)) & 1`), the OBU is still parsed and indexed but `FrameInfo` gets `shown = false` and Diagnostic Info `SYN-AV1-0002 "not in selected operating point"`. OVT's default operating point is 0. |
| 2 | `sequence_header_obu()` AV1 §5.5.1 | `sequence_header_obu` | `seq_profile f(3)` (0 Main, 1 High, 2 Professional; > 2 → ParseError), `still_picture f(1)`, `reduced_still_picture_header f(1)`; if reduced: `seq_level_idx[0] f(5)` and derived `timing_info_present_flag = 0`, `decoder_model_info_present_flag = 0`, `initial_display_delay_present_flag = 0`, `operating_points_cnt_minus_1 = 0`, `operating_point_idc[0] = 0`, `seq_tier[0] = 0`; else `timing_info_present_flag` → `timing_info()` (#3) + `decoder_model_info_present_flag` → `decoder_model_info()` (#4); `initial_display_delay_present_flag`; `operating_points_cnt_minus_1 f(5)`; per op: `operating_point_idc[i] f(12)`, `seq_level_idx[i] f(5)`, (> 7) `seq_tier[i] f(1)`, (decoder model) `decoder_model_present_for_this_op[i]` → `operating_parameters_info(i)` (#5), (initial display delay) `initial_display_delay_present_for_this_op[i]` → `initial_display_delay_minus_1[i] f(4)`. Then `frame_width_bits_minus_1 f(4)`, `frame_height_bits_minus_1 f(4)`, `max_frame_width_minus_1 f(n)`, `max_frame_height_minus_1 f(n)`, (!reduced) `frame_id_numbers_present_flag`; if set `delta_frame_id_length_minus_2 f(4)`, `additional_frame_id_length_minus_1 f(3)`; `use_128x128_superblock`, `enable_filter_intra`, `enable_intra_edge_filter`; if reduced: derived zeros for the inter tools and `seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS (2)`, `seq_force_integer_mv = SELECT_INTEGER_MV (2)`, `OrderHintBits = 0`; else `enable_interintra_compound`, `enable_masked_compound`, `enable_warped_motion`, `enable_dual_filter`, `enable_order_hint`, (if) `enable_jnt_comp`, `enable_ref_frame_mvs`, `seq_choose_screen_content_tools` → `seq_force_screen_content_tools` (2 or f(1)), (if != 0) `seq_choose_integer_mv` → `seq_force_integer_mv`, (enable_order_hint) `order_hint_bits_minus_1 f(3)`; `enable_superres`, `enable_cdef`, `enable_restoration`, `color_config()` (#6), `film_grain_params_present`. `trailing_bits()`. **Derived:** `OrderHintBits = order_hint_bits_minus_1 + 1` (0 when !enable_order_hint), `level`/`tier` names ("5.1 Main"), `operating point count`, `max frame size`, `NumPlanes`, `BitDepth`, `ovt_ColorDescription` candidate. |
| 3 | `timing_info()` AV1 §5.5.3 | `timing_info` | `num_units_in_display_tick f(32)`, `time_scale f(32)`, `equal_picture_interval f(1)` → `num_ticks_per_picture_minus_1 uvlc()`. Derived frame rate when equal_picture_interval. |
| 4 | `decoder_model_info()` AV1 §5.5.4 | `decoder_model_info` | `buffer_delay_length_minus_1 f(5)`, `num_units_in_decoding_tick f(32)`, `buffer_removal_time_length_minus_1 f(5)`, `frame_presentation_time_length_minus_1 f(5)`. |
| 5 | `operating_parameters_info(op)` AV1 §5.5.5 | `operating_parameters_info[op]` | `decoder_buffer_delay[op] f(n)`, `encoder_buffer_delay[op] f(n)`, `low_delay_mode_flag[op] f(1)` with n = `buffer_delay_length_minus_1+1`. |
| 6 | `color_config()` AV1 §5.5.2 | `color_config` | `high_bitdepth f(1)`; if seq_profile == 2 && high_bitdepth: `twelve_bit f(1)` → BitDepth 12/10; else BitDepth = high_bitdepth ? 10 : 8; `mono_chrome f(1)` (not for profile 1); `color_description_present_flag` → `color_primaries f(8)`, `transfer_characteristics f(8)`, `matrix_coefficients f(8)` (else 2/2/2 = unspecified); if mono: `color_range f(1)`, derived subsampling 1/1, `chroma_sample_position = CSP_UNKNOWN`, `separate_uv_delta_q = 0`; else if primaries 1 && transfer 13 && matrix 0 (sRGB): `color_range = 1`, subsampling 0/0; else `color_range f(1)`, subsampling by profile (0 → 1/1; 1 → 0/0; 2 → (BitDepth == 12) ? `subsampling_x f(1)` and (if x) `subsampling_y f(1)` : 1/0), (if 420) `chroma_sample_position f(2)` (0 UNKNOWN, 1 VERTICAL, 2 COLOCATED, 3 reserved), `separate_uv_delta_q f(1)`. Derived: `NumPlanes = mono_chrome ? 1 : 3`, `chroma_format` (400/420/422/444), `ovt_ColorDescription.chroma_siting` (COLOCATED → 3 top-left, VERTICAL → 1 left, UNKNOWN → 0). |
| 7 | `temporal_delimiter_obu()` AV1 §5.6 | `temporal_delimiter_obu` | Empty; sets `SeenFrameHeader = 0` (7.3.9). |
| 8 | `padding_obu()` AV1 §5.7 | `padding_obu` | `obu_padding_byte[]` opaque; derived byte count. |
| 9 | `metadata_obu()` AV1 §5.8.1 | `metadata_obu` | `metadata_type leb128()`; 1 `metadata_hdr_cll()` (AV1 §5.8.3: `max_cll f(16)`, `max_fall f(16)`); 2 `metadata_hdr_mdcv()` (AV1 §5.8.4: `primary_chromaticity_x[i] f(16)`, `primary_chromaticity_y[i] f(16)` for i = 0..2 **in R, G, B order**, 0.16 fixed point → ×50000/65536 for `ovt_ColorDescription` 0.00002 units; `white_point_chromaticity_x/y f(16)`; `luminance_max f(32)` 24.8 fixed → `mdcv_max_luminance = luminance_max * 10000 / 256`; `luminance_min f(32)` 18.14 fixed → `luminance_min * 10000 / 16384`); 3 `metadata_scalability()` (AV1 §5.8.5: `scalability_mode_idc f(8)`; 14 → `scalability_structure()` AV1 §5.8.6: `spatial_layers_cnt_minus_1 f(2)`, `spatial_layer_dimensions_present_flag`, `spatial_layer_description_present_flag`, `temporal_group_description_present_flag`, `scalability_structure_reserved_3bits`, per layer `spatial_layer_max_width/height f(16)`, `spatial_layer_ref_id f(8)`, `temporal_group_size f(8)`, per group `temporal_group_temporal_id f(3)`, `temporal_group_temporal_switching_up_point_flag`, `temporal_group_spatial_switching_up_point_flag`, `temporal_group_ref_cnt f(3)`, `temporal_group_ref_pic_diff[j] f(8)`); 4 `metadata_itut_t35()` (AV1 §5.8.2: `itu_t_t35_country_code f(8)`, (0xFF) `itu_t_t35_country_code_extension_byte f(8)`, `itu_t_t35_payload_bytes` to the end of the OBU **minus trailing bits**: AV1 T.35 payload length is `obu_size − bytes consumed`, and the trailing_bits are inside the payload bytes per Errata 1 — implement: payload = remaining bytes; then the T.35 dispatcher 7.3.11 parses `terminal_provider_code u(16)`, `terminal_provider_oriented_code u(16)`); 5 `metadata_timecode()` (AV1 §5.8.7: `counting_type f(5)`, `full_timestamp_flag`, `discontinuity_flag`, `cnt_dropped_flag`, `n_frames f(9)`, time fields with flags, `time_offset_length f(5)`, `time_offset_value f(n)`); 6–31 unregistered user private (opaque), others reserved (opaque + Diagnostic Info). Then `trailing_bits()`. |
| 10 | `frame_header_obu()` AV1 §5.9.1 | `frame_header_obu` (type 3), `redundant_frame_header_obu` (type 7) | if `SeenFrameHeader == 1`: `frame_header_copy()` — the parser re-parses the header identically and compares the bytes with the stored frame header; mismatch → Diagnostic Error `SYN-AV1-0010 "redundant frame header differs"`; the redundant header's `FrameInfo` contribution is none (it belongs to the current frame's units). Else `SeenFrameHeader = 1`, `uncompressed_header()` (#11); if `show_existing_frame`: `decode_frame_wrapup()` (slot load + possible key-frame refresh, 7.3.10.4), `SeenFrameHeader = 0`; else `TileNum = 0`, `SeenFrameHeader = 1`. Type 3 then `trailing_bits()`. |
| 11 | `uncompressed_header()` AV1 §5.9.2 | `uncompressed_header` | Implement **every** line of AV1 §5.9.2 in order: `frame_id_numbers_present_flag` → `idLen`; `allFrames = 255`; if `reduced_still_picture_header`: derived `show_existing_frame = 0`, `frame_type = KEY_FRAME`, `FrameIsIntra = 1`, `show_frame = 1`, `showable_frame = 0`; else `show_existing_frame f(1)`; if set: `frame_to_show_map_idx f(3)`, (decoder_model_info_present && !equal_picture_interval) `temporal_point_info()` (AV1 §5.9.31: `frame_presentation_time f(n)`), `refresh_frame_flags = 0`, (frame_id_numbers_present) `display_frame_id f(idLen)`, derived `frame_type = RefFrameType[frame_to_show_map_idx]`; if KEY_FRAME: `refresh_frame_flags = allFrames`; (film_grain_params_present) `load_grain_params(frame_to_show_map_idx)`; **return**. Else `frame_type f(2)` (enum 0 KEY_FRAME, 1 INTER_FRAME, 2 INTRA_ONLY_FRAME, 3 SWITCH_FRAME), `FrameIsIntra = (frame_type ∈ {INTRA_ONLY, KEY})`, `show_frame f(1)`; if show_frame && decoder model && !equal_picture_interval: `temporal_point_info()`; `showable_frame = show_frame ? (frame_type != KEY_FRAME) : f(1)`; `error_resilient_mode = (frame_type == SWITCH \|\| (KEY && show_frame)) ? 1 : f(1)`. If KEY && show_frame: for i in 0..7: `RefValid[i] = 0`, `RefOrderHint[i] = 0`; for i in 0..6: `OrderHints[LAST_FRAME+i] = 0`. `disable_cdf_update f(1)`; `allow_screen_content_tools = (seq_force_screen_content_tools == 2) ? f(1) : seq_force…`; if allow_screen_content_tools: `force_integer_mv = (seq_force_integer_mv == 2) ? f(1) : seq_force…` else 0; if FrameIsIntra: `force_integer_mv = 1`. If frame_id_numbers_present_flag: `PrevFrameID = current_frame_id`, `current_frame_id f(idLen)`, `mark_ref_frames(idLen)` (AV1 §5.9.4). `frame_size_override_flag = (SWITCH) ? 1 : (reduced) ? 0 : f(1)`; `order_hint f(OrderHintBits)`, `OrderHint = order_hint`; `primary_ref_frame = (FrameIsIntra \|\| error_resilient_mode) ? PRIMARY_REF_NONE (7) : f(3)`; if decoder_model_info_present_flag: `buffer_removal_time_present_flag f(1)` → per op with `decoder_model_present_for_this_op[opNum]`: compute `inTemporalLayer/inSpatialLayer` and if both: `buffer_removal_time[opNum] f(n)`. `allow_high_precision_mv = 0`, `use_ref_frame_mvs = 0`, `allow_intrabc = 0`; `refresh_frame_flags = (SWITCH \|\| (KEY && show_frame)) ? allFrames : f(8)`; if !FrameIsIntra \|\| refresh_frame_flags != allFrames: if error_resilient_mode && enable_order_hint: for i in 0..7: `ref_order_hint[i] f(OrderHintBits)`; if `ref_order_hint[i] != RefOrderHint[i]`: `RefValid[i] = 0` (and the decoder would create a grey frame — OVT records Diagnostic Info `SYN-AV1-0011`). If FrameIsIntra: `frame_size()` (#12), `render_size()` (#13), (allow_screen_content_tools && UpscaledWidth == FrameWidth) `allow_intrabc f(1)`; else: if !enable_order_hint: `frame_refs_short_signaling = 0` else `frame_refs_short_signaling f(1)` → if set `last_frame_idx f(3)`, `gold_frame_idx f(3)`, `set_frame_refs()` (AV1 §7.8 — implement the full algorithm: it fills `ref_frame_idx[]` from `RefOrderHint` using `shiftedOrderHints`; see 7.3.10.4); for i in 0..6: (!short) `ref_frame_idx[i] f(3)`, (frame_id_numbers_present) `delta_frame_id_minus_1 f(delta_frame_id_length_minus_2+2)` → derived `expectedFrameId`; if frame_size_override_flag && !error_resilient_mode: `frame_size_with_refs()` (#14) else `frame_size()`, `render_size()`; `allow_high_precision_mv = force_integer_mv ? 0 : f(1)`; `read_interpolation_filter()` (AV1 §5.9.10: `is_filter_switchable f(1)` → `interpolation_filter = SWITCHABLE (4)` or `f(2)` enum EIGHTTAP, EIGHTTAP_SMOOTH, EIGHTTAP_SHARP, BILINEAR); `is_motion_mode_switchable f(1)`; `use_ref_frame_mvs = (error_resilient_mode \|\| !enable_ref_frame_mvs) ? 0 : f(1)`; for i in 0..6: `refFrame = LAST_FRAME+i; hint = RefOrderHint[ref_frame_idx[i]]; OrderHints[refFrame] = hint; RefFrameSignBias[refFrame] = enable_order_hint ? (get_relative_dist(hint, OrderHint) > 0) : 0`. Then `disable_frame_end_update_cdf = (reduced_still_picture_header \|\| disable_cdf_update) ? 1 : f(1)`; if primary_ref_frame == PRIMARY_REF_NONE: `init_non_coeff_cdfs()`, `setup_past_independence()` (derived: `loop_filter_ref_deltas = {1,0,0,0,-1,0,-1,-1}`, mode deltas 0, `FeatureData` zero, `PrevGmParams` identity) else `load_cdfs(ref_frame_idx[primary_ref_frame])`, `load_previous()` (PrevGmParams = SavedGmParams[prevFrame], loop filter deltas and FeatureData loaded from the slot). if use_ref_frame_mvs: `motion_field_estimation()` — **not** performed (decoder only); `tile_info()` (#15), `quantization_params()` (#16), `segmentation_params()` (#17), `delta_q_params()` (#18), `delta_lf_params()` (#19); if primary_ref_frame == NONE `init_coeff_cdfs()` else `load_previous_segment_ids()` (no-op for OVT); derived `CodedLossless = 1; for segmentId in 0..7: qindex = get_qindex(1, segmentId); LosslessArray[segmentId] = (qindex == 0 && DeltaQYDc == 0 && DeltaQUAc == 0 && DeltaQUDc == 0 && DeltaQVAc == 0 && DeltaQVDc == 0); if !LosslessArray[segmentId] CodedLossless = 0; if using_qmatrix: SegQMLevel[0..2][segmentId] = LosslessArray ? 15 : qm_y/qm_u/qm_v`; `AllLossless = CodedLossless && (FrameWidth == UpscaledWidth)`; `loop_filter_params()` (#20), `cdef_params()` (#21), `lr_params()` (#22), `read_tx_mode()` (#23), `frame_reference_mode()` (#24), `skip_mode_params()` (#25); `allow_warped_motion = (FrameIsIntra \|\| error_resilient_mode \|\| !enable_warped_motion) ? 0 : f(1)`; `reduced_tx_set f(1)`; `global_motion_params()` (#26); `film_grain_params()` (#27). **Derived summary for FrameInfo:** `FrameType` (Key/Inter/IntraOnly/Switch), `shown = show_frame`, `showable_frame`, `is_rap = (frame_type == KEY_FRAME && show_frame)`, `poc = OrderHint`, `qp_base = base_q_idx`, `coded_size = {0,0,FrameWidth,FrameHeight}`, `crop = {0,0,RenderWidth,RenderHeight}` (plus `UpscaledWidth` in `derived`), `declared_refs` from `ref_frame_idx[]` slots (7.4.4). |
| 12 | `frame_size()` AV1 §5.9.5, `superres_params()` AV1 §5.9.8, `compute_image_size()` AV1 §5.9.9 | `frame_size` | if frame_size_override_flag: `frame_width_minus_1 f(n)`, `frame_height_minus_1 f(n)` (n = frame_width_bits_minus_1+1 …) else from sequence max; `superres_params()`: `use_superres = enable_superres ? f(1) : 0`; if set `coded_denom f(3)` → `SuperresDenom = coded_denom + 9` else 8; `UpscaledWidth = FrameWidth`; `FrameWidth = (UpscaledWidth*8 + (SuperresDenom/2)) / SuperresDenom`; `compute_image_size()`: `MiCols = 2*((FrameWidth+7) >> 3)`, `MiRows = 2*((FrameHeight+7) >> 3)`. |
| 13 | `render_size()` AV1 §5.9.6 | `render_size` | `render_and_frame_size_different f(1)` → `render_width_minus_1 f(16)`, `render_height_minus_1 f(16)`; else `RenderWidth = UpscaledWidth`, `RenderHeight = FrameHeight`. |
| 14 | `frame_size_with_refs()` AV1 §5.9.7 | `frame_size_with_refs` | for i in 0..6: `found_ref f(1)`; if found: `UpscaledWidth = RefUpscaledWidth[ref_frame_idx[i]]`, `FrameWidth = UpscaledWidth`, `FrameHeight = RefFrameHeight[…]`, `RenderWidth = RefRenderWidth[…]`, `RenderHeight`; break. If none: `frame_size()`, `render_size()`; else `superres_params()`, `compute_image_size()`. |
| 15 | `tile_info()` AV1 §5.9.15 | `tile_info` | `sbCols = use_128x128_superblock ? ((MiCols+31)>>5) : ((MiCols+15)>>4)`, `sbRows` likewise, `sbShift = use_128 ? 5 : 4`, `sbSize = sbShift+2`, `maxTileWidthSb = MAX_TILE_WIDTH (4096) >> sbSize`, `maxTileAreaSb = MAX_TILE_AREA (4096*2304) >> (2*sbSize)`, `minLog2TileCols = tile_log2(maxTileWidthSb, sbCols)`, `maxLog2TileCols = tile_log2(1, Min(sbCols, MAX_TILE_COLS=64))`, `maxLog2TileRows = tile_log2(1, Min(sbRows, MAX_TILE_ROWS=64))`, `minLog2Tiles = Max(minLog2TileCols, tile_log2(maxTileAreaSb, sbRows*sbCols))`; `uniform_tile_spacing_flag f(1)`; if set: `TileColsLog2 = minLog2TileCols; while TileColsLog2 < maxLog2TileCols: increment_tile_cols_log2 f(1); if 1 TileColsLog2++ else break`; `tileWidthSb = (sbCols + (1<<TileColsLog2) − 1) >> TileColsLog2`; `MiColStarts[i] = startSb << sbShift` for startSb = 0, tileWidthSb, …; `TileCols = i`; `MiColStarts[TileCols] = MiCols`; `minLog2TileRows = Max(minLog2Tiles − TileColsLog2, 0)`; `TileRowsLog2 = minLog2TileRows; while TileRowsLog2 < maxLog2TileRows: increment_tile_rows_log2 f(1) …`; `tileHeightSb`; `MiRowStarts[]`; `TileRows`; else: `widestTileSb = 0; startSb = 0; for i while startSb < sbCols: MiColStarts[i] = startSb << sbShift; maxWidth = Min(sbCols − startSb, maxTileWidthSb); width_in_sbs_minus_1 ns(maxWidth); sizeSb = width_in_sbs_minus_1+1; widestTileSb = Max(sizeSb, widestTileSb); startSb += sizeSb`; `MiColStarts[i] = MiCols; TileCols = i; TileColsLog2 = tile_log2(1, TileCols)`; `maxTileAreaSb = (minLog2Tiles > 0) ? (sbRows*sbCols) >> (minLog2Tiles+1) : sbRows*sbCols`; `maxTileHeightSb = Max(maxTileAreaSb / widestTileSb, 1)`; rows loop with `height_in_sbs_minus_1 ns(maxHeight)`; `TileRows`, `TileRowsLog2`. If `TileColsLog2 > 0 \|\| TileRowsLog2 > 0`: `context_update_tile_id f(TileRowsLog2 + TileColsLog2)`, `tile_size_bytes_minus_1 f(2)` → `TileSizeBytes = +1`; else `context_update_tile_id = 0`. Derived: `NumTiles = TileCols*TileRows`, `MiColStarts[]`, `MiRowStarts[]`, tile rectangles in luma samples (`MiColStarts[i]*4`). `tile_log2(blkSize, target)`: `k = 0; while (blkSize << k) < target: k++; return k`. |
| 16 | `quantization_params()` AV1 §5.9.12 | `quantization_params` | `base_q_idx f(8)`, `DeltaQYDc = read_delta_q()`; if NumPlanes > 1: `diff_uv_delta = separate_uv_delta_q ? f(1) : 0`; `DeltaQUDc = read_delta_q()`, `DeltaQUAc = read_delta_q()`; if diff_uv_delta: `DeltaQVDc`, `DeltaQVAc` = read_delta_q() else copies of U; else all 0. `using_qmatrix f(1)` → `qm_y f(4)`, `qm_u f(4)`, `qm_v = separate_uv_delta_q ? f(4) : qm_u`. Derived: `get_qindex(ignoreDeltaQ, segmentId)` (AV1 §7.12.2) available for 7.6; `qp_base = base_q_idx`. |
| 17 | `segmentation_params()` AV1 §5.9.14 | `segmentation_params` | `segmentation_enabled f(1)`; if set: if primary_ref_frame == NONE: `segmentation_update_map = 1`, `segmentation_temporal_update = 0`, `segmentation_update_data = 1` else `segmentation_update_map f(1)`, (if) `segmentation_temporal_update f(1)`, `segmentation_update_data f(1)`; if update_data: for i in 0..7, j in 0..7 (SEG_LVL_MAX = 8): `feature_value = 0; feature_enabled f(1); FeatureEnabled[i][j] = feature_enabled; clippedValue = 0; if enabled: bitsToRead = Segmentation_Feature_Bits[j] = {8,6,6,6,6,3,0,0}; limit = Segmentation_Feature_Max[j] = {255, MAX_LOOP_FILTER (63), 63, 63, 63, 7, 0, 0}; if Segmentation_Feature_Signed[j] = {1,1,1,1,1,0,0,0}: feature_value su(1+bitsToRead); clippedValue = Clip3(−limit, limit, feature_value) else feature_value f(bitsToRead); clippedValue = Clip3(0, limit, feature_value)`; `FeatureData[i][j] = clippedValue`; else all FeatureEnabled/Data = 0. Derived: `SegIdPreSkip` and `LastActiveSegId` (AV1 §5.9.14 end: for i, j: if FeatureEnabled[i][j]: LastActiveSegId = i; if j >= SEG_LVL_REF_FRAME (5): SegIdPreSkip = 1). Feature names enum: 0 ALT_Q, 1 ALT_LF_Y_V, 2 ALT_LF_Y_H, 3 ALT_LF_U, 4 ALT_LF_V, 5 REF_FRAME, 6 SKIP, 7 GLOBALMV. |
| 18 | `delta_q_params()` AV1 §5.9.17 | `delta_q_params` | `delta_q_res = 0; delta_q_present = 0; if base_q_idx > 0: delta_q_present f(1); if delta_q_present: delta_q_res f(2)`. |
| 19 | `delta_lf_params()` AV1 §5.9.18 | `delta_lf_params` | `delta_lf_present = 0; delta_lf_res = 0; delta_lf_multi = 0; if delta_q_present: if !allow_intrabc: delta_lf_present f(1); if delta_lf_present: delta_lf_res f(2), delta_lf_multi f(1)`. |
| 20 | `loop_filter_params()` AV1 §5.9.11 | `loop_filter_params` | if CodedLossless \|\| allow_intrabc: `loop_filter_level[0..1] = 0`, `loop_filter_ref_deltas = {1,0,0,0,−1,0,−1,−1}`, mode deltas 0; return. `loop_filter_level[0] f(6)`, `loop_filter_level[1] f(6)`; if NumPlanes > 1 && (level[0] \|\| level[1]): `loop_filter_level[2] f(6)`, `loop_filter_level[3] f(6)`; `loop_filter_sharpness f(3)`; `loop_filter_delta_enabled f(1)`; if set: `loop_filter_delta_update f(1)`; if set: for i in 0..7: `update_ref_delta f(1)` → `loop_filter_ref_deltas[i] su(1+6)`; for i in 0..1: `update_mode_delta f(1)` → `loop_filter_mode_deltas[i] su(1+6)`. |
| 21 | `cdef_params()` AV1 §5.9.19 | `cdef_params` | if CodedLossless \|\| allow_intrabc \|\| !enable_cdef: `cdef_bits = 0`, `cdef_y_pri_strength[0] = cdef_y_sec_strength[0] = cdef_uv_pri/sec_strength[0] = 0`, `CdefDamping = 3`; return. `cdef_damping_minus_3 f(2)` → `CdefDamping = +3`; `cdef_bits f(2)`; for i < (1 << cdef_bits): `cdef_y_pri_strength[i] f(4)`, `cdef_y_sec_strength[i] f(2)` (if == 3 → 4), if NumPlanes > 1: `cdef_uv_pri_strength[i] f(4)`, `cdef_uv_sec_strength[i] f(2)` (3 → 4). |
| 22 | `lr_params()` AV1 §5.9.20 | `lr_params` | if AllLossless \|\| allow_intrabc \|\| !enable_restoration: `FrameRestorationType[0..2] = RESTORE_NONE`, `UsesLr = 0`; return. `UsesLr = 0; usesChromaLr = 0; for i < NumPlanes: lr_type f(2); FrameRestorationType[i] = Remap_Lr_Type[lr_type] = {RESTORE_NONE (0), RESTORE_SWITCHABLE (3), RESTORE_WIENER (1), RESTORE_SGRPROJ (2)}; if != NONE: UsesLr = 1; if i > 0 usesChromaLr = 1`. if UsesLr: if use_128x128_superblock: `lr_unit_shift f(1); lr_unit_shift++` else `lr_unit_shift f(1); if lr_unit_shift: lr_unit_extra_shift f(1); lr_unit_shift += lr_unit_extra_shift`; `LoopRestorationSize[0] = RESTORATION_TILESIZE_MAX (256) >> (2 − lr_unit_shift)`; if subsampling_x && subsampling_y && usesChromaLr: `lr_uv_shift f(1)` else 0; `LoopRestorationSize[1] = LoopRestorationSize[2] = LoopRestorationSize[0] >> lr_uv_shift`. Derived: LR unit counts per plane (AV1 §7.17: `unitSize`, `count_units_in_frame`). |
| 23 | `read_tx_mode()` AV1 §5.9.21 | `tx_mode` | if CodedLossless: `TxMode = ONLY_4X4`; else `tx_mode_select f(1)` → `TxMode = tx_mode_select ? TX_MODE_SELECT : TX_MODE_LARGEST`. |
| 24 | `frame_reference_mode()` AV1 §5.9.23 | `frame_reference_mode` | `reference_select = FrameIsIntra ? 0 : f(1)`. |
| 25 | `skip_mode_params()` AV1 §5.9.22 | `skip_mode_params` | if FrameIsIntra \|\| !reference_select \|\| !enable_order_hint: `skipModeAllowed = 0`; else: `forwardIdx = −1; backwardIdx = −1; for i in 0..6: refHint = RefOrderHint[ref_frame_idx[i]]; if get_relative_dist(refHint, OrderHint) < 0: if forwardIdx < 0 \|\| get_relative_dist(refHint, forwardHint) > 0: forwardIdx = i; forwardHint = refHint; else if get_relative_dist(refHint, OrderHint) > 0: if backwardIdx < 0 \|\| get_relative_dist(refHint, backwardHint) < 0: backwardIdx = i; backwardHint = refHint`; if forwardIdx < 0: skipModeAllowed = 0; else if backwardIdx >= 0: skipModeAllowed = 1; `SkipModeFrame[0] = LAST_FRAME + Min(forwardIdx, backwardIdx)`, `SkipModeFrame[1] = LAST_FRAME + Max(…)`; else: `secondForwardIdx = −1; for i: refHint = …; if get_relative_dist(refHint, forwardHint) < 0: if secondForwardIdx < 0 \|\| get_relative_dist(refHint, secondForwardHint) > 0: secondForwardIdx = i; secondForwardHint = refHint`; if secondForwardIdx < 0: 0 else 1 with `SkipModeFrame[0] = LAST_FRAME + Min(forwardIdx, secondForwardIdx)`, `[1] = Max`. `skip_mode_present = skipModeAllowed ? f(1) : 0`. |
| 26 | `global_motion_params()` AV1 §5.9.24 (+ `read_global_param()` AV1 §5.9.25, `decode_signed_subexp_with_ref()` AV1 §5.9.26, `decode_unsigned_subexp_with_ref()` AV1 §5.9.27, `decode_subexp()` AV1 §5.9.28, `inverse_recenter()` AV1 §5.9.29) | `global_motion_params` | for ref in LAST_FRAME..ALTREF_FRAME: `GmType[ref] = IDENTITY`; `gm_params[ref][i] = (i % 3 == 2) ? 1 << WARPEDMODEL_PREC_BITS (16) : 0`; if FrameIsIntra return. For each ref: `is_global f(1)`; if set: `is_rot_zoom f(1)`; if set TRANSLATION… exactly: `if is_rot_zoom: type = ROTZOOM else: is_translation f(1); type = is_translation ? TRANSLATION : AFFINE`; `GmType[ref] = type`; if type >= ROTZOOM: `read_global_param(type, ref, 2)`, `read_global_param(type, ref, 3)`; if type == AFFINE: `read_global_param(…, 4)`, `(…, 5)` else `gm_params[ref][4] = −gm_params[ref][3]`, `gm_params[ref][5] = gm_params[ref][2]`; if type >= TRANSLATION: `read_global_param(type, ref, 0)`, `(…, 1)`. `read_global_param`: `absBits = GM_ABS_ALPHA_BITS (12); precBits = GM_ALPHA_PREC_BITS (15); if idx < 2: if type == TRANSLATION: absBits = GM_ABS_TRANS_ONLY_BITS (9) − !allow_high_precision_mv; precBits = GM_TRANS_ONLY_PREC_BITS (3) − !allow_high_precision_mv else absBits = GM_ABS_TRANS_BITS (12); precBits = GM_TRANS_PREC_BITS (6)`; `precDiff = WARPEDMODEL_PREC_BITS − precBits; round = (idx % 3) == 2 ? (1 << WARPEDMODEL_PREC_BITS) : 0; sub = (idx % 3) == 2 ? (1 << precBits) : 0; mx = (1 << absBits); r = (PrevGmParams[ref][idx] >> precDiff) − sub; gm_params[ref][idx] = (decode_signed_subexp_with_ref(−mx, mx+1, r) << precDiff) + round`. `decode_signed_subexp_with_ref(low, high, r)`: `x = decode_unsigned_subexp_with_ref(high − low, r − low); return x + low`. `decode_unsigned_subexp_with_ref(mx, r)`: `v = decode_subexp(mx); if (r << 1) <= mx: return inverse_recenter(r, v) else return mx − 1 − inverse_recenter(mx − 1 − r, v)`. `decode_subexp(numSyms)`: `i = 0; mk = 0; k = 3; loop: b2 = i ? k + i − 1 : k; a = 1 << b2; if numSyms <= mk + 3*a: subexp_final_bits ns(numSyms − mk); return subexp_final_bits + mk; else subexp_more_bits f(1); if subexp_more_bits: i++; mk += a else subexp_bits f(b2); return subexp_bits + mk`. `inverse_recenter(r, v)`: `if v > 2*r return v; else if v & 1 return r − ((v+1) >> 1); else return r + (v >> 1)`. Derived: per-ref type name and the 6 params as text; also the decoded `subexp_*` syntax nodes (so trace_headers comparisons match). |
| 27 | `film_grain_params()` AV1 §5.9.30 | `film_grain_params` | if !film_grain_params_present \|\| (!show_frame && !showable_frame): `reset_grain_params()`; return. `apply_grain f(1)`; if !apply_grain: reset; return. `grain_seed f(16)`; `update_grain = (frame_type == INTER_FRAME) ? f(1) : 1`; if !update_grain: `film_grain_params_ref_idx f(3)` (must be one of ref_frame_idx[]; else Diagnostic Error), `tempGrainSeed = grain_seed; load_grain_params(film_grain_params_ref_idx); grain_seed = tempGrainSeed; return`. `num_y_points f(4) ≤ 14`; `point_y_value[i] f(8)` (strictly increasing, Diagnostic), `point_y_scaling[i] f(8)`; `chroma_scaling_from_luma = mono_chrome ? 0 : f(1)`; if mono \|\| chroma_scaling_from_luma \|\| (subsampling_x == 1 && subsampling_y == 1 && num_y_points == 0): `num_cb_points = num_cr_points = 0` else `num_cb_points f(4) ≤ 10`, `point_cb_value/scaling[i] f(8)`, `num_cr_points f(4)`, `point_cr_value/scaling[i]`; `grain_scaling_minus_8 f(2)`, `ar_coeff_lag f(2)`, `numPosLuma = 2*ar_coeff_lag*(ar_coeff_lag+1)`; if num_y_points: `numPosChroma = numPosLuma + 1`, `ar_coeffs_y_plus_128[i] f(8)` × numPosLuma else numPosChroma = numPosLuma; if chroma_scaling_from_luma \|\| num_cb_points: `ar_coeffs_cb_plus_128[i] f(8)` × numPosChroma; same for cr; `ar_coeff_shift_minus_6 f(2)`, `grain_scale_shift f(2)`; if num_cb_points: `cb_mult f(8)`, `cb_luma_mult f(8)`, `cb_offset f(9)`; if num_cr_points: `cr_mult`, `cr_luma_mult`, `cr_offset f(9)`; `overlap_flag f(1)`, `clip_to_restricted_range f(1)`. |
| 28 | `frame_obu()` AV1 §5.10 | `frame_obu` | `frame_header_obu()` (#10) then `byte_alignment()` then `tile_group_obu(sz − headerBytes)` (#29). |
| 29 | `tile_group_obu(sz)` AV1 §5.11.1 | `tile_group_obu` | `NumTiles = TileCols*TileRows; startBitPos = bit_pos(); tile_start_and_end_present_flag = (NumTiles > 1) ? f(1) : 0; if NumTiles == 1 \|\| !flag: tg_start = 0; tg_end = NumTiles − 1 else tileBits = TileColsLog2 + TileRowsLog2; tg_start f(tileBits); tg_end f(tileBits)`; `byte_alignment()`; `endBitPos; headerBytes = (endBitPos − startBitPos)/8; sz −= headerBytes`; for TileNum = tg_start..tg_end: `tileRow = TileNum / TileCols; tileCol = TileNum % TileCols; lastTile = (TileNum == tg_end); if lastTile: tileSize = sz else: tile_size_minus_1 le(TileSizeBytes); tileSize = tile_size_minus_1 + 1; sz −= tileSize + TileSizeBytes`; `MiRowStart/End`, `MiColStart/End`; **tile data is opaque:** node `tile_data[TileNum]` kind Bytes with span of `tileSize` bytes; derived `tile byte range` (file offset, for `PartitionInfo`). After tg_end == NumTiles − 1: the frame is complete → `SeenFrameHeader = 0` and the reference-update process (7.3.10.4) runs. Tile size overrun → ParseError `SYN-AV1-0020`. |
| 30 | `tile_list_obu()` §5.12 | `tile_list_obu` | `output_frame_width_in_tiles_minus_1 f(8)`, `output_frame_height_in_tiles_minus_1 f(8)`, `tile_count_minus_1 f(16)`; per tile `tile_list_entry()`: `anchor_frame_idx f(8)`, `anchor_tile_row f(8)`, `anchor_tile_col f(8)`, `tile_data_size_minus_1 le(2)`, opaque `coded_tile_data`. Large-scale tile streams are indexed but not analysed (Diagnostic Info). |

### 7.3.7 Typed output structs

Every structure row above produces a C++ struct in `core/include/ovt/cxx/syntax/<codec>/*.hpp` with: all syntax elements (spec names, `uint32_t`/`int32_t`/`bool`/`std::array`/`std::vector`), all derived variables (spec names), `BitSpan span` of the whole structure and a `const SyntaxNodeBuffer* tree` (owned by the Unit). Containers: `Sps`, `Pps`, `SliceHeader`, `Sei` (vector of `SeiMessage{payload_type, payload_size, std::variant<…>}`), `Vps`, `SliceSegmentHeader`, `StRefPicSet`, `SequenceHeader`, `FrameHeader`, `TileGroup`, `Metadata`. Parsers are declared as:

```cpp
namespace ovt::syntax::h264 { Sps parse_sps(BitReader&, SyntaxBuilder&); Pps parse_pps(BitReader&, SyntaxBuilder&, const SpsTable&); SliceHeader parse_slice_header(BitReader&, SyntaxBuilder&, const NalHeader&, const ActiveSets&); Sei parse_sei(BitReader&, SyntaxBuilder&, const ActiveSets&); … }
namespace ovt::syntax::h265 { … SliceSegmentHeader parse_slice_segment_header(BitReader&, SyntaxBuilder&, const NalHeader&, const ActiveSets&, const SliceSegmentHeader* prev_independent); … }
namespace ovt::syntax::av1  { SequenceHeader parse_sequence_header(BitReader&, SyntaxBuilder&); FrameHeader parse_frame_header(BitReader&, SyntaxBuilder&, const SequenceHeader&, DecoderState&, bool is_redundant); TileGroup parse_tile_group(BitReader&, SyntaxBuilder&, const FrameHeader&, size_t sz); Metadata parse_metadata(BitReader&, SyntaxBuilder&); … }
```

### 7.3.8 Parameter-set activation tracking

`ovt::syntax::ActiveSets` (one per stream, owned by `index`):

| Codec | Tables | Activation rule (normative) |
|---|---|---|
| H.264 | `sps[32]`, `pps[256]`, `subset_sps[32]` — each entry stores the **latest received** parsed struct plus the `Unit.index` it came from and a `generation` counter. | A slice's `pic_parameter_set_id` selects `pps[id]`; `pps.seq_parameter_set_id` selects `sps[id]` (or `subset_sps[id]` for NAL types 20 with MVC/SVC — H.7.4.1.2.1). Activation happens when the first VCL NAL of a picture is parsed (§7.4.1.2.1: a PPS/SPS received *before* the picture replaces the entry; a PPS with the same id received *inside* a picture after its first slice MUST have identical content — otherwise Diagnostic Error `SYN-H264-0021 "PPS %d changed within a picture"`). `FrameInfo` records `active_sps_unit`, `active_pps_unit` (as `derived` text and in the index's per-frame private struct). When a PPS arrives before its SPS, it is parsed provisionally (7.3.4 #6) and **re-parsed** when the SPS arrives (the `generation` counter changes, so cached derived values are recomputed). Scaling-list fall-back (Table 7-2) is resolved at activation: `Flat_4x4/8x8` when SPS has no matrix and PPS has none; fall-back rule A (from previous list / defaults) when SPS lists present. |
| H.265 | `vps[16]`, `sps[16]`, `pps[64]` | `slice_pic_parameter_set_id` → `pps`; `pps_seq_parameter_set_id` → `sps`; `sps_video_parameter_set_id` → `vps` (missing VPS is a Warning, not an error — many streams omit it in-band). Same "replace before / identical within" rule (§7.4.2.4.2). PPS tile tables (`colBd`, `CtbAddrRsToTs`, …) are computed at activation because they need the SPS; store them in `ActivePps` alongside the parsed PPS. Dependent slice segments copy the preceding independent slice segment header of the same picture (passed as `prev_independent`). |
| AV1 | `seq` (single current sequence header), `DecoderState` | A sequence header OBU replaces `seq`; a change in any field other than `operating_parameters_info`/`initial_display_delay` starts a new coded video sequence and MUST be followed by a KEY frame with show_frame = 1 (else Diagnostic Warning `SYN-AV1-0003`). The 8-slot `DecoderState` (7.3.10.4) is reset at every such KEY frame. |

### 7.3.9 Frame-boundary detection

The `index` module groups units into frames (§0.5.3 `FrameInfo.units`) with these rules. Non-VCL units preceding a frame's first VCL unit belong to no frame (`Unit.frame.decode_index = −1`) except that OVT includes them in `FrameInfo.units` *for size accounting* when they are AUD, SEI, parameter sets or metadata immediately preceding that frame within the same access unit / temporal unit (`FrameInfo.size_bytes` then equals the access unit size, which is what users expect in the frame-size timeline; a `derived` note "size includes N non-VCL bytes" is emitted).

**H.264 (§7.4.1.2.4 first VCL NAL unit of a new primary coded picture).** For each VCL NAL (types 1, 5; and 20 for the base view only) compare its parsed slice header `cur` with the previous VCL NAL's header `prev` of the current picture; a new picture starts when ANY of the following holds:
1. `cur.frame_num != prev.frame_num`
2. `cur.pic_parameter_set_id != prev.pic_parameter_set_id`
3. `cur.field_pic_flag != prev.field_pic_flag`
4. `cur.bottom_field_flag != prev.bottom_field_flag` (both field pictures)
5. `(cur.nal_ref_idc == 0) != (prev.nal_ref_idc == 0)`
6. `pic_order_cnt_type == 0` and (`cur.pic_order_cnt_lsb != prev.pic_order_cnt_lsb` or `cur.delta_pic_order_cnt_bottom != prev.delta_pic_order_cnt_bottom`)
7. `pic_order_cnt_type == 1` and (`delta_pic_order_cnt[0]` or `[1]` differ)
8. `cur.IdrPicFlag != prev.IdrPicFlag`
9. both IDR and `cur.idr_pic_id != prev.idr_pic_id`
10. An AUD (9), SPS (7), PPS (8), SEI (6), prefix NAL (14), subset SPS (15), or NAL types 16–18 was seen since the previous VCL NAL (§7.4.1.2.3: these may only precede the first VCL NAL of a primary coded picture).
`first_mb_in_slice == 0` is used **only** as a hint for Diagnostic `SYN-H264-0060 "slice with first_mb_in_slice 0 does not start a new picture"` (legal with arbitrary slice order in Baseline). Redundant coded pictures (`redundant_pic_cnt > 0`) belong to the same `FrameInfo` and are flagged in `derived`. **Field pairing:** two consecutive field pictures of opposite parity with the same `frame_num` form a complementary field pair (§7.4.1.2.4 note; the second field of a reference pair is never an IDR unless the first was). OVT keeps **one `FrameInfo` per coded field** (`structure` = TopField or BottomField) and records the pairing in `derived` (`"second field of frame-store F"`); `display_index` counts complementary field pairs once: **both** fields of a pair carry the same `display_index` (chapter 6 §6.2.5), `StreamIndex.display_order[d]` holds the decode index of the **first decoded** field, and `show_existing_of` stays `-1` for every H.264 picture (it is an AV1-only field; the pairing is recoverable from `derived`, equal `display_index` and `structure`). An unpaired field gets its own `display_index`. This keeps `decode_index` aligned with the decoder's picture count.

**H.265 (§7.4.2.4.4).** A new picture starts at a VCL NAL whose `first_slice_segment_in_pic_flag == 1`. Additionally, the first of AUD (35), VPS (32), SPS (33), PPS (34), prefix SEI (39), NAL types 41–44 or 48–55 after the last VCL NAL of a picture starts a new access unit (§7.4.2.4.4). A VCL NAL with `first_slice_segment_in_pic_flag == 0` arriving when no picture is open → Diagnostic Error `SYN-H265-0060 "slice segment without first slice segment"`; it is attached to a synthetic frame. Pictures with `nuh_layer_id > 0` (MV-HEVC/SHVC) get `layer_id` and their own `FrameInfo` in decode order; they do not interrupt the base-layer picture detection (compare only within the same `nuh_layer_id`).

**AV1 (§7.5 "Ordering of OBUs").** State: `SeenFrameHeader`, `TileNum`. A **frame** is: (a) one `OBU_FRAME`, or (b) one `OBU_FRAME_HEADER` with `show_existing_frame == 1` (complete by itself), or (c) one `OBU_FRAME_HEADER` (show_existing_frame = 0) followed by `OBU_TILE_GROUP`s until a tile group with `tg_end == NumTiles − 1` (intervening `OBU_REDUNDANT_FRAME_HEADER`, `OBU_METADATA`, `OBU_PADDING` belong to the same frame). A **temporal unit** starts at each `OBU_TEMPORAL_DELIMITER` and MUST contain exactly one *shown* frame per spatial layer (`show_frame == 1` or `show_existing_frame == 1`); a temporal unit with zero or ≥ 2 shown frames (same spatial_id) → Diagnostic Warning `SYN-AV1-0030`. Hidden frames (`show_frame == 0`) get their own `FrameInfo` with `shown = false`, `display_index = −1`; a later `show_existing_frame` frame gets `FrameInfo{shown = true, show_existing_of = <decode_index of the frame in slot frame_to_show_map_idx>, units = [that frame header OBU], size_bytes = its OBU size, type = RefFrameType[slot]}` and receives the display index. A tile group arriving with `SeenFrameHeader == 0` → Diagnostic Error `SYN-AV1-0031 "tile group without frame header"`. In Annex B streams the `temporal_unit_size` boundaries MUST agree with temporal delimiters (Diagnostic `SYN-AV1-0032` otherwise).

### 7.3.10 POC / frame_num / order_hint derivation

The derivations run inside `index` (7.4) over the parsed headers of the first VCL unit of each frame. All state variables are kept in `ovt::index::<Codec>PocState`.

#### 7.3.10.1 H.264 §8.2.1

State: `prevPicOrderCntMsb`, `prevPicOrderCntLsb`, `prevFrameNumOffset`, `prevFrameNum`, `prevRefHadMmco5`, `prevRefWasBottomField`, `prevRefTopFieldOrderCnt`, `prevRefFrameNum`. Reset at every IDR.

```
// input: slice header h (first slice of the picture), sps, nal_ref_idc, memory_management ops of the PREVIOUS reference picture
if h.IdrPicFlag:                                 // §8.2.1: IDR resets
    prevPicOrderCntMsb = 0; prevPicOrderCntLsb = 0; prevFrameNumOffset = 0 (used by types 1/2)
switch sps.pic_order_cnt_type:
case 0:  // §8.2.1.1
    if !IdrPicFlag:
        if prevRefHadMmco5:  prevPicOrderCntMsb = 0; prevPicOrderCntLsb = prevRefWasBottomField ? 0 : prevRefTopFieldOrderCnt
        // else prevPicOrderCntMsb/Lsb are those of the previous REFERENCE picture (nal_ref_idc != 0), set in step "after"
    MaxLsb = sps.MaxPicOrderCntLsb; lsb = h.pic_order_cnt_lsb
    if      lsb < prevPicOrderCntLsb && (prevPicOrderCntLsb - lsb) >= MaxLsb/2: PicOrderCntMsb = prevPicOrderCntMsb + MaxLsb
    else if lsb > prevPicOrderCntLsb && (lsb - prevPicOrderCntLsb) >  MaxLsb/2: PicOrderCntMsb = prevPicOrderCntMsb - MaxLsb
    else                                                                      PicOrderCntMsb = prevPicOrderCntMsb
    if !h.field_pic_flag:            TopFieldOrderCnt = PicOrderCntMsb + lsb; BottomFieldOrderCnt = TopFieldOrderCnt + h.delta_pic_order_cnt_bottom
    else if !h.bottom_field_flag:    TopFieldOrderCnt = PicOrderCntMsb + lsb
    else:                            BottomFieldOrderCnt = PicOrderCntMsb + lsb
    after (if nal_ref_idc != 0): prevPicOrderCntMsb = PicOrderCntMsb; prevPicOrderCntLsb = lsb (if mmco5 present: handled above at next picture)
case 1:  // §8.2.1.2
    if IdrPicFlag: FrameNumOffset = 0
    else if prevRefHadMmco5 ... (prevFrameNumOffset = 0 when the previous picture had mmco5)
    else if prevFrameNum > h.frame_num: FrameNumOffset = prevFrameNumOffset + sps.MaxFrameNum
    else: FrameNumOffset = prevFrameNumOffset
    absFrameNum = (sps.num_ref_frames_in_pic_order_cnt_cycle != 0) ? FrameNumOffset + h.frame_num : 0
    if nal_ref_idc == 0 && absFrameNum > 0: absFrameNum -= 1
    if absFrameNum > 0:
        picOrderCntCycleCnt = (absFrameNum - 1) / sps.num_ref_frames_in_pic_order_cnt_cycle
        frameNumInPicOrderCntCycle = (absFrameNum - 1) % sps.num_ref_frames_in_pic_order_cnt_cycle
        expectedPicOrderCnt = picOrderCntCycleCnt * sps.ExpectedDeltaPerPicOrderCntCycle + Σ_{i=0..frameNumInPicOrderCntCycle} sps.offset_for_ref_frame[i]
    else: expectedPicOrderCnt = 0
    if nal_ref_idc == 0: expectedPicOrderCnt += sps.offset_for_non_ref_pic
    if !field_pic_flag:   TopFieldOrderCnt = expectedPicOrderCnt + h.delta_pic_order_cnt[0]; BottomFieldOrderCnt = TopFieldOrderCnt + sps.offset_for_top_to_bottom_field + h.delta_pic_order_cnt[1]
    else if !bottom_field_flag: TopFieldOrderCnt = expectedPicOrderCnt + h.delta_pic_order_cnt[0]
    else: BottomFieldOrderCnt = expectedPicOrderCnt + sps.offset_for_top_to_bottom_field + h.delta_pic_order_cnt[0]
    after: prevFrameNumOffset = (mmco5 in this picture) ? 0 : FrameNumOffset; prevFrameNum = (mmco5) ? 0 : h.frame_num
case 2:  // §8.2.1.3
    FrameNumOffset as in case 1
    tempPicOrderCnt = IdrPicFlag ? 0 : (nal_ref_idc == 0) ? 2*(FrameNumOffset + h.frame_num) - 1 : 2*(FrameNumOffset + h.frame_num)
    frame: Top = Bottom = tempPicOrderCnt; top field: Top = temp; bottom field: Bottom = temp
PicOrderCnt(picture) = frame ? Min(Top, Bottom) : the present field's value   (§8.2.1 eq. 8-1)
FrameInfo.poc = PicOrderCnt(picture)
```
Pitfall: for type 0, `prev*` are updated only by **reference** pictures; the mmco5 adjustment uses the previous reference picture's `TopFieldOrderCnt` *after* its own mmco5 adjustment (`tempPicOrderCnt` subtraction, §8.2.1: "the picture shall be inferred to have TopFieldOrderCnt = TopFieldOrderCnt − PicOrderCnt(CurrPic)"). The `index` keeps the adjusted value.

#### 7.3.10.2 H.265 §8.3.1

State: `prevTid0Pic` = the previous picture with `TemporalId == 0` that is not a RASL, RADL or sub-layer non-reference picture; its `PicOrderCntVal` gives `prevPicOrderCntLsb = prevTid0Pic.PicOrderCntVal & (MaxPicOrderCntLsb − 1)` and `prevPicOrderCntMsb = prevTid0Pic.PicOrderCntVal − prevPicOrderCntLsb`.

```
NoRaslOutputFlag: 1 for IDR, BLA, the first picture in the bitstream, the first picture after an EOS NAL (36); for CRA: 1 if HandleCraAsBlaFlag (OVT: 1 only when the CRA is the first picture decoded after a seek), else 0
if IRAP && NoRaslOutputFlag: PicOrderCntMsb = 0
else:
    lsb = slice_pic_order_cnt_lsb; MaxLsb = MaxPicOrderCntLsb
    if      lsb < prevPicOrderCntLsb && (prevPicOrderCntLsb - lsb) >= MaxLsb/2: PicOrderCntMsb = prevPicOrderCntMsb + MaxLsb
    else if lsb > prevPicOrderCntLsb && (lsb - prevPicOrderCntLsb) >  MaxLsb/2: PicOrderCntMsb = prevPicOrderCntMsb - MaxLsb
    else PicOrderCntMsb = prevPicOrderCntMsb
PicOrderCntVal = PicOrderCntMsb + slice_pic_order_cnt_lsb      // IDR: slice_pic_order_cnt_lsb inferred 0
after: if TemporalId == 0 && !RASL && !RADL && !sub-layer-non-ref: prevTid0Pic = this picture
FrameInfo.poc = PicOrderCntVal
```
RASL pictures associated with a CRA that has `NoRaslOutputFlag = 1` are marked `shown = false` with Diagnostic Info `SYN-H265-0101 "RASL picture not output (random access at CRA)"` **only in the analysis path when decoding starts at that CRA**; in the full index they are normal pictures.

#### 7.3.10.3 H.265 `st_ref_pic_set` derivation (§7.4.8, needed for RPS/DPB)

```
if inter_ref_pic_set_prediction_flag:
    RefRpsIdx = stRpsIdx - (delta_idx_minus1 + 1)        // delta_idx_minus1 = 0 when parsed inside the SPS
    deltaRps = (1 - 2*delta_rps_sign) * (abs_delta_rps_minus1 + 1)
    // eq. 7-61: S0 of the new set
    i = 0
    for j = NumPositivePics[RefRpsIdx]-1 downto 0:
        dPoc = DeltaPocS1[RefRpsIdx][j] + deltaRps
        if dPoc < 0 && use_delta_flag[NumNegativePics[RefRpsIdx] + j]: DeltaPocS0[i] = dPoc; UsedByCurrPicS0[i++] = used_by_curr_pic_flag[NumNegativePics[RefRpsIdx] + j]
    if deltaRps < 0 && use_delta_flag[NumDeltaPocs[RefRpsIdx]]: DeltaPocS0[i] = deltaRps; UsedByCurrPicS0[i++] = used_by_curr_pic_flag[NumDeltaPocs[RefRpsIdx]]
    for j = 0 .. NumNegativePics[RefRpsIdx]-1:
        dPoc = DeltaPocS0[RefRpsIdx][j] + deltaRps
        if dPoc < 0 && use_delta_flag[j]: DeltaPocS0[i] = dPoc; UsedByCurrPicS0[i++] = used_by_curr_pic_flag[j]
    NumNegativePics = i
    // eq. 7-62: S1
    i = 0
    for j = NumNegativePics[RefRpsIdx]-1 downto 0:
        dPoc = DeltaPocS0[RefRpsIdx][j] + deltaRps
        if dPoc > 0 && use_delta_flag[j]: DeltaPocS1[i] = dPoc; UsedByCurrPicS1[i++] = used_by_curr_pic_flag[j]
    if deltaRps > 0 && use_delta_flag[NumDeltaPocs[RefRpsIdx]]: DeltaPocS1[i] = deltaRps; UsedByCurrPicS1[i++] = used_by_curr_pic_flag[NumDeltaPocs[RefRpsIdx]]
    for j = 0 .. NumPositivePics[RefRpsIdx]-1:
        dPoc = DeltaPocS1[RefRpsIdx][j] + deltaRps
        if dPoc > 0 && use_delta_flag[NumNegativePics[RefRpsIdx] + j]: DeltaPocS1[i] = dPoc; UsedByCurrPicS1[i++] = used_by_curr_pic_flag[NumNegativePics[RefRpsIdx] + j]
    NumPositivePics = i
else:
    NumNegativePics = num_negative_pics; NumPositivePics = num_positive_pics
    DeltaPocS0[0] = -(delta_poc_s0_minus1[0] + 1); for i ≥ 1: DeltaPocS0[i] = DeltaPocS0[i-1] - (delta_poc_s0_minus1[i] + 1)
    DeltaPocS1[0] =  (delta_poc_s1_minus1[0] + 1); for i ≥ 1: DeltaPocS1[i] = DeltaPocS1[i-1] + (delta_poc_s1_minus1[i] + 1)
    UsedByCurrPicS0/S1 from the flags
NumDeltaPocs = NumNegativePics + NumPositivePics
use_delta_flag[j] defaults to 1 when not present (used_by_curr_pic_flag[j] == 1)
```

#### 7.3.10.4 AV1 order hints, reference slots and `set_frame_refs()` (§5.9.2, §7.8, §7.20, §7.21)

`DecoderState` (per stream, reset on a shown KEY frame): for i in 0..7: `RefValid[i]`, `RefFrameId[i]`, `RefUpscaledWidth[i]`, `RefFrameWidth[i]`, `RefFrameHeight[i]`, `RefRenderWidth[i]`, `RefRenderHeight[i]`, `RefMiCols[i]`, `RefMiRows[i]`, `RefFrameType[i]`, `RefSubsamplingX/Y[i]`, `RefBitDepth[i]`, `RefOrderHint[i]`, `RefShowableFrame[i]`, `SavedOrderHints[i][8]`, `SavedGmParams[i][8][6]`, `SavedLoopFilterRefDeltas[i][8]`, `SavedLoopFilterModeDeltas[i][2]`, `SavedFeatureEnabled/Data[i]`, `SavedFilmGrainParams[i]`, plus OVT's `slot_decode_index[i]` (the `FrameId.decode_index` stored in the slot, −1 if empty). CDFs and motion fields are **not** modelled.

```
get_relative_dist(a, b):                        // AV1 §5.9.3
    if !enable_order_hint return 0
    diff = a - b; m = 1 << (OrderHintBits - 1); diff = (diff & (m - 1)) - (diff & m); return diff

Reference frame update process (AV1 §7.20), run after the last tile group of a frame (or for show_existing_frame of a KEY frame after the loading process):
    if frame_type == INTRA_ONLY_FRAME && refresh_frame_flags == 0xFF: Diagnostic Error SYN-AV1-0012 (conformance violation)
    for i in 0..7:
        if (refresh_frame_flags >> i) & 1:
            RefValid[i] = 1; RefFrameId[i] = current_frame_id; RefUpscaledWidth[i] = UpscaledWidth; RefFrameWidth[i] = FrameWidth
            RefFrameHeight[i] = FrameHeight; RefRenderWidth[i] = RenderWidth; RefRenderHeight[i] = RenderHeight
            RefMiCols[i] = MiCols; RefMiRows[i] = MiRows; RefFrameType[i] = frame_type; RefSubsamplingX/Y[i]; RefBitDepth[i] = BitDepth
            RefOrderHint[i] = OrderHint; RefShowableFrame[i] = showable_frame
            for j in 0..7: SavedOrderHints[i][j] = OrderHints[j]
            SavedGmParams[i] = gm_params; SavedLoopFilter*Deltas[i] = loop_filter_*_deltas; SavedFeature*[i] = Feature*; SavedFilmGrainParams[i] = film grain params
            slot_decode_index[i] = this frame's decode_index

Reference frame loading process (AV1 §7.21), run for show_existing_frame == 1 with slot s = frame_to_show_map_idx:
    if !RefValid[s]: Diagnostic Error SYN-AV1-0013 "show_existing_frame of an empty slot"
    current_frame_id = RefFrameId[s]; UpscaledWidth = RefUpscaledWidth[s]; FrameWidth; FrameHeight; RenderWidth; RenderHeight; MiCols; MiRows
    subsampling, BitDepth, OrderHint = RefOrderHint[s]; OrderHints[j] = SavedOrderHints[s][j]; gm_params = SavedGmParams[s]; loop filter deltas; Feature*; film grain
    if RefFrameType[s] == KEY_FRAME: refresh_frame_flags = 0xFF and run the update process (all slots now hold this frame)   // AV1 §7.21 + AV1 §5.9.2
    FrameInfo.show_existing_of = slot_decode_index[s]; FrameInfo.poc = OrderHint

set_frame_refs() (§7.8), when frame_refs_short_signaling == 1:
    for i in 0..REFS_PER_FRAME-1 (7): ref_frame_idx[i] = -1
    ref_frame_idx[LAST_FRAME - LAST_FRAME] = last_frame_idx; ref_frame_idx[GOLDEN_FRAME - LAST_FRAME] = gold_frame_idx
    for i in 0..7: usedFrame[i] = 0
    usedFrame[last_frame_idx] = 1; usedFrame[gold_frame_idx] = 1
    curFrameHint = 1 << (OrderHintBits - 1)
    for i in 0..7: shiftedOrderHints[i] = curFrameHint + get_relative_dist(RefOrderHint[i], OrderHint)
    lastOrderHint = shiftedOrderHints[last_frame_idx]; if lastOrderHint >= curFrameHint: Diagnostic Error SYN-AV1-0014
    goldOrderHint = shiftedOrderHints[gold_frame_idx]; if goldOrderHint >= curFrameHint: Diagnostic Error SYN-AV1-0014
    ref = find_latest_backward():  // largest shiftedOrderHints[i] >= curFrameHint among !usedFrame
    if ref >= 0: ref_frame_idx[ALTREF_FRAME - LAST_FRAME] = ref; usedFrame[ref] = 1
    ref = find_earliest_backward(): // smallest shiftedOrderHints[i] >= curFrameHint among !usedFrame
    if ref >= 0: ref_frame_idx[BWDREF_FRAME - LAST_FRAME] = ref; usedFrame[ref] = 1
    ref = find_earliest_backward(); if ref >= 0: ref_frame_idx[ALTREF2_FRAME - LAST_FRAME] = ref; usedFrame[ref] = 1
    Ref_Frame_List = { LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME }
    for refFrame in Ref_Frame_List: if ref_frame_idx[refFrame - LAST_FRAME] < 0: ref = find_latest_forward() // largest shiftedOrderHints[i] < curFrameHint among !usedFrame
                                       if ref >= 0: ref_frame_idx[refFrame - LAST_FRAME] = ref; usedFrame[ref] = 1
    ref = -1; for i in 0..7: hint = shiftedOrderHints[i]; if ref < 0 || hint < earliestOrderHint: ref = i; earliestOrderHint = hint
    for i in 0..6: if ref_frame_idx[i] < 0: ref_frame_idx[i] = ref
```
`FrameInfo.poc = OrderHint` (`OrderHintBits == 0` → poc = decode_index, Diagnostic Info). `display_order` for AV1 is the order of shown frames in decode order (AV1 output order is decode order of shown/show_existing frames; order hints are *not* used for display ordering).

### 7.3.11 ITU-T T.35 and `user_data_unregistered` registries (`core/src/syntax/common/user_data_registry.cpp`)

T.35 dispatch after `itu_t_t35_country_code` (+ extension byte): read `itu_t_t35_terminal_provider_code u(16)` and `itu_t_t35_terminal_provider_oriented_code u(16)` **only for country code 0xB5 (US)**; other countries: payload opaque with `enum_name "T.35 country 0x%02X"`.

| country_code | provider_code | provider_oriented_code | Further match | `enum_name` | Parser |
|---|---|---|---|---|---|
| 0xB5 | 0x003C (Samsung) | 0x0001 | `application_identifier u(8) == 4`, `application_version u(8)` (0 or 1) | `"HDR10+ (SMPTE ST 2094-40)"` | Full ST 2094-40 Annex / CTA-861-H parse: `num_windows u(2)`, per extra window ellipse params, `targeted_system_display_maximum_luminance u(27)`, `targeted_system_display_actual_peak_luminance_flag` (+ 25×25 grid), per window `maxscl[3] u(17)`, `average_maxrgb u(17)`, `num_distribution_maxrgb_percentiles u(4)`, `distribution_maxrgb_percentages[i] u(7)`, `distribution_maxrgb_percentiles[i] u(17)`, `fraction_bright_pixels u(10)`, `mastering_display_actual_peak_luminance_flag` (+ grid), `tone_mapping_flag` → `knee_point_x u(12)`, `knee_point_y u(12)`, `num_bezier_curve_anchors u(4)`, `bezier_curve_anchors[i] u(10)`, `color_saturation_mapping_flag` → `color_saturation_weight u(6)`. Sets `ovt_ColorDescription.has_hdr10plus = 1`. |
| 0xB5 | 0x0031 (ATSC) | — (ATSC A/53 has no provider_oriented_code; the next field is `user_identifier u(32)`) | `user_identifier u(32) == 0x47413934 ('GA94')`, then `user_data_type_code u(8)`: 0x03 = `cc_data()` (CEA-708 DTVCC), 0x06 = bar data (`bar_data()`), any other value opaque | `"ATSC A/53 (GA94) CEA-708 captions"` | `cc_data()`: `process_em_data_flag`, `process_cc_data_flag`, `additional_data_flag`, `cc_count u(5)`, `em_data u(8)`, per cc: `marker_bits u(5)`, `cc_valid`, `cc_type u(2)`, `cc_data_1 u(8)`, `cc_data_2 u(8)`; derived: count of valid CEA-608 (types 0/1) vs 708 (2/3) packets. The caption bytes are **not** decoded into text in phase 1. |
| 0xB5 | 0x003B (Dolby) | 0x0000 | — | `"Dolby Vision RPU in T.35 (AV1)"` | Opaque; sets `has_dolby_vision = 1` (profile/level come from the container `dvcC/dvvC`, Container chapter). Verify provider code against the Dolby Vision for AV1 spec at implementation time (WI-1.120 pitfall). |
| 0xB5 | any other | any | — | `"T.35 provider 0x%04X / 0x%04X"` | Opaque. The registry is a `constexpr` array so adding entries needs no code changes elsewhere. |
| 0x26 (China) | 0x0004 | — | HDR Vivid (CUVA) | `"HDR Vivid (T/UWA 005)"` | Opaque; `enum_name` only. |

`user_data_unregistered` UUID registry (`uuid_iso_iec_11578`):

| UUID (hex, as bytes in the stream) | `enum_name` | Payload rendering |
|---|---|---|
| `DC45E9BD-E6D9-48B7-962C-D820D923EEEF` | `"x264 options"` | ASCII string (`svalue`), derived: parsed `options:` key=value list as children `x264_option[name]` |
| `2CA2DE09-B517-47DB-BB55-A4FE7FC2FC4E` | `"x265 options"` | ASCII string; children per option |
| `A68D1A59-6338-4A2E-82A7-0EC29D1B6E9F` (Netflix/… not standard) | — | not in registry; shown as hex |
| any other | `"user data (UUID %s)"` | hex bytes, plus ASCII rendering when all bytes are printable |

Other markers OVT MUST recognise (non-SEI): HEVC NAL type 62 with `nuh_layer_id == 0` → `"Dolby Vision RPU"`, `has_dolby_vision = 1`; HEVC NAL type 63 → `"Dolby Vision EL"`; H.264 NAL type 28 containing an RPU in DV profile streams (detected only when the container signals `dvcC`); AV1 metadata type 4 with the Dolby row above. SCTE-35/SCTE-128: `user_data_type_code 0x03` under GA94 covers CEA-708; `user_data_type_code 0x04` (SCTE-128 additional data) is opaque.

---

## 7.4 Module `index` — building the `StreamIndex`

### 7.4.1 Inputs and outputs

Input: an `ovt_Stream` (one video track or elementary stream) giving a sequence of `(file_offset, unit_bytes, prefix_span, sample_index, pts, dts, timebase)` from the container module's unit map (ISOBMFF samples split with 7.3.3; IVF frames split into OBUs; Annex B / OBU scans). Output: the `StreamIndex` FlatBuffer of §0.5.3. The build is a **single forward pass** over units; no unit is read twice except PPS re-parse (7.3.8).

### 7.4.2 Algorithm (step by step)

```
build_index(stream, job):
  S0  state = { codec, ActiveSets, PocState, DpbModel, frames = [], units = [], open_frame = null, pending_nonvcl = [], decode_index = 0, display_counter = 0 }
  S1  for each unit u (in stream order), every 256 units or 50 ms: job.check_cancel() → throw Cancelled; job.progress(bytes_done / total_bytes)
  S2    rbsp = (H.26x) rbsp_from_nal(u.bytes) : u.bytes;   br = BitReader(rbsp);   sb = SyntaxBuilder(br)
        sb.set_recording(record_full_tree(u))                   // 7.4.5: false for VCL units beyond the lazy threshold
  S3    header = parse_nal_or_obu_header(br, sb)               // 7.3.4 #1 / 7.3.5 #1 / 7.3.6 #1
        unit = Unit{ index = units.size(), file_span = u.span, prefix_span, type, type_name, layer_id, temporal_id, epb_positions, frame = {-1,-1}, sample_index }
  S4    try: dispatch by type:
          parameter set   → parse; ActiveSets.store(id, parsed, unit.index); parameter_sets.push(unit); pending_nonvcl.push(unit)
          SEI / metadata  → parse (needs ActiveSets for HRD lengths); sei.push(unit); pending_nonvcl.push(unit); remember recovery_point / decoded_picture_hash / HDR SEIs for the frame they precede (prefix) or follow (suffix)
          AUD / TD        → pending_nonvcl.push(unit); (AV1) SeenFrameHeader = 0
          VCL / frame hdr / tile group → S5
          other           → opaque; pending_nonvcl.push(unit)
        catch ParseError e: unit.header = sb.finish() (partial); unit.header.diagnostics += Diagnostic{Error, e.code, e.message, {e.bit_offset,0}}; index.diagnostics += same with "unit %d"; continue with next unit
  S5    is_new_frame = frame_boundary(state, parsed header)     // 7.3.9
        if is_new_frame:
           if open_frame: finish_frame(open_frame)             // S7
           open_frame = FrameInfo{ id = {decode_index++, -1}, units = pending_nonvcl ∪ {unit}, structure, temporal_id, layer_id, ... }
           for v in pending_nonvcl: units[v].frame = open_frame.id  (AUD/SEI/PS immediately preceding)
           pending_nonvcl.clear()
           classify(open_frame, header)                        // S6
        else: open_frame.units.push(unit)
        unit.frame = open_frame.id
        (H.26x) open_frame.slice_count++; majority vote for FrameType (I < P < B: a frame with any B slice is B, else any P slice → P, else I; §7.4.3 slice_type)
        (AV1 tile group) if tg_end == NumTiles-1: reference_update(open_frame); finish_frame(open_frame); open_frame = null
  S6    classify(frame, h):
          H.264: type from slice_type; is_rap = IdrPicFlag; structure = field_pic_flag ? (bottom ? BottomField : TopField) : (MbaffFrameFlag ? Mbaff : Frame)
                 poc = 7.3.10.1; qp_base = SliceQPY; coded_size = {0,0,PicWidthInSamplesL, PicHeightInMbs*16} (= FrameHeightInMbs*16 for frames, half that for a field picture — chapter 6 §6.2.4); crop; chroma_format; bit depths
                 recovery RAP: if a recovery_point SEI with recovery_frame_cnt == 0 preceded this frame and (slice is I or the SEI's exact_match_flag): frame.is_rap = true and derived "recovery point"
          H.265: type from slice_type; is_rap = nal_unit_type in 16..23 (IRAP); shown = pic_output_flag && !(RASL after CRA-with-NoRaslOutputFlag in analysis path); poc = 7.3.10.2; qp_base = SliceQpY; coded_size = {0,0,pic_width_in_luma_samples,pic_height}; crop = conformance window
          AV1:   type: KEY→Key, INTER→Inter, INTRA_ONLY→IntraOnly, SWITCH→Switch; shown = show_frame; show_existing: see 7.3.9; is_rap = (KEY && show_frame) || (show_existing_frame && RefFrameType[slot] == KEY);
                 poc = OrderHint; qp_base = base_q_idx; coded_size = {0,0,FrameWidth,FrameHeight}; crop = {0,0,RenderWidth,RenderHeight}
          all:   pts/dts/timebase from the container (the first unit's sample); temporal_id; layer_id
  S7    finish_frame(f):
          f.size_bytes = Σ units[i].file_span.length + prefix_span.length (prefix bytes included: they are the AU's bytes in the file)
          declared_refs, dpb_after = DpbModel.step(f, headers)   // 7.4.4
          if f.shown: f.id.display_index = -1 (assigned in S9)
          frames.push(f)
  S8  after the last unit: if open_frame: finish_frame(open_frame) (AV1: a frame without its last tile group → Diagnostic Error SYN-AV1-0101 "truncated frame")
  S9  display order:
          H.264/H.265: within each coded video sequence (IDR..IDR / IRAP-with-NoRaslOutputFlag..), sort frames by poc (stable; fields of a pair share a slot); concatenate sequences in decode order. display_order[k] = decode_index; frames[decode_index].id.display_index = k for shown frames (H.264: all frames are output; H.265: pic_output_flag == 1 and not a dropped RASL)
          AV1: display_order = decode_index of each frame with shown == true (show_frame == 1 or show_existing_frame == 1), in decode order; a hidden frame keeps display_index = -1 even when a later show_existing_frame shows it (the show_existing entry gets the display index, 7.4.3)
  S10 fill parameter_sets, sei, source_hash (the single definition is chapter 6 §6.2.8: SHA-256 over the first 1 MiB of the file ∥ 8-byte LE file size ∥ 8-byte LE mtime_ns; no other chapter defines it), schema_version = 1, codec
  S11 random access map (private to index, exported via ovt_index_rap_before): rap_before[d] = max{ r ≤ d : frames[r].is_rap } (−1 if none; H.265 CRA: RASL frames with poc < CRA.poc that depend on pre-CRA pictures are marked "skip when starting at r" in a side bitmap)
```

Determinism (§0.7.5): the pass is single-threaded and the output contains no timestamps or pointers, so the FlatBuffer is byte-identical for identical input. Parsing *within* a unit is sequential; parallelism across units is **not** used in phase 1 (activation state is sequential); a future optimisation may pre-scan units in parallel and parse VCL headers in parallel after parameter sets — not required to meet the budget (7.4.6).

### 7.4.3 What is stored per frame for AV1 hidden frames and `show_existing_frame`

| Case | `FrameInfo` fields |
|---|---|
| Hidden frame (`show_frame == 0`, typically ALTREF) | `shown = false`, `display_index = −1` (kept at −1 even when later shown via `show_existing_frame`), `show_existing_of = −1`, `is_rap = false` (also for a hidden KEY frame; the RAP is the `show_existing_frame` entry), `type` from `frame_type`, `units` = its frame header + tile groups, `size_bytes` = those bytes, `declared_refs` from `ref_frame_idx`, `dpb_after` = the 8 slot decode indices after `refresh_frame_flags`, `derived "showable_frame = 1"`. |
| `show_existing_frame == 1` of a non-KEY slot | `shown = true`, `display_index` assigned, `show_existing_of = slot_decode_index[frame_to_show_map_idx]`, `type = RefFrameType[slot]`, `is_rap = false`, `units` = the frame header OBU (+ preceding TD/metadata), `size_bytes` = those bytes only (the GUI sums the hidden frame's bytes into the display slot when the user asks for "bytes per shown frame"), `declared_refs = []`, `dpb_after` = unchanged slots, `poc = RefOrderHint[slot]`, `qp_base = frames[show_existing_of].qp_base`. |
| `show_existing_frame == 1` of a KEY slot (forward key frame) | As above plus `is_rap = true` and `dpb_after` = all 8 slots set to `show_existing_of` (AV1 §7.21 loading + refresh all). The analysis path treats this frame as a RAP whose decode starts at `frames[show_existing_of]`. |
| Frame not in the selected operating point | `shown = false`, Diagnostic Info `SYN-AV1-0002`. |

### 7.4.4 Declared references and DPB simulation

`DpbModel.step(frame, headers)` returns `declared_refs` (decode indices of frames the frame's headers reference) and `dpb_after` (decode indices of reference frames / slots after decoding the frame). Non-reference pictures that only wait for output are **not** in `dpb_after` (it models reference marking, which is what the GOP graph needs). All three models operate on frame stores keyed by `decode_index`; H.264 fields of a pair share a frame store (`FrameStore{decode_index_first_field, decode_index_second_field}`), and `declared_refs` lists the decode index of the first field.

**H.264 (§8.2.4 ref list construction + §8.2.5 marking).**
```
state: stores = list of FrameStore{ decode_index(es), FrameNum, FrameNumWrap, LongTermFrameIdx, is_long_term, is_ref (per field for field pairs), TopFieldOrderCnt, BottomFieldOrderCnt }
       MaxLongTermFrameIdx = "no long-term frame indices"
step(frame f with first slice header h, sps, pps):
  if h.IdrPicFlag: for s in stores: s.is_ref = false; stores.clear(); MaxLongTermFrameIdx = h.long_term_reference_flag ? 0 : none
  // 8.2.4.1 picture numbers
  for s in stores (reference): FrameNumWrap = (s.FrameNum > h.frame_num) ? s.FrameNum - sps.MaxFrameNum : s.FrameNum
      frames:  PicNum = FrameNumWrap; LongTermPicNum = LongTermFrameIdx
      fields:  PicNum = 2*FrameNumWrap + 1 (same parity) / 2*FrameNumWrap (opposite parity); LongTermPicNum likewise
  // gaps_in_frame_num (§8.2.5.2): if h.frame_num != prevRefFrameNum && h.frame_num != (prevRefFrameNum+1) % MaxFrameNum and not IDR:
  //     if sps.gaps_in_frame_num_value_allowed_flag: insert "non-existing" frames (decode_index = -1, FrameNum = each missing value) applying the sliding window for each; Diagnostic Info SYN-H264-0102
  //     else Diagnostic Warning SYN-H264-0103 "frame_num gap"
  // 8.2.4.2 initial lists (per slice; OVT computes for the FIRST slice only and records a Diagnostic Info if later slices differ in num_ref_idx or modification ops)
  if P or SP slice:
     frames: RefPicList0 = short-term refs sorted by PicNum descending, then long-term sorted by LongTermPicNum ascending
     fields: 8.2.4.2.5 alternation: refFrameList0ShortTerm sorted by FrameNumWrap descending (including frames where only one field is a reference), refFrameListLongTerm by LongTermFrameIdx ascending; then alternate same-parity / opposite-parity fields starting with the same parity as the current field, skipping fields that are not references
  if B slice:
     frames: refs with POC < cur POC sorted by POC descending, then POC > cur sorted ascending, then long-term ascending → L0; L1 = POC > cur ascending, then POC < cur descending, then long-term
             if L1.size() > 1 and L1 == L0: swap L1[0], L1[1]
     fields: 8.2.4.2.4 with refFrameList0ShortTerm ordered by POC (entry-wise with field POC rules) then 8.2.4.2.5 alternation
  truncate each list to num_ref_idx_lX_active_minus1 + 1 (lists shorter than that: entries "no reference picture" are left out and Diagnostic Warning SYN-H264-0104)
  // 8.2.4.3 modification: for each op in ref_pic_list_modification (list X), refIdxLX = 0, picNumLXPred = CurrPicNum:
     idc 0: picNumLXNoWrap = picNumLXPred - (abs_diff_pic_num_minus1+1) (+MaxPicNum if < 0); idc 1: + (…) (−MaxPicNum if ≥ MaxPicNum); picNumLXPred = picNumLXNoWrap
            picNumLX = picNumLXNoWrap > CurrPicNum ? picNumLXNoWrap - MaxPicNum : picNumLXNoWrap
            insert the short-term picture with PicNum == picNumLX at refIdxLX++, removing its later duplicate (eq. 8-37)
     idc 2: insert long-term picture with LongTermPicNum == long_term_pic_num at refIdxLX++ (eq. 8-38)
     missing picture → Diagnostic Error SYN-H264-0105 "ref_pic_list_modification references a missing picture"; keep going
  declared_refs = unique decode indices of RefPicList0 ∪ RefPicList1 (order: L0 then L1); RefSlot names "L0[i]" / "L1[i]" are kept in the private per-frame struct for the plugin host (7.5) and the GOP graph
  // 8.2.5 marking, only if nal_ref_idc != 0:
  if IDR: (handled above) store current as short-term (or long-term with LongTermFrameIdx 0 if long_term_reference_flag)
  else if !adaptive_ref_pic_marking_mode_flag:   // 8.2.5.3 sliding window
     numShortTerm + numLongTerm == Max(sps.max_num_ref_frames, 1) → mark the short-term frame store with the smallest FrameNumWrap as unused (a second field of a pair whose first field is already a reference does NOT trigger the window, §8.2.5.3)
  else for each MMCO op in order (§8.2.5.4):
     1: picNumX = CurrPicNum - (difference_of_pic_nums_minus1+1); mark the short-term picture with PicNum == picNumX unused
     2: mark long-term picture with LongTermPicNum == long_term_pic_num unused
     3: picNumX as op 1; if another picture already has LongTermFrameIdx == long_term_frame_idx (and is not the other field of this frame) mark it unused; mark picNumX picture long-term with that idx
     4: MaxLongTermFrameIdx = max_long_term_frame_idx_plus1 - 1 (or "none" if 0); all long-term with LongTermFrameIdx > MaxLongTermFrameIdx unused
     5: all reference pictures unused; MaxLongTermFrameIdx = none; set PocState.prevRefHadMmco5 (7.3.10.1); the current picture's frame_num is treated as 0 afterwards
     6: current picture → long-term with long_term_frame_idx (unmark another picture with the same idx first)
  store current picture: is_ref = true, FrameNum = (mmco5 ? 0 : h.frame_num), long-term per op 6 else short-term
  if number of reference frame stores > Max(max_num_ref_frames,1): Diagnostic Error SYN-H264-0106 "DPB overflow" and drop the oldest short-term
  dpb_after = decode indices of all stores with is_ref (first field index), ascending by decode index
```

**H.265 (§8.3.2 RPS + §8.3.4 RefPicList).**
```
state: dpb = list of { decode_index, PicOrderCntVal, is_long_term, is_ref }
step(frame f, slice header h, sps, pps):
  if IRAP && NoRaslOutputFlag: all pictures in dpb marked unused (removed from the reference set)
  if IDR: RPS empty; dpb_after = [current]; declared_refs = []; return
  // 8.3.2 derive the five lists from CurrRpsIdx set (st_ref_pic_set) and long-term entries:
  for i < NumNegativePics: poc = PicOrderCntVal + DeltaPocS0[i]; UsedByCurrPicS0[i] ? PocStCurrBefore.push(poc) : PocStFoll.push(poc)
  for i < NumPositivePics: poc = PicOrderCntVal + DeltaPocS1[i]; UsedByCurrPicS1[i] ? PocStCurrAfter.push(poc) : PocStFoll.push(poc)
  for i < num_long_term_sps + num_long_term_pics: pocLt = PocLsbLt[i]; if delta_poc_msb_present_flag[i]: pocLt += PicOrderCntVal - DeltaPocMsbCycleLt[i]*MaxPicOrderCntLsb - (PicOrderCntVal & (MaxPicOrderCntLsb-1))
      UsedByCurrPicLt[i] ? (PocLtCurr.push(pocLt), CurrDeltaPocMsbPresentFlag.push(flag)) : (PocLtFoll.push(pocLt), FollDeltaPocMsbPresentFlag.push(flag))
  // picture lookup (eq. 8-5..8-7): long-term entries match on full POC when msb present, else on (POC & (MaxLsb-1)); short-term match on full POC among short-term reference pictures
  RefPicSetLtCurr[i] = find(...) or "no reference picture" (Diagnostic Error SYN-H265-0102 "missing reference picture POC %d" — for RASL pictures after a CRA start this is expected and downgraded to Info)
  RefPicSetLtFoll, RefPicSetStCurrBefore, RefPicSetStCurrAfter, RefPicSetStFoll likewise
  mark: pictures in Lt sets → long-term; all pictures in dpb not in any of the five sets → unused
  // 8.3.4 list construction (P and B):
  NumRpsCurrTempList0 = Max(num_ref_idx_l0_active_minus1 + 1, NumPicTotalCurr)
  RefPicListTemp0 = repeat until size == NumRpsCurrTempList0: StCurrBefore…, StCurrAfter…, LtCurr…, (pps_curr_pic_ref_enabled_flag ? current picture : nothing)
  RefPicList0[rIdx] = ref_pic_list_modification_flag_l0 ? RefPicListTemp0[list_entry_l0[rIdx]] : RefPicListTemp0[rIdx]   for rIdx ≤ num_ref_idx_l0_active_minus1
  (B) RefPicListTemp1 = StCurrAfter…, StCurrBefore…, LtCurr…, (current if pps_curr_pic_ref); RefPicList1 via list_entry_l1
  declared_refs = unique decode indices of RefPicList0 ∪ RefPicList1 (the "Foll" pictures are kept in the DPB but not declared)
  store current: is_ref = true (every picture is potentially a reference until a later RPS drops it; sub-layer non-reference pictures (TRAIL_N etc.) are still kept — they are excluded by later RPSs)
  dpb_after = decode indices of pictures in the five RPS sets ∪ {current}, ascending
```

**AV1 (slot model, §7.20/§7.21).**
```
state: DecoderState of 7.3.10.4 (slot_decode_index[8])
step(frame f):
  if show_existing_frame: loading process (7.3.10.4); declared_refs = []; dpb_after = slot_decode_index[0..7] (after a KEY-slot refresh all equal show_existing_of); return
  if FrameIsIntra: declared_refs = []
  else: declared_refs = unique(slot_decode_index[ref_frame_idx[i]] for i in 0..6, order LAST, LAST2, LAST3, GOLDEN, BWDREF, ALTREF2, ALTREF), dropping -1 entries with Diagnostic Error SYN-AV1-0102 "ref_frame_idx points to an empty slot"
        RefSlot names "LAST".."ALTREF" with the slot number, sign bias (RefFrameSignBias) and OrderHints[refFrame] are kept for the plugin host and GOP graph
  reference update process with refresh_frame_flags (7.3.10.4)
  dpb_after = slot_decode_index[0..7] (8 entries, -1 for empty slots; duplicates allowed — the GUI shows the slot table)
```

### 7.4.5 Lazy header trees and memory

A full `SyntaxNode` tree for every slice header of a 1-hour stream would need > 1 GB. Rule: `record_full_tree(u)` is **true** for all non-VCL units (parameter sets, SEI, AUD, metadata, sequence headers, temporal delimiters, padding) and for VCL units of the first 10 000 frames; beyond that, VCL units are parsed with `set_recording(false)` (values only) and `Unit.header` is null. The C API `ovt_stream_unit_syntax(ovt_Stream*, uint32_t unit_index, ovt_Buffer* out /* SyntaxNode */)` (this chapter adds it to §0.6.1; chapter 12 §12.3 carries the declaration) re-parses one unit on demand using the activation state recorded for that unit: `index` stores per frame the `Unit.index` of the active SPS/PPS/VPS (or sequence header) and the AV1 `DecoderState` snapshot at every KEY frame plus the per-frame `ref_frame_idx`/`OrderHint` scalars, so a single unit can be re-parsed without replaying the stream. Memory target for the index of a 1-hour 1080p stream (≈ 108 000 frames, ≈ 250 000 units): ≤ 200 MB in RAM, ≤ 40 MB zstd-compressed on disk.

### 7.4.6 Complexity, performance and incrementality

- Complexity: O(total units) time, O(units + frames) memory; each unit is parsed once (plus O(1) PPS re-parses). DPB simulation is O(DPB size) per frame (≤ 16 H.264, ≤ 16 H.265, 8 AV1).
- Budget (§0.7.4): header-only index of a 1-hour 1080p H.264 stream ≤ 2 s on an M1 MacBook Air. Derived per-unit budget: ≈ 8 µs per unit including I/O. Rules to meet it: `BitReader` 64-bit cache; `SyntaxBuilder` nodes stored in an arena (`std::pmr::monotonic_buffer_resource`) and not allocated at all when recording is off; `rbsp_from_nal` only on the header bytes needed (for VCL units, strip EPBs **lazily**: process the first 2 KiB of the NAL; if the slice header parse runs past the stripped prefix, strip the next 2 KiB and continue — slice headers rarely exceed 100 bytes); `memchr`-based start-code scan; no per-unit heap allocation in the hot path other than the `Unit` record. Performance test `bench_index_h264_1h` (7.8 WI-1.125) fails CI if > 2.5 s (25 % margin for CI noise; the hard target is measured locally).
- Incrementality: `build_index` is a coroutine-style loop that can `job.check_cancel()` every 256 units or 50 ms (§0.7.2). On cancellation the partial index is discarded and `OVT_E_CANCELLED` is returned, **unless** the caller passed the `OVT_INDEX_ALLOW_PARTIAL` flag (new C API flag on `ovt_stream_build_index`'s job options), in which case the partial `StreamIndex` with `diagnostics += Info "index truncated at unit %d"` is returned so the GUI can show the first part of a very large file while indexing continues in a second job that resumes from the saved `state` (the state object is serialisable: ActiveSets, PocState, DpbModel, pending units). The GUI chapter decides when to use partial indices; the timeline MUST show "indexing… 37 %" while the job runs.
- Growing files (live capture): not supported in phase 1; `source_hash` includes the size, so a changed file re-indexes.

### 7.4.7 Diagnostics codes introduced by `index`

`SYN-H264-0102..0106`, `SYN-H265-0101..0102`, `SYN-AV1-0101..0102` as used above; `SYN-GEN-0101 "no video units found"`, `SYN-GEN-0102 "no parameter sets before first VCL unit"` (H.26x; frames before the first SPS/PPS are indexed with `type = Unknown` and skipped by the analysis path), `SYN-GEN-0103 "index truncated at unit %d"`, `SYN-GEN-0104 "no timing information; frame-count time base"` (chapter 6 §6.2.6). Index-level diagnostics use the `SYN` family with qualifier `GEN`/`H264`/`H265`/`AV1` and numbers 0101–0199 (the former `IDX-*` codes); the full registry is §7.9.

### 7.4.8 HRD buffer model (phase 4)

Consumed by `ovt hrd` (chapter 12 §12.2.21) and the GUI Buffer panel (chapter 4 FR-GUI-032); requirement FR-DEC-013. Implemented by WI-4.112 in `core/src/index/hrd_model.cpp` as a post-pass over a finished `StreamIndex` (it reads only header trees and unit sizes; no pixel decoding).

**Inputs.**

| Codec | Required syntax | Source (lazy header trees, §7.4.5) |
|---|---|---|
| H.264 | `hrd_parameters()` of the active SPS VUI (E.1.2; NAL HRD or VCL HRD per `--type`), `buffering_period` SEI (D.1.2: `initial_cpb_removal_delay[SchedSelIdx]`, `initial_cpb_removal_delay_offset[SchedSelIdx]`), `pic_timing` SEI (D.1.3: `cpb_removal_delay`, `dpb_output_delay`), per-access-unit byte counts from `StreamIndex.units[].file_span` (`Unit.file_span`) | §7.3.4 rows 4, 5, 8, 9 |
| H.265 | `hrd_parameters(1, sps_max_sub_layers_minus1)` of the active SPS VUI or of the VPS (E.2.2/E.2.3; `sub_pic_hrd_params_present_flag` → access-unit level only, decoding-unit level is **not** modelled, `DEC-HRD-0003` Info), `buffering_period` SEI (D.2.2: `nal_initial_cpb_removal_delay[i]`, `nal_initial_cpb_removal_offset[i]`, `vcl_*`, `irap_cpb_params_present_flag`, `cpb_delay_offset`), `pic_timing` SEI (D.2.3: `au_cpb_removal_delay_minus1`, `pic_dpb_output_delay`) | §7.3.5 rows 6, 7, 14 |
| AV1 | — | `ovt hrd` returns `OVT_E_UNSUPPORTED` with `DEC-HRD-0001` ("AV1 decoder model (AV1 Annex E) not implemented; `decoder_model_info` is parsed and displayed only"). |

A stream without `hrd_parameters` (`nal_hrd_parameters_present_flag == 0 && vcl_hrd_parameters_present_flag == 0`) → `OVT_E_UNSUPPORTED` with `SYN-HRD-0001` (chapter 12 §12.2.21: exit 4). A `pic_timing` SEI without a preceding `buffering_period` SEI in the same coded video sequence → `DEC-HRD-0002` (Error) and the trace starts at the next buffering period.

**Variables** (H.264 Annex C / H.265 Annex C; identical structure, HEVC names in brackets): `BitRate[SchedSelIdx] = (bit_rate_value_minus1 + 1) × 2^(6 + bit_rate_scale)`, `CpbSize[SchedSelIdx] = (cpb_size_value_minus1 + 1) × 2^(4 + cpb_size_scale)` (H.264 §E.2.2 / H.265 §E.3.3 semantics of `bit_rate_value_minus1` and `cpb_size_value_minus1`); `cbr_flag[SchedSelIdx]`; `tc = num_units_in_tick × 90000 / time_scale` — OVT stores all times in **90 kHz ticks** (`*_90k`) using exact rational arithmetic (`muldiv64`) so that traces are bit-identical across platforms; `initial_cpb_removal_delay[SchedSelIdx]` and `initial_cpb_removal_delay_offset[SchedSelIdx]` are already in 90 kHz units; `low_delay_hrd_flag`; `b(n)` = size in bits of access unit `n` counted over the units that Annex C includes (type I: VCL NAL units and filler data NAL units only; type II: all NAL units including SEI/parameter sets and — for byte stream input — start codes and `zero_byte`s, H.264 §C.1 / H.265 §C.1 conformance points).

**Equations.** Clause references are to H.264 (08/2021) §C.1.1 "Timing of bitstream arrival" and §C.1.2 "Timing of coded picture removal" and to H.265 (09/2023) §C.2.2 / §C.2.3 (same structure, with `AuCpbRemovalDelayVal = au_cpb_removal_delay_minus1 + 1` in place of `cpb_removal_delay` and `AuNominalRemovalTime` in place of `t_r,n`). Equation **numbers** differ between editions of both standards and are therefore not used here; an implementer checks the formulas against the clause text. Notation: `n_b` = the first access unit of the **current** buffering period (the period that contains `n`); `n_b,prev` = the first access unit of the **previous** buffering period; the `initial_cpb_removal_delay[SchedSelIdx]` / `initial_cpb_removal_delay_offset[SchedSelIdx]` values are those of the buffering period SEI associated with `n_b` (i.e. with the current period).

1. Arrival of access unit 0 (the AU that initialises the HRD): `t_ai(0) = 0` (H.264 §C.1.1; H.265 §C.2.2).
2. Initial arrival time of access unit `n > 0`: `t_ai(n) = t_af(n−1)` when `cbr_flag == 1`; otherwise `t_ai(n) = max(t_af(n−1), t_ai,earliest(n))`, where `t_ai,earliest(n) = t_r,n(n) − (initial_cpb_removal_delay + initial_cpb_removal_delay_offset) / 90000` when `n` is **not** the first access unit of a subsequent buffering period, and `t_ai,earliest(n) = t_r,n(n) − initial_cpb_removal_delay / 90000` when `n` **is** the first access unit of a subsequent buffering period (H.264 §C.1.1; H.265 §C.2.2 with `nal_/vcl_initial_cpb_removal_delay`/`_offset`).
3. Final arrival time: `t_af(n) = t_ai(n) + b(n) / BitRate` (H.264 §C.1.1).
4. Nominal removal time (H.264 §C.1.2; H.265 §C.2.3): for the access unit that initialises the HRD, `t_r,n(0) = initial_cpb_removal_delay / 90000`; for the first access unit `n` of a **subsequent** buffering period, `t_r,n(n) = t_r,n(n_b,prev) + tc × cpb_removal_delay(n)` — the recursion is on the first access unit of the **previous** buffering period, because `cpb_removal_delay` is signalled relative to the access unit that carried the previous buffering period SEI (not relative to `n − 1`); for every other access unit, `t_r,n(n) = t_r,n(n_b) + tc × cpb_removal_delay(n)`. For H.265 the `concatenation_flag` / `cpb_delay_offset` cases of H.265 §C.2.3 are implemented as the clause states (they modify only this first-AU-of-a-subsequent-period formula).
5. Removal time: when `low_delay_hrd_flag == 0` or `t_r,n(n) ≥ t_af(n)`: `t_r(n) = t_r,n(n)`; otherwise (low-delay, big picture) `t_r(n) = t_r,n(n) + tc × Ceil((t_af(n) − t_r,n(n)) / tc)` (H.264 §C.1.2).
6. Fullness after the arrival of `n` and before its removal: `cpb_fullness_bits(n) = Σ_{m ≤ n, t_r(m) > t_af(n)} b(m)` (bits present at `t_af(n)`); **overflow** when `cpb_fullness_bits(n) > CpbSize` (violates H.264 §C.3 / H.265 §C.4 "the CPB never overflows"); **underflow** when `t_r,n(n) < t_af(n)` with `low_delay_hrd_flag == 0` (same clauses: "the removal time shall not precede the final arrival time").
7. Type-I versus type-II conformance (`--type` in chapter 12 is `nal` = type II with NAL HRD parameters, `vcl` = type I with VCL HRD parameters): the choice selects both the `hrd_parameters` set used for `BitRate`/`CpbSize` and the units counted in `b(n)`.
8. `initial_cpb_removal_delay` override (`--initial-removal-delay`): replaces the value of **every** buffering period SEI in the formulas of items 2 and 4 that use `initial_cpb_removal_delay` alone; the offset is kept.

**Output.** One `HrdPoint` per access unit in decode order, in the `HrdTrace` table of chapter 06 §6.3 (`init_arrival_time_90k = t_ai × 90000`, `final_arrival_time_90k = t_af × 90000`, `removal_time_90k = t_r × 90000`, all rounded to nearest with `muldiv64`), plus `bit_rate_bps`, `cpb_size_bits`, `sched_sel_idx`, `nal_hrd`, `cbr`, `low_delay_hrd`, `conformance_type`, counts, and `Diagnostic`s `DEC-HRD-0001..0003` and `DEC-HRD-0010` (Error, "CPB overflow at access unit n by k bits") / `DEC-HRD-0011` (Error, "CPB underflow at access unit n: removal precedes final arrival by t ticks"). Determinism: integer 90 kHz arithmetic only; no floating point in the trace.

**Oracle.** No open-source decoder prints a CPB trace. Verification uses encoder-side conformance: x264 `--nal-hrd cbr --vbv-bufsize B --vbv-maxrate R` and x265 `--hrd --vbv-bufsize B --vbv-maxrate R` encodes MUST produce traces with zero overflow and zero underflow and, for x264 CBR, `t_ai(n) == t_af(n−1)` for all `n`; `ffmpeg -bsf:v trace_headers` is the oracle for the parsed `hrd_parameters`/SEI values that feed the model; a hand-computed 5-frame trace (`tests/golden/hrd/hand_trace_cbr.json`, derived in a spreadsheet from the equations above with the stream's exact unit sizes) pins the arithmetic.

---

## 7.5 Module `analysis` — plugin host

### 7.5.1 Plugin discovery and loading

| Item | Rule |
|---|---|
| File name | `libovt_plugin_<id>.dylib` (macOS), `libovt_plugin_<id>.so` (Linux, Android). `<id>` ∈ {`aom`, `de265`, `ffmpeg`, `hm`} for the plugins of this chapter; third-party ids are lowercase `[a-z0-9_]{1,32}`. The `lib` prefix is mandatory on Android (the APK packager only extracts `lib*.so`). |
| Search order | 1. `OVT_PLUGIN_PATH` (colon-separated directories, highest priority; a trailing `:` appends the defaults). 2. Executable-relative: macOS app bundle `<App>.app/Contents/PlugIns/ovt/`, then `<exe_dir>/../lib/ovt/plugins/`; Linux `<exe_dir>/../lib/ovt/plugins/`, `/usr/local/lib/ovt/plugins/`, `/usr/lib/ovt/plugins/`, `/usr/lib/<multiarch>/ovt/plugins/`, `$XDG_DATA_HOME/ovt/plugins/`; Android: `ApplicationInfo.nativeLibraryDir` (passed from Kotlin via `ovt_set_plugin_dir()`), then `<filesDir>/plugins/` for user-installed decoder plugins (ADR-015). The first directory that contains a given `<id>` wins; a later duplicate is logged at level 1 and ignored. |
| Loading | `dlopen(path, RTLD_NOW | RTLD_LOCAL)` (Qt builds MAY use `QLibrary` in the GUI, but `libovt-core` uses `dlopen` directly so the CLI has no Qt dependency). Resolve exactly one symbol: `ovt_analysis_decoder_v1`. Call it; check `api->abi_version == OVT_ANALYSIS_ABI_VERSION` (1); mismatch → log level 3 `"plugin %s: ABI %u, expected %u"` and `dlclose`. Then `caps = api->caps()`; validate `caps->codec` ∈ `ovt.fb.Codec`, `caps->id` equals `<id>` from the file name (else reject), `caps->version`, `caps->license` non-null. |
| Registry | `ovt::analysis::PluginRegistry` (process singleton, built lazily on first `ovt_analysis_open`, refreshable with `ovt_analysis_rescan_plugins()` — declared in chapter 12 §12.3 (this chapter requests the declaration)): `std::vector<LoadedPlugin{path, handle, api, caps}>`; `default_for(codec)`: AV1 → `aom`; H265 → `de265`, fallback `hm`; H264 → `ffmpeg`; a plugin listed in the user settings (`analysis.default_plugin.<codec>`) overrides. Plugins are never unloaded while an `ovt_Analysis` using them exists (reference count). |
| Sandboxing | None in phase 1 (ADR-013). Plugin entry points are C functions and MUST NOT let a C++ exception escape (`noexcept` at the ABI: a C++ plugin catches internally and returns `OVT_E_PLUGIN`). The host additionally wraps every call in `try`/`catch(...)` as a **best-effort** guard that turns an escaped exception into `OVT_E_PLUGIN` and marks the plugin broken; unwinding through C frames is undefined behaviour, so plugins may not rely on it. A crash kills the process — the Risks chapter tracks the out-of-process mode. |

The ABI structs (`ovt_AnalysisCaps`, `ovt_AnalysisDecoderApi`, `OVT_ANALYSIS_ABI_VERSION`) are defined once, in chapter 0 §0.6.2; this chapter does not restate them. WI-1.100 writes `core/include/ovt/abi/analysis_decoder_v1.h` from that block verbatim: both structs begin with `uint32_t struct_size` (D4), and the `codec_private` buffer passed to `create()` is valid only during that call (the plugin copies what it needs).

### 7.5.2 `ovt_Analysis` object

```cpp
namespace ovt::analysis {
class Session {                         // one per ovt_Analysis handle; NOT thread-safe; used from one job thread at a time
public:
  Session(StreamRef stream, const StreamIndex& index, LoadedPlugin plugin, CacheRef cache);
  ovt_status frame(int decode_index, uint32_t plane_stage_mask, jobs::Job* job, FlatBufferHandle& out);   // 7.5.3
  ovt_status plane(std::string_view blob_id, PlaneView& out);
private:
  ovt_AnalysisDecoder* dec_ = nullptr;   // current decoder instance, or null
  int last_decoded_ = -1;                // decode_index of the last frame pulled from dec_
  int current_rap_ = -1;                 // RAP the current decoder instance started from
  std::deque<FlatBufferHandle> pending_; // frames pulled but not yet consumed (plugins may emit several per push)
};
}
```

### 7.5.3 GOP-local decode algorithm (`Session::frame`)

```
frame(T, stage_mask, job):
  A0 if T < 0 || T >= index.frames.size(): return OVT_E_INVALID_ARG
     f = index.frames[T]
     if f.show_existing_of >= 0: result = frame(f.show_existing_of, stage_mask, job); patch result.frame = f (shown=true, display_index, show_existing_of) ; return   // AV1 show_existing
     if f.type == Unknown (no parameter sets): return OVT_E_UNSUPPORTED + Diagnostic
  A1 cache lookup: key = (index.source_hash, plugin.caps.id, plugin.caps.version, T). hit with cached_stage_mask ⊇ stage_mask → return cached buffer (planes for stage bits set are loaded from the cache blob store; the Cache chapter defines the blob format)
  A2 R = rap_before(T) (7.4.2 S11). If R < 0: return OVT_E_UNSUPPORTED, Diagnostic "no random access point before frame %d"
     AV1: if frames[R] is a show_existing KEY, R = frames[R].show_existing_of
  A3 decide whether the warm decoder can continue:
        continue_ok = dec_ != null && current_rap_ == R && last_decoded_ < T && last_decoded_ >= R
     if !continue_ok: destroy dec_; create: api->create(codec_private (avcC/hvcC/av1C bytes from the container, or NULL for elementary streams), threads = min(4, hw_threads) (Android: 2), &dec_); current_rap_ = R; last_decoded_ = R - 1
        feed parameter sets: H.26x: for each id in ActiveSets snapshot at R (the SPS/PPS/VPS units active for frame R, and every parameter set unit between R's first unit and T's last unit in stream order will be pushed anyway in A4) push the active VPS/SPS/PPS units (units[idx]) in that order. AV1: push the sequence header unit active at R.
  A4 for d in last_decoded_+1 .. T:
        every frame: if job && job->cancelled(): return OVT_E_CANCELLED (decoder kept for reuse — state is still consistent because we stop between frames)
        H.265 only: if starting at a CRA (frames[R].nal_unit_type == 21) and frames[d] is RASL associated with R: skip pushing (the decoder would fail its references); if d == T: return OVT_E_UNSUPPORTED "RASL picture after random access cannot be decoded; decode from the previous RAP" (the GUI offers "decode from previous RAP" which calls frame(T) with rap_before(R-1) — implemented by a `force_rap` optional parameter)
        for u in frames[d].units (in order, including the AUD/SEI/PS units attached to the frame): st = api->push(dec_, raw unit bytes (H.26x: NAL without start code / length prefix; AV1: the whole OBU with its size field — for Annex B OBUs without obu_has_size_field the host REWRITES the OBU header to has_size=1 and inserts obu_size), len, d, frames[d].pts)
            if st == OVT_E_CORRUPT: record Diagnostic from the plugin (via pull's FrameAnalysis.diagnostics later or a host Diagnostic "push failed for unit %u"), continue (max 32 consecutive failures → abort with OVT_E_CORRUPT)
            else if st != OVT_OK: destroy dec_; return st
        loop: api->pull(dec_, (d == T) ? stage_mask : 0, &fb); if fb.data == NULL break; pending_.push(fb)
        drain pending_: for each fb: idx = FrameAnalysis.frame.id.decode_index (plugin fills it from the hint); if idx < T: cache.store(idx, fb, planes none) (grids/blocks are cheap and make scrubbing backwards fast); api->release(fb); if idx == T: result = copy(fb) (+ planes referenced by PlaneRef read via api->plane and copied into the Picture store); api->release; last_decoded_ = max(last_decoded_, idx)
  A5 if result not produced after pushing all units of T: api->flush(dec_); pull/drain again (decoders with frame delay: libaom with frame-parallel off has none; libde265 may need a flush for the last picture; FFmpeg h264 needs flush unless `has_b_frames` 0). If still none: destroy dec_; return OVT_E_CORRUPT with Diagnostic "plugin produced no analysis for frame %d"
     after flush the decoder cannot continue → destroy dec_ (next request recreates; the cache keeps the GOP's frames)
  A6 post-process: verify result.frame.id.decode_index == T; fill result.frame from index.frames[T] where the plugin left zeros (pts/dts/size_bytes/units); fill RefSlot.frame from the host's DPB model when the plugin only gave slot names (7.4.4 keeps L0/L1 and AV1 slot → decode_index maps); compute qp_hist if empty; cache.store(T, result, stage_mask)
  A7 return result
```

Plane stage handling: `plane_stage_mask` bits follow `ovt.fb.PlaneStage` (bit = enum value). The host passes the mask only for the target frame; a plugin that cannot produce a requested stage leaves it out of `planes[]` and the GUI shows "n/a" for that stage (the host adds Diagnostic Info `ANA-0001 "stage %s not available from plugin %s"`). Planes are copied into the `Picture` store (LRU of 4 frames, §0.7.3) immediately after `pull`, because the plugin's buffers are only valid until `release()`.

### 7.5.4 Threading, cancellation, errors

- One `Session` is driven by one job at a time; the GUI's analysis controller serialises requests per stream and cancels the pending request when the user moves on (§0.7.2). A second `Session` (second decoder instance) is used for A/B comparison streams.
- Cancellation is checked between frames (A4) and inside long plugin calls only if the plugin supports threads (`supports_threads` says the plugin's internal threads are safe; it does not make the API re-entrant). Worst-case latency to cancel = one frame decode (≤ 250 ms at 1080p for libde265 single thread; the GUI shows a spinner after 100 ms).
- `OVT_E_CORRUPT` from `push` never aborts the session unless repeated (32×). Any other error destroys the decoder instance (the plugin is assumed to be in an undefined state) and is returned.
- The host never calls a plugin from two threads concurrently (§0.6.2 rule); `create`/`destroy` happen on the job thread.
- Memory: the host keeps at most one live decoder per `Session`; `FrameAnalysis` LRU of 64 (grids/blocks), planes LRU of 4 (§0.7.3); everything else goes through the cache module.

### 7.5.5 Cache interaction summary

The analysis host talks to the disk cache (chapter 6 §6.7, `ovt::cache::Cache` of WI-0.091/WI-0.092) only through this adapter (WI-2.152):

```cpp
// core/include/ovt/cxx/analysis/frame_cache.hpp
namespace ovt::analysis {
struct CacheKey {
  std::array<uint8_t, 32> source_hash;   // chapter 6 §6.2.8
  std::string plugin_id, plugin_version;  // from the plugin caps
  uint32_t    decode_index;
};
struct CacheHit {
  std::vector<uint8_t> frame_analysis;    // verified FrameAnalysis buffer, not size-prefixed
  uint32_t             stage_mask;        // PlaneStage bits whose planes are also on disk for this frame
};
class FrameCache {
public:
  explicit FrameCache(ovt::cache::Cache* cache);   // nullptr = caching disabled (--no-cache): lookups miss, stores are no-ops
  std::optional<CacheHit> lookup(const CacheKey&) const;   // applies chapter 6 §6.7.3 I5; any failure is a miss
  std::optional<ovt::model::PlaneView> lookup_plane(const CacheKey&, uint8_t stage, uint8_t plane) const;   // I6
  void store(const CacheKey&, std::span<const uint8_t> frame_analysis, uint32_t stage_mask,
             std::span<const ovt::model::PlaneView> planes = {});   // atomic per file; I/O errors are logged (warn), never thrown
};
}
```

| When | Action |
|---|---|
| Before decoding (A1) | `lookup({source_hash, plugin_id, plugin_version, T})` → `CacheHit` (FlatBuffer + stage mask); on a hit whose `stage_mask` covers the requested stages the request is answered without decoding. Frames R..T−1 are **not** looked up (decoding must restart from R anyway). |
| After each intermediate frame (A4) | `store({…, idx}, fb, stage_mask = 0)` — zstd-compressed FlatBuffer (grids compress 5–20×, Appendix D.8). |
| After the target (A6) | `store({…, T}, fb, stage_mask, planes)` with the plane views of the requested stages (written only when "cache planes" is on, chapter 6 §6.6.3). |
| Invalidation | key includes `plugin_version` (so a plugin upgrade re-analyses) and `schema_version`; `source_hash` changes when the file changes. |

---

## 7.6 Plugin specifications

Common rules for all plugins (§0.6.2): one exported symbol `ovt_analysis_decoder_v1`; `create()` never touches global state other than the decoder library's own thread-safe init; every `push` is wrapped so that **no** library error escapes as a crash (`setjmp` is not used; libraries return error codes); `FrameAnalysis.frame.id.decode_index = decode_index_hint`; `FrameAnalysis.plugin_id/plugin_version` filled from caps; grids are **4×4 luma cells in coded-picture coordinates** (`cells_w = (coded_w + 3) / 4`); MVs are converted to **1/16 luma sample units** (§0.1.9); every per-block syntax element the decoder read is stored in the codec extension table (§0.5.4 rule) — field names are those of the Data Model chapter (`AvcMb`, `HevcCu`, `HevcPu`, `HevcTu`, `Av1Block`, `Av1Txb`, `*FilterParams`). Each plugin directory has `CMakeLists.txt`, `src/plugin.cpp` (ABI glue), `src/map_*.cpp` (data mapping), `tests/` (Catch2 golden tests against the oracles of §7.7), `patches/` where applicable, and `README.md` with the pinned upstream version and build flags (also recorded in `third_party/VERSIONS.md`).

Grid field availability is announced in `caps.grid_fields` with these bit assignments (shared header `core/include/ovt/abi/analysis_decoder_v1.h`): bit 0 `block_id`, 1 `partition_id`, 2 `pred`, 3 `qp`, 4 `intra_mode_y`, 5 `intra_mode_c`, 6 `mv0`, 7 `mv1`, 8 `ref0`, 9 `ref1`, 10 `tx_log2_w/h`, 11 `tx_type`, 12 `cbf`, 13 `bits_x8`, 14 `deblock_bs_v/h`, 15 `filter_idx`. A vector not announced MUST be empty in the FlatBuffer and the GUI shows the overlay as "n/a (not provided by <plugin>)".

### 7.6.1 `ovt-plugin-aom` — AV1 block analysis via libaom inspection

| Item | Value |
|---|---|
| Upstream | libaom, https://aomedia.googlesource.com/aom/ — pin tag **v3.12.1** (or the latest v3.x tag at the start of WI-2.154; the exact tag is recorded in `third_party/VERSIONS.md` and never changed without re-running the golden tests). |
| Licence | BSD-2-Clause + Alliance for Open Media Patent License 1.0 (ships in every official build with the PATENTS notice, ADR-015). Plugin code: BSD-2-Clause. |
| Build (`third_party/libaom.cmake`) | `cmake -DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1 -DCONFIG_AV1_ENCODER=0 -DCONFIG_AV1_DECODER=1 -DCONFIG_MULTITHREAD=1 -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_TOOLS=0 -DENABLE_DOCS=0 -DBUILD_SHARED_LIBS=0 -DCONFIG_PIC=1 -DAOM_TARGET_CPU=<arm64|x86_64>` (Android: `-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a|x86_64 -DANDROID_PLATFORM=android-28`). Static library linked into `libovt_plugin_aom.{dylib,so}` with `-fvisibility=hidden` so only `ovt_analysis_decoder_v1` is exported. `av1/decoder/inspection.h` and `av1/decoder/accounting.h` are **not installed** by libaom: the plugin includes them from the ExternalProject source tree (`${LIBAOM_SOURCE_DIR}`), never from a vendored copy, so a version bump cannot silently use a stale header. |
| Caps | `codec = AV1`, `plane_stages = Reconstruction` **only** with stock libaom (the stock `insp_frame_data` of libaom v3.12.1 has no plane buffers; `Prediction`, `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration` and `Residual` are announced only when the plugin is built against the snapshot patch of this section — a compile-time `#ifdef OVT_AOM_SNAPSHOTS` set by `third_party/libaom.cmake` when the patch applied), `grid_fields` = all 16 bits except 14 (`deblock_bs`: AV1 has no bS; announced absent) and, with stock libaom, except 11 (`tx_type`: see the `mi.tx_type` row; announced only with the extension patch); bit 15 `filter_idx` is exact only with the patch (see the `cdef` row); bit 13 `bits_x8` only when built with `CONFIG_ACCOUNTING=1` (always the case for `third_party/libaom.cmake`); `has_symbol_accounting = 1`, `bit_exact = 1`, `supports_threads = 1`. |

**Decoder driving** (verified against libaom v3.12.1 `av1/av1_dx_iface.c` `decoder_inspect()` and `examples/inspect.c` `read_frame()`). `create`: `aom_codec_dec_init(&ctx, aom_codec_av1_dx(), &cfg{threads, allow_lowbitdepth = 1}, 0)` (`allow_lowbitdepth = 1` so that 8-bit streams are output as 8-bit `aom_image_t` planes, as `aomdec`/`inspect` do; 10/12-bit streams still produce 16-bit samples); `aom_codec_control(&ctx, AV1D_SET_IS_ANNEXB, 0)` (the host always delivers Section 5 OBUs); `aom_codec_control(&ctx, AV1D_SET_OUTPUT_ALL_LAYERS, 0)`; `aom_codec_control(&ctx, AV1D_SET_OPERATING_POINT, 0)`; `aom_inspect_init ii = { .inspect_cb = on_frame, .inspect_ctx = this }; aom_codec_control(&ctx, AV1_SET_INSPECTION_CALLBACK, &ii)` — `AOM_CODEC_INCAPABLE` here means the library was built without inspection → `create` returns `OVT_E_PLUGIN` with message `"libaom built without CONFIG_INSPECTION"`. `AV1D_SET_ROW_MT = 1`; `AV1D_SET_SKIP_FILM_GRAIN = 1` (reconstruction is exported before film grain synthesis, chapter 6 §6.6.1 rule 5); `AV1_SET_SKIP_LOOP_FILTER` (this is the control's name; there is no `AV1D_` variant) is never set in analysis mode (we want filtered reconstruction; pre-filter planes come from snapshots). **Inspection mode changes the decode loop.** In libaom v3.12.1 `decoder_decode()` (`av1/av1_dx_iface.c`) dispatches on `user_priv`: `#if CONFIG_INSPECTION if (user_priv != 0) return decoder_inspect(ctx, data, data_sz, user_priv); #endif`. So `aom_codec_decode()` runs `decoder_inspect()` **iff `user_priv != NULL`** — the registered callback is *not* the trigger. `decoder_inspect()` copies `ctx->inspect_cb`/`inspect_ctx` into the decoder (`pbi->inspect_cb = ctx->inspect_cb`), so the inspection callback is honoured **only** on this path; with `user_priv == NULL` the normal multi-frame `decoder_decode()` path runs and the callback never fires. `decoder_inspect()` decodes **exactly one frame** (one frame header plus its tile groups, or one `show_existing_frame` header) from the buffer and reports where it stopped through `user_priv`, which therefore **must** point to an `Av1DecodeReturn` (declared in the public header `aom/aomdx.h`: `{ unsigned char *buf; int idx; int show_existing; }`). Consequences: the plugin passes `&ret` on **every** data call and never `NULL` (a future refactor that passes `NULL` silently loses all block data); `user_priv` is **not** available for hints. `push`: the host delivers one OBU per call; the plugin accumulates the OBUs of one temporal unit (flushed when an `OBU_TEMPORAL_DELIMITER` arrives or at `flush`), then runs the `inspect.c` loop over the TU bytes: `while (len > 0) { Av1DecodeReturn ret{}; if (aom_codec_decode(&ctx, p, len, &ret) != AOM_CODEC_OK) → OVT_E_CORRUPT (continue with the next TU); consumed = ret.buf − p; p = ret.buf; len −= consumed; if (consumed == 0) break; }`. Inside each iteration exactly one frame was handled: if the frame had tile data the inspection callback fired once (also for hidden frames) and inside it `ifd_inspect(&fd, decoder, /*skip_not_transform=*/0)` filled `insp_frame_data`, which the plugin maps (below) into a `FrameAnalysis` builder; if `ret.show_existing == 1` no callback fired (`show_existing_frame` headers decode no tiles) and the plugin emits no `FrameAnalysis` — the host synthesises that entry (7.5.3 A0). The plugin assigns `decode_index` **positionally**: it counts frames it has handled (callback frames plus `show_existing` iterations) and sets `decode_index = decode_index_hint_of_first_frame_in_TU + position`; the running count MUST equal the host's hint for the next TU, otherwise `OVT_E_PLUGIN "frame count mismatch at decode index %d"` (the index and libaom disagree about the TU's frame structure). After each iteration `aom_codec_get_frame(&ctx, &iter)` is drained: it returns an `aom_image_t` only for frames that are **shown** in that iteration (a `show_frame = 1` frame, or the frame revealed by `show_existing_frame`); hidden frames have **no** `aom_image_t`, so without the snapshot patch they have no `planes[]` (the host adds Diagnostic Info `ANA-0001`). The image is attached to the `FrameAnalysis` of the frame handled in the same iteration (for `show_existing` it is attached to nothing: the shown frame's own `FrameAnalysis` was emitted when it was decoded; the GUI resolves planes through `show_existing_of`). `pull`: returns the next finished `FrameAnalysis` in decode order. `flush`: `aom_codec_decode(&ctx, NULL, 0, NULL)` then drain `aom_codec_get_frame`.

**Mapping `insp_frame_data` / `insp_mi_data` → OVT model.** Source names are libaom's (`av1/decoder/inspection.h`, `av1/common/enums.h` / `blockd.h`); `mi` is the `insp_mi_data` at MI position `(mi_row, mi_col)` (one per 4×4 luma; `mi_grid[mi_row * mi_cols + mi_col]`). The OVT cell at `(x4, y4) = (mi_col, mi_row)`.

| libaom source | OVT target | Conversion |
|---|---|---|
| `fd.frame_number`, `fd.show_frame`, `fd.show_existing_frame`, `fd.frame_type` | `FrameAnalysis.frame.id.decode_index` (from hint), `frame.shown`, `frame.type` | `frame_type`: 0 KEY→Key, 1 INTER→Inter, 2 INTRA_ONLY→IntraOnly, 3 S_FRAME→Switch |
| `fd.base_qindex` | `frame.qp_base` | direct (qindex 0..255) |
| `fd.mi_rows`, `fd.mi_cols`; superblock size from the host's parsed sequence header (`use_128x128_superblock`; `insp_frame_data` has **no** superblock field) | `grid.cells_w = (coded_w + 3) / 4`, `grid.cells_h = (coded_h + 3) / 4`; `blocks[0].children` = superblocks of 64 or 128 luma samples in raster order | libaom's `mi_cols` is `2*((w+7)>>3)` (even) and `mi_rows` likewise; MIs beyond `cells_w`/`cells_h` are padding and are dropped (chapter 6 §6.2.1 rule 4) |
| `fd.tile_mi_rows`, `fd.tile_mi_cols` + `AV1D_GET_TILE_DATA`/tile info from the host's `FrameHeader` | `partitions[]` (`PartitionInfo` kind Tile) and `grid.partition_id` | `partition_id = tileRow * TileCols + tileCol` from `MiRowStarts/MiColStarts` (host passes the parsed `tile_info` to the plugin through the FrameAnalysis builder — the plugin itself does not parse headers; the host fills `partitions[].header` and byte ranges from `index`) |
| `fd.y_dequant[seg][2]`, `u_dequant`, `v_dequant`, `fd.delta_q_present_flag`, `fd.delta_q_res` | `Av1FilterParams` (`frame-level quantizer table`) | copied as-is (DC, AC per segment) |
| `mi.bsize` (`BLOCK_SIZE`) | leaf `Block.rect` and `Block.kind = Block`; `grid.block_id` | Block w/h from the table: `BLOCK_4X4 0→4×4, 4X8 1→4×8, 8X4 2→8×4, 8X8 3, 8X16 4, 16X8 5, 16X16 6, 16X32 7, 32X16 8, 32X32 9, 32X64 10, 64X32 11, 64X64 12, 64X128 13, 128X64 14, 128X128 15, 4X16 16, 16X4 17, 8X32 18, 32X8 19, 16X64 20, 64X16 21`. libaom stores the same `insp_mi_data` content in every MI of a block but does not mark block origins; the plugin therefore reconstructs blocks by walking each superblock recursively with the **partition tree**: for each square at `(r,c,size)`, `bsize` of the MI at the origin tells the leaf size; if `bsize` is the square itself → leaf; if `bsize` is a rectangle (e.g. `BLOCK_64X32`) → PARTITION_HORZ with two leaves (second at `r + h4`); if `bsize` is smaller than the square: look at the four quadrants' `bsize` values to distinguish `PARTITION_SPLIT` (recurse) from `HORZ_A/HORZ_B/VERT_A/VERT_B` (one half is a rectangle, the other two squares) and `HORZ_4/VERT_4` (four 1:4 rectangles). With the extension patch (`mi.partition`) the partition type is read directly and the reconstruction becomes a check. `Block.partition_type` = `PARTITION_*` enum (0 NONE, 1 HORZ, 2 VERT, 3 SPLIT, 4 HORZ_A, 5 HORZ_B, 6 VERT_A, 7 VERT_B, 8 HORZ_4, 9 VERT_4) stored on the **parent** square. |
| `mi.skip` | `grid.cbf = skip ? 0 : 1` (bit0..2 all set when not skip; per-plane cbf is unknown → extension `Av1Block.skip` keeps the raw flag); `Block.skip` | AV1 `skip` = "no residual coded" (§5.11.5 `skip`); not the same as `skip_mode` |
| `mi.ref_frame[0]`, `mi.ref_frame[1]` (`MV_REFERENCE_FRAME`: −1 NONE, 0 INTRA_FRAME, 1 LAST_FRAME … 7 ALTREF_FRAME) | `grid.ref0/ref1` (index into `refs[]`), `grid.pred` | `ref_frame[0] == 0` (INTRA_FRAME) → `pred = Intra`, `ref0 = ref1 = -1`; `> 0` → `Inter`, `ref0 = ref_frame[0]` (the AV1 reference type **is** the `refs[]` index, chapter 6 §6.3.1 `RefSlot.name`); `mi.intrabc` → `IntraBC`, `ref0 = -1`; `mi.palette > 0 && ref_frame[0] == 0` → `Palette`; `ref_frame[1] == 0` (INTRA_FRAME) with inter `ref_frame[0]` = interintra (flag in ext, `ref1 = -1`); `ref_frame[1] == -1` (NONE) → `ref1 = -1`; `ref_frame[1] > 0` → `ref1 = ref_frame[1]`. `refs[]` is built once per frame with **8 entries**: `refs[0] = RefSlot{index 0, name "INTRA", frame.decode_index = -1, slot = -1}` (placeholder, `usage_cells = 0`), and for `t = 1..7`: `RefSlot{index = t, name = "LAST","LAST2","LAST3","GOLDEN","BWDREF","ALTREF2","ALTREF"[t−1], slot = ref_frame_idx[t−1], frame = host's slot_decode_index[ref_frame_idx[t−1]]}` (intra frames: `slot = -1`, `frame.decode_index = -1`). `usage_cells` = count of cells with `ref0 == t` plus cells with `ref1 == t`. |
| `mi.mv[0]`, `mi.mv[1]` (`insp_mv{int16 row, col}` in **1/8 luma sample units**) | `grid.mv0_x = col*2`, `mv0_y = row*2`, `mv1_*` | ×2 → 1/16 units; `MotionVector` entries: one per leaf inter block per list with `rect = block rect`, `mv_x/mv_y`, `mvd` unavailable (−32768 sentinel → "n/a") until the ext patch, `predictor` = `mode` name (`"NEARESTMV"`, `"NEWMV"`, `"GLOBALMV"`, compound names), `flags` bit0 if `ref_frame[1] > 0`, bit2 (global/warped) if (`mode ∈ {GLOBALMV, GLOBAL_GLOBALMV}` and the host's `GmType[ref_frame[0]] > TRANSLATION`) or `motion_mode == WARPED_CAUSAL (2)`, bit3 if `motion_mode == OBMC_CAUSAL (1)` (chapter 6 §6.2.3; `Av1Block.is_global` = the first condition only). Intrabc blocks: MV in whole-pel ×16 with `predictor = "INTRABC"`. |
| `mi.mode` (`PREDICTION_MODE`) | `grid.intra_mode_y` (intra) or `MotionVector.predictor` / `Av1Block.YMode` (inter; `Av1Block.YMode` is stored for every block) | intra 0 DC_PRED, 1 V_PRED, 2 H_PRED, 3 D45_PRED, 4 D135_PRED, 5 D113_PRED, 6 D157_PRED, 7 D203_PRED, 8 D67_PRED, 9 SMOOTH_PRED, 10 SMOOTH_V_PRED, 11 SMOOTH_H_PRED, 12 PAETH_PRED; inter 13 NEARESTMV, 14 NEARMV, 15 GLOBALMV, 16 NEWMV, 17 NEAREST_NEARESTMV, 18 NEAR_NEARMV, 19 NEAREST_NEWMV, 20 NEW_NEARESTMV, 21 NEAR_NEWMV, 22 NEW_NEARMV, 23 GLOBAL_GLOBALMV, 24 NEW_NEWMV. For inter blocks `intra_mode_y = -1`. |
| `mi.uv_mode` (`UV_PREDICTION_MODE`) | `grid.intra_mode_c` | 0..12 as luma names with `UV_` prefix, 13 `UV_CFL_PRED`; −1 for inter |
| `mi.cfl_alpha_idx`, `mi.cfl_alpha_sign` | `Av1Block.cfl_alpha_signs`, `cfl_alpha_u`, `cfl_alpha_v` | §5.11.45 derivation: `signU = (cfl_alpha_signs + 1) / 3`, `signV = (cfl_alpha_signs + 1) % 3`; `alpha_u = signU == CFL_SIGN_ZERO (0) ? 0 : (signU == CFL_SIGN_NEG (1) ? -1 : 1) * ((cfl_alpha_idx >> CFL_ALPHABET_SIZE_LOG2 (4)) + 1)`, `alpha_v` likewise with `cfl_alpha_idx & 15` |
| `mi.segment_id` | `Av1Block.segment_id` | direct 0..7 |
| `mi.current_qindex` | `grid.qp` | direct; equals `base_q_idx` when `delta_q_present == 0` and no ALT_Q feature |
| `mi.tx_size` (`TX_SIZE`; for inter and IntraBC blocks `ifd_inspect()` takes `inter_tx_size[]` at the MI, so the value is per transform unit) | `grid.tx_log2_w/h` | 0 TX_4X4→(2,2), 1 8X8→(3,3), 2 16X16→(4,4), 3 32X32→(5,5), 4 64X64→(6,6), 5 4X8→(2,3), 6 8X4→(3,2), 7 8X16→(3,4), 8 16X8→(4,3), 9 16X32→(4,5), 10 32X16→(5,4), 11 32X64→(5,6), 12 64X32→(6,5), 13 4X16→(2,4), 14 16X4→(4,2), 15 8X32→(3,5), 16 32X8→(5,3), 17 16X64→(4,6), 18 64X16→(6,4); −1 occurs only when `ifd_inspect()` is called with `skip_not_transform = 1`, which the plugin never does (it passes 0, as `inspect` does by default) — should it appear, it maps to `tx_log2_* = 0xFF` |
| `mi.tx_type` (`TX_TYPE`) — **stock value unusable**: `ifd_inspect()` at v3.12.1 copies `tx_type_map` only when `mi->skip` is set and writes 0 for every coded block (inverted condition; `inspect`'s `transformType` layer has the same values). Without the extension patch `grid.tx_type` stays empty (caps bit 11 cleared) and `Av1Txb.TxType` is 255; the patch fixes the condition (row `tx_type` of the patch table) | `grid.tx_type` (patched builds) | 0 DCT_DCT, 1 ADST_DCT, 2 DCT_ADST, 3 ADST_ADST, 4 FLIPADST_DCT, 5 DCT_FLIPADST, 6 FLIPADST_FLIPADST, 7 ADST_FLIPADST, 8 FLIPADST_ADST, 9 IDTX, 10 V_DCT, 11 H_DCT, 12 V_ADST, 13 H_ADST, 14 V_FLIPADST, 15 H_FLIPADST; −1 → 0xFF. Per-TXB `Av1Txb` records are emitted from the inspection TX maps (one per transform block: `tx_size` grid is per MI, so TXBs are reconstructed by grouping MIs with the same tx size inside a block — exact because libaom stores `tx_size` per TX unit for inter var-tx via `inter_tx_size`; with the ext patch `inter_tx_size[]` is copied explicitly) |
| `mi.filter[0]`, `mi.filter[1]`, `mi.dual_filter_type` | `Av1Block.interp_filter[2]` | `InterpFilter`: 0 EIGHTTAP_REGULAR, 1 EIGHTTAP_SMOOTH, 2 MULTITAP_SHARP, 3 BILINEAR; `[0]` = y (vertical) filter, `[1]` = x |
| `mi.motion_mode` | `Av1Block.motion_mode` | 0 SIMPLE_TRANSLATION, 1 OBMC_CAUSAL, 2 WARPED_CAUSAL |
| `mi.compound_type` (`mbmi->interinter_comp.type`) | `Av1Block.compound_type` | libaom `COMPOUND_TYPE` numbering (`av1/common/enums.h`: 0 COMPOUND_AVERAGE, 1 COMPOUND_DISTWTD, 2 COMPOUND_WEDGE, 3 COMPOUND_DIFFWTD) differs from the spec enum used by `Av1CompoundType` (chapter 6: WEDGE 0, DIFFWTD 1, AVERAGE 2, INTRA 3, DISTANCE 4): remap `{0→2, 1→4, 2→0, 3→1}`; non-compound blocks → `Av1CompoundType_Unknown` (255); interintra blocks → `COMPOUND_INTRA` (3) |
| `mi.intrabc`, `mi.palette`, `mi.uv_palette` | `Av1Block.use_intrabc`, `palette_size_y`, `palette_size_uv` | direct (palette sizes 0 or 2..8) |
| `mi.cdef_level`, `mi.cdef_strength` | `grid.filter_idx` (= `cdef_idx` of the 64×64; **with ext patch**), `Av1FilterParams.cdef.*` | Without the patch `filter_idx` is derived: find the index `i` in the frame's `cdef_y_pri_strength[]/cdef_y_sec_strength[]` matching `(cdef_level, cdef_strength)`; ambiguous → 0xFF |
| — (`insp_mi_data` v3.12.1 has **no** `sb_bits` field; libaom exports no per-superblock bit count, D30) | `Block.bits_x8` of the SB node | Σ of its children's `bits_x8`, which are accounting-derived (next row); 0 when the library lacks `CONFIG_ACCOUNTING` |
| `Accounting` (`fd.accounting->syms.syms[i]`: `context.x`, `context.y` (MI units), `id`, `bits` (1/8-bit units, `AOM_ACCT_BITRES = 3`), `samples`; `syms.dictionary.strs[id]`) | `symbols[]` = `SymbolCost{x4 = context.x, y4 = context.y, symbol_id = id, bits_x8 = bits, count = samples}`; `symbol_names[id] = strs[id]`; `grid.bits_x8`: for each leaf block, `blockBits = Σ bits of symbols with (x4,y4) inside the block rect`, then each cell gets `blockBits / cellCount` (remainder added to the first cell so the sum is exact); `Block.bits_x8 = blockBits` (leaves) or Σ children | Frame-level symbols (context x,y = −1 or outside the frame — libaom records header symbols with the last context) are attributed to `Block[0]` (frame root) only. libaom resets the accounting when it starts decoding a frame's tiles (`aom_accounting_reset`, `av1/decoder/decodeframe.c`); the plugin reads it inside the callback, which fires after the frame's last tile group and before the next `aom_codec_decode`. `aom_accounting_reset` is called at the start of `decode_tiles()`, i.e. **per tile group** call, so for a frame with several tile groups the stock accounting at callback time covers only the last tile group: the plugin compares Σ `bits` with 8 × (payload bytes of the last tile group) and, when `NumTileGroups > 1`, sets `derived "accounting covers last tile group only"` on the frame root and clears `grid.bits_x8` (announced absent for that frame); the extension patch moves the reset to frame start (one line in `decode_tiles`, guarded by `pbi->inspect_cb != NULL`) so patched builds account the whole frame. WI-2.157 tests both paths on `syn_av1_tiles_cdef_lr` (several tile groups) |
| `aom_image_t` from `aom_codec_get_frame` (drained after each `aom_codec_decode` iteration; present only for the frame shown in that iteration, see "Decoder driving") | `planes[]` stage `Reconstruction`, planes 0..2 — for shown frames only without the patch (hidden frames: no image → no planes; with the snapshot patch the `recon` snapshot is used for every frame) | `img->planes[p]`, `img->stride[p]`, `img->bit_depth`, `sample_bytes = (fmt & AOM_IMG_FMT_HIGHBITDEPTH) ? 2 : 1`; width/height from `img->d_w/d_h` (**upscaled** output size: `d_w = UpscaledWidth`; for super-resolution frames `PlaneRef.width ≠ coded_size.w` — the V-023 exception of chapter 6 §6.9; the renderer stretches overlays horizontally by `UpscaledWidth / FrameWidth`). Film grain: the analysis decoder sets `aom_codec_control(&ctx, AV1D_SET_SKIP_FILM_GRAIN, 1)` at `create` (control present in libaom ≥ 2.0 `aom/aomdx.h`; verified in v3.12.1), so `Reconstruction` is the pre-grain picture as chapter 6 §6.6.1 rule 5 requires; the plugin records `derived "film grain present (not applied)"` on frames with `apply_grain = 1` |
| (nothing else) — the stock `insp_frame_data` (libaom v3.12.1) has exactly these members: `accounting`, `mi_grid`, `frame_number`, `show_frame`, `frame_type`, `base_qindex`, `mi_rows`, `mi_cols`, `tile_mi_rows`, `tile_mi_cols`, `y/u/v_dequant[MAX_SEGMENTS][2]`, `delta_q_present_flag`, `delta_q_res`, `show_existing_frame`; it has **no** plane buffers and no superblock size | `Prediction`, `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration`, `Residual` stages and planes of hidden frames | only with the snapshot patch (`0002-inspection-frame-snapshots.patch`, below) |

**What is NOT available without the patch (UI shows "n/a")**: every plane stage except `Reconstruction` (and `Reconstruction` itself for hidden frames), `angle_delta_y/uv`, `use_filter_intra`/`filter_intra_mode`, `interintra`/`interintra_mode`/`wedge_interintra`/`wedge_index`/`wedge_sign`/`mask_type`, `drl` (`ref_mv_idx`), `skip_mode`, warp parameters, `delta_lf[4]`, per-64×64 `cdef_idx`, loop-restoration unit parameters, palette colours, `partition` (reconstructed heuristically), `inter_tx_size[]`, `mvd`, `comp_group_idx/compound_idx`, `seg_id_predicted`, `num_proj_ref`, `overlappable_neighbors`, coefficients, `PreSaoOrCdef`/`PreLoopRestoration` planes. The GUI's Selection Info panel lists these fields with value "n/a (libaom inspection without OVT extension)".

**The upstream-able extension patch** (`plugins/ovt-plugin-aom/patches/0001-inspection-extend-mi-data.patch`, 100–300 lines, touches only `av1/decoder/inspection.h`, `av1/decoder/inspection.c`, `av1/decoder/decodeframe.c` (snapshots) and `examples/inspect.c` (new JSON flags, so the oracle can print the new fields)). Exact additions to `insp_mi_data`, each copied in `ifd_inspect()` from the `MB_MODE_INFO *mbmi` of the MI (`cm->mi_params.mi_grid_base[...]`):

| New `insp_mi_data` field | Source `MB_MODE_INFO` field (`av1/common/blockd.h`) | Condition / default |
|---|---|---|
| `int16_t partition` | `mbmi->partition` | always (value of the **containing square**'s partition as libaom stores it per block) |
| `int16_t angle_delta[2]` | `mbmi->angle_delta[PLANE_TYPE_Y]`, `[PLANE_TYPE_UV]` | intra directional modes; else 0 |
| `int16_t filter_intra_mode` | `mbmi->filter_intra_mode_info.use_filter_intra ? mbmi->filter_intra_mode_info.filter_intra_mode : -1` | 0 FILTER_DC_PRED, 1 FILTER_V_PRED, 2 FILTER_H_PRED, 3 FILTER_D157_PRED, 4 FILTER_PAETH_PRED |
| `int16_t interintra_mode`, `int16_t wedge_interintra`, `int16_t interintra_wedge_index` | `mbmi->interintra_mode`, `mbmi->use_wedge_interintra`, `mbmi->interintra_wedge_index` | only when `is_interintra_pred(mbmi)` (`ref_frame[1] == INTRA_FRAME`), else −1 |
| `int16_t interinter_comp_type`, `wedge_index`, `wedge_sign`, `mask_type` | `mbmi->interinter_comp.type`, `.wedge_index`, `.wedge_sign`, `.mask_type` | compound only, else −1 |
| `int16_t comp_group_idx`, `int16_t compound_idx` | `mbmi->comp_group_idx`, `mbmi->compound_idx` | compound only |
| `int16_t ref_mv_idx` | `mbmi->ref_mv_idx` | inter; this is the DRL index (`drl_mode` count) |
| `int16_t skip_mode` | `mbmi->skip_mode` | always |
| `int16_t seg_id_predicted` | `mbmi->seg_id_predicted` | always |
| `int32_t wm_params[6]`, `int16_t wm_type`, `int16_t wm_invalid` | `mbmi->wm_params.wmmat[0..5]`, `.wmtype`, `.invalid` | when `motion_mode == WARPED_CAUSAL`; else `wm_type = -1` |
| `int16_t num_proj_ref`, `int16_t overlappable_neighbors` | `mbmi->num_proj_ref`, `mbmi->overlappable_neighbors` | inter |
| `int16_t delta_lf_from_base`, `int16_t delta_lf[4]` | `mbmi->delta_lf_from_base`, `mbmi->delta_lf[FRAME_LF_COUNT]` | when `delta_lf_present`; else 0 |
| `int16_t cdef_idx` | `mbmi->cdef_strength` (libaom stores the per-64×64 `cdef_idx` here; −1 when not coded because all blocks skipped) | always |
| `uint16_t palette_colors[3][8]` | `mbmi->palette_mode_info.palette_colors[plane*PALETTE_MAX_SIZE + i]` | when `palette_size[plane] > 0` |
| `int16_t inter_tx_size[16]` | `mbmi->inter_tx_size[INTER_TX_SIZE_BUF_LEN]` (var-tx) | inter blocks with `TX_MODE_SELECT` |
| `tx_type` (existing field, corrected) | `mi_params->tx_type_map[]` at the top-left MI of the transform unit covering the MI | for every **non-skipped** block (stock code copies it only for skipped blocks); skipped blocks → −1 |
| `int16_t mvd[2][2]`, `int16_t mv_joint` | **not stored in `MB_MODE_INFO`** — recorded in `read_mv()` (`av1/decoder/decodemv.c`) into a new `MB_MODE_INFO`-adjacent inspection array `pbi->insp_mvd[mi_idx]` only `#if CONFIG_INSPECTION` | NEWMV family only |
| frame-level: `RestorationUnitInfo lr_units[3][]` with unit counts | `cm->rst_info[plane].unit_info[u]` (`restoration_type`, `wiener_info.vfilter[7]/hfilter[7]`, `sgrproj_info.ep, xqd[2]`), `cm->rst_info[plane].units_per_tile` / `horz_units`, `vert_units` | when `UsesLr` |
| frame-level: `cdef_info` | `cm->cdef_info` (`cdef_damping`, `cdef_bits`, `cdef_strengths[]`, `cdef_uv_strengths[]`) | always (also parsed by the host; kept for cross-check) |
| frame-level: `gm_params[8]`, `lf` (`filter_level[2]`, `filter_level_u/v`, `sharpness_level`, `ref_deltas[8]`, `mode_deltas[2]`) | `cm->global_motion[]`, `cm->lf` | always |
| snapshots: `prefiltered_frame_buffer` (before `av1_loop_filter_frame`), `pre_cdef_frame_buffer` (after deblock, before `av1_cdef_frame`), `pre_lr_frame_buffer` (after CDEF, before `av1_loop_restoration_filter_frame`), `predicted_frame_buffer`, `residual` (int16 planes, reconstructed − predicted) | copies made in `av1/decoder/decodeframe.c` at the three filter call sites, under `#if CONFIG_INSPECTION && pbi->inspect_cb != NULL` (zero cost when inspection is unused) | all stages → `PlaneStage` `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration`, `Prediction`, `Residual` |

The patch adds `-plt`-style flags to `examples/inspect.c` for each new field (`-ad` angle delta, `-fi` filter intra, `-ii` interintra, `-wg` wedge, `-drl`, `-sm` skip mode, `-wm` warp, `-dlf` delta lf, `-cdi` cdef idx, `-lr` LR units, `-plc` palette colours) so the oracle in §7.7.5 covers them. **Upstreaming WI-2.160**: submit to `aomedia-review.googlesource.com` as two changes (fields; snapshots), referencing the `aomanalyzer` use case; until merged the ExternalProject applies the patch series with `tools/apply_patches.py --check` (a dry run of the whole series with `git apply --check`, failing on the first patch that does not apply, then the real apply; `git apply --3way` cannot run on the extracted tarball, which is not a git repository, issue #275); a failing apply fails the build (never silently skips), and a patch that needs a three-way merge after a libaom bump is rebased by hand (WI-2.160).

**Performance expectations**: libaom inspection+accounting decodes 1080p AV1 at ≈ 5–20 fps single-threaded on an M1 (Appendix D.8; the accounting allocations dominate). Budget (chapter 14 §14.9.3 / `tests/perf/budgets.json` is normative, D19; headline in §0.7.4): a GOP of ≤ 60 frames at 1080p ≤ 3 s → with 4 threads (row-MT) the plugin MUST reach ≥ 20 fps at 1080p; the benchmark `bench_analysis_aom_gop60_1080p` enforces the budget on `m1-air-8gb`. Android (Pixel Tablet, arm64, NEON on): expect 2–3× slower; the GUI shows progress.

### 7.6.2 `ovt-plugin-de265` — HEVC block analysis via libde265 internals

| Item | Value |
|---|---|
| Upstream | libde265, https://github.com/strukturag/libde265 — pin tag **v1.0.15** (re-check at the start of WI-2.162 that no newer 1.0.x tag exists; record the chosen tag in `third_party/VERSIONS.md`). |
| Licence | LGPL-3.0-or-later → the plugin is a **separately loaded shared library** (`libovt_plugin_de265.so` links libde265 statically; the plugin itself is LGPL-3.0-or-later; `libovt-core` never links it). Not part of the "no pooled-patent codecs" default download variant (ADR-015). |
| Build (`third_party/libde265.cmake`) | `cmake -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON` (these four are the **only** options of libde265 v1.0.15's `CMakeLists.txt`; there is no `DISABLE_SSE` option — the SSE4.1 kernels (`libde265/x86/sse*.cc`, the only SIMD code in v1.0.15) are selected by a compile check (`HAVE_X86`/`HAVE_SSE4_1`) and simply absent on arm64); patches applied in order `0001-de265-internals-api.patch`, `0002-de265-persist-pu-tu-syntax.patch`, `0003-de265-plane-snapshots.patch` (all behind `-DDE265_INTERNALS=ON`, a new CMake option added by patch 0001 that defaults to OFF so the patched tree builds identically to upstream when OFF). |
| Caps | `codec = H265`, `plane_stages = Reconstruction` (+ `Prediction | Residual | Coefficients | PreDeblock | PreSaoOrCdef` with patch 0003), `grid_fields` = all 16 bits (bit 13 `bits_x8` only with patch 0002, bit 11 `tx_type` always: DST-7 vs DCT is derivable: 4×4 intra luma TB → DST), `has_symbol_accounting = 0`, `bit_exact = 1` (conformance-exact for Main/Main10), `supports_threads = 1`. **Profile coverage (normative for OVT):** libde265 1.0.x is relied upon for **Main and Main 10 only** (4:2:0, 8/10-bit; `general_profile_idc` 1, 2 and Main Still Picture 3). Appendix D.2 records that it lacks RExt/SCC/MV-HEVC; its README-level RExt claims are **not** trusted. Unsupported streams (any RExt profile — `general_profile_idc == 4`: 4:0:0, 4:2:2, 4:4:4 or > 10-bit; SCC `general_profile_idc == 9`; MV-HEVC/SHVC enhancement layers) → `push` returns `OVT_E_UNSUPPORTED` with Diagnostic `DEC-H265-0010 "use ovt-plugin-hm"`; the host then retries with `hm` (phase 4, §7.6.4 — the RExt/SCC path) if present, and playback still works through the FFmpeg `hevc` decoder (which does support RExt). |

**Decoder driving.** `create`: `de265_new_decoder()`, `de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, 0)` (hash check done by OVT's verify job instead), `de265_set_parameter_int(ctx, DE265_DECODER_PARAM_ACCELERATION_CODE, de265_acceleration_AUTO)`, `de265_start_worker_threads(ctx, threads)`; with patch 0001: `de265_internals_set_parameter_bool(ctx, DE265_INTERNALS_PARAM_KEEP_SYNTAX, 1)` and, when planes are requested, `..._SAVE_PREDICTION/RESIDUAL/TR_COEFF/PRE_DEBLOCK/PRE_SAO`. `push`: `de265_push_NAL(ctx, unit, len, pts = decode_index_hint, user_data = (void*)(intptr_t)decode_index_hint)` then `de265_decode(ctx, &more)` in a loop while `more` (errors `DE265_WARNING_*` are collected as Diagnostics; `DE265_ERROR_*` → `OVT_E_CORRUPT`); after each decode call `de265_get_next_picture(ctx)` and, for each image, build the `FrameAnalysis` (decode_index from `de265_get_image_user_data`), copy everything the model needs (metadata arrays via the patch-0001 accessors, and the requested planes) into the plugin's own buffers **immediately** and call `de265_release_next_picture(ctx)` before the next `de265_decode` — libde265 recycles `de265_image` objects from a small pool and holding images across decode calls stalls or starves the decoder (v1.0.15 `decoder_context::decode`), so no `de265_image*` is ever kept beyond one `push`. `flush`: `de265_flush_data(ctx)` + decode loop. Note: libde265 outputs pictures in **output (POC) order** with a DPB delay; the plugin therefore maps `decode_index` through `user_data`, never through the output order, and the host's A4/A5 drain logic tolerates frames arriving late (that is why `flush` is required for the last GOP frames).

**Patch 0001 — read-only internals API** (`libde265/de265_internals.h`, new public header, installed; implementation `libde265/de265_internals.cc`; all functions `LIBDE265_API`, C linkage, read `de265_image` `MetaDataArray`s and the slice header table; **no decoding code is modified except one write** — see `deblk_bS_v` below):

```c
/* de265_internals.h — read-only access to per-picture metadata (OVT patch 0001) */
typedef struct de265_internals_layout { int log2_unit_size; int width_in_units; int height_in_units; } de265_internals_layout;

/* CTB (coding tree block) info: unit = CTB */
LIBDE265_API de265_internals_layout de265_internals_get_CTB_info_layout(const struct de265_image*);
LIBDE265_API uint16_t de265_internals_get_CTB_slice_header_index(const struct de265_image*, int ctb_x, int ctb_y);   /* index into the slice header table */
LIBDE265_API int      de265_internals_get_CTB_slice_addr_rs(const struct de265_image*, int ctb_x, int ctb_y);        /* SliceAddrRs */
typedef struct de265_internals_sao { uint8_t SaoTypeIdx[3]; int8_t SaoOffsetVal[3][4]; uint8_t SaoBandPosition[3]; uint8_t SaoEoClass[3]; } de265_internals_sao;
LIBDE265_API void     de265_internals_get_CTB_sao(const struct de265_image*, int ctb_x, int ctb_y, de265_internals_sao* out);  /* from sao_info<> MetaDataArray */
LIBDE265_API uint8_t  de265_internals_get_CTB_flags(const struct de265_image*, int ctb_x, int ctb_y);   /* bit0 has_pcm_or_cu_transquant_bypass, bit1 deblocking disabled for this CTB's slice */

/* CB (coding block) info: unit = MinCb (log2 = MinCbLog2SizeY), values valid at every unit covered by the CB */
LIBDE265_API de265_internals_layout de265_internals_get_CB_info_layout(const struct de265_image*);
typedef struct de265_internals_cb { uint8_t log2CbSize; uint8_t PartMode; uint8_t ctDepth; uint8_t PredMode; /* libde265 slice.h enum PredMode: 0 MODE_INTRA 1 MODE_INTER 2 MODE_SKIP */ uint8_t pcm_flag; uint8_t cu_transquant_bypass; int8_t QP_Y; } de265_internals_cb;
LIBDE265_API void     de265_internals_get_CB_info(const struct de265_image*, int unit_x, int unit_y, de265_internals_cb* out);   /* from CB_ref_info: log2CbSize, PartMode, ctDepth, PredMode, pcm_flag, cu_transquant_bypass, QP_Y */

/* PB (prediction block) motion: unit = 4x4 */
LIBDE265_API de265_internals_layout de265_internals_get_PB_info_layout(const struct de265_image*);
typedef struct de265_internals_pb { uint8_t predFlag[2]; int8_t refIdx[2]; int16_t mv_x[2]; int16_t mv_y[2]; /* quarter-sample */ int32_t refPOC[2]; int8_t refIsLongTerm[2]; } de265_internals_pb;
LIBDE265_API void     de265_internals_get_PB_info(const struct de265_image*, int x4, int y4, de265_internals_pb* out);   /* PBMotion + slice header RefPicList_POC[list][refIdx] / LongTermRefPic */

/* Intra prediction modes: unit = 4x4 */
LIBDE265_API de265_internals_layout de265_internals_get_intra_info_layout(const struct de265_image*);
LIBDE265_API void     de265_internals_get_intra_modes(const struct de265_image*, int x4, int y4, uint8_t* intraPredModeY /*0..34, 255 n/a*/, uint8_t* intraPredModeC);

/* TU (transform unit) info: unit = 4x4 */
LIBDE265_API de265_internals_layout de265_internals_get_TU_info_layout(const struct de265_image*);
LIBDE265_API uint8_t  de265_internals_get_TU_info(const struct de265_image*, int x4, int y4);   /* raw tu_info byte (libde265 image.h, v1.0.15): TU_FLAG_SPLIT_TRANSFORM_MASK = 0x1F (bits 0..4 = split_transform_flag per depth 0..4), TU_FLAG_NONZERO_COEFF = 1<<7 */

/* Deblocking: unit = 4x4 edges */
LIBDE265_API de265_internals_layout de265_internals_get_deblk_info_layout(const struct de265_image*);
LIBDE265_API uint8_t  de265_internals_get_deblk_flags(const struct de265_image*, int x4, int y4);  /* raw deblk_info byte (image.h v1.0.15): DEBLOCK_BS_MASK = 0x03 (bits 0..1: bS of the LAST derived edge), DEBLOCK_FLAG_VERTI = 1<<4, DEBLOCK_FLAG_HORIZ = 1<<5, DEBLOCK_PB_EDGE_VERTI = 1<<6, DEBLOCK_PB_EDGE_HORIZ = 1<<7 */
/* libde265 keeps ONE bS per 4x4 and the horizontal-edge pass overwrites the vertical-edge value (deblock.cc derive_boundaryStrength), so after decoding the byte above holds the HORIZONTAL bS.
   Patch 0001 therefore adds MetaDataArray<uint8_t> deblk_bS_v to de265_image and ONE store in deblock.cc (right after derive_boundaryStrength() for vertical edges: img->deblk_bS_v.set(x,y,bS)) — this is the only decoding-code change of the patch series, compiled only when DE265_INTERNALS is ON. */
LIBDE265_API uint8_t  de265_internals_get_deblk_bS_v(const struct de265_image*, int x4, int y4);   /* vertical-edge bS 0..2 of the 4x4 at (x4,y4); 0 when DEBLOCK_FLAG_VERTI is clear */

/* Slice headers and picture-level data */
LIBDE265_API int      de265_internals_get_num_slice_headers(const struct de265_image*);
typedef struct de265_internals_slice_header { int slice_type; int SliceAddrRs; int slice_qp_delta; int SliceQPY; int num_ref_idx_l0_active; int num_ref_idx_l1_active; int32_t RefPicList_POC[2][16]; int8_t RefPicList_LongTerm[2][16]; int MaxNumMergeCand; int slice_deblocking_filter_disabled_flag; int slice_beta_offset_div2; int slice_tc_offset_div2; int slice_sao_luma_flag; int slice_sao_chroma_flag; int slice_cb_qp_offset; int slice_cr_qp_offset; int collocated_from_l0_flag; int collocated_ref_idx; int slice_temporal_mvp_enabled_flag; } de265_internals_slice_header;
LIBDE265_API void     de265_internals_get_slice_header(const struct de265_image*, int index, de265_internals_slice_header* out);
LIBDE265_API int      de265_internals_get_picture_poc(const struct de265_image*);   /* PicOrderCntVal */
LIBDE265_API void     de265_internals_get_sps_sizes(const struct de265_image*, int* CtbLog2SizeY, int* MinCbLog2SizeY, int* MinTbLog2SizeY, int* PicWidthInCtbsY, int* PicHeightInCtbsY);
```

Function bodies are 1–5 lines each (`return img->get_log2CbSize(x,y)` …), using the existing accessors `get_pred_mode`, `get_cu_skip_flag`, `get_PartMode`, `get_log2CbSize`, `get_ctDepth`, `get_pcm_flag`, `get_cu_transquant_bypass`, `get_QPY`, `get_mv_info`, `get_IntraPredMode`, `get_IntraPredModeC`, `get_SliceHeaderIndex`, `get_SliceAddrRS`, `get_sao_info`, `get_deblk_flags`, `get_deblk_bS` (Appendix D.2). `PredMode` MODE_SKIP is reported when `get_cu_skip_flag` is set.

**Patch 0002 — persistence of transient PU/TU syntax and bit positions** (behind `DE265_INTERNALS` and the runtime flag `KEEP_SYNTAX`): adds `MetaDataArray<de265_internals_pu_syntax> pu_syntax` (unit 4×4: `merge_flag`, `merge_idx`, `inter_pred_idc`, `mvd_x[2]`, `mvd_y[2]`, `mvp_lX_flag[2]`, `ref_idx[2]` as **signalled**), `MetaDataArray<de265_internals_tu_syntax> tu_syntax` (unit 4×4: `transform_skip_flag[3]`, `cbf[3]` (cbf_luma, cbf_cb, cbf_cr at this TU's depth), `rqt_depth`, `explicit_rdpcm_flag/dir`, `cu_qp_delta_abs`/sign at the quantization group origin, `cu_chroma_qp_offset_flag/idx`, `nnz[3]` = number of non-zero coefficients per component, `last_sig_coeff_x/y[3]`), and `MetaDataArray<uint32_t> cu_bits_x8` (unit MinCb: entropy cost of the CU in 1/8 bits). Write points in `libde265/slice.cc`: `read_prediction_unit()` (merge/mvd/mvp/ref_idx), `read_transform_unit()`/`residual_coding()` (tu syntax), `read_coding_unit()` begin/end (bit position). Bit position: `CABAC_decoder` in `cabac.cc` exposes `bitstream_curr`, `bitstream_start`, `bits_needed`; add `inline uint64_t de265_internals_cabac_tell_x8(const CABAC_decoder* d) { return ((d->bitstream_curr - d->bitstream_start) * 8 - (−d->bits_needed) /* bits buffered but unread */) * 8 − range_fraction; }` where `range_fraction` follows libaom's `od_ec_dec_tell_frac` idea: `floor(8 * log2(range / 256))`-style fractional bits estimated from the 9-bit `range` register via a 256-entry table (`−8·log2(range/512)` scaled); the delta between CU start and CU end is `cu_bits_x8`. Getters: `de265_internals_get_PU_syntax(img, x4, y4, out)`, `de265_internals_get_TU_syntax(img, x4, y4, out)`, `de265_internals_get_CU_bits_x8(img, unit_x, unit_y)`. Not persisted (out of scope): merge candidate *source* (spatial/temporal) — recorded as `predictor = "merge[idx]"` only.

**Patch 0003 — plane snapshots and signal planes** (behind flags): keeps copies of the picture (a) after reconstruction before deblocking (`deblock.cc` `apply_deblocking_filter` entry: copy `img` planes into `img->internals_pre_deblock`), (b) after deblocking before SAO (`sao.cc` `apply_sample_adaptive_offset` entry — SAO already works from a copy; expose that copy), (c) prediction planes (`intrapred.cc`/`motion.cc` write the prediction into `img->internals_pred` before adding the residual, done at the TU reconstruction point in `transform.cc` `decode_TU()`/`residual` add: pred = recon − residual stored as int16 residual planes), (d) coefficient planes (`int32` per TB, dequantised levels, written in `transform.cc` `scale_coefficients()`). Memory: ≈ 5× picture size; enabled only when the host requests those stages. Compared with the alternative "decode twice with `DE265_DECODER_PARAM_DISABLE_DEBLOCKING` / `DISABLE_SAO`", snapshots are **bit-exact for inter frames** (re-decoding with filters off changes the references), so the patch is required for correctness; the parameter-based path is used **only** in the plugin's self-test to validate patch 0003 on intra frames.

**Mapping to the model** (per picture; `x4,y4` cells; MinCb unit `u = 1 << (MinCbLog2SizeY − 2)` cells):

| libde265 source | OVT target | Conversion |
|---|---|---|
| `CB_info` at every MinCb unit → CU set (a CU covers `1 << (log2CbSize − 2)` cells square at the first unit where `(unit_x % size, unit_y % size) == 0`) | `Block{kind = CU, rect, level = ctDepth + 1}` children of the CTU block; `blocks[0].children` = CTUs (`CtbSizeY` squares raster order, `kind = CTU`) | `partition_type = PartMode` (0 PART_2Nx2N, 1 PART_2NxN, 2 PART_Nx2N, 3 PART_NxN, 4 PART_2NxnU, 5 PART_2NxnD, 6 PART_nLx2N, 7 PART_nRx2N); `pred`: MODE_INTRA → Intra (pcm_flag → PCM), MODE_SKIP → Skip, MODE_INTER → Inter; `skip = (PredMode == MODE_SKIP)`; `qp = QP_Y`; `HevcCu{ct_depth, part_mode, transquant_bypass, pcm, cu_qp_delta (patch 0002), chroma_qp_offset_idx, rqt_root_cbf = any TU cbf}` |
| CU partition + `PB_info` | `Block{kind = PU}` children of the CU (1, 2 or 4 PUs by PartMode; AMP rectangles per Table 7-10); `grid.mv0/mv1`, `ref0/ref1`, `MotionVector` per PU per list | `mv_x*4` (quarter → 1/16); `ref0 = refIdx[0]` mapped to `refs[]` entries named `"L0[i]"`/`"L1[i]"` with `frame = host decode index of refPOC` (host maps POC → decode_index via the DPB model of 7.4.4; the plugin stores `refPOC[0]/refPOC[1]` in `HevcPu.ref_poc_l0/ref_poc_l1`); `flags` bit0 when both predFlags; `predictor = merge_flag ? "merge[merge_idx]" : "amvp[mvp_lX_flag]"` (patch 0002; else `"?"`); `mvd` from patch 0002 ×4 |
| `intraPredModeY/C` per 4×4 | `grid.intra_mode_y` (0 planar, 1 DC, 2–34 angular; 255 → −1), `grid.intra_mode_c` (0–34; mode 4 = derived from luma is already resolved by libde265 — the signalled `intra_chroma_pred_mode` 0..4 is stored in `HevcCu.intra_chroma_pred_mode` with patch 0002) | direct |
| `TU_info` split bits per 4×4 → TU tree | `Block{kind = TU}` under the CU (depth from split flags: TU size = CU size >> depth where depth = number of set split bits along the path), `grid.tx_log2_w = tx_log2_h = log2(TU size)`, `grid.cbf` bit0 = `TU_FLAG_NONZERO_COEFF` (luma+chroma any) — with patch 0002 bits 0/1/2 from `cbf[3]`; `grid.tx_type` = 1 (DST-7) when intra && TU 4×4 && luma, else 0 (DCT-2); `HevcTu{depth, cbf[3], transform_skip[3], rdpcm, nnz[3], last_pos, ccp_alpha = n/a}` | — |
| `deblk_flags` and `deblk_bS_v` per 4×4 | `grid.deblock_bs_v = (flags & DEBLOCK_FLAG_VERTI) ? de265_internals_get_deblk_bS_v(x4,y4) : 0` (left edge), `grid.deblock_bs_h = (flags & DEBLOCK_FLAG_HORIZ) ? (flags & DEBLOCK_BS_MASK) : 0` (top edge; the stock byte holds the horizontal pass's bS, see the header comment) | values 0..2 (H.265 §8.7.2.4); `DEBLOCK_PB_EDGE_*` tell whether the edge is a PU edge (stored in `HevcTu`/`derived` only) |
| `CTB_sao` | `grid.filter_idx = SaoTypeIdx[0]` (luma: 0 off, 1 band, 2 edge) for all cells of the CTB; `HevcCtu.SaoTypeIdx[3]`, `HevcCtu.SaoOffsetVal[12]`, `HevcCtu.sao_band_position[3]`, `HevcCtu.sao_eo_class[3]` of the CTU's `Block.ext` (`SaoOffsetVal` already shifted by libde265 as 7.4.9.3.2 requires); deblocking β/tC offsets per slice go to `HevcFilterParams` from the slice header table | — |
| `CTB_slice_header_index`, `SliceAddrRs` | `grid.partition_id` (slice index), `partitions[]` kind Slice with `header` taken from the host's parsed slice segment header for that unit and byte range from `index` | tiles: `partition_id` encodes the **slice segment**; the tile index is `HevcCtu.tile_idx` (= `PartitionInfo.index` of the `Tile` partition), computed by the host from the PPS tile tables |
| `cu_bits_x8` (patch 0002) | `Block.bits_x8` of the CU; `grid.bits_x8` = CU bits / cells (remainder to the first cell) | — |
| `de265_get_image_plane(img, c, &stride)`, `de265_get_bits_per_pixel(img, c)`, `de265_get_image_width/height` | `planes[]` stage Reconstruction | `sample_bytes = bpp > 8 ? 2 : 1` |
| patch 0003 planes | stages `PreDeblock`, `PreSaoOrCdef`, `Prediction`, `Residual` (int16, `is_signed = true`), `Coefficients` (int32) | — |
| `de265_internals_get_slice_header` | `FrameAnalysis.frame.qp_base = SliceQPY of slice 0`; RefSlot table | — |

**n/a without patches**: merge/AMVP flags, mvd, mvp idx, cbf per component, transform_skip, nnz, bits per CU (patch 0002); pre-filter/prediction/residual/coefficient planes (patch 0003). The GUI shows `"n/a (libde265 without OVT patch 0002/0003)"`.

**Upstreaming WI-2.167**: open a PR on `strukturag/libde265` with patch 0001 only (pure accessors + CMake option), referencing sherlock265/YUView as consumers; patches 0002/0003 follow if 0001 is accepted. **Performance / Android mitigation**: libde265 at the pinned v1.0.15 tag has SSE4.1 kernels only (nothing beyond SSE4.1 exists) (no NEON in that release; later upstream work adds NEON and is tracked by WI-2.168) — on arm64 (Apple Silicon, Android) it runs scalar: ≈ 15–30 fps at 1080p Main with 4 WPP/tile threads [estimate, Appendix B.1]. Mitigations (normative): (1) `de265_start_worker_threads(ctx, min(4, cores))` — WPP/tile parallelism works only if the stream has WPP or tiles; (2) GOP-local decode with caching (7.5.3) so a scrub within a GOP decodes each frame once; (3) `DE265_DECODER_PARAM_DISABLE_DEBLOCKING/SAO` are **not** used to speed up (bit-exactness); (4) Android build passes `-O3 -ffast-math=off -march=armv8-a`; (5) the budget §0.7.4 (≤ 1.5 s per GOP ≤ 60 frames at 1080p) applies to the M1; on Android the GUI must show progress and allow cancellation; (6) WI-2.168 evaluates adding NEON to libde265's `fallback-*` kernels as a separate upstream contribution (not blocking).

### 7.6.3 `ovt-plugin-ffmpeg` — H.264 analysis (public API) and playback decoders for all codecs

| Item | Value |
|---|---|
| Upstream | FFmpeg, https://ffmpeg.org — pin **n7.1.1** (≥ 7.1 is the project minimum per chapter 4 NFRs and chapter 13; the features this plugin uses are older: `AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS` since 4.3, `AV_FRAME_DATA_DOVI_METADATA` since 5.0, `AV_CODEC_FLAG_COPY_OPAQUE` since 6.0 — 6.0 is the hard floor the plugin compiles against; 7.1 is pinned for the libdav1d/VideoToolbox/MediaCodec fixes listed in chapter 13 and so that one FFmpeg build serves every chapter; the pin is recorded in `third_party/VERSIONS.md`; moving to 8.x requires re-running §7.7 golden tests). Built from source via `third_party/ffmpeg.cmake` (configure). |
| Licence | LGPL-2.1-or-later build (`--disable-gpl`, `--disable-nonfree`) for the default download; a GPL build variant (`--enable-gpl`, e.g. for libx264 in a future encoder feature) is allowed since the app is GPL-3 (ADR-002). Plugin code: LGPL-2.1-or-later. H.264/HEVC decoders are in this plugin so the "no pooled codecs" variant can omit it (ADR-015); AV1 playback (libdav1d) is also in this plugin, so the pooled-codec-free variant ships an FFmpeg build with `--disable-decoder=h264,hevc` and the AVC/HEVC decoders arrive via a separately downloadable `libovt_plugin_ffmpeg_full`. |
| Configure (desktop) | `--disable-shared --enable-static --enable-pic --disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-avfilter --disable-encoders --disable-muxers --enable-demuxer=mov,matroska,webm_dash_manifest,mpegts,ivf,h264,hevc,av1,obu,yuv4mpegpipe,rawvideo,dash,hls --enable-decoder=h264,hevc,av1,libdav1d,vp9,mpeg2video,rawvideo --enable-parser=h264,hevc,av1,vp9 --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,av1_frame_split,extract_extradata,trace_headers,filter_units --enable-libdav1d --enable-zlib` + macOS `--enable-videotoolbox`; Linux `--enable-vaapi --enable-libdrm`; Android `--enable-mediacodec --enable-jni --target-os=android --arch=aarch64|x86_64 --enable-cross-compile --cc=$NDK/.../clang`. `trace_headers` is kept so the test oracle (§7.7.1) can run in-process. |
| Caps (analysis) | `codec = H264`, `plane_stages = Reconstruction`, `grid_fields` = bits 0 (block_id, MB granularity), 1 (partition_id: **n/a** → not set), 2 (pred: Intra/Inter/Skip only), 3 (qp), 6, 7 (mv0/mv1), 8/9 (ref0/ref1 = list only, see below), others not set; `has_symbol_accounting = 0`, `bit_exact = 1`, `supports_threads = 1`. With the phase-4 patch (below): bits 1, 4, 5, 8, 9 (true ref_idx), 10, 12 added. |

**Analysis decoder driving (H.264).** `create`: `avcodec_find_decoder(AV_CODEC_ID_H264)`, `avcodec_alloc_context3`, `ctx->export_side_data = AV_CODEC_EXPORT_DATA_MVS | AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS`, `ctx->thread_count = threads`, `ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE` for the **analysis** decoder (frame threading is fine: the exported tables are per picture — Appendix D.1 — but it delays output by up to `thread_count − 1` frames and the side data is only available when a frame is received, so `push` may return no frame for a while and the host drains with `avcodec_send_packet(NULL)` at GOP end (§7.5.3); the playback decoders of this plugin use `FF_THREAD_FRAME` only, slice threading brings nothing there), **no `ctx->extradata`**: with an `avcC`/`hvcC` extradata FFmpeg's `h264`/`hevc` decoders switch to length-prefixed (`is_avc`/`nal_length_size`) packet parsing and would mis-split Annex B packets, so the plugin never sets extradata and instead relies on the host, which always pushes the active SPS/PPS (and VPS) as units before the first slice of a GOP-local decode (7.5.3 A2) — the host delivers **raw NAL units** (no length prefix, no start code), and the plugin prepends `00 00 00 01` to each and feeds Annex B packets, `ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL`, `avcodec_open2`. `push`: accumulate NALs of one access unit (the host pushes one NAL per call; AU boundary = `decode_index_hint` changes or `flush`), build an `AVPacket` (`pkt->pts = decode_index_hint`, `pkt->opaque` likewise with `AV_CODEC_FLAG_COPY_OPAQUE` set so `frame->opaque` carries the decode index), `avcodec_send_packet`, then `avcodec_receive_frame` loop → `FrameAnalysis` from the frame's side data. Hardware acceleration is **never** enabled for the analysis decoder (`hwaccel` would suppress the tables). `flush`: `avcodec_send_packet(NULL)` + drain.

**Exact mapping of side data → model.**

| FFmpeg source | OVT target | Conversion |
|---|---|---|
| `AVFrameSideData AV_FRAME_DATA_MOTION_VECTORS` → `AVMotionVector mv[]` (`libavutil/motion_vector.h`): `source` (−1 = past/L0, +1 = future/L1 — sign only, **not** the ref index), `w`, `h`, `src_x`, `src_y`, `dst_x`, `dst_y` (block **centre** in the current frame), `flags` (always 0 for h264), `motion_x`, `motion_y`, `motion_scale` (= 4 for H.264: quarter-sample) | `MotionVector{block_id, list = (source < 0) ? 0 : 1, ref = list (see RefSlot note), rect = {dst_x − w/2, dst_y − h/2, w, h}, mv_x = motion_x * (16 / motion_scale), mv_y = motion_y * (16 / motion_scale), mvd_x/y = −32768 (n/a), predictor = "", flags = (both lists present for the same rect) ? bit0 : 0}`; `grid.mv0_x/y` for cells inside `rect` when list 0, `mv1_*` for list 1 | `16/motion_scale = 4` → integer multiply, never divide `src_x − dst_x` (it is truncated). Intra MBs produce no vectors → cells keep 0 and `ref0 = ref1 = −1`. Partition granularity: FFmpeg emits 1 (16×16), 2 (16×8 / 8×16) or 4 (8×8, at quadrant centres) vectors per MB per list; sub-8×8 partitions (8×4, 4×8, 4×4) are **collapsed** to the 8×8 vector (Appendix D.1) → `Block.kind = SubMB` nodes are emitted only for 8×8 with `derived "sub-partition MVs not available"`. Field/MBAFF: for field-coded macroblocks FFmpeg reports the vertical MV in field units (the `AVMotionVector` of a field MB covers alternate frame rows) and scales `motion_y` accordingly only for 16×8/8×16 field partitions of **interlaced** pictures; the plugin stores what FFmpeg gives and sets `flags` bit4 (field) when `frame->flags & AV_FRAME_FLAG_INTERLACED`, with Diagnostic Info `ANA-H264-0001 "field MV semantics approximate"`. |
| `AV_FRAME_DATA_VIDEO_ENC_PARAMS` → `AVVideoEncParams` (`libavutil/video_enc_params.h`): `type = AV_VIDEO_ENC_PARAMS_H264`, `qp` (= `26 + pic_init_qp_minus26 + QpBdOffsetY` of the active PPS), `delta_qp[1][0]` (= `chroma_qp_index_offset`), `delta_qp[2][0]` (= `second_chroma_qp_index_offset`), `nb_blocks`, blocks via `av_video_enc_params_block(par, i)` → `AVVideoBlockParams{src_x, src_y, w = 16, h = 16, delta_qp}` (one per MB, raster order) | `grid.qp[cell] = par->qp + block->delta_qp − QpBdOffsetY` for the 16 cells of the MB; `Block{kind = MB, rect = {src_x, src_y, 16, 16}, qp = same}`; `grid.block_id` = that MB; `FrameAnalysis.frame.qp_base = par->qp` is **not** used (the host already has `SliceQPY`); `qp_hist` from the MB QPs | `delta_qp` is `int32_t`; FFmpeg's values **include** `QpBdOffsetY` (n7.1.1 `h264_ps.c`: `pps->init_qp = pic_init_qp_minus26 + 26 + 6·(bit_depth_luma − 8)`; `par->qp = init_qp`; `qscale_table` holds QP′Y), so the plugin stores `grid.qp = par->qp + block->delta_qp − 6·(bit_depth_luma − 8)` = `QP_Y` in `−QpBdOffsetY..51`, matching libde265's convention of QP without `QpBdOffset` (V-028); `bit_depth_luma` from `av_pix_fmt_desc_get(frame->format)->comp[0].depth` |
| MB classification (derived, no side data): a MB with ≥ 1 MV → `pred = Inter`; with none and `qp` present → `Intra`; FFmpeg exports skip/direct MBs as inter with derived MVs | `grid.pred`, `Block.pred`; `Block.skip` **n/a** (false), `merge_or_direct` n/a | Diagnostic Info `ANA-H264-0002 "mb_type unavailable: intra/inter inferred from motion vectors"` once per frame |
| `RefSlot` | `refs[] = [{index 0, name "L0", frame = host's declared_refs L0[0] … }, {index 1, name "L1"}]` — two **pseudo-slots** because the side data carries only the direction; `is_long_term` n/a; `usage_cells` counted | The GUI's reference-index map shows "L0 / L1 (ref_idx n/a)" for this plugin |
| `AVFrame` planes (`data[0..2]`, `linesize[]`, `format`) | `planes[]` stage Reconstruction | `sample_bytes` from `av_pix_fmt_desc_get(format)->comp[0].depth > 8 ? 2 : 1`; width/height = `frame->width/height` (**cropped** by libavcodec unless `ctx->apply_cropping = 0`: the plugin sets `apply_cropping = 0` so planes are in coded-picture coordinates like the grid, and reports the crop rect separately) |

**Limitations of the phase-1 mapping (shown as "n/a")**: sub-8×8 MVs, `ref_idx` per partition (only L0/L1 direction), `mb_type`/`sub_mb_type` (partition shapes below 16×16 are only implied by the MV geometry), intra 4×4/8×8/16×16 prediction modes and chroma mode, `coded_block_pattern`, `transform_size_8x8_flag`, `mb_qp_delta` (only the final QP), `mvd`, slice id per MB (`partition_id`), weighted-prediction tables, deblocking bS, bits per MB, prediction/residual/pre-deblock planes. The Selection Info panel groups them under "requires FFmpeg MB-params patch (phase 4)".

**Phase-4 upstream patch spec (`plugins/ovt-plugin-ffmpeg/patches/0001-h264-export-mb-params.patch`, WI-4.100..4.103).** Goal: a new frame side-data type carrying per-MB syntax, modelled on `AVVideoEncParams` (self-describing header + array of fixed-size block records, accessed by an inline helper). Design (to be adjusted during review on ffmpeg-devel):

```c
/* libavutil/video_block_params.h (new) */
enum AVVideoBlockParamsType { AV_VIDEO_BLOCK_PARAMS_H264 = 0 };
typedef struct AVVideoBlockParamsHeader { unsigned int nb_blocks; size_t blocks_offset; size_t block_size; enum AVVideoBlockParamsType type; int plane_w, plane_h; /* in blocks */ } AVVideoBlockParamsHeader;
typedef struct AVH264MacroblockParams {
    int32_t src_x, src_y;            /* luma position, coded picture */
    uint8_t  slice_id;               /* H264Picture: h->slice_table[mb_xy] */
    uint8_t  mb_type_class;          /* 0 I_NxN, 1 I_16x16, 2 I_PCM, 3 P_16x16, 4 P_16x8, 5 P_8x16, 6 P_8x8, 7 P_Skip, 8 B_Direct_16x16, 9 B_16x16, 10 B_16x8, 11 B_8x16, 12 B_8x8, 13 B_Skip (from mb_type bitmask MB_TYPE_*) */
    uint8_t  sub_mb_type[4];         /* 0 8x8, 1 8x4, 2 4x8, 3 4x4 (+ bit 4 direct) from sub_mb_type[] */
    uint8_t  pred_dir[4];            /* per 8x8: bit0 L0, bit1 L1 (MB_TYPE_P0L0 … P1L1 flags) */
    int8_t   ref_idx[2][4];          /* per list, per 8x8: h->cur_pic.ref_index[list][...], -1 none */
    int8_t   intra4x4_pred_mode[16]; /* -1 n/a; from sl->intra4x4_pred_mode_cache at MB end (needs persistence) */
    int8_t   intra8x8_pred_mode[4];
    int8_t   intra16x16_pred_mode;   /* -1 n/a; from mb_type & MB_TYPE_INTRA16x16 → sl->intra16x16_pred_mode */
    int8_t   intra_chroma_pred_mode; /* sl->chroma_pred_mode */
    uint8_t  cbp;                    /* h->cur_pic.cbp_table? → currently sl->cbp per MB: persist in a per-picture table */
    uint8_t  transform_8x8;          /* MB_TYPE_8x8DCT */
    uint8_t  mb_field_decoding_flag; /* MB_TYPE_INTERLACED */
    int8_t   qp_delta;               /* mb_qp_delta as signalled (persist) */
    int16_t  mvd[2][16][2];          /* signalled mvd per 4x4 per list (from sl->mvd_cache at MB end; CABAC path keeps it, CAVLC path must store it) */
} AVH264MacroblockParams;
AVVideoBlockParamsHeader* av_video_block_params_alloc(enum AVVideoBlockParamsType, unsigned nb_blocks, size_t* out_size);
AVVideoBlockParamsHeader* av_video_block_params_create_side_data(AVFrame*, enum AVVideoBlockParamsType, unsigned nb_blocks);
static inline void* av_video_block_params_block(AVVideoBlockParamsHeader* par, unsigned idx);
/* libavutil/frame.h: AV_FRAME_DATA_VIDEO_BLOCK_PARAMS */
/* libavcodec/avcodec.h: AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS (1 << 5) ; option name "block_params" in options_table.h */
```

Files/functions to touch (Appendix D.1): `libavcodec/h264dec.h` (`H264Picture`: add `int8_t *intra_pred_mode` (16 per MB), `uint8_t *cbp_table`, `int8_t *qp_delta_table`, `int16_t (*mvd_table[2])[2]`, allocated in `alloc_picture()` in `libavcodec/h264dec.c` only when `avctx->export_side_data & AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS`), `libavcodec/h264_mb.c`/`h264_cavlc.c` (`ff_h264_decode_mb_cavlc`) and `h264_cabac.c` (`ff_h264_decode_mb_cabac`) (write the new tables at MB end, after `write_back_intra_pred_mode`/`write_back_motion`), `libavcodec/h264_slice.c` (nothing — `slice_table` already exists), `libavcodec/h264dec.c` `finalize_frame()`/`h264_export_frame_props`-adjacent export function `h264_export_block_params()` that iterates `mb_xy` and fills `AVH264MacroblockParams` from `cur_pic.mb_type[]`, `ref_index[]`, `slice_table[]`, `qscale_table[]`, the new tables; `libavutil/video_block_params.{h,c}`, `libavutil/frame.h/.c` (side data name), `libavcodec/options_table.h` (`export_side_data` flag), `libavcodec/version.h` / `libavutil/version.h` minor bumps, `doc/APIchanges`, `tests/fate` (a FATE test comparing the new side data of `h264-conformance` samples via `ffprobe -show_frames`). The plugin consumes the side data into `AvcMb{mb_type, sub_mb_type[4], transform_8x8, cbp, mb_qp_delta, intra4x4[16], intra8x8[4], intra16x16, chroma_mode, mb_field, ref_idx[2][4], mvd[2][16], direct_flags, pcm}`, fills `grid.intra_mode_y/c`, true `ref0/ref1` (→ `refs[]` becomes the real `RefPicList0/1` from the host's DPB model), `tx_log2_* = 3` when `transform_8x8` else 2 (inter/intra 4×4) or 4 for I_16x16 DC path (`derived`), `cbf` from `cbp` (bit0 = luma cbp of the owning 8×8, bits 1–2 = chroma cbp != 0), `partition_id = slice_id`. Until merged upstream, the ExternalProject applies the patch; if the patch fails to apply on a version bump the build fails (never silently).

**Playback decoders (ADR-009 playback path, `ovt_decoder_*` of §0.6.1).** Implemented in `plugins/ovt-plugin-ffmpeg/src/playback.cpp` and exposed to `libovt-core` through a second, OVT-private C table `ovt_playback_decoder_v1` resolved by `dlsym` (the `ovt_analysis_decoder_v1` symbol stays the only *analysis* symbol; the playback table is documented in the Architecture chapter as the second plugin entry point — the Data Model/API chapters reference `ovt_Decoder`). Rules:

| Platform | Preferred decoder / hwaccel (`prefer_hw = 1`) | Software fallback | Output pixel formats |
|---|---|---|---|
| macOS | `AVHWDeviceType AV_HWDEVICE_TYPE_VIDEOTOOLBOX` via `av_hwdevice_ctx_create` + `ctx->get_format` selecting `AV_PIX_FMT_VIDEOTOOLBOX` for h264/hevc. **AV1 on macOS is software (`libdav1d`) at the n7.1.1 pin**: FFmpeg 7.1.1 has no `av1_videotoolbox` hwaccel (`libavcodec/hwaccels.h`), so `AV_PIX_FMT_VIDEOTOOLBOX` is never offered for AV1; AV1 VideoToolbox decoding (M3+ hardware) arrived in FFmpeg 8.0 and is the upgrade path when the pin moves to 8.x. Frames are **kept on the GPU** when the renderer accepts `CVPixelBufferRef` (libplacebo `pl_map_avframe` / `pl_frame` from IOSurface — Rendering chapter); otherwise `av_hwframe_transfer_data` to `AV_PIX_FMT_NV12` (8-bit) / `AV_PIX_FMT_P010` (10-bit). | native `h264`, `hevc` (NEON), `libdav1d` for AV1 (`ctx->thread_count = 0` auto, `max_frame_delay` default), `vp9` | `yuv420p`, `yuv420p10le`, `yuv422p10le`, `yuv444p`, `yuv444p10le`, `gray`, `gray10le`, `nv12`, `p010le`, `gbrp`, `gbrp10le`; anything else is converted by `libovt-render` (zimg) — the plugin never converts. |
| Android | `h264_mediacodec`, `hevc_mediacodec`, `av1_mediacodec`, `vp9_mediacodec` decoders (NDK `AMediaCodec`, `--enable-mediacodec --enable-jni`; `av_jni_set_java_vm` called from the Kotlin shell at startup). Output to **buffers** (`AV_PIX_FMT_NV12`) for 8-bit streams — not to a `Surface`, because the renderer needs pixel access (A/B, scopes). **10-bit HEVC/AV1:** FFmpeg n7.1.1's MediaCodec wrapper has no `COLOR_FormatYUVP010` mapping (`mediacodec_wrapper.c`), so buffer-mode P010 output is unavailable at this pin; the plugin uses the surface path (`AVMediaCodecDeviceContext.surface`, display-only: A/B and scopes disabled for that clip with an info banner) when the renderer requests display only, else falls back to the software decoder. Option `ctx->opts "ndk_codec" = 1`. | native `h264`/`hevc` (NEON), `libdav1d` | as above |
| Linux | `AV_HWDEVICE_TYPE_VAAPI` (`/dev/dri/renderD128`, `get_format` → `AV_PIX_FMT_VAAPI`; `av_hwframe_transfer_data` → `nv12`/`p010`; DRM-PRIME zero-copy when the renderer supports `pl_map_avframe`) | native decoders, `libdav1d` | as above |
| Any | `prefer_hw = 0` → software only. Every open failure of a hardware path logs level 2 and falls back once; the chosen path is reported in `ovt_Decoder`'s `info()` (`"h264 / videotoolbox"`) for the status bar. | | |

Seeking (`ovt_decoder_seek(display_index)`): map `display_index → decode_index` via `StreamIndex.display_order`, `R = rap_before(decode_index)`, `av_seek_frame`/`avformat_seek_file` to the sample of `frames[R]` (`AVSEEK_FLAG_BACKWARD` by the frame's `dts`), `avcodec_flush_buffers`, decode and discard frames until `frame->opaque`/pts matches the target's pts (frames before the target are dropped; `ovt_decoder_next` returns the target first). H.265 RASL after a CRA start are dropped by libavcodec automatically. Output `ovt_Picture` carries `FrameId` resolved by matching `pts` to `StreamIndex` frames (`pts` is unique per shown frame; AV1 hidden frames are never output by playback decoders).

**Filling `ovt_ColorDescription` (playback and analysis).**

| Source | Target field | Conversion |
|---|---|---|
| `AVCodecContext.color_primaries` / `AVFrame.color_primaries` (`AVColorPrimaries`, H.273 codes) | `primaries` | identity (FFmpeg uses H.273 numbering; `AVCOL_PRI_UNSPECIFIED = 2`) — frame value wins over context when ≠ 2 |
| `color_trc` (`AVColorTransferCharacteristic`) | `transfer` | identity (`AVCOL_TRC_SMPTE2084 = 16`, `AVCOL_TRC_ARIB_STD_B67 = 18`) |
| `colorspace` (`AVColorSpace`) | `matrix` | identity (`AVCOL_SPC_BT2020_NCL = 9`, `AVCOL_SPC_ICTCP = 14`) |
| `color_range` | `full_range` | `AVCOL_RANGE_JPEG → 1`, `MPEG → 0`, `UNSPECIFIED → 0` + Diagnostic Info |
| `chroma_sample_location` (`AVChromaLocation`) | `chroma_siting` | `AVCHROMA_LOC_LEFT (1) → 1`, `CENTER (2) → 2`, `TOPLEFT (3) → 3`, others → 0 |
| `pix_fmt` via `av_pix_fmt_desc_get` | `chroma_format`, `bit_depth` | `log2_chroma_w/h`: (1,1)→1 (420), (1,0)→2 (422), (0,0)→3 (444); `nb_components == 1` → 0 (400); `comp[0].depth` → `bit_depth` |
| `AV_FRAME_DATA_MASTERING_DISPLAY_METADATA` → `AVMasteringDisplayMetadata{display_primaries[3][2] (AVRational, RGB order), white_point[2], min_luminance, max_luminance (AVRational cd/m²), has_primaries, has_luminance}` (also available before decoding as `AVPacketSideData AV_PKT_DATA_MASTERING_DISPLAY_METADATA` in `AVCodecParameters.coded_side_data` from `mdcv`/`colr` boxes) | `mdcv_primaries[i][j] = lround(av_q2d(display_primaries[i][j]) * 50000)` (R,G,B order — FFmpeg already reorders the SEI's GBR), `mdcv_white_point`, `mdcv_max_luminance = lround(av_q2d(max_luminance) * 10000)`, `mdcv_min_luminance` likewise | only when `has_primaries`/`has_luminance`; else 0 |
| `AV_FRAME_DATA_CONTENT_LIGHT_LEVEL` → `AVContentLightMetadata{MaxCLL, MaxFALL}` | `max_cll`, `max_fall` | direct (cd/m²) |
| `AV_FRAME_DATA_DYNAMIC_HDR_PLUS` → `AVDynamicHDRPlus` (parsed by FFmpeg from T.35 for hevc/av1 via libdav1d) | `has_hdr10plus = 1`; the full struct is forwarded to the HDR panel as a nested FlatBuffer (`HdrMetadata`, Container/Data Model chapters) | — |
| `AV_FRAME_DATA_DOVI_RPU_BUFFER` / `AV_FRAME_DATA_DOVI_METADATA` (per frame), `AV_PKT_DATA_DOVI_CONF` → `AVDOVIDecoderConfigurationRecord{dv_version_major/minor, dv_profile, dv_level, rpu_present_flag, el_present_flag, bl_present_flag, dv_bl_signal_compatibility_id}` in `AVCodecParameters.coded_side_data` | `has_dolby_vision = 1`, `dv_profile`, `dv_level` | from the stream's `coded_side_data` if present, else from the frame side data presence (profile 0) |
| Fallback when context values are `UNSPECIFIED` | `primaries/transfer/matrix` from the container (`colr`/`av1C`/`Matroska Colour`) or the syntax module's VUI/`color_config` | the host merges: frame side data > codec context > container > VUI; conflicts produce Diagnostic Warning `DEC-HDR-0001 "colour signalling differs between container (%s) and bitstream (%s)"` |

### 7.6.4 `ovt-plugin-hm` — HEVC oracle (HM reference decoder)

| Item | Value |
|---|---|
| Upstream | HM (JCT-VC HEVC reference software), https://vcgit.hhi.fraunhofer.de/jvet/HM — pin tag **HM-18.0** (re-check the tag name at WI-4.104; SCM branch for SCC is **not** pinned in phase 4 — SCC streams are decoded by HM-18.0 only if it supports them; else `OVT_E_UNSUPPORTED`). |
| Licence | BSD-3-Clause. Plugin code: BSD-3-Clause. Optional component (`OVT_BUILD_PLUGIN_HM=OFF` by default; never in the default download — size and speed). |
| Build (`third_party/hm.cmake`) | OVT's own CMake list `third_party/hm/CMakeLists.txt` compiles HM's unmodified library sources `source/Lib/TLibCommon` (HM compiles `libmd5` into it), `TLibDecoder` and `Utilities` (HM-18.0 has no `TLibVideoIO`; `TVideoIOYuv` lives in `Utilities`) as static libraries with `RExt__HIGH_BIT_DEPTH_SUPPORT=1` set publicly (16-bit internal); `ENC_DEC_TRACE` stays at HM's hard-coded default (`#define ENC_DEC_TRACE 0` in `TypeDef.h` is not under `#ifndef`, so passing it only redefines the macro); HM's own `CMakeLists.txt` is not used because it forces `-msse4.1` on every UNIX target, warnings-as-errors, OpenMP and ccache, and HM already sets `CMAKE_POSITION_INDEPENDENT_CODE`, so no `-fPIC` patch is needed; **no patches** to HM (issue #345). The plugin includes HM headers (`TDecTop.h`, `TComPic.h`, `TComDataCU.h`, `TComSlice.h`, `NALread.h`). |
| Caps | `codec = H265`, `plane_stages = Reconstruction | Coefficients` (HM applies deblocking and SAO in place; pre-filter, prediction and residual planes are **n/a** in this plugin), `grid_fields` = all bits except 13 (`bits_x8`: `TComDataCU::getTotalBits()` exists but is populated only in the encoder; n/a) and 14 (deblock bS: HM computes bS transiently in `TComLoopFilter::xGetBoundaryStrengthSingleCU` — n/a), `has_symbol_accounting = 0`, `bit_exact = 1`, `supports_threads = 0`. |

**Decoder driving.** `create`: `TDecTop dec; dec.create(); dec.init(); dec.setDecodedPictureHashSEIEnabled(0); dec.setTargetDecLayer(0)`. `push`: wrap the NAL bytes in an `InputNALUnit` (`read(nalu)` after `nalu.getBitstream().getFifo()` is filled with the raw bytes — HM expects the NAL **without** start code and performs its own EPB removal), `bNewPicture = dec.decode(nalu, m_iSkipFrame, m_iPOCLastDisplay)`; when `bNewPicture` is true the NAL must be re-fed after `dec.executeLoopFilters(poc, pcListPic)` (standard `TAppDecTop::decode` loop — copy that control flow exactly, including `setFirstSliceInPicture`, `checkNoOutputPriorPics` and `xFlushOutput`). After loop filters, walk `pcListPic` for pictures with `getReconMark()` not yet exported; map each `TComPic` (below). `decode_index` is tracked by the plugin by counting pictures in decode order and asserting equality with the hint carried in a side map keyed by the slice's POC (HM has no `user_data`; the plugin keeps `std::map<int poc, int decode_index>` filled at `push` from the host hint). `flush`: `dec.decode` with an empty NAL list is not supported → call `executeLoopFilters` for the pending picture and export it.

**Mapping via public `TComDataCU` getters (per CTU `pcPic->getCtu(ctuRsAddr)`, per partition index `uiAbsPartIdx` in Z-scan of 4×4 units; convert with `g_auiRasterToZscan`/`g_auiZscanToRaster`):** `getDepth(idx)` → CU size `CtbSizeY >> depth` and CU set; `getPredictionMode(idx)` (MODE_INTER 0, MODE_INTRA 1, MODE_NONE) → `pred`; `getSkipFlag(idx)` → `Skip`; `getPartitionSize(idx)` (`SIZE_2Nx2N` … `SIZE_nRx2N`) → `partition_type` (same numbering as PartMode); `getMergeFlag(idx)`, `getMergeIndex(idx)`, `getInterDir(idx)` (1 L0, 2 L1, 3 Bi), `getCUMvField(REF_PIC_LIST_0/1)->getMv(idx)` (`TComMv.getHor()/getVer()` quarter-sample → ×4), `->getRefIdx(idx)`, `->getMvd(idx)` → `MotionVector` (`predictor = merge ? "merge[idx]" : "amvp[getMVPIdx(list, idx)]"`), ref POC via `pcPic->getSlice(sliceIdx)->getRefPOC(list, refIdx)` and `getRefPic(...)->getIsLongTerm()`; `getIntraDir(CHANNEL_TYPE_LUMA/CHROMA, idx)` → `intra_mode_y/c` (chroma `DM_CHROMA_IDX (36)` → resolve to luma mode as the spec's mode 4); `getQP(idx)` → `qp`; `getCbf(idx, COMPONENT_Y/Cb/Cr, trDepth)` and `getTransformIdx(idx)` → TU tree and `cbf` bits; `getTransformSkip(idx, comp)`; `getIPCMFlag(idx)` → PCM; `getCUTransquantBypass(idx)`; `getChromaQpAdj(idx)`; `getExplicitRdpcmMode(comp, idx)`; `getCrossComponentPredictionAlpha(idx, comp)` (RExt); `getCodedQP`; slice index from `pcPic->getPicSym()->getCtuTsToRsAddrMap` + `TComSlice* getSlice(getCtu(..)->getSlice())`; SAO from `pcPic->getPicSym()->getSAOBlkParam(ctuRsAddr)` (`SAOBlkParam[comp]{modeIdc, typeIdc, typeAuxInfo, offset[4]}`) → `filter_idx` and the `HevcCtu` SAO fields (`SaoTypeIdx`, `SaoOffsetVal`, `sao_band_position`, `sao_eo_class`); planes from `pcPic->getPicYuvRec()->getAddr(comp)`, `getStride(comp)` (`Pel` = int16 in HM for every bit depth; `bit_depth = getSlice()->getSPS()->getBitDepth(ch)`. For 8-bit pictures the plugin **packs** each plane to `uint8_t` (one narrowing copy per plane) and reports `sample_bytes = 1`, so V-023 (`sample_bytes == 1 ⇔ bit_depth == 8 && !is_signed`, chapter 6) holds; for bit depth > 8 it copies the `Pel` rows with `sample_bytes = 2`). `Coefficients`: `getCoeff(comp)` per CU (dequantised levels are not kept; the signalled levels are) → `PlaneStage::Coefficients` as int32 **levels** with `derived "levels, not dequantised"`.

**Verify mode** (`ovt analyze --verify-with hm`, also the GUI "Verify" action; WI-4.106): decode the same frame with the primary plugin (`de265`) and with `hm`; compare (1) reconstruction planes sample-by-sample (any difference → Diagnostic Error `VER-0001 "reconstruction mismatch at (x,y) plane p: a != b"` with up to 100 positions and a mismatch heat-map blob), (2) every `CellGrid` vector both plugins announce (per-cell equality; `qp`, `pred`, `intra_mode_*`, `mv*`, `ref*` (compared through resolved `RefSlot.frame`), `tx_log2_*`, `cbf`, `filter_idx`) → `VER-0002 "<field> differs in N cells"`, (3) block trees (same leaf rectangles and kinds) → `VER-0003`, (4) the stream's `decoded_picture_hash` SEI (when present) against each plugin's reconstruction (MD5/CRC/checksum per H.265 §D.3.19, computed over the decoded picture **before cropping**, per colour component, with the sample packing rules of D.3.19: 8-bit samples as one byte, samples wider than 8 bits as two bytes little-endian) → `VER-0004 "decoded picture hash mismatch (plugin %s)"`. Output: `VerifyReport` FlatBuffer (Data Model chapter) + JSON. HM is 10–100× slower than libde265 (≈ 1–5 fps 1080p, Appendix B.1); verify runs are explicit user actions with progress and cancellation, never automatic, and `hm` is never the default plugin for H265 unless `de265` is absent or returns `OVT_E_UNSUPPORTED` (RExt/SCC).

### 7.6.5 dav1d — playback only

dav1d (BSD-2-Clause, https://code.videolan.org/videolan/dav1d, pin 1.5.x as configured by FFmpeg's `--enable-libdav1d`) is used **exclusively** through FFmpeg's `libdav1d` wrapper in `ovt-plugin-ffmpeg` for AV1 playback (and as the software fallback when hardware AV1 is unavailable). **OVT does not fork dav1d, does not patch dav1d and does not link dav1d directly** (ADR-008; Appendix B.1 and F.3: the YUView analyzer fork rotted; the maintainers reject hot-path analyzer code). The only dav1d-related work item is WI-4.108: a *design proposal* for an upstream zero-cost inspection callback (modelled on libaom's `AV1_SET_INSPECTION_CALLBACK`), posted to the VideoLAN GitLab; OVT would adopt it as a second AV1 analysis plugin only after it is merged upstream and released. Frame-header syntax that dav1d exposes (`Dav1dFrameHeader` on each picture) is **not** used — the `syntax` module is the single source of header data (ADR-007).

---

## 7.7 Test oracles for this chapter

All oracles are **test-time** tools (CI and developer machines); none is linked into OVT products. `tools/fetch_corpora.py` downloads the corpora listed in `tests/corpora/*.json` (URL + SHA-256) into `tests/corpora/cache/`, each archive into its own directory; golden files generated by the oracles are committed under `core/tests/golden/<codec>/<stream>.<oracle>.txt.zst` so CI does not need the oracle binaries (a nightly job regenerates and diffs them).

### 7.7.1 FFmpeg `trace_headers` (syntax trees for H.264 / H.265 / AV1)

Command: `ffmpeg -hide_banner -loglevel trace -i <file> -map 0:v:0 -c:v copy -bsf:v trace_headers -f null - 2> trace.txt` (for raw Annex B / IVF / OBU files add `-f h264|hevc|ivf|obu` before `-i`). Output format (per unit): a header line naming the structure (`Sequence Parameter Set`, `Picture Parameter Set`, `Slice Header`, `Supplemental Enhancement Information`, AV1 `Sequence Header`, `Frame Header`, `Tile Group`, `Metadata`, …) followed by one line per syntax element: `<bit position> <element name>[<indices>] <bit string> = <value>` (CBS `cbs_trace_syntax_element`: position is the bit offset **from the start of the unit's RBSP including the NAL header** (H.26x) or **from the OBU header bit 0** (AV1) — the same convention as OVT's `BitSpan`; the bit string shows the consumed bits; indices appear as `[i]`, `[i][j]`). Elements whose value is inferred (not read) are not printed.

Diff procedure (`tools/trace_diff.py`, WI-1.126): (1) run `ovt syntax <file> --trace` which prints the OVT tree in the **same line format** (`bit_offset name[indices] bits = value`, recursing in document order, skipping `derived` nodes and nodes with `bit_length == 0`); (2) normalise both sides: drop CBS header/footer lines, drop FFmpeg log prefixes (`[trace_headers @ 0x…]`), collapse whitespace, map the few CBS-specific names to spec names via `tools/trace_names.json` (e.g. CBS prints `nal_unit_header()` fields with their spec names already; AV1 `obu_header` fields identical; known differences: CBS `payload_type`/`payload_size` vs spec `last_payload_type_byte` — the mapping table lists each), and drop CBS "`...`" lines for skipped data; (3) `difflib.unified_diff`; the test passes when the diff is empty for the **first VCL unit of each frame and every non-VCL unit**. A mismatch in a bit position is a parser bug (not a naming issue) and MUST be fixed, not mapped away.

### 7.7.2 GStreamer `codecparsers` (struct-level cross-check)

`gst-plugins-bad` ≥ 1.24, library `gstcodecparsers-1.0` (LGPL-2.1+, test dependency only, found via `pkg-config`; the test is skipped when absent). Harness `core/tests/syntax/gst_crosscheck.cpp` (WI-1.127): for each unit, call `gst_h264_parser_parse_sps/pps/slice_hdr/sei`, `gst_h265_parser_parse_vps/sps/pps/slice_hdr/sei`, `gst_av1_parser_parse_sequence_header_obu/frame_header_obu/tile_group_obu/metadata_obu` and compare every scalar field of the GStreamer struct with the OVT typed struct by name (a generated table `gst_field_map.inc` lists `(gst struct, gst field, ovt struct, ovt field)` triples — ~600 entries; array fields compared element-wise). GStreamer does not expose bit offsets; this oracle checks **values** and the derived variables GStreamer computes (`GstH265SPS.width/height`, `GstAV1FrameHeaderOBU.upscaled_width`, `GstAV1TileInfo.mi_col_starts`, `GstH264SliceHdr.max_pic_num`, `GstH265SliceHdr.NumPicTotalCurr`, …).

### 7.7.3 JM `ldecod -DTRACE` (`trace_dec.txt`, H.264)

JM 19.0 (https://iphome.hhi.de/suehring/tml/; custom ITU/ISO reference-software licence, test use only). Build with `-DTRACE=1` (in `ldecod/inc/defines.h`: `#define TRACE 1`); run `ldecod -i stream.264 -o /dev/null`; it writes `trace_dec.txt` with one line per syntax element including slice data: `@<bitpos> <name padded> <bitstring padded> (<value>)`. Use: cross-check H.264 **slice headers** and SEI (elements up to and including `slice_data()` start) — `tools/trace_diff.py --oracle jm` converts JM lines to the normalised format (JM's names are mostly spec names; mapping table `tools/trace_names_jm.json`; JM bit positions count from the start of the NAL *payload* **after** the header byte in RBSP bits → add 8). Also the reference for POC values: JM prints `POC: %d` per picture in its log (`-p` to `ldecod.cfg` `POCScale`); the golden test `h264_poc_vs_jm` compares `FrameInfo.poc` per decode index with JM's printed POC for all JVT conformance streams (JM's `-p 2` POC scale is set to 1 for the comparison).

### 7.7.4 HM `ENC_DEC_TRACE` (H.265)

HM (BSD-3) built with `-DENC_DEC_TRACE=1` (`TypeDef.h`) writes `TraceDec.txt` (`g_hTrace`) containing every syntax element with bit positions in the form `<name> <value>` grouped by `========= Slice =========` markers; CABAC bins are also traced (`DEBUG_CABAC_BINS`). Use: cross-check HEVC slice segment headers, `st_ref_pic_set` derivations (`HM prints "RPS" lists`) and the RPS/RefPicList contents per picture (`TraceDec` prints `RefPicList0/1 POCs`) → golden test `h265_refpiclist_vs_hm` compares `declared_refs` (resolved to POC) for the JCT-VC conformance suite. Also the decoded picture hash check of HM (`--SEIDecodedPictureHash 1` prints `[MD5:OK]`) is the reference for OVT's hash verification implementation. For CU/PU/TU **grid** parity the oracle is not the trace but `tests/oracles/hm_cu_dump.cpp` (chapter 14 §14.4.4): a small program linked against HM's `TLibDecoder` that walks `TComDataCU` getters per picture and writes `tests/golden/h265/<id>.hm-grid.json`; `TraceDec.txt` is kept only as a secondary artefact (first 2 frames, `<id>.hm-trace.txt`) for header/RPS cross-checks.

### 7.7.5 libaom `inspect` JSON (AV1 block data)

Built with the pinned libaom (v3.12.1) and `-DENABLE_EXAMPLES=1` in the *test* build (`third_party/libaom.cmake` option `OVT_BUILD_AOM_INSPECT=ON`), `examples/inspect` prints a JSON array with one object per decoded frame (`[`, each object followed by `,` and a newline, `null`, `]`; `show_existing_frame` outputs produce no object). The frame-level keys, the per-MI layer keys with their `"<name>Map"` enum objects (`blockSize`, `transformSize`, `transformType`, `dualFilterType`, `mode`, `uv_mode`, `motion_mode`, `compound_type`, `skip`, `filter`, `cdef_level`, `cdef_strength`, `cfl_alpha_idx`, `cfl_alpha_sign`, `delta_q`, `seg_id`, `motionVectors`, `intrabc`, `palette`, `uv_palette`, `referenceFrame`), the accounting keys `symbolsMap`/`symbols` and the value quirks are specified **once** in chapter 6 §6.8.3 and are not repeated here; there is **no** per-superblock bits key (`sb`) in stock libaom, so AV1 `Block.bits_x8` comes from accounting only. Command: `inspect -A stream.ivf > frames.json` (`-A`/`--all` enables every layer including accounting; RLE compression `-x` is not used). Golden test `aom_grid_vs_inspect` (WI-2.161): for each frame, compare OVT `CellGrid` vectors cell by cell with the JSON layers after unit conversion (OVT 1/16-pel MVs ÷ 2 = libaom 1/8-pel; enum identities from the §7.6.1 tables; `transformType` compared only for skipped blocks or with the OVT extension patch, §7.6.1) and `symbols` totals per `(x4,y4,id)`.

### 7.7.6 Conformance corpora (`tests/corpora/*.json`)

| Corpus | Content | URL | Use |
|---|---|---|---|
| JVT H.264 conformance (ITU-T H.264.1) | ~250 Annex B streams: Baseline/Main/High/High 10/4:2:2/4:4:4, MBAFF, PAFF, FMO/ASO, MVC/SVC sets | https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/ (mirror of the JVT site; the professional profiles are under `professional_profiles/`, MVC under `MVC/`, SVC under `SVC/`) | syntax trees (7.7.1, 7.7.3), POC, DPB/ref lists, frame boundary; FFmpeg-plugin MV/QP vs `ffprobe` |
| JCT-VC HEVC_v1 conformance (ITU-T H.265.1) | ~150 bitstreams (Main, Main 10, Main Still Picture: `AMP_A_Samsung`, `CAINIT_*`, `DBLK_*`, `ENTP_*`, `NUT_A`, `RAP_A/B`, `RPS_*`, `SAO_*`, `TILES_*`, `WPP_*`, `VPSID_A`, `POC_A`, `PICSIZE_*`, …) with MD5 SEI | https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/ | syntax trees, POC, RPS/RefPicList (7.7.4), de265/HM plugin verify (hash SEI), frame boundaries |
| JCT-VC RExt / SCC / MV-HEVC / SHVC | `RExt/`, `SCC/`, `MV-HEVC/`, `SHVC/` sibling directories | https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/ | header parsing of extensions; HM plugin coverage; headers-only indexing for multi-layer |
| AOM AV1 conformance (Argon) | Argon streams: exhaustive coverage of every syntax element and decoder path (profile 0/1/2, Annex B and Section 5, large-scale tile), with per-frame MD5 | https://aomedia.org/av1-conformance/ (registration; also https://storage.googleapis.com/aom-test-data/ for the libaom test vectors `av1-1-b8-*.ivf`, `av1-1-b10-*.ivf`, `av1-1-b8-22-svc-*`, `av1-1-b8-23-film_grain-*`, Annex B `*.obu`) | OBU/sequence/frame header trees (7.7.1 for the libaom test vectors; Argon for exhaustive header coverage), reference slot model, aom plugin grid (7.7.5) |
| AOM `av1-conformance` test vectors (libaom `test/test_vectors.cc` list) | `av1-1-b8-01-size-*`, `av1-1-b8-03-sizeup/sizedown`, `av1-1-b10-00-quantizer-*`, `av1-1-b8-16-intra_only-*`, `av1-1-b8-22-svc-L1T2/L2T1/L2T2`, `av1-1-b8-24-*`, `av1-1-b8-05-mv`, `av1-1-b8-06-mfmv` | https://storage.googleapis.com/aom-test-data/<name> (+ `.md5`) | as above; hidden frames / show_existing (`av1-1-b8-05-mv`, `-06-mfmv`), operating points (`-22-svc-*`), intra-only (`-16-*`) |
| OVT HDR fixtures | the chapter 14 §14.3 synthetic recipes `syn_h265_main10_pq_hdr`, `syn_h265_main10_pq_hdr_mismatch`, `syn_h265_pq_4000nit`, `syn_hlg_1080p`, `syn_av1_pq_hdr_obu`, `syn_av1_hlg_metadata`, the `syn_sei_<codec>_<payload>` family (hand-encoded SEI payloads incl. MDCV/CLL/alternative transfer/GA94) and the public clip `hdr_hdr10plus_dv81` | chapter 14 recipes (deterministic encoder options; chapter 14 is the single fixture registry, D26) | SEI/metadata parsers (7.3.11), `ovt_ColorDescription` filling (7.6.3) |
| FFmpeg FATE samples (`h264-conformance`, `hevc-conformance`, `av1`) | the subset FFmpeg uses in FATE, with FATE reference outputs | https://fate-suite.ffmpeg.org/ (`rsync://fate-suite.ffmpeg.org/fate-suite/`) | reference for the FFmpeg plugin mapping via the `tests/oracles/ffmpeg_mvs.c` dumper of chapter 14 §14.4.3a (`ffprobe` prints only the side-data type and size for `AV_FRAME_DATA_MOTION_VECTORS`, never the vectors) |

---

## 7.8 Work items

All WIs use the §0.1.6 template. Oracle names refer to §7.7; corpus names to §7.7.6. "Golden" means a committed reference output generated once by the oracle and diffed in CI. Scheduling note: WI-1.100–1.107 are phase-0 deliverables (chapter 16 §16.3.11); WI-2.150, WI-2.169, WI-2.171 and WI-2.172 are delivered in phase 1 (milestone per chapter 16 §16.1); their numbering is kept.

### Phase 1 — `io`, `bitreader`, `syntax`, `index`, CLI hooks (WI-1.100 … WI-1.130)

### WI-1.100 — Implement `ovt::io::RandomAccessFile` (mmap and pread backends)
**Goal:** Provide the single bounds-checked file access class of §7.1 for desktop.
**Depends on:** WI-0.000 (repo skeleton).
**Produces:** `core/include/ovt/cxx/io/random_access_file.hpp`, `core/src/io/random_access_file.cpp`, `core/src/io/mmap_file.cpp`, `core/src/io/pread_file.cpp`, `core/tests/io/test_io_random_access_file.cpp`.
**Interface:** §7.1.2.
**Steps:** 1. Write the header exactly as §7.1.2. 2. Implement `MmapFile` (open, fstat, mmap, madvise, view, read). 3. Implement `PreadFile` with the 1 MiB block LRU cache and `EINTR` retry. 4. Implement `open()` with the fallback rule of §7.1.3 and logging. 5. Implement `IoError` and the error rules of §7.1.4. 6. Write the tests of §7.1.5.
**Acceptance tests:** 1. Given a 10 MiB random file, When 10 000 random `read()`s are issued on both backends, Then every result equals `std::ifstream` bytes (`ctest -R io_pread_backend_matches_mmap`). 2. Given `offset == size`, When `read()` is called, Then it returns 0 and does not throw (`io_read_past_eof_returns_zero`). 3. Given a path to a directory, When `open()` is called, Then `IoError{OVT_E_INVALID_ARG}` is thrown. 4. Given 8 threads reading concurrently, When checksums are compared, Then they match (`io_concurrent_reads`).
**Size:** M.
**Notes / pitfalls:** `mmap` of files > 2 GiB on 32-bit is irrelevant (no 32-bit targets) but `size_t`/`off_t` must be 64-bit everywhere (`-D_FILE_OFFSET_BITS=64` on Linux). Never `MAP_POPULATE` (slow on network drives).

### WI-1.101 — Add fd-based open and the Android content URI path
**Goal:** Make `RandomAccessFile::open_fd` and `ovt_file_open_fd` work with descriptors from `ParcelFileDescriptor`.
**Depends on:** WI-1.100.
**Produces:** `core/src/io/random_access_file.cpp` (open_fd), `core/src/api/file_api.cpp` (`ovt_file_open`, `ovt_file_open_fd`, `ovt_file_close`), `android/app/src/main/kotlin/.../FileOpener.kt` (resolve `content://` → fd + size + display name), `core/tests/io/test_io_fd.cpp`.
**Interface:** §7.1.2 `open_fd`; §0.6.1 `ovt_file_open_fd`.
**Steps:** 1. Implement `open_fd` with `fstat` fallback for `size == UINT64_MAX`, ownership flag, `Pread` default. 2. Implement the C API wrappers mapping `IoError` → `ovt_status`. 3. Kotlin: `contentResolver.openFileDescriptor(uri, "r")`, `getStatSize()`, `detachFd()`, pass display name from `OpenableColumns.DISPLAY_NAME`. 4. Tests: open a temp file by fd; verify ownership semantics; verify `UINT64_MAX` size resolution.
**Acceptance tests:** 1. Given an fd of a 1 MiB file and `size = UINT64_MAX`, When `open_fd` is called, Then `info().size == 1 MiB`. 2. Given `take_ownership = true`, When the object is destroyed, Then `fcntl(fd, F_GETFD)` fails with `EBADF`. 3. Given an Android instrumentation test with a `content://` URI from `DocumentsProvider`, When `ovt_file_open_fd` is called, Then `OVT_OK` and reads succeed (`androidTest FileOpenTest`).
**Size:** S.
**Notes / pitfalls:** `getStatSize()` returns −1 for pipes; such URIs must be copied to the cache dir by the shell before opening (out of scope here; the shell shows an error in phase 1). **Overlap:** this WI delivers only the C++ `open_fd` and the C API wrappers; the Kotlin resolver (`content://` → fd, display name) is `FilePicker.kt` of WI-0.051 — no `FileOpener.kt`. See chapter 16 §16.3.

### WI-1.102 — Implement `ovt::BitReader` core descriptors and `ParseError`
**Goal:** Bounds-checked reader for `u/f/b8/i/ue/se/te/me/st`, position API, `more_rbsp_data`, `rbsp_trailing_bits`, and the optional `DiagnosticSink` attachment of §7.2.2.
**Depends on:** none.
**Produces:** `core/include/ovt/cxx/bitreader.hpp`, `core/src/bitreader/bitreader.cpp`, `core/tests/bitreader/test_bitreader.cpp`.
**Interface:** §7.2.2, §7.2.3, §7.2.4.
**Steps:** 1. Header as declared. 2. 64-bit cache refill (byte-wise) + `u(n)` for n ≤ 64 (two-step read for n > 57). 3. `ue/se/te/i/b8/st/me`. 4. `mark/span_since/seek/skip`, `peek`. 5. `more_rbsp_data` with constructor-time `stop_bit_pos_`. 6. `rbsp_trailing_bits`, `expect_f`. 7. `BitReader::DiagnosticSink`, `set_diagnostic_sink`/`diagnostic_sink` (a null sink means recoverable reader warnings are dropped, never thrown). 8. Tests of §7.2.7 (non-AV1 rows).
**Acceptance tests:** 1. Given the Table 9-2 bit strings, When `ue()` is read, Then codeNums 0..7 result (`br_ue_table`). 2. Given a buffer with one bit left, When `u(2)` is called, Then `ParseError{"BR-0001"}` with `bit_offset == size_bits()-1` and `bit_pos()` unchanged (`br_bounds`). 3. Given `C0 00 00`, When `more_rbsp_data()` is called at bit 1, Then it returns false (`br_more_rbsp_data`). 4. Given a 1 KiB random buffer, When `u(n)` for all n is compared with a bit-by-bit reference, Then equal (`br_u_all_widths`). 5. Given a test sink attached with `set_diagnostic_sink`, When the sink is replaced by `nullptr`, Then `diagnostic_sink()` returns `nullptr` and reads behave identically (`br_sink_attach`).
**Size:** M.
**Notes / pitfalls:** `u(0)` must return 0 without reading. `se()` of codeNum 2^32−2 must not overflow (`int64` intermediate). `te(cmax)` with `cmax == 1` reads **one** bit and inverts it.

### WI-1.103 — Add AV1 descriptors to `BitReader`
**Goal:** `leb128`, `uvlc`, `su`, `ns`, `le`, `delta_q`, `trailing_bits`, `byte_alignment`, `L(n)` guard.
**Depends on:** WI-1.102.
**Produces:** `core/src/bitreader/bitreader_av1.cpp`, tests in `core/tests/bitreader/test_bitreader_av1.cpp`.
**Interface:** §7.2.2 (AV1 block).
**Steps:** 1. Implement each function exactly as the comment formulas. 2. Tests `br_leb128`, `br_uvlc`, `br_su_ns`, `br_le`, `br_delta_q`, `br_trailing_bits`.
**Acceptance tests:** 1. Given `FF FF FF FF FF FF FF 7F`, When `leb128()`, Then 2^56−1. 2. Given eight `0x80` bytes read through a `SyntaxBuilder` constructed on the reader (which attaches itself as the reader's `DiagnosticSink`, §7.3.2), When `leb128()` is called, Then the value is 0, 8 bytes are consumed and a Warning diagnostic `BR-0009` is recorded on the current node; and with no sink attached the same call returns 0 and throws nothing (`br_leb128_overlong`). 3. Given `ns(5)` inputs `110`/`111`, When read, Then 3/4. 4. Given the bits `1000000`, When `su(7)` is read, Then the value is −64.
**Size:** S.
**Notes / pitfalls:** `uvlc()` with 32 leading zeros returns `(1<<32)−1` **without** reading the value bits (AV1 §4.10.3). `le(n)` requires byte alignment.

### WI-1.104 — Implement `rbsp_from_nal` and raw↔RBSP offset mapping
**Goal:** EPB removal with position recording and the two mapping functions of §7.2.5.
**Depends on:** none.
**Produces:** `core/include/ovt/cxx/rbsp.hpp`, `core/src/bitreader/rbsp.cpp`, `core/tests/bitreader/test_bitreader_rbsp.cpp`.
**Interface:** §7.2.5.
**Steps:** 1. Implement the loop of §7.2.5 with the `zeros` counter. 2. Implement `rbsp_bit_to_raw_bit` (incremental frontier) and `raw_byte_to_rbsp_byte` (binary search). 3. Implement a test-only reference EPB *inserter*. 4. Tests `rbsp_golden` (vector §7.2.6), `rbsp_mapping_bijection`, random round-trips.
**Acceptance tests:** 1. Given `67 42 00 1E 00 00 03 01 E9`, When `rbsp_from_nal`, Then `rbsp == 67 42 00 1E 00 00 01 E9` and `epb_positions == {6}`. 2. Given that result, When `rbsp_bit_to_raw_bit(51)`, Then 59. 3. Given 1 000 random NALs with inserted EPBs, When stripped, Then equal to the pre-insertion payload and every RBSP byte maps back bijectively.
**Size:** S.
**Notes / pitfalls:** `00 00 03` at the very end of the NAL (cabac_zero_word padding) is still an EPB. Do not require the following byte to be ≤ 3.

### WI-1.105 — BitReader and RBSP fuzz targets and sanitizer CI hook
**Goal:** libFuzzer targets per §0.7.1 for `BitReader` and `rbsp_from_nal`.
**Depends on:** WI-1.102, WI-1.103, WI-1.104.
**Produces:** `core/tests/fuzz/fuzz_bitreader.cpp`, `core/tests/fuzz/fuzz_rbsp.cpp`, CMake target `ovt_fuzz_bitreader` under `OVT_ENABLE_FUZZ`, `.github/workflows/fuzz-smoke.yml` step (60 s per target).
**Interface:** §7.2.7.
**Steps:** 1. Fuzz driver: first byte selects a descriptor sequence; run until `ParseError`. 2. Assert no other exception type. 3. Seed corpus from the unit tests. 4. CI: build with `-fsanitize=fuzzer,address,undefined`, run 60 s, fail on crash.
**Acceptance tests:** 1. Given the fuzz target, When run 60 s in CI with ASan/UBSan, Then no crash or sanitizer report. 2. Given a crafted input that makes `ue()` see 40 zero bits, When fuzzed, Then only `BR-0005` is thrown.
**Size:** S.
**Notes / pitfalls:** Build fuzzers with `-O1 -g`; keep `MAX_LEN` 64 KiB. **Overlap:** this WI delivers only `fuzz_rbsp` and the BitReader seed corpus, registered with `ovt_add_fuzz_target` of WI-0.557; WI-0.557 owns `fuzz_bitreader`, the fuzz CMake and the smoke workflow. See chapter 16 §16.3.

### WI-1.106 — Implement `SyntaxBuilder` and the `SyntaxNode` writer
**Goal:** The recording helper of §7.3.2 producing `ovt.fb.SyntaxNode` FlatBuffers and a JSON/trace dump.
**Depends on:** WI-1.102, WI-0.080 (`ovt_model.fbs`, table `SyntaxNode`).
**Produces:** `core/include/ovt/cxx/syntax/syntax_builder.hpp`, `core/src/syntax/common/syntax_builder.cpp`, `core/src/syntax/common/syntax_node_buffer.{h,cpp}` (arena tree → FlatBuffer), `core/src/syntax/common/syntax_dump.cpp` (JSON and `--format trace` writers), `core/tests/syntax/test_syntax_builder.cpp`.
**Interface:** §7.3.2; output format §7.7.1 (trace lines).
**Steps:** 1. Arena node struct (`pmr`), begin/end struct, array element indices; constructor attaches the builder as the reader's `DiagnosticSink` and the destructor restores the previous sink (§7.3.2). 2. `read_*` wrappers emitting nodes with descriptor text and spans. 3. `derived`, `diag`, `set_enum`, `opaque_rest`, `set_recording(false)` fast path. 4. FlatBuffer conversion. 5. JSON writer (field names = FlatBuffer field names) and trace-line writer. 6. Tests: a toy structure read twice (recording on/off) gives identical values; spans nest correctly; partial tree returned after a `ParseError`.
**Acceptance tests:** 1. Given a struct with three `u(3)` reads, When finished, Then the struct node span is `{0, 9}` and children spans `{0,3},{3,3},{6,3}`. 2. Given `set_recording(false)`, When reading 1 M elements, Then no node is allocated (arena size 0) and values match. 3. Given a `ParseError` after two reads, When `finish()` is called, Then the tree has the two nodes. 4. Given a builder on a reader that already has sink `S`, When the reader emits `reader_warning("BR-0009", …)` and then the builder is destroyed, Then the warning is a child diagnostic of the current node and `diagnostic_sink() == &S` afterwards (`syntax_builder_sink`).
**Size:** M.
**Notes / pitfalls:** `descriptor` text must substitute literal widths (`"u(8)"`), except for spec `u(v)` elements where the caller sets `"u(v)"`.

### WI-1.107 — Implement unit scanners (Annex B, length-prefixed, OBU low-overhead and Annex B)
**Goal:** The four scanners of §7.3.3 producing `file_span`/`prefix_span` per unit.
**Depends on:** WI-1.100, WI-1.103.
**Produces:** `core/include/ovt/cxx/syntax/unit_scan.hpp`, `core/src/syntax/common/unit_scan.cpp`, `core/tests/syntax/test_syntax_unit_scan.cpp`.
**Interface:** §7.3.3.
**Steps:** 1. `scan_annexb` with `memchr` windows and overlap handling; trailing zero stripping. 2. `split_length_prefixed` for N ∈ {1,2,4}. 3. `scan_obu_low_overhead` with header parse + `leb128`. 4. `scan_obu_annexb` nested sizes. 5. Tests on synthetic buffers and on one stream of each corpus type (count of units vs `ffprobe -show_packets`/`-bsf trace_headers` unit count).
**Acceptance tests:** 1. Given a synthetic Annex B buffer `00 00 00 01 67 .. 00 00 01 68 .. 00 00 00 00 01 65 ..`, When scanned, Then 3 units with `prefix_span` lengths 4, 3, 5 and no trailing zeros inside units. 2. Given an IVF frame with 4 OBUs, When scanned, Then 4 units whose spans tile the frame exactly. 3. Given a 4 GiB sparse Annex B file with a start code straddling a 1 MiB window boundary, When scanned, Then the unit is found once.
**Size:** M.
**Notes / pitfalls:** A `00 00 01` inside an OBU stream is **not** a start code (never run the Annex B scanner on AV1). `00 00 02` is a forbidden pattern in H.26x NALs → Diagnostic, not a boundary.

### WI-1.108 — H.264 NAL header, SPS (with VUI/HRD/scaling lists), PPS parsers
**Goal:** Rows 1–6 of §7.3.4 with all derived values.
**Depends on:** WI-1.104, WI-1.106.
**Produces:** `core/src/syntax/h264/{nal_header,sps,pps,vui,scaling_list}.cpp`, `core/include/ovt/cxx/syntax/h264/{nal_header,sps,pps}.hpp`, `core/tests/syntax/h264/test_syntax_h264_sps_pps.cpp`. The manifest tag `h264-ps-golden` in `tests/corpora/manifest.json` (chapter 14 §14.3.2) names the 20 JVT streams whose full `trace_headers` parity is checked by the golden suite WI-1.572 (not by this WI): `BA1_Sony_D`, `BA_MW_D`, `BAMQ1_JVC_C`, `CAPAMA3_Sand_F`, `CI1_FT_B`, `CVSE2_Sony_B`, `CVWP1_TOSHIBA_E`, `HPCV_BRCM_A`, `FRExt1_Panasonic_D`, `HCAFF1_HHI_B`, `Hi422FR1_SONY_A`, `HPCAFL_BRCM_C`, `MR1_BT_A`, `MR6_BT_B`, `CAQP1_Sony_B`, `sp1_bt_a`, `CVFC1_SVA_C`, `FM1_BT_B`, `CABAST3_Sony_E`, `SVCBST-1` (ids are the JVT file stems; the manifest records their directories).
**Interface:** §7.3.4 rows 1–6, §7.3.7.
**Steps:** 1. NAL header incl. SVC/MVC/3D extension headers. 2. `seq_parameter_set_data` + scaling lists + derived table (ChromaArrayType … MaxDpbFrames with Table A-1). 3. VUI + HRD with `BitRate/CpbSize`. 4. PPS incl. FMO types 0–6, `more_rbsp_data()` tail, provisional parse flag. 5. Enum names (profiles, aspect ratios, H.273 tables). 6. Unit tests on the fixtures below; expected values are hard-coded in the test (taken once from `ffmpeg -bsf:v trace_headers` output and cited in a comment).
**Acceptance tests:** 1. Given the first SPS of `syn_h264_gop60_1080p` (chapter 14 recipe 26, x264 High), When parsed, Then `profile_idc == 100`, `pic_width_in_mbs_minus1 == 119`, `pic_height_in_map_units_minus1 == 67`, `frame_cropping_flag == 1`, `frame_crop_bottom_offset == 4`, the derived crop rectangle is 1920×1080, and the `seq_parameter_set_id` node span starts at bit 32 of the NAL unit (`h264_sps_unit`). 2. Given `CAPAMA3_Sand_F` (FMO), When PPS is parsed, Then `slice_group_map_type == 6` and `slice_group_id[]` has `pic_size_in_map_units_minus1+1` entries. 3. Given `FRExt/HPCV_BRCM_A` (High, 8×8 transform), When PPS is parsed, Then `transform_8x8_mode_flag == 1` and 8 scaling lists are read when present.
**Size:** L.
**Notes / pitfalls:** Full parity against `trace_headers` over the `h264-ps-golden` streams is WI-1.572's job (chapter 14; D25: the golden suite depends on the parsers, not the reverse); `ovt syntax --trace` (WI-1.504) is a consumer. `level_idc == 11 && constraint_set3_flag` is level 1b for Baseline/Main/Extended. `pic_order_cnt_type == 1` with `num_ref_frames_in_pic_order_cnt_cycle == 0` is legal.

### WI-1.109 — H.264 slice header (+ ref list modification, pred weight table, dec ref pic marking), AUD, SPS ext, subset SPS, prefix NAL, data partitions
**Goal:** Rows 19–28 of §7.3.4.
**Depends on:** WI-1.108.
**Produces:** `core/src/syntax/h264/{slice_header,ref_pic_list_modification,pred_weight_table,dec_ref_pic_marking,aud,sps_ext,subset_sps,prefix_nal,data_partition}.cpp`, headers, `core/tests/syntax/h264/test_syntax_h264_slice_header.cpp`.
**Interface:** §7.3.4 rows 19–28; `ActiveSets` §7.3.8.
**Steps:** 1. Slice header with activation callback and all derived values. 2. Modification op list, pred weight table (defaults), MMCO op list with maxima. 3. AUD, end of seq/stream, filler. 4. SPS extension, subset SPS (SVC/MVC/MVCD extension fields), prefix NAL, slice header in scalable extension (G.7.3.3.4) for SVC, MVC slice ext using subset SPS. 5. Data partitions A/B/C. 6. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); first-slice parity over the 40 `h264-slice-golden` streams (`trace_headers`, JM `trace_dec.txt` §7.7.3) is WI-1.572.
**Acceptance tests:** 1. Given `CVWP1_TOSHIBA_E` (weighted prediction), When the first P slice header is parsed, Then `num_ref_idx_l0_active_minus1`, `luma_log2_weight_denom` and every `luma_weight_l0[i]`/`luma_offset_l0[i]` equal the values recorded from `trace_headers` (`h264_slice_pwt_unit`). 2. Given `MR9_BT_B` (MMCO), When parsed, Then the MMCO op list and `MaxLongTermFrameIdx` equal the values recorded from JM's `trace_dec.txt`. 3. Given `MVC/MVCNV-1` subset SPS (nightly `mvc` entry; skipped when absent), When parsed, Then `num_views_minus1` and `view_id[]` equal the values recorded from GStreamer `gst_h264_parser_parse_subset_sps`.
**Size:** L.
**Notes / pitfalls:** `num_ref_idx_l1_active_minus1` is read only for B slices; `slice_group_change_cycle` width uses `PicSizeInMapUnits`, not `PicSizeInMbs`. For NAL 20 the active SPS is the subset SPS.

### WI-1.110 — H.264 SEI framing and payload parsers
**Goal:** Rows 7–18 of §7.3.4 and the shared SEI framing.
**Depends on:** WI-1.108, WI-1.120.
**Produces:** `core/src/syntax/common/sei_framing.cpp`, `core/src/syntax/h264/sei_*.cpp`, `core/tests/syntax/h264/test_syntax_h264_sei.cpp`.
**Interface:** §7.3.4 rows 7–18.
**Steps:** 1. Framing with sub-reader per payload, `ff_byte` loops, payload bits/alignment. 2. Buffering period / pic timing with HRD lengths from `ActiveSets` (Diagnostic when absent). 3. Recovery point, user data registered/unregistered (dispatch to registry), frame packing, display orientation, MDCV, CLL, alternative transfer, film grain, tone mapping. 4. Unknown payloads as Bytes with spec names (table of 0..56 names). 5. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); full SEI parity is WI-1.572.
**Acceptance tests:** 1. Given `syn_sei_h264_mastering_display_colour_volume` and `syn_sei_h264_content_light_level_info` (chapter 14 recipe 47 family), When SEIs are parsed, Then the field values equal `tests/corpora/sei_expected.json` and the derived nits text is correct. 2. Given `CVSEFDFT3_Sony_E`, When `pic_timing` is parsed, Then `pic_struct` and `clock_timestamp` fields of the first three pictures equal the values recorded from `trace_headers`. 3. Given a SEI whose `payloadSize` exceeds the RBSP, When parsed, Then `ParseError BR-0001` is caught by the framing and the remaining messages are not parsed (one Diagnostic).
**Size:** M.
**Notes / pitfalls:** `pic_timing` needs the active SPS; in the index the SEI precedes the slice that activates it, so the parser uses the SPS of the **previous** picture unless a buffering period SEI in the same AU names a `seq_parameter_set_id` (H.264 §D.2.2).

### WI-1.111 — H.264 activation, POC (types 0/1/2) and frame-boundary detection
**Goal:** §7.3.8 (H.264 rows), §7.3.9 (H.264), §7.3.10.1.
**Depends on:** WI-1.109.
**Produces:** `core/src/syntax/h264/active_sets.cpp`, `core/src/index/h264_poc.cpp`, `core/src/index/h264_frame_boundary.cpp`, `core/tests/index/test_index_h264_poc.cpp`.
**Interface:** §7.3.8, §7.3.9, §7.3.10.1.
**Steps:** 1. `ActiveSets` tables with generation counters and PPS re-parse. 2. The 10 boundary rules. 3. POC state machine for all three types incl. mmco5 and field pictures. 4. Tests: synthetic sequences for each rule; POC unit tests with the per-picture POC sequence recorded from JM; POC parity over all JVT streams is the StreamIndex golden suite WI-1.573.
**Acceptance tests:** 1. Given `CVPCMNL1_SVA_C` (POC type 0) and `MR6_BT_B`, When indexed, Then per-picture `poc` equals the sequence recorded from JM's output (`h264_poc_unit`). 2. Given `MR9_BT_B` (mmco5), When indexed, Then the picture after the mmco5 has `prevPicOrderCntMsb == 0` and POC values continue as JM prints. 3. Given `CVFI1_Sony_D` (field pictures), When indexed, Then frame count equals JM's decoded picture count and each field has its own `FrameInfo` with `structure` Top/Bottom.
**Size:** M.
**Notes / pitfalls:** For type 0 only reference pictures update `prev*`; a non-reference picture's lsb must not be remembered.

### WI-1.112 — H.265 NAL header, VPS, PTL, SPS (VUI/HRD/scaling/range/SCC), PPS (tiles/range/SCC)
**Goal:** Rows 1–8 of §7.3.5.
**Depends on:** WI-1.104, WI-1.106.
**Produces:** `core/src/syntax/h265/{nal_header,vps,profile_tier_level,sps,pps,scaling_list,vui,hrd_parameters}.cpp`, headers, `core/tests/syntax/h265/test_syntax_h265_parameter_sets.cpp`.
**Interface:** §7.3.5 rows 1–8, §7.3.7.
**Steps:** 1. NAL header + derived classification. 2. PTL with per-profile constraint bit layout and sub-layers. 3. VPS incl. HRD list. 4. SPS incl. `st_ref_pic_set` call (uses WI-1.113's function — implement `st_ref_pic_set` here first, used by both), long-term SPS entries, range/SCC/multilayer extension flags. 5. PPS incl. tiles and extensions; derived tile tables computed in an `ActivePps` helper. 6. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); parity over the HEVC_v1/RExt/SCC lists is WI-1.572.
**Acceptance tests:** 1. Given `SLIST_A_Sony_5` (scaling lists), When VPS/SPS/PPS are parsed, Then `scaling_list_enabled_flag`, the PTL fields and every `scaling_list_dc_coef_minus8` equal the values recorded from `trace_headers` (`h265_ps_unit`). 2. Given `TILES_B_Cisco_1`, When the PPS is activated, Then `colBd[]`/`rowBd[]` equal the boundaries recorded from GStreamer `GstH265PPS.column_width_minus1` and `CtbAddrRsToTs` is a permutation. 3. Given `RExt/Bitdepth_A_RExt_Sony_1` and the first `scc`-tagged stream (nightly; skipped when absent), When parsed, Then `sps_range_extension`/`sps_scc_extension` fields equal the values recorded from `trace_headers`.
**Size:** L.
**Notes / pitfalls:** `sps_max_sub_layers_minus1` controls the PTL sub-layer loop; `vps_reserved_0xffff_16bits` is `f(16)`; `log2_max_pic_order_cnt_lsb_minus4 ≤ 12`.

### WI-1.113 — H.265 slice segment header, `st_ref_pic_set`, pred weight table, entry points
**Goal:** Rows 9–12 of §7.3.5 incl. the §7.3.10.3 derivation.
**Depends on:** WI-1.112.
**Produces:** `core/src/syntax/h265/{slice_segment_header,st_ref_pic_set,pred_weight_table,ref_pic_lists_modification}.cpp`, `core/tests/syntax/h265/test_syntax_h265_slice.cpp`.
**Interface:** §7.3.5 rows 9–12, §7.3.10.3.
**Steps:** 1. `st_ref_pic_set` with inter-RPS prediction exactly per H.265 §7.3.7 / §7.4.8. 2. Slice segment header, dependent segments copying from `prev_independent`, `NumPicTotalCurr`, LT entries, entry points with raw-byte offsets. 3. Pred weight table with the SCC current-picture skip rule. 4. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); first-slice parity over all HEVC_v1 streams is WI-1.572.
**Acceptance tests:** 1. Given `WP_A_Toshiba_3`, When the first P slice segment header is parsed, Then `num_ref_idx_l0_active_minus1`, `luma_log2_weight_denom` and the `pred_weight_table` entries equal the values recorded from `trace_headers` (`h265_slice_unit`). 2. Given `RPS_A_docomo_5`, `RPS_B_qualcomm_5`, `RPS_C_ericsson_5`, `RPS_D_ericsson_6`, `RPS_E_qualcomm_5` and `RPS_F_docomo_2`, When `st_ref_pic_set` is derived, Then `DeltaPocS0/S1` and `UsedByCurrPic*` of the first 8 pictures equal the RPS recorded from HM's trace. 3. Given `ENTP_A_QUALCOMM_1` (entry points), When parsed, Then substream byte ranges tile `slice_segment_data` exactly in raw bytes. 4. Given `DSLICE_A_HHI_5` (dependent slices), When parsed, Then dependent segments inherit `slice_type`/QP from the independent segment.
**Size:** L.
**Notes / pitfalls:** `short_term_ref_pic_set_idx` width is `Ceil(Log2(num_short_term_ref_pic_sets))` and is **absent** when `num_short_term_ref_pic_sets == 1`. `collocated_ref_idx` is read only when the relevant list has > 1 entries. `num_entry_point_offsets` upper bound depends on tiles/WPP (H.265 §7.4.7.1).

### WI-1.114 — H.265 SEI payload parsers
**Goal:** Rows 13–16 of §7.3.5 (mandatory types 0, 1, 4, 5, 6, 45, 47, 129, 132, 136, 137, 144, 147, 148; others as named Bytes).
**Depends on:** WI-1.112, WI-1.110 (shared framing), WI-1.120.
**Produces:** `core/src/syntax/h265/sei_*.cpp`, `core/tests/syntax/h265/test_syntax_h265_sei.cpp`.
**Interface:** §7.3.5 rows 13–16.
**Steps:** 1. Payload table with names 0..205. 2. Mandatory parsers. 3. Prefix/suffix placement Diagnostic. 4. `decoded_picture_hash` storage for verify. 5. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); full SEI parity is WI-1.572.
**Acceptance tests:** 1. Given the first three pictures of `AMP_A_Samsung_7` (MD5 `decoded_picture_hash` SEI), When suffix SEIs are parsed, Then `hash_type` and `picture_md5[c][]` equal the values recorded from `trace_headers`; and given `HRD_A_Fujitsu_3`, `initial_cpb_removal_delay` and `au_cpb_removal_delay_minus1` of the first AU equal the recorded values. 2. Given `syn_h265_main10_pq_hdr` (MDCV/CLL SEI) and `hdr_hdr10plus_dv81` (HDR10+ T.35), When prefix SEIs are parsed, Then the MDCV/CLL fields equal the recipe's `--master-display`/`--max-cll` values and the HDR10+ `application_identifier == 4`. 3. Given `active_parameter_sets` in `VPSID_A_VIDYO_2`, When parsed, Then `active_video_parameter_set_id` and `active_seq_parameter_set_id[0]` equal the ids of the VPS/SPS in the stream.
**Size:** M.
**Notes / pitfalls:** `time_code` fields are `u(9)` for `n_frames` (not `u(8)` as in H.264 `pic_timing`).

### WI-1.115 — H.265 activation, POC, frame boundary, RPS/DPB and RefPicList derivation
**Goal:** §7.3.8 (H.265), §7.3.9 (H.265), §7.3.10.2, §7.4.4 (H.265).
**Depends on:** WI-1.113.
**Produces:** `core/src/syntax/h265/active_sets.cpp`, `core/src/index/h265_poc.cpp`, `core/src/index/h265_frame_boundary.cpp`, `core/src/index/h265_dpb.cpp`, `core/tests/index/test_index_h265_rps.cpp`.
**Interface:** §7.3.10.2, §7.4.4.
**Steps:** 1. ActiveSets with VPS/SPS/PPS + tile tables. 2. Boundary rules. 3. POC with `prevTid0Pic`. 4. RPS derivation (five lists), marking, RefPicList0/1 with modification. 5. Unit tests with the POC/RefPicList sequences recorded from HM; parity over all HEVC_v1 streams is the StreamIndex golden suite WI-1.573.
**Acceptance tests:** 1. Given `POC_A_Bossen_3` and `RPS_A_docomo_5`, When indexed, Then `poc` per picture equals the sequence recorded from HM's output. 2. Given `RPS_A_docomo_5`, When `declared_refs` is resolved to POCs, Then it equals the recorded HM RefPicList0 ∪ RefPicList1 of the first slice of each picture (`h265_refpiclist_unit`). 3. Given `RAP_B_Bossen_2` (CRA with leading pictures), When indexed, Then RASL pictures are flagged in the RAP side bitmap and `is_rap` is true for the CRA. 4. Given `NUT_A_ericsson_5` (all NAL types), When indexed, Then sub-layer non-reference pictures do not update `prevTid0Pic`.
**Size:** M.
**Notes / pitfalls:** Long-term lookup by LSB only when `delta_poc_msb_present_flag == 0`. The current picture is in RefPicList only when `pps_curr_pic_ref_enabled_flag`.

### WI-1.116 — AV1 OBU header, sequence header, temporal delimiter, metadata, padding, tile list parsers
**Goal:** Rows 1–9 and 30 of §7.3.6.
**Depends on:** WI-1.103, WI-1.106, WI-1.120.
**Produces:** `core/src/syntax/av1/{obu_header,sequence_header,color_config,timing,metadata,tile_list}.cpp`, headers, `core/tests/syntax/av1/test_syntax_av1_sequence_metadata.cpp`.
**Interface:** §7.3.6 rows 1–9, 30.
**Steps:** 1. OBU header + size + operating point rule. 2. Sequence header incl. reduced still picture path, operating points, decoder model, color_config, `OrderHintBits`. 3. Metadata types 1–5 with unit conversions for `ovt_ColorDescription`. 4. Padding, tile list. 5. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); parity over the `aom` vectors and the Argon `argon-header-golden` subset is WI-1.572.
**Acceptance tests:** 1. Given `av1-1-b8-01-size-16x16`, When the sequence header is parsed, Then `seq_profile == 0`, `max_frame_width_minus_1 == 15`, `max_frame_height_minus_1 == 15`, `BitDepth == 8` and `OrderHintBits` equal the values recorded from `trace_headers` (`av1_seq_unit`). 2. Given `syn_av1_pq_hdr_obu` (MDCV/CLL metadata OBUs, chapter 14 recipe 15a), When parsed, Then `mdcv_max_luminance` and primaries convert to the expected 0.0001 cd/m² and 0.00002 units (hand-computed expected values in the test). 3. Given `av1-1-b8-22-svc-L2T2`, When parsed with operating point 0, Then OBUs outside the point are flagged `SYN-AV1-0002`.
**Size:** L.
**Notes / pitfalls:** `seq_level_idx == 31` means "maximum parameters"; the tier bit **is** still read when `seq_level_idx > 7`, including 31. `color_config` for profile 1 never reads `mono_chrome`.

### WI-1.117 — AV1 `uncompressed_header` part 1
**Goal:** §7.3.6 row 11 up to and including `delta_lf_params`, with rows 12–19.
**Depends on:** WI-1.116, WI-1.119 (DecoderState interface; implement the struct here and the update processes in WI-1.119).
**Produces:** `core/src/syntax/av1/{frame_header,frame_size,tile_info,quantization,segmentation,delta_params}.cpp`, `core/include/ovt/cxx/syntax/av1/frame_header.hpp`, `core/tests/syntax/av1/test_syntax_av1_frame_header.cpp`.
**Interface:** §7.3.6 rows 11–19.
**Steps:** 1. show_existing path, frame type/flags, frame ids, `mark_ref_frames`. 2. `frame_size`, `superres_params`, `render_size`, `frame_size_with_refs`, `compute_image_size`. 3. `set_frame_refs` call (WI-1.119) and `ref_frame_idx`, OrderHints, sign bias. 4. `tile_info` with `tile_log2`, uniform/non-uniform. 5. `quantization_params`, `segmentation_params` (feature tables), `delta_q_params`, `delta_lf_params`, lossless derivations. 6. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); parity is WI-1.572.
**Acceptance tests:** 1. Given `av1-1-b8-02-allintra` and `av1-1-b8-04-cdfupdate`, When the first two frame headers are parsed up to `delta_lf_params`, Then `frame_type`, `base_q_idx`, `segmentation_enabled` and `delta_q_present` equal the values recorded from `trace_headers` (`av1_fh1_unit`). 2. Given `av1-1-b8-03-sizeup`, When `frame_size_with_refs` is exercised, Then `UpscaledWidth/FrameWidth/MiCols` equal GStreamer `GstAV1FrameHeaderOBU` values. 3. Given `syn_av1_tiles_cdef_lr` (1280×720, 128×128 superblocks, `--tile-columns=2 --tile-rows=1`), When `tile_info` is parsed, Then `TileCols == 4`, `TileRows == 2`, `MiColStarts == {0, 96, 192, 288, 320}` and `MiRowStarts == {0, 96, 180}`; and given a hand-assembled `tile_info` with `uniform_tile_spacing_flag = 0` and tile widths {2, 5, 3} superblocks on the same sequence header, Then `MiColStarts == {0, 64, 224, 320}`.
**Size:** L.
**Notes / pitfalls:** `frame_refs_short_signaling` requires `enable_order_hint`. `ref_order_hint[i]` is read only when `error_resilient_mode && enable_order_hint`. `delta_q_present` is read only if `base_q_idx > 0`.

### WI-1.118 — AV1 `uncompressed_header` part 2, frame OBU, tile group sizes
**Goal:** §7.3.6 rows 20–29 (loop filter, CDEF, LR, tx mode, reference mode, skip mode, global motion, film grain, frame OBU, tile group).
**Depends on:** WI-1.117.
**Produces:** `core/src/syntax/av1/{loop_filter,cdef,lr,tx_mode,skip_mode,global_motion,film_grain,frame_obu,tile_group}.cpp`, tests `core/tests/syntax/av1/test_syntax_av1_frame_header2.cpp`.
**Interface:** §7.3.6 rows 20–29.
**Steps:** 1. Loop filter / CDEF / LR with lossless and intrabc shortcuts. 2. `read_tx_mode`, `frame_reference_mode`, `skip_mode_params` (SkipModeFrame). 3. Global motion with subexp decoding and `PrevGmParams`. 4. Film grain incl. `load_grain_params`. 5. Frame OBU splitting and tile group tile sizes. 6. Unit tests on the fixtures of the acceptance tests with expected values hard-coded in the test from a recorded oracle output (cited in a comment); full-header parity over the `aom` vectors and the Argon subset, tile byte ranges vs GStreamer `GstAV1TileGroupOBU.entry[]` and tile counts vs `inspect` are WI-1.572.
**Acceptance tests:** 1. Given `syn_av1_tiles_cdef_lr`, When the first frame header is parsed, Then `cdef_bits`, `cdef_y_pri_strength[0]`, `FrameRestorationType[0..2]` and `LoopRestorationSize[0]` equal the values recorded from `trace_headers` (`av1_fh2_unit`). 2. Given `av1-1-b8-23-film_grain-50`, When parsed, Then film grain fields match `trace_headers` and `update_grain = 0` frames copy from the referenced slot. 3. Given `syn_av1_globalmotion` (chapter 14 synthetic recipe, global motion enabled on a panning source), When parsed, Then `gm_params[ref][0..5]` of the first inter frame with `is_global` set equal the values recorded from GStreamer `GstAV1GlobalMotionParams.gm_params`. 4. Given a tile group whose `tile_size_minus_1` overruns, When parsed, Then `SYN-AV1-0020` is raised and the frame is marked truncated.
**Size:** L.
**Notes / pitfalls:** `cdef_y_sec_strength == 3` becomes 4. `lr_unit_shift` reads differ for 128×128 superblocks. `decode_subexp` uses `ns()` for the final bits.

### WI-1.119 — AV1 reference slot model, OrderHints, `set_frame_refs`, frame/temporal-unit boundaries
**Goal:** §7.3.10.4, §7.3.9 (AV1), §7.4.3, §7.4.4 (AV1).
**Depends on:** WI-1.116.
**Produces:** `core/include/ovt/cxx/syntax/av1/decoder_state.hpp`, `core/src/syntax/av1/decoder_state.cpp` (update/loading/set_frame_refs/get_relative_dist), `core/src/index/av1_frame_boundary.cpp`, `core/src/index/av1_slots.cpp`, `core/tests/index/test_index_av1_slots.cpp`.
**Interface:** §7.3.10.4.
**Steps:** 1. `DecoderState` struct and the three processes as pseudo-code. 2. `SeenFrameHeader`/`TileNum` frame grouping and temporal unit checks. 3. Hidden/show_existing `FrameInfo` rules. 4. Tests: hand-built slot sequences; golden vs libaom `inspect` `frame`/`showFrame`/`showExistingFrame` and vs `aomdec --rawvideo` frame count (shown frames) for the test vectors.
**Acceptance tests:** 1. Given `av1-1-b8-05-mv` and `-06-mfmv` (hidden ALTREF + show_existing), When indexed, Then the number of `FrameInfo` entries equals libaom `inspect`'s frame count and the number of shown frames equals `aomdec` output frames. 2. Given 1 000 random slot states (`RefValid`, `RefOrderHint`, `OrderHintBits`, `last_frame_idx`, `gold_frame_idx`), When `set_frame_refs` runs, Then `ref_frame_idx[]` equals the result of a literal transcription of AV1 §7.8 kept in the test, and a hand-worked example in the test gives the documented indices (`av1_set_frame_refs_prop`). 3. Given a shown KEY via `show_existing_frame`, When indexed, Then all 8 `dpb_after` entries equal `show_existing_of` and `is_rap` is true.
**Size:** M.
**Notes / pitfalls:** `RefOrderHint` must be reset to 0 for all slots on a shown KEY frame *before* parsing `ref_order_hint[]`. A `show_existing_frame` of a non-KEY slot does **not** refresh any slot.

### WI-1.120 — ITU-T T.35 and `user_data_unregistered` registry with HDR10+/CEA-708/x264/x265/DV detection
**Goal:** §7.3.11 tables and parsers.
**Depends on:** WI-1.102.
**Produces:** `core/src/syntax/common/user_data_registry.{h,cpp}`, `core/src/syntax/common/t35_hdr10plus.cpp`, `core/src/syntax/common/t35_atsc_cc.cpp`, `core/tests/syntax/test_syntax_user_data.cpp`.
**Interface:** §7.3.11.
**Steps:** 1. `constexpr` registry tables. 2. HDR10+ ST 2094-40 parser (all fields). 3. GA94 `cc_data()` parser (counts only). 4. UUID table + x264/x265 option splitting. 5. DV markers (HEVC NAL 62/63, AV1 T.35 Dolby). 6. Tests with hand-assembled payloads and the fixtures below.
**Acceptance tests:** 1. Given `hdr_hdr10plus_dv81` (HDR10+ T.35 SEI), When the T.35 SEI is parsed, Then `enum_name == "HDR10+ (SMPTE ST 2094-40)"`, `application_identifier == 4`, and `maxscl[0..2]` of the first picture equal the values recorded from `hdr10plus_tool extract`. 2. Given `syn_sei_h264_ga94_cc` (hand-encoded ATSC A/53 `GA94` payload, chapter 14 recipe 47), When parsed, Then `cc_count` equals the value in `tests/corpora/sei_expected.json`. 3. Given `syn_h264_gop60_1080p` (x264-encoded), When `user_data_unregistered` is parsed, Then `enum_name == "x264 options"` and child `x264_option[cabac]` exists.
**Size:** S.
**Notes / pitfalls:** Dolby Vision RPU carried in an AV1 `OBU_METADATA` of type `METADATA_TYPE_ITUT_T35` is identified by `itu_t_t35_country_code == 0xB5`, `itu_t_t35_terminal_provider_code == 0x003B` (Dolby Laboratories) and a 32-bit `itu_t_t35_terminal_provider_oriented_code == 0x00000800` (as implemented by FFmpeg `libavcodec/av1dec.c`, `export_itut_t35()`; the RPU payload follows and is parsed by `ff_dovi_rpu_parse`). HDR10+ uses provider `0x003C` (Samsung), 16-bit provider-oriented code `0x0001`, `application_identifier == 4`. Do not confuse the two provider-oriented-code widths. T.35 payloads in AV1 include trailing bits — strip the final `0x80` + zero bytes before parsing the ST 2094-40 structure.

### WI-1.121 — `StreamIndex` builder core
**Goal:** §7.4.2 steps S0–S11 for all codecs, excluding DPB models (stubs returning empty refs).
**Depends on:** WI-1.107, WI-1.111, WI-1.115, WI-1.119, WI-1.110, WI-1.114, WI-1.231 (container unit map: the unit splitter output of ch. 8).
**Produces:** `core/include/ovt/cxx/index/stream_index_builder.hpp`, `core/src/index/stream_index_builder.cpp`, `core/src/index/frame_classify.cpp`, `core/src/index/display_order.cpp`, `core/src/index/rap_map.cpp`, `core/src/api/stream_api.cpp` (`ovt_stream_build_index` only; `ovt_stream_unit_bytes` is delivered by WI-1.130), `core/tests/index/test_index_builder.cpp`.
**Interface:** §0.5.3 `StreamIndex`; §0.6.1 `ovt_stream_build_index`; §7.4.2.
**Steps:** 1. Builder state machine. 2. Dispatch and error capture per unit. 3. Frame open/close, non-VCL attachment, size accounting. 4. Classification per codec. 5. Display order per codec. 6. `source_hash`, RAP map. 7. Tests on 5 streams per codec (H.264: `BA1_Sony_D`, `CVSE2_Sony_B`, `MR1_BT_A`, `CAPAMA3_Sand_F`, `FRExt/HPCV_BRCM_A`; H.265: `WPP_A_ericsson_MAIN_2`, `AMP_A_Samsung_7`, `RAP_A_docomo_6`, `POC_A_Bossen_3`, `NoOutPrior_A_Qualcomm_1`; AV1: `av1-1-b8-01-size-16x16`, `av1-1-b8-02-allintra`, `av1-1-b8-05-mv`, `av1-1-b8-06-mfmv`, `av1-1-b8-22-svc-L2T2`) comparing frame count/types/sizes/POC with `ffprobe -show_frames -show_packets` (`pict_type`, `pkt_size`, `coded_picture_number`, `pts`).
**Acceptance tests:** 1. Given `ffprobe -show_frames` of each test stream, When compared with `StreamIndex.frames` (shown frames), Then counts, `pict_type` (I/P/B ↔ FrameType), and `pts` match for every frame (`index_vs_ffprobe`). 2. Given an AV1 stream with hidden frames, When indexed, Then `frames.size() == shown + hidden` and `display_order.size() == shown`. 3. Given a truncated file (last unit cut), When indexed, Then `OVT_OK` with Diagnostic `IO-0001`/`SYN-AV1-0101` and all complete frames present.
**Size:** L.
**Notes / pitfalls:** H.264 `pkt_size` from ffprobe includes AUD/SEI of the AU — matches OVT's `size_bytes` rule. H.265 `pict_type` from FFmpeg follows the first slice, OVT uses majority → compare on single-slice streams only.

### WI-1.122 — H.264 DPB simulation (sliding window, MMCO, ref list init/modification)
**Goal:** §7.4.4 H.264 block, frames and fields.
**Depends on:** WI-1.121.
**Produces:** `core/src/index/h264_dpb.cpp`, `core/tests/index/test_index_h264_dpb.cpp`.
**Interface:** §7.4.4.
**Steps:** 1. Frame stores, picture numbers, FrameNumWrap. 2. P and B initial lists (frames). 3. Field alternation (§8.2.4.2.5). 4. Modification ops. 5. Sliding window and MMCO 1–6; gaps in frame_num. 6. Golden: JM's `ldecod` built with `TRACE` logs the reference picture lists per slice (`RefPicList0[i]`/`RefPicList1[i]` lines with POC values) in `trace_dec.txt`; compare resolved POCs (`h264_reflist_vs_jm`).
**Acceptance tests:** 1. Given all JVT streams, When `declared_refs` is resolved to POC, Then it equals JM's RefPicList0 ∪ RefPicList1 for the first slice (`h264_reflist_vs_jm`). 2. Given `MR6_BT_B`…`MR9_BT_B` (MMCO + fields), When indexed, Then `dpb_after` sizes never exceed `max_num_ref_frames` and no `SYN-H264-0105/0006` diagnostics occur. 3. Given `MR1_BT_A` with `gaps_in_frame_num`, When indexed, Then non-existing frames are inserted with `SYN-H264-0102`.
**Size:** L.
**Notes / pitfalls:** The second field of a reference frame does not trigger the sliding window. Long-term fields: `LongTermPicNum = 2*LongTermFrameIdx + 1` for same parity.

### WI-1.123 — H.265 RPS/RefPicList integration into the index
**Goal:** Wire WI-1.115's DPB model into the builder; `dpb_after`, `declared_refs`, RASL bitmap.
**Depends on:** WI-1.121, WI-1.115.
**Produces:** `core/src/index/h265_dpb.cpp` (integration), tests extend `core/tests/index/test_index_h265_rps.cpp`.
**Interface:** §7.4.4.
**Steps:** 1. Integrate; 2. RAP map with RASL skip bitmap; 3. Golden `h265_refpiclist_vs_hm` runs through the full builder.
**Acceptance tests:** 1. Given HEVC_v1, When indexed end-to-end, Then `h265_refpiclist_vs_hm` passes. 2. Given `CONFWIN_A_Sony_1`, When indexed, Then `crop` equals the conformance window in luma samples (`SubWidthC` scaled). 3. Given `RAP_A_docomo` with EOS, When indexed, Then the picture after EOS has `NoRaslOutputFlag = 1` and POC MSB 0.
**Size:** M.
**Notes / pitfalls:** none beyond WI-1.115.

### WI-1.124 — AV1 slot model integration (hidden frames, show_existing, operating points)
**Goal:** Wire WI-1.119 into the builder with §7.4.3 rules.
**Depends on:** WI-1.121, WI-1.119.
**Produces:** `core/src/index/av1_slots.cpp` (integration), `core/tests/index/test_index_av1_index.cpp`.
**Interface:** §7.4.3, §7.4.4.
**Steps:** 1. Integrate; 2. `show_existing_of`, `dpb_after` slot vectors; 3. Annex B temporal unit checks; 4. tests on libaom vectors incl. `-22-svc-*`.
**Acceptance tests:** 1. Given `av1-1-b8-22-svc-L2T2`, When indexed with operating point 0, Then frames of spatial layer 1 are `shown = false` with `SYN-AV1-0002`. 2. Given `av1-1-b8-06-mfmv`, When indexed, Then every `declared_refs` entry points to an existing decode index and matches the `referenceFrame` → slot map of libaom `inspect`. 3. Given an Annex B `.obu` stream, When indexed, Then unit count and frame count equal the Section 5 version of the same stream.
**Size:** M.
**Notes / pitfalls:** `refresh_frame_flags` of an `INTRA_ONLY` frame must not be 0xFF.

### WI-1.125 — Incremental/cancellable indexing, lazy header trees, performance test
**Goal:** §7.4.5 and §7.4.6: job integration, partial results, lazy trees, `ovt_stream_unit_syntax`, ≤ 2 s budget.
**Depends on:** WI-1.121, WI-1.512 (jobs module).
**Produces:** `core/src/index/stream_index_builder.cpp` (cancel/progress/partial), `core/src/index/lazy_syntax.cpp`, `core/src/api/stream_api.cpp` (`ovt_stream_unit_syntax`, `OVT_INDEX_ALLOW_PARTIAL`), `core/tests/perf/bench_index_h264_1h.cpp`, `tools/make_long_stream.sh` (concatenates JVT streams / x264-encodes 1 h of test pattern deterministically).
**Interface:** §7.4.5, §7.4.6; §0.6.1.
**Steps:** 1. `check_cancel` every 256 units/50 ms; progress. 2. Partial index flag and resumable state. 3. Lazy recording threshold; activation snapshots per frame; `ovt_stream_unit_syntax` re-parse. 4. Lazy EPB stripping (2 KiB chunks). 5. Arena allocation audit (no heap allocs per unit when recording is off — verified with a counting allocator in the test). 6. Perf test with a 1-hour 1080p H.264 file (≈ 108 000 frames).
**Acceptance tests:** 1. Given the 1-hour 1080p H.264 file on the reference M1 (CI: ≤ 2.5 s), When `ovt index` runs, Then wall time ≤ 2.0 s and RSS ≤ 200 MB (`bench_index_h264_1h_1080p` on `syn_h264_1h_1080p.mp4`; budget owner chapter 14 §14.9.3). 2. Given a job cancelled after 100 ms, When `ovt_stream_build_index` returns, Then `OVT_E_CANCELLED` within 150 ms. 3. Given `OVT_INDEX_ALLOW_PARTIAL` and a cancel, When the partial index is returned, Then it contains only complete frames and `SYN-GEN-0103`. 4. Given unit 200 000 of the long file, When `ovt_stream_unit_syntax` is called, Then the tree equals the eagerly recorded tree of the same unit from a small-file run (byte-identical FlatBuffer).
**Size:** M.
**Notes / pitfalls:** Measure with a warm page cache and a cold one; the 2 s target is warm-cache. `source_hash` reads only the first 1 MiB.

### WI-1.126 — `trace_headers` diff tool and golden-test infrastructure
**Goal:** `tools/trace_diff.py` and the CMake/CTest glue for all `*_trace_golden` tests.
**Depends on:** WI-1.106, WI-1.504 (`ovt syntax --trace`), WI-0.555 (oracle normaliser framework).
**Produces:** `tools/trace_diff.py`, `tools/trace_names.json`, `tools/trace_names_jm.json`, `tools/regen_golden.py`, `core/tests/golden/README.md`, CMake function `ovt_add_trace_golden_test(<stream> <codec>)`.
**Interface:** §7.7.1, §7.7.3.
**Steps:** 1. Normalisers for FFmpeg, JM, OVT formats. 2. Name mapping tables with a unit test that every mapped name exists in the spec cross-reference appendix. 3. Diff + report (first 20 mismatches with bit positions). 4. `regen_golden.py` runs the oracles (requires `ffmpeg` with `trace_headers`) and writes zstd-compressed goldens. 5. CTest integration: golden tests compare OVT output with the committed golden, no oracle binary needed.
**Acceptance tests:** 1. Given identical traces, When diffed, Then exit code 0. 2. Given one bit-position mismatch, When diffed, Then exit code 1 and the report names the element and both positions. 3. Given a missing `ffmpeg`, When `regen_golden.py` runs, Then it fails with a clear message (never writes empty goldens).
**Size:** M.
**Notes / pitfalls:** `trace_headers` prints with `-loglevel trace` only; capture stderr. **Overlap:** this WI delivers only `tools/trace_names*.json` and `tools/trace_diff.py` as a normaliser consumed by WI-0.555's `syntax-elements` family; the CTest glue is `ovt_add_golden_test` of WI-0.556 and goldens are regenerated in the oracle image (WI-0.554, WI-0.567) — no `regen_golden.py`. See chapter 16 §16.3.

### WI-1.127 — GStreamer codecparsers cross-check harness (optional test dependency)
**Goal:** §7.7.2 value-level comparison for all three codecs.
**Depends on:** WI-1.109, WI-1.113, WI-1.118.
**Produces:** `core/tests/syntax/gst_crosscheck.cpp`, `core/tests/syntax/gst_field_map.inc` (generated by `tools/gen_gst_field_map.py` from a YAML list), CMake option `OVT_TEST_WITH_GST`.
**Interface:** §7.7.2.
**Steps:** 1. CMake detection via `pkg-config gstreamer-codecparsers-1.0`. 2. Field map YAML (~600 entries) + generator. 3. Harness iterating units and comparing. 4. CI job on Linux with GStreamer installed.
**Acceptance tests:** 1. Given GStreamer present and the corpora, When the harness runs, Then zero field mismatches for H.264 SPS/PPS/slice headers, H.265 VPS/SPS/PPS/slice headers and AV1 sequence/frame headers on all streams in §7.7.6. 2. Given GStreamer absent, When CTest runs, Then the test is reported SKIPPED, not failed.
**Size:** M.
**Notes / pitfalls:** GStreamer's `GstH264SliceHdr.max_pic_num` and OVT `MaxPicNum` are both derived; derived-field comparisons are the point of this oracle. **Overlap:** superseded by WI-0.562 (GStreamer oracle program in the oracle image) and WI-1.572 (golden suite); no in-process link against GStreamer; the field map YAML idea becomes `tools/gen_gst_fields.py`. See chapter 16 §16.3.

### WI-1.128 — CLI hooks: `ovt units`, `ovt syntax`, `ovt index` (superseded by WI-1.504/WI-1.505; see chapter 16 §16.3)
**Goal:** Expose this chapter's outputs through the CLI (full grammar in the CLI chapter; this WI implements the backend calls and the trace/JSON formats used by tests).
**Depends on:** WI-1.121, WI-1.125, WI-1.500.
**Produces:** nothing of its own (superseded): `cli/src/cmd/syntax.cpp` and `cli/src/cmd/index.cpp` are produced by WI-1.504 and WI-1.505 (chapter 12); the golden-test entry point `tools/trace_diff.py` is WI-1.126's.
**Interface:** superseded — the grammar is chapter 12 §12.2.3 (`ovt syntax <file> [--list | --unit N | --frame N | --type T] [--trace] [-f json|text|fb|csv]`, `ovt units` = `ovt syntax --list`) and §12.2.4 (`ovt index <file> [-f json|fb] [-o path]`); exit codes per chapter 12 §12.1.5.
**Steps:** 1. Argument parsing (CLI chapter's library). 2. `units`: table/JSON of `Unit` records. 3. `syntax`: tree/JSON/trace writers (WI-1.106) with unit/frame selection. 4. `index`: FlatBuffer (`.ovtidx`) or JSON via reflection; `--diagnostics-only`. 5. Shell tests on one stream per codec.
**Acceptance tests:** 1. Given `ovt syntax --unit 0 --trace` on a JVT stream, When compared to `trace_headers`, Then identical (this is the golden test entry point). 2. Given a corrupt file, When `ovt index` runs, Then the exit code of chapter 12 §12.1.5 for Error-severity diagnostics and JSON diagnostics on stdout. 3. Given `ovt index -f fb -o x.ovtidx`, When run twice, Then both files are byte-identical.
**Size:** M.
**Notes / pitfalls:** Stdout is data only; logs go to stderr. **Overlap:** superseded by WI-1.504 (`ovt syntax`, incl. list mode replacing `ovt units`) and WI-1.505 (`ovt index`); its acceptance tests 1–3 move there; chapter 12 §12.2 grammar wins; subcommand sources live in `cli/src/cmd/`. See chapter 16 §16.3.

### WI-1.129 — Fuzz targets for the H.264, H.265 and AV1 header parsers
**Goal:** §0.7.1 fuzz coverage of every parser; harness feeding random units through the index builder.
**Depends on:** WI-1.121.
**Produces:** `core/tests/fuzz/fuzz_h264_units.cpp`, `fuzz_h265_units.cpp`, `fuzz_av1_obus.cpp`, `fuzz_index_builder.cpp`, seed corpora from §7.7.6 (first 64 KiB of 10 streams each), CI smoke step.
**Interface:** §7.3 parsers; §7.4.
**Steps:** 1. Per-codec target: treat input as one unit, run every parser that accepts its type with a fresh `ActiveSets` seeded with a valid SPS/PPS. 2. Index target: input as an Annex B / OBU stream. 3. Assert: only `ParseError` escapes; no sanitizer findings; parse time < 1 s per input (catch quadratic loops).
**Acceptance tests:** 1. Given 60 s per target in CI with ASan/UBSan, When run, Then no crash/timeouts. 2. Given a 10-minute local run, When finished, Then coverage of `core/src/syntax/**` ≥ 85 % lines (`llvm-cov` report checked in `docs/dev/fuzz.md`).
**Size:** S.
**Notes / pitfalls:** Reset `DecoderState`/`ActiveSets` per input; otherwise crashes are not reproducible.

### WI-1.130 — C API for unit bytes and on-demand syntax; Python/CLI smoke
**Goal:** Implement `ovt_stream_unit_bytes` (raw and RBSP) and finalize `ovt_stream_unit_syntax`, with API tests.
**Depends on:** WI-1.125.
**Produces:** `core/src/api/stream_api.cpp`, `core/include/ovt/index.h`, `core/tests/api/test_api_stream_api.cpp`.
**Interface:** §0.6.1 `ovt_stream_unit_bytes`; §7.4.5 `ovt_stream_unit_syntax`.
**Steps:** 1. Raw bytes via `RandomAccessFile::view`; RBSP via `rbsp_from_nal` (cached per unit for 64 units). 2. Error mapping. 3. Tests: bytes match `dd`-extracted ranges; RBSP equals the test-only reference EPB stripper of WI-1.104.
**Acceptance tests:** 1. Given unit N, When `ovt_stream_unit_bytes` is called, Then `out_raw` equals the file bytes at `file_span` and `out_rbsp` equals `rbsp_from_nal(out_raw)`. 2. Given `unit_index >= units.size()`, When called, Then `OVT_E_INVALID_ARG`.
**Size:** S.
**Notes / pitfalls:** Out-parameter `ovt_Buffer`s are caller-owned and freed with `ovt_buffer_free` (D4, chapter 12 §12.3.1 rule 3a); the stream keeps no reference to them. **Overlap:** the header text of `core/include/ovt/index.h` is owned by WI-1.511 (chapter 12 §12.3); this WI implements the functions and must not diverge from §12.3. See chapter 16 §16.3.

### Phase 2 — plugin host and plugins (WI-2.150 … WI-2.174; WI-2.150, WI-2.169, WI-2.171, WI-2.172 are scheduled in phase 1, chapter 16 §16.3.12)

### WI-2.150 — Plugin discovery, loading and ABI check
**Goal:** §7.5.1 `PluginRegistry`.
**Depends on:** WI-0.000, WI-1.100 (this WI writes `core/include/ovt/abi/analysis_decoder_v1.h` verbatim from §0.6.2 and §7.5.1; no earlier WI publishes it).
**Produces:** `core/include/ovt/cxx/analysis/decoder_plugin_registry.hpp`, `core/src/analysis/decoder_plugin_registry.cpp`, `core/src/analysis/plugin_paths_{macos,linux,android}.cpp`, `core/tests/analysis/test_analysis_plugin_registry.cpp`, `core/tests/analysis/mock_plugin/` (a tiny plugin returning synthetic grids; also used by the GUI tests).
**Interface:** §7.5.1; §0.6.2.
**Steps:** 1. Path resolution per platform + `OVT_PLUGIN_PATH`. 2. `dlopen`/`dlsym`/ABI/caps validation; logging. 3. Registry with defaults and settings override. 4. Mock plugin (reports codec `H264`, produces synthetic grids from the unit sizes it receives) built as `libovt_plugin_mock.so`. 5. Tests: wrong ABI version rejected; wrong `caps->id` rejected; duplicate id precedence.
**Acceptance tests:** 1. Given `OVT_PLUGIN_PATH` pointing to the mock plugin dir, When `rescan`, Then the mock plugin is listed with its caps. 2. Given a library exporting `abi_version = 2`, When loaded, Then it is rejected and logged. 3. Given two dirs with the same id, When scanned, Then the first wins.
**Size:** M.
**Notes / pitfalls:** On macOS, `dlopen` of an unsigned plugin inside a notarised app needs the plugin to be signed too (Platforms chapter); the loader logs the `dlerror()` text. **Overlap:** this WI publishes `core/include/ovt/abi/analysis_decoder_v1.h`; it is a phase-1 deliverable (milestone M1.1) because playback decoding (WI-2.171) is a phase-1 exit requirement. See chapter 16 §16.3.

### WI-2.151 — GOP-local analysis driver (`ovt_analysis_open/frame/plane/close`)
**Goal:** §7.5.2–7.5.4 `Session` with warm decoder reuse, cancellation, error policy.
**Depends on:** WI-2.150, WI-1.121, WI-1.124, WI-0.091 (cache module API).
**Produces:** `core/src/analysis/session.cpp`, `core/src/api/analysis_api.cpp`, `core/tests/analysis/test_analysis_session.cpp` (with the mock plugin and a scripted decode).
**Interface:** §0.6.1 `ovt_analysis_*`; §7.5.3.
**Steps:** 1. Steps A0–A7 as code. 2. Parameter-set feeding from `ActiveSets` snapshots. 3. H.265 RASL skip when the GOP-local decode starts at a CRA. 4. AV1 Annex B OBU rewrite (has_size). 5. Cancellation and error counting. 6. Tests with the mock plugin: request T then T+1 (warm path: decoder not recreated), T−1 (recreated), cancel mid-GOP.
**Acceptance tests:** 1. Given a mock stream with RAPs every 30 frames, When frames 45 then 46 are requested, Then `create` is called once and `push` only for frame 46's units. 2. Given a job cancelled during frame 20 of 30, When `frame()` returns, Then `OVT_E_CANCELLED` and a subsequent request for frame 25 continues from frame 21 without recreating. 3. Given a plugin returning `OVT_E_CORRUPT` for 32 consecutive pushes, When `frame()` runs, Then it returns `OVT_E_CORRUPT` with a Diagnostic. 4. Given an AV1 `show_existing_frame` target, When requested, Then the returned `FrameAnalysis.frame.show_existing_of` is set and no decoding beyond the referenced frame occurs.
**Size:** L.
**Notes / pitfalls:** Never `flush` unless the target did not appear; after a flush the decoder must be destroyed.

### WI-2.152 — Cache integration for `FrameAnalysis` and planes
**Goal:** §7.5.5: lookup/store keys, intermediate-frame storage, plane blobs, LRUs (§0.7.3).
**Depends on:** WI-2.151, WI-0.091, WI-0.092 (disk cache store and eviction).
**Produces:** `core/include/ovt/cxx/analysis/frame_cache.hpp` (the `FrameCache` adapter of §7.5.5), `core/src/analysis/analysis_cache.cpp`, `core/tests/analysis/test_analysis_cache.cpp`.
**Interface:** §7.5.5.
**Steps:** 1. Key struct and hashing. 2. Store/lookup with stage masks. 3. In-memory LRU 64 grids / 4 planes. 4. Tests: second request is a cache hit (no plugin calls); plugin version change misses.
**Acceptance tests:** 1. Given frame T analysed once, When requested again, Then no `push` occurs and the FlatBuffer is byte-identical. 2. Given a plugin `version` change, When requested, Then a decode occurs. 3. Given 70 frames analysed, When memory is inspected, Then ≤ 64 grids are resident.
**Size:** M.
**Notes / pitfalls:** Plane blobs for intermediate frames are never stored.

### WI-2.153 — Plugin conformance harness
**Goal:** A reusable test driver that feeds a corpus stream through any plugin via the host and checks ABI rules (decode_index fill, no crash on corrupt data, determinism).
**Depends on:** WI-2.151.
**Produces:** `core/tests/analysis/plugin_conformance.cpp` (parameterised by plugin id and stream list), `tools/corrupt_stream.py` (bit-flip/truncate generator).
**Interface:** §0.6.2 rules.
**Steps:** 1. Determinism: analyse frame twice → identical bytes. 2. `decode_index` equality. 3. 1 000 corrupted variants → never a crash, only `OVT_E_CORRUPT`/diagnostics. 4. Extension table presence: every `Block` has `ext_type` from the codec's allowed set.
**Acceptance tests:** 1. Given any plugin and 5 streams, When frame 10 is analysed twice, Then byte-identical FlatBuffers. 2. Given 1 000 corrupted streams, When analysed under ASan, Then zero crashes. 3. Given any `FrameAnalysis`, When validated, Then `grid.cells_w*cells_h == length of every non-empty vector`.
**Size:** M.
**Notes / pitfalls:** Run under a 60 s per-stream timeout; libaom on corrupted data can be slow. **Overlap:** this WI also delivers the cross-plugin consistency checks of chapter 14 §14.6.2 (moved from WI-2.580). See chapter 16 §16.3.

### WI-2.154 — `third_party/libaom.cmake` with inspection build
**Goal:** Reproducible libaom build (pinned tag, flags of §7.6.1) for macOS/Linux/Android, with the extension patch hook.
**Depends on:** WI-0.013 (owns `third_party/libaom.cmake`), WI-0.010.
**Produces:** `third_party/libaom.cmake`, `third_party/VERSIONS.md` entry, CI cache keys.
**Interface:** §7.6.1 build row.
**Steps:** 1. ExternalProject with the exact flags. 2. Patch application step (`tools/apply_patches.py --check`, i.e. `git apply --check` then apply, fails hard; issue #275). 3. Export `LIBAOM_SOURCE_DIR` for the semi-public headers. 4. Android toolchain args. 5. Verify `AV1_SET_INSPECTION_CALLBACK` works via a 20-line smoke program.
**Acceptance tests:** 1. Given `cmake --preset macos-arm64-release`, When built, Then `libaom.a` exists and the smoke program receives one callback per frame on `av1-1-b8-01-size-16x16.ivf`. 2. Given a deliberately broken patch, When built, Then the build fails with the patch name in the error.
**Size:** S.
**Notes / pitfalls:** `CONFIG_INSPECTION` requires `CONFIG_ACCOUNTING` for `-A`; both must be 1. **Overlap:** this WI delivers only the extension-patch hook, `LIBAOM_SOURCE_DIR` export and the inspection-callback smoke program; WI-0.013 owns `third_party/libaom.cmake`. See chapter 16 §16.3.

### WI-2.155 — `ovt-plugin-aom` skeleton: ABI glue, temporal-unit batching, callback plumbing
**Goal:** A loadable plugin that decodes AV1 and emits `FrameAnalysis` with frame-level fields and reconstruction planes only.
**Depends on:** WI-2.154, WI-2.150.
**Produces:** `plugins/ovt-plugin-aom/CMakeLists.txt`, `plugins/ovt-plugin-aom/src/plugin.cpp`, `plugins/ovt-plugin-aom/src/decoder.cpp`, `plugins/ovt-plugin-aom/src/fb_builder.cpp`, `plugins/ovt-plugin-aom/tests/test_aom_plugin_smoke.cpp`.
**Interface:** §0.6.2; §7.6.1 "Decoder driving".
**Steps:** 1. `caps()`. 2. `create/destroy` with controls. 3. OBU accumulation per temporal unit; the per-frame `aom_codec_decode` / `Av1DecodeReturn` loop of §7.6.1 with positional `decode_index` assignment and `show_existing` handling; callback → `ifd_init/ifd_inspect/ifd_clear`. 4. Frame-level mapping and reconstruction planes of shown frames. 5. `pull/release/flush`. 6. Smoke test through the host on libaom vectors: frame count equals the index.
**Acceptance tests:** 1. Given `av1-1-b8-01-size-66x66.ivf`, When all frames are analysed via the host, Then one `FrameAnalysis` per `FrameInfo` (hidden included) with matching `decode_index`. 2. Given a library built without inspection (test fixture), When `create` is called, Then `OVT_E_PLUGIN` with the specified message.
**Size:** M.
**Notes / pitfalls:** The callback runs on libaom's decode thread when row-MT is on; copy data inside the callback and hand off; never call libaom from inside the callback. `user_priv` of `aom_codec_decode` is **owned by the inspection path** (`Av1DecodeReturn`), never pass a hint pointer there. A TU with a hidden ALTREF followed by an overlay frame produces two callbacks and two `aom_codec_decode` iterations; `av1-1-b8-05-mv`/`-06-mfmv` exercise this.

### WI-2.156 — `ovt-plugin-aom`: `insp_mi_data` → CellGrid / Block tree / MotionVector / RefSlot
**Goal:** The mapping table of §7.6.1 (all rows except accounting and patch-only fields).
**Depends on:** WI-2.155.
**Produces:** `plugins/ovt-plugin-aom/src/map_mi.cpp`, `plugins/ovt-plugin-aom/src/partition_reconstruct.cpp`, `plugins/ovt-plugin-aom/src/enums.h` (tables), `plugins/ovt-plugin-aom/tests/test_aom_map_mi.cpp`.
**Interface:** §7.6.1 mapping table; Data Model `Av1Block`, `Av1Txb`, `Av1FilterParams`.
**Steps:** 1. Enum tables (bsize → w/h, tx_size → log2, mode names). 2. Partition-tree reconstruction per superblock. 3. Cell vectors. 4. `MotionVector` and `refs[]`. 5. TXB grouping. 6. `Av1Block` ext per leaf. 7. Tests vs `inspect` JSON (`aom_grid_vs_inspect`, WI-2.161 provides the harness; here unit tests on 3 frames).
**Acceptance tests:** 1. Given frame 0 of `av1-1-b8-02-allintra.ivf`, When mapped, Then `grid.intra_mode_y` equals `inspect`'s `mode` array cell by cell. 2. Given `av1-1-b8-05-mv`, When mapped, Then `mv0_x == 2 * motionVectors[..][0]` for every inter cell and `ref0 == referenceFrame[..][0]` for every inter cell (the reference type is the `refs[]` index), `refs[ref0].name` is the matching `LAST…ALTREF` name and `refs[ref0].slot == ref_frame_idx[ref0 − 1]`. 3. Given the synthetic 128×128-superblock stream `syn_av1_tiles_cdef_lr --sb-size=128` (chapter 14 synthetic corpus; the Argon `partition` streams run in the nightly tier only), When partitions are reconstructed, Then every leaf rect is covered exactly once and HORZ_A/VERT_4 cases match `inspect`'s `blockSize` layout.
**Size:** L.
**Notes / pitfalls:** `mi_cols` is even; drop the padding column when `coded_w % 8 ∈ {1..4}`. `ref_frame[1] == 0` (INTRA_FRAME) marks interintra, not a second reference. Partition disambiguation without the patch (`mi.partition` absent): for a square `S` at `(r, c)` of size `n` MIs with `b = bsize` at its origin — `b == S` → leaf; `b` is `n × n/2` → HORZ, `n/2 × n` → VERT; `b` is `n/2 × n/2` and all four quadrant origins have `bsize == n/2 × n/2` → **SPLIT unless** the top-right and bottom quadrants form one `n × n/2` rectangle (then HORZ_A/HORZ_B) — decide by comparing the quadrant origins' `bsize`: HORZ_A = two `n/2` squares on top + `n × n/2` bottom; HORZ_B = `n × n/2` top + two squares; VERT_A/VERT_B mirrored; `b` is `n × n/4` → HORZ_4, `n/4 × n` → VERT_4; any other combination → recurse as SPLIT and set `derived "partition ambiguous"` on the parent block. SPLIT vs four-leaf is the only truly ambiguous case and is resolved as SPLIT (both produce identical leaves).

### WI-2.157 — `ovt-plugin-aom`: accounting → `SymbolCost` and `bits_x8`
**Goal:** Per-symbol costs and per-block/cell bit heat map from `Accounting`.
**Depends on:** WI-2.156.
**Produces:** `plugins/ovt-plugin-aom/src/map_accounting.cpp`, tests.
**Interface:** §7.6.1 accounting row.
**Steps:** 1. Read `fd.accounting` in the callback (copy symbols). 2. `symbol_names` dictionary. 3. Attribute symbols to leaf blocks by `(x,y)`; frame-level symbols to root. 4. Distribute to cells with exact remainder handling. 5. Test: Σ `symbols[].bits_x8` == Σ leaf `Block.bits_x8` + root bits; compare totals per SB with `inspect -A` per-symbol lists.
**Acceptance tests:** 1. Given any frame, When summed, Then `Σ grid.bits_x8 == Σ leaf Block.bits_x8` exactly. 2. Given `inspect -A` output, When compared per `(x4,y4,id)`, Then equal bits and counts.
**Size:** M.
**Notes / pitfalls:** Accounting x,y are MI units of the block origin as set by `aom_accounting_set_context`; some symbols are recorded before the context update of the next block (libaom quirk) — attribute by the recorded context, do not "fix" it, so the oracle matches.

### WI-2.158 — `ovt-plugin-aom`: planes (reconstruction, prediction, pre-filter) → `PlaneRef`
**Goal:** Export the `Reconstruction` planes of shown frames from `aom_codec_get_frame` (the only stage stock libaom provides) with the superres/film-grain notes of §7.6.1.
**Depends on:** WI-2.155.
**Produces:** `plugins/ovt-plugin-aom/src/planes.cpp`, tests.
**Interface:** §7.6.1 plane rows; §0.5.4 `PlaneRef`.
**Steps:** 1. Reconstruction from `aom_codec_get_frame`, attached positionally per §7.6.1 "Decoder driving"; hidden frames get no planes (host adds `ANA-0001`). 2. Superres: record `UpscaledWidth`; `PlaneRef.width = d_w`. 3. Blob store with ids `"<decode_index>/<stage>/<plane>"`. 4. Tests: reconstruction equals `aomdec --rawvideo -o` output for the shown frame (MD5 of planes); a stream with `apply_grain = 1` carries `derived "reconstruction includes film grain"`.
**Acceptance tests:** 1. Given `av1-1-b8-01-size-16x16`, When planes are exported, Then MD5 of Y/U/V equals the `.md5` file shipped with the vector. 2. Given `plane_stage_mask` without Reconstruction, When pulled, Then `planes[]` is empty and no copy happens.
**Size:** M.
**Notes / pitfalls:** `aom_image_t` is in output (upscaled) size; superres streams need the stretch note in the FlatBuffer.

### WI-2.159 — `ovt-plugin-aom`: extension patch `0001-inspection-extend-mi-data`
**Goal:** The ~100–300-line libaom patch of §7.6.1 and the plugin mapping of the new fields into `Av1Block`, `grid.filter_idx`, `MotionVector.mvd/predictor`, LR/CDEF params and the new plane stages.
**Depends on:** WI-2.156, WI-2.158.
**Produces:** `plugins/ovt-plugin-aom/patches/0001-inspection-extend-mi-data.patch`, `plugins/ovt-plugin-aom/patches/0002-inspection-frame-snapshots.patch`, `plugins/ovt-plugin-aom/src/map_mi_ext.cpp`, `plugins/ovt-plugin-aom/tests/test_aom_map_mi_ext.cpp`.
**Interface:** §7.6.1 extension table.
**Steps:** 1. Add fields to `insp_mi_data`/`insp_frame_data`; copy in `ifd_inspect`. 2. `read_mv` mvd recording array. 3. Snapshots in `decodeframe.c`. 4. `inspect.c` flags. 5. Plugin mapping. 6. Golden vs patched `inspect` for the new arrays.
**Acceptance tests:** 1. Given a patched build, When `av1-1-b8-23-film_grain-50` frame 5 is analysed, Then `Av1Block.AngleDeltaY` equals `inspect -ad` output for every intra directional block. 2. Given warped-motion blocks (Argon), When analysed, Then `wm_params[6]` equal `inspect -wm`. 3. Given Argon streams whose frame headers disable CDEF and loop restoration, When `PreSaoOrCdef` and `PreLoopRestoration` are requested, Then both planes are identical to `Reconstruction` (and `PreDeblock` differs from it exactly where deblocking modified samples).
**Size:** L.
**Notes / pitfalls:** Keep every addition under `#if CONFIG_INSPECTION`; the default libaom build must be byte-identical.

### WI-2.160 — Upstream the libaom inspection extension
**Goal:** Submit the two patches to AOM Gerrit and track review.
**Depends on:** WI-2.159.
**Produces:** Gerrit change URLs recorded in `plugins/ovt-plugin-aom/README.md`; "Upstreaming" section of `plugins/ovt-plugin-aom/README.md`.
**Interface:** n/a.
**Steps:** 1. Sign the AOM CLA process as applicable (Licensing chapter). 2. Split into two changes with tests (`test/inspection_test.cc` additions). 3. Address review for two cycles; rebase the patch files on each libaom bump.
**Acceptance tests:** 1. Given the changes, When posted, Then URLs are recorded and CI applies the same patch files cleanly to the pinned tag.
**Size:** S (initial submission; review time excluded).
**Notes / pitfalls:** Upstream may prefer a `aom_inspect_init` version bump; adapt the plugin to both.

### WI-2.161 — `ovt-plugin-aom` golden tests vs `inspect` JSON
**Goal:** §7.7.5 harness over all libaom test vectors + 30 Argon streams.
**Depends on:** WI-2.157, WI-2.154 (`OVT_BUILD_AOM_INSPECT`).
**Produces:** `plugins/ovt-plugin-aom/tests/test_aom_inspect_golden.cpp`, `tools/aom_inspect_to_golden.py`, goldens.
**Interface:** §7.7.5.
**Steps:** 1. Run `inspect -A` to JSON (§7.7.5); convert to compact golden. 2. Compare every grid vector and symbol totals. 3. Report first mismatches with MI coordinates.
**Acceptance tests:** 1. Given all vectors, When compared, Then zero mismatches (`aom_grid_vs_inspect`). 2. Given a vector with hidden frames, When compared, Then `frame` numbering in JSON equals `decode_index`.
**Size:** M.
**Notes / pitfalls:** `inspect` prints `-1` for n/a; map to OVT sentinels before comparing. **Overlap:** superseded by WI-2.581 (golden suite vs `inspect`, using the goldens of WI-0.566); `tools/aom_inspect_to_golden.py` is replaced by the normaliser of WI-0.555. See chapter 16 §16.3.

### WI-2.162 — `third_party/libde265.cmake` and patch 0001 (internals API)
**Goal:** Pinned libde265 build with `DE265_INTERNALS` and the read-only accessor header of §7.6.2.
**Depends on:** WI-0.014 (owns `third_party/libde265.cmake` and the patch step), WI-0.010.
**Produces:** (reduced per §16.3: `third_party/libde265.cmake` and the patch step are owned by WI-0.014, chapter 13) `plugins/ovt-plugin-de265/patches/0001-de265-internals-api.patch` (adds the upstream header `de265_internals.h` and source `de265_internals.cc` under libde265's own `libde265` directory, the `deblk_bS_v` array and store, and the `DE265_INTERNALS` CMake option), `third_party/VERSIONS.md` entry, `plugins/ovt-plugin-de265/tests/internals_smoke.cpp`.
**Interface:** §7.6.2 patch 0001 header.
**Steps:** 1. ExternalProject + patch step. 2. Write the header exactly as specified; implement each function over existing accessors. 3. Smoke: decode `WPP_A_ericsson_MAIN_2` and print CB info of CTU 0. 4. Android/arm64 build (no option needed: v1.0.15 selects SSE kernels only on x86 processors; verify the arm64 build has no `sse` objects).
**Acceptance tests:** 1. Given the patched build, When `DE265_INTERNALS=OFF`, Then the resulting library's exported symbol list equals upstream's (`nm` diff) **and** the object code of `deblock.cc` is identical to upstream's (the `deblk_bS_v` store is inside `#ifdef DE265_INTERNALS`); with `DE265_INTERNALS=ON` the only added symbols are the `de265_internals_*` functions. 2. Given `AMP_A_Samsung_7` frame 0, When `de265_internals_get_CB_info` is called for every MinCb, Then `PartMode` values include AMP modes 4–7 and `log2CbSize` ∈ [3,6].
**Size:** L.
**Notes / pitfalls:** `MetaDataArray` getters take luma sample coordinates in some accessors and unit coordinates in others — the C header uses **unit** coordinates everywhere; convert inside. **Overlap:** this WI delivers only patch 0001 (internals header and implementation) and the smoke program; WI-0.014 owns `third_party/libde265.cmake` and the patch-series mechanism (this WI replaces the placeholder patch). See chapter 16 §16.3.

### WI-2.163 — `ovt-plugin-de265` skeleton: ABI glue, push/decode/pull with decode-index tracking
**Goal:** Loadable plugin producing frame-level `FrameAnalysis` + reconstruction planes.
**Depends on:** WI-2.162, WI-2.150.
**Produces:** `plugins/ovt-plugin-de265/CMakeLists.txt`, `plugins/ovt-plugin-de265/src/plugin.cpp`, `plugins/ovt-plugin-de265/src/decoder.cpp`, `plugins/ovt-plugin-de265/tests/test_de265_plugin_smoke.cpp`.
**Interface:** §0.6.2; §7.6.2 "Decoder driving".
**Steps:** 1. `caps()`. 2. `create` with threads. 3. `push_NAL` + decode loop; errors/warnings → diagnostics. 4. Picture retrieval with `user_data` decode index; outstanding-image limit. 5. `flush`. 6. Smoke via host on HEVC_v1 streams: every frame produced; reconstruction MD5 equals the stream's `decoded_picture_hash` SEI.
**Acceptance tests:** 1. Given 20 HEVC_v1 streams, When all frames are analysed, Then each `FrameAnalysis` reconstruction matches the MD5 SEI (`de265_md5_vs_sei`). 2. Given an SCC stream, When pushed, Then `OVT_E_UNSUPPORTED` with the "use ovt-plugin-hm" diagnostic.
**Size:** M.
**Notes / pitfalls:** libde265 returns pictures in output order; never infer `decode_index` from arrival order.

### WI-2.164 — `ovt-plugin-de265`: CTB/CB/PB/TU/intra/deblock/SAO → model
**Goal:** The mapping table of §7.6.2 for data available with patch 0001.
**Depends on:** WI-2.163.
**Produces:** `plugins/ovt-plugin-de265/src/map_blocks.cpp`, `plugins/ovt-plugin-de265/src/map_grid.cpp`, `plugins/ovt-plugin-de265/tests/test_de265_map.cpp`.
**Interface:** §7.6.2 mapping table; Data Model `HevcCtu/HevcCu/HevcPu/HevcTu/HevcFilterParams`.
**Steps:** 1. CTU/CU tree from CB info. 2. PU rectangles per PartMode; MVs/refs; `refs[]` from slice header RefPicList POCs. 3. Intra modes. 4. TU tree from split bits; cbf; DST flag. 5. Deblock bS (vertical from `deblk_bS_v`, horizontal from the flags byte); SAO into `HevcCtu`; slice partition ids. 6. Tests vs the `hm_cu_dump` JSON oracle of §7.7.4 (`tests/golden/h265/<stream-id>.hm-grid.json`, chapter 14 §14.4.4 — the only location; produced by the `hm_cu_dump` getter dumper of chapter 14: per-CU `split_cu_flag`, `part_mode`, `pred_mode_flag`, `merge_idx`, `ref_idx`, `mvd`, `cbf_*`, `cu_qp_delta`) for the 10 streams tagged `h265-cu-golden` (the HM *plugin* arrives only in WI-4.105).
**Acceptance tests:** 1. Given `AMP_A_Samsung_7` frame 0, When mapped, Then every CU rect from OVT equals the CU partition traced by HM (`h265_cu_tree_vs_hm`). 2. Given `MVCLIP_A_qualcomm_3` (nightly tier — 1080p, not in the fast set), When mapped, Then `mv0_x/y` (÷4) equal HM's `mvL0` per PU; the fast-tier variant uses `MVEDGE_A_qualcomm_3`. 3. Given `SAO_A_MediaTek_4`, When mapped, Then `filter_idx` per CTB equals HM's traced `sao_type_idx_luma`.
**Size:** L.
**Notes / pitfalls:** `intraPredModeC` from libde265 is already the derived mode (after mode-4 and 4:2:2 remap).

### WI-2.165 — `ovt-plugin-de265`: patch 0002 (persist PU/TU syntax, CU bit positions)
**Goal:** §7.6.2 patch 0002 and the resulting `merge/mvd/mvp/cbf[3]/transform_skip/nnz/bits_x8` mapping.
**Depends on:** WI-2.164.
**Produces:** `plugins/ovt-plugin-de265/patches/0002-de265-persist-pu-tu-syntax.patch`, `plugins/ovt-plugin-de265/src/map_syntax.cpp`, `plugins/ovt-plugin-de265/tests/test_de265_map_syntax.cpp`.
**Interface:** §7.6.2 patch 0002.
**Steps:** 1. New `MetaDataArray`s and write points in `slice.cc`. 2. CABAC position helper and fractional-bit table. 3. Getters. 4. Plugin mapping (`predictor`, `mvd`, `HevcPu/HevcTu` fields, `bits_x8`). 5. Tests vs HM trace (`merge_idx`, `mvd`, `cbf`) and bit totals: Σ CU `bits_x8` per slice within ±1 % of the slice segment data size × 8 × 8.
**Acceptance tests:** 1. Given 10 HEVC_v1 streams, When PU syntax is compared to HM's trace, Then `merge_flag/merge_idx/mvd/ref_idx` equal for every PU. 2. Given any slice, When CU bits are summed, Then within ±1 % of the exact slice data bits (tolerance for CABAC termination). 3. Given `DE265_INTERNALS=OFF`, When built, Then decoding speed is unchanged (±2 %) vs WI-2.162's build.
**Size:** L.
**Notes / pitfalls:** `read_coding_unit` is re-entered for dependent slices; record positions per CU, not per call.

### WI-2.166 — `ovt-plugin-de265`: patch 0003 (pre-deblock / pre-SAO snapshots, prediction, residual, coefficients)
**Goal:** §7.6.2 patch 0003 and the `PlaneRef` export of the five extra stages.
**Depends on:** WI-2.163.
**Produces:** `plugins/ovt-plugin-de265/patches/0003-de265-plane-snapshots.patch`, `plugins/ovt-plugin-de265/src/planes.cpp`, `plugins/ovt-plugin-de265/tests/test_de265_planes.cpp`.
**Interface:** §7.6.2 patch 0003; §0.5.4 `PlaneStage`.
**Steps:** 1. Allocation of snapshot planes behind runtime flags. 2. Copies at the deblock/SAO entry points. 3. Prediction/residual/coefficient capture in `transform.cc`. 4. Export. 5. Tests: on an intra-only stream, pre-deblock plane equals a decode with `DE265_DECODER_PARAM_DISABLE_DEBLOCKING=1` and `DISABLE_SAO=1`; `pred + residual == recon(pre-deblock)` exactly for every sample.
**Acceptance tests:** 1. Given `ipcm_A_NEC_3` (intra), When `PreDeblock` is compared with a filters-off decode, Then identical. 2. Given any frame, When `Prediction + Residual` is computed, Then equals `PreDeblock` for every sample (`de265_pred_plus_resid`). 3. Given flags off, When decoding, Then memory use equals WI-2.163's build (±1 MB).
**Size:** L.
**Notes / pitfalls:** PCM and transquant-bypass CUs have residual = recon − pred with pred = 0 by convention; document in `derived`.

### WI-2.167 — Upstream libde265 patch 0001 (PR) and track
**Goal:** Open the PR on strukturag/libde265 with patch 0001; record status; keep patch files rebased.
**Depends on:** WI-2.162.
**Produces:** PR URL and review status in the "Upstreaming" section of `plugins/ovt-plugin-de265/README.md`.
**Interface:** n/a.
**Steps:** 1. PR with tests (`libde265/tests/internals_test.cc`), CMake option, documentation. 2. Respond to review; 3. On acceptance, drop patch 0001 and bump the pin.
**Acceptance tests:** 1. Given the PR, When CI runs on the pinned tag plus patch, Then it applies cleanly (`git apply --check`).
**Size:** S.
**Notes / pitfalls:** Keep the patch independent of 0002/0003 so it can merge alone.

### WI-2.168 — `ovt-plugin-de265`: Android/arm64 performance measurement and mitigations
**Goal:** Measure GOP decode time on Pixel Tablet / M1 and apply §7.6.2 mitigations; decide on a NEON contribution.
**Depends on:** WI-2.164.
**Produces:** `tests/perf/bench_analysis_de265_gop60_1080p.cpp`, the "Performance" section of `plugins/ovt-plugin-de265/README.md` with measurements, build flag tuning in `third_party/libde265.cmake`.
**Interface:** §0.7.4 budget.
**Steps:** 1. Perf test harness (GOP of 60 frames 1080p Main, WPP stream and non-WPP stream). 2. Thread count sweep. 3. Compiler flag sweep. 4. Report; propose NEON work items if > 2× over budget on Android.
**Acceptance tests:** 1. Given the M1 reference and a 60-frame 1080p GOP, When analysed via the host, Then ≤ 1.5 s (`bench_analysis_de265_gop60_1080p`; planning value, gating per chapter 14 §14.9.3). 2. Given a Pixel Tablet, When measured, Then the time is recorded in `perf-de265.md` and the GUI progress requirement is confirmed.
**Size:** M.
**Notes / pitfalls:** Non-WPP, non-tiled streams decode single-threaded in libde265; state this in the GUI status ("stream has no WPP/tiles — single-threaded analysis").

### WI-2.169 — `third_party/ffmpeg.cmake` and `ovt-plugin-ffmpeg` analysis skeleton (H.264)
**Goal:** Pinned FFmpeg build (configure flags of §7.6.3) and a plugin that decodes H.264 with `export_side_data = mvs+venc_params`.
**Depends on:** WI-0.012 (owns `third_party/ffmpeg.cmake`), WI-2.150.
**Produces:** `third_party/ffmpeg.cmake`, `plugins/ovt-plugin-ffmpeg/CMakeLists.txt`, `plugins/ovt-plugin-ffmpeg/src/plugin.cpp`, `plugins/ovt-plugin-ffmpeg/src/h264_analysis.cpp`, `plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_plugin_smoke.cpp`, `third_party/VERSIONS.md` entry.
**Interface:** §7.6.3 "Analysis decoder driving".
**Steps:** 1. configure/make ExternalProject per platform (incl. Android cross flags, `--enable-mediacodec --enable-jni`). 2. `caps()`. 3. `create` with export flags, Annex B prefixing, `COPY_OPAQUE`. 4. AU batching by hint; send/receive. 5. Smoke: frame count and `decode_index` via `opaque`.
**Acceptance tests:** 1. Given 10 JVT streams, When analysed via the host, Then one `FrameAnalysis` per frame with correct `decode_index` and reconstruction MD5 equal to `ffmpeg -f md5` per frame. 2. Given the Android build, When `ffmpeg -version` equivalent (`avcodec_configuration()`) is logged, Then it contains `--enable-mediacodec`.
**Size:** M.
**Notes / pitfalls:** `AV_CODEC_FLAG_COPY_OPAQUE` requires FFmpeg ≥ 6.0; verify `frame->opaque` is set for every output frame even with frame threading. **Overlap:** this WI delivers only the `ovt-plugin-ffmpeg` analysis skeleton; WI-0.012 owns `third_party/ffmpeg.cmake` (both flavours); scheduled in phase 1 (M1.2) because WI-2.171 is a phase-1 deliverable. See chapter 16 §16.3.

### WI-2.170 — `ovt-plugin-ffmpeg`: AVMotionVector / AVVideoEncParams → model, n/a handling
**Goal:** The mapping table of §7.6.3 and the n/a diagnostics.
**Depends on:** WI-2.169.
**Produces:** `plugins/ovt-plugin-ffmpeg/src/map_h264_sidedata.cpp`, `plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_map_sidedata.cpp`.
**Interface:** §7.6.3 mapping table; Data Model `AvcMb`.
**Steps:** 1. MB blocks and QP from `AVVideoEncParams`. 2. MVs: rect from centre, ×4 scaling, list from sign, bi-pred detection. 3. Pred classification + diagnostics. 4. Pseudo `refs[]`. 5. Tests vs the `tests/oracles/ffmpeg_mvs.c` oracle of chapter 14 §14.4.3a (libavcodec with `AV_CODEC_FLAG2_EXPORT_MVS` + `AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS`, one CSV row per `AVMotionVector` and per-block `delta_qp`; `ffprobe` prints only the side-data type and size, never the vectors): all vectors and QPs equal after conversion.
**Acceptance tests:** 1. Given `CVWP1_TOSHIBA_E` (weighted P), When mapped, Then every `MotionVector` equals the corresponding row of `ffmpeg_mvs <file>` (chapter 14 §14.4.3a) for that frame after `tests/oracles/normalize/ffmpeg_mvs.py` (`rect`, `mv`, `list`) and counts are equal (`ffmpeg_mv_vs_oracle`). 2. Given `CAQP1_Sony_B` (QP changes), When mapped, Then `grid.qp` per MB equals `qp + delta_qp − QpBdOffsetY` from the `ffmpeg_mvs` per-block `delta_qp` output. 3. Given an intra MB, When mapped, Then `pred == Intra`, `ref0 == -1` and the frame has Diagnostic `ANA-H264-0002`. 4. Given `FRExt/Hi422FR1_SONY_A` (10-bit), When mapped, Then every `grid.qp` value lies in `−12..51` and equals the slice's `SliceQPY` plus `mb_qp_delta` accumulation recorded from JM's `trace_dec.txt` for the first picture (QpBdOffset removed, V-028).
**Size:** M.
**Notes / pitfalls:** ffprobe prints `motion_scale`; always divide 16 by it rather than assuming 4 (MPEG-2 streams would be 2).

### WI-2.171 — `ovt-plugin-ffmpeg`: playback decoders with hwaccel selection and software fallback
**Goal:** `ovt_decoder_open/seek/next` (§0.6.1) implemented by the plugin's `ovt_playback_decoder_v1` table for all codecs on macOS/Android/Linux.
**Depends on:** WI-2.169, WI-1.230 (libavformat demux wrapper), WI-1.232 (track ↔ stream matching), WI-1.121 (`StreamIndex`).
**Produces:** `plugins/ovt-plugin-ffmpeg/src/playback.cpp`, `plugins/ovt-plugin-ffmpeg/src/hwaccel_{videotoolbox,mediacodec,vaapi}.cpp`, `core/src/decode/playback_host.cpp`, `core/include/ovt/abi/playback_decoder_v1.h` (OVT-private), tests `core/tests/decode/test_decode_playback.cpp`.
**Interface:** §7.6.3 playback table; §0.6.1 `ovt_decoder_*`.
**Steps:** 1. Private ABI table (`open(stream, prefer_hw)`, `seek(display_index)`, `next(Picture*)`, `info()`, `close`). 2. Per-platform hw device creation + `get_format`; fallback. 3. Output format negotiation list; GPU-resident frames where supported. 4. Seek by RAP + discard. 5. `FrameId` resolution by pts. 6. Tests: software path decodes all corpora frames with MD5 equal to `ffmpeg -f framemd5`; seek to every 50th frame returns the right `display_index`; hw paths tested on CI runners where available (macOS VideoToolbox), else skipped.
**Acceptance tests:** 1. Given any corpus stream, When decoded with `prefer_hw = 0`, Then per-frame MD5 equals `ffmpeg -f framemd5`. 2. Given `prefer_hw = 1` on macOS, When an H.264 stream is opened, Then `info()` reports `videotoolbox` and the first picture arrives ≤ 500 ms after open (§0.7.4). 3. Given `ovt_decoder_seek(display_index = 1234)`, When `next()` is called, Then the returned `FrameId.display_index == 1234`. 4. Given an AV1 stream on a machine without hw AV1, When opened with `prefer_hw = 1`, Then fallback to `libdav1d` is logged and decoding succeeds.
**Size:** L.
**Notes / pitfalls:** MediaCodec needs the JavaVM set before `avcodec_open2`; `av_jni_set_java_vm` is called from `JNI_OnLoad` of the Android shell. VideoToolbox async output order: use `pts`, never arrival order. **Overlap:** phase-1 deliverable (milestone M1.2) despite its numbering: the §0.8 phase-1 exit demo requires playback decode. See chapter 16 §16.3.

### WI-2.172 — `ovt-plugin-ffmpeg`: fill `ovt_ColorDescription` from context and side data
**Goal:** The colour table of §7.6.3 with merge precedence and conflict diagnostics.
**Depends on:** WI-2.171.
**Produces:** `plugins/ovt-plugin-ffmpeg/src/color_description.cpp`, `core/src/color/merge.cpp`, `plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_color_description.cpp` (HDR fixtures of §7.7).
**Interface:** §0.5.6 `ovt_ColorDescription`; §7.6.3 colour table.
**Steps:** 1. Context/frame scalar mapping. 2. MDCV/CLL conversions with units. 3. HDR10+/DOVI presence and profile/level. 4. Merge with container/VUI values; `DEC-HDR-0001`. 5. Tests: each HDR fixture of §7.7 yields the expected struct (hand-written expected values).
**Acceptance tests:** 1. Given `syn_h265_main10_pq_hdr` (x265 PQ, MDCV `G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)`, When decoded, Then `mdcv_primaries == {{34000,16000},{13250,34500},{7500,3000}}`, `mdcv_max_luminance == 10000000`, `mdcv_min_luminance == 1`. 2. Given `hdr_hdr10plus_dv81` (DV profile 8.1), When opened, Then `has_dolby_vision == 1`, `dv_profile == 8`, `dv_level` from `dvvC`. 3. Given `syn_h265_main10_pq_hdr_mismatch` (container `colr` disagrees with the VUI), When opened, Then `DEC-HDR-0001` is emitted and the frame side data value wins.
**Size:** M.
**Notes / pitfalls:** FFmpeg MDCV `display_primaries` are already RGB-ordered; the SEI is GBR — do not reorder twice. **Overlap:** phase-1 deliverable (milestone M1.2) despite its numbering. See chapter 16 §16.3.

### WI-2.173 — `ovt-plugin-ffmpeg` golden tests vs ffprobe
**Goal:** Automate §7.6.3 comparisons over the JVT corpus and FATE h264 samples.
**Depends on:** WI-2.170.
**Produces:** `plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_ffprobe_golden.cpp`, `tools/ffprobe_sidedata_to_golden.py`, goldens.
**Interface:** §7.7.6 FATE row.
**Steps:** 1. Generate goldens with `tests/oracles/ffmpeg_mvs.c` (chapter 14 §14.4.3a; CSV per `AVMotionVector` plus `AVVideoEncParams` block QPs) in the oracle container image. 2. Compare MVs and QPs. 3. CI without the oracle image uses the committed goldens.
**Acceptance tests:** 1. Given all JVT Baseline/Main/High streams, When compared, Then zero MV/QP mismatches (`ffmpeg_sidedata_golden`).
**Size:** S.
**Notes / pitfalls:** ffprobe prints MVs per frame in **output** order; map by pts. **Overlap:** superseded by WI-2.583 (golden suite vs ffprobe side data and JM QP, goldens from WI-0.567). See chapter 16 §16.3.

### WI-2.174 — CLI hooks: `ovt analyze` and `ovt plugins`
**Goal:** `ovt plugins list` (registry dump) and `ovt analyze <file> --frame N [--plugin id] [--planes stages] [--format fb|json|yuview]`.
**Depends on:** WI-2.151, WI-2.152.
**Produces:** `cli/src/cmd/plugins.cpp`, `cli/src/cmd/analyze.cpp` (the YUView CSV exporter `core/src/model/yuview_csv.cpp` is produced by WI-0.089 of chapter 6 and only wired here), `cli/tests/cli_analyze_test.sh`.
**Interface:** CLI grammar above (final grammar in the CLI chapter); exit codes per chapter 12 §12.1.3.
**Steps:** 1. `plugins list` JSON/table. 2. `analyze` with host session, plane export to `.y4m`/raw per stage. 3. JSON via FlatBuffers reflection; YUView CSV writer. 4. Tests: determinism (two runs identical), `--plugin` override, missing plugin → exit 3.
**Acceptance tests:** 1. Given `ovt analyze a.ivf --frame 7 --format fb`, When run twice, Then identical files. 2. Given `--plugin hm` without the HM plugin installed, When run, Then exit code 3 and a message listing available plugins. 3. Given `--format yuview-csv`, When loaded in YUView 2.x, Then the statistics display without errors (manual check documented; automated: CSV header `%;syntax-version;v1.2` present and row count equals block count).
**Size:** M.
**Notes / pitfalls:** Plane export must use coded (uncropped) size and state it in the file name (`_coded`). **Overlap:** superseded by WI-2.520 (`ovt plugins`) and WI-2.521 (`ovt analyze`); the YUView CSV writer is WI-0.089; acceptance tests 1–3 move to WI-2.521/WI-2.520. See chapter 16 §16.3.

### Phase 4 — FFmpeg patch, HM oracle, upstreaming, HRD model (WI-4.100 … WI-4.112)

### WI-4.100 — FFmpeg patch part 1: `libavutil` block-params side data type
**Goal:** Add `AV_FRAME_DATA_VIDEO_BLOCK_PARAMS`, `libavutil/video_block_params.{h,c}`, `AVH264MacroblockParams` as specified in §7.6.3.
**Depends on:** WI-2.169.
**Produces:** `plugins/ovt-plugin-ffmpeg/patches/0001-h264-export-mb-params.patch` (part 1 hunks, including — inside the patch, in FFmpeg's own tree — the `APIchanges` entry and the `libavutil` unit test `video_block_params.c`).
**Interface:** §7.6.3 C declarations.
**Steps:** 1. Header/implementation (alloc, create_side_data, block accessor). 2. Side-data name registration in `frame.c`. 3. Version bump. 4. `libavutil` test. 5. `make fate-libavutil` passes.
**Acceptance tests:** 1. Given the patched tree, When `make fate` runs, Then all existing FATE tests pass. 2. Given `av_video_block_params_create_side_data(frame, H264, 8160)`, When blocks are written and read back, Then values round-trip and `av_frame_get_side_data` finds the entry.
**Size:** L (includes writing it in FFmpeg style and running FATE).
**Notes / pitfalls:** Follow `video_enc_params.h` conventions exactly (offset-based variable-size records) — reviewers will compare.

### WI-4.101 — FFmpeg patch part 2: `h264` decoder persistence and export
**Goal:** Persist per-MB tables and export `AVH264MacroblockParams` when `AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS` is set.
**Depends on:** WI-4.100.
**Produces:** patch hunks for `h264dec.h`, `h264dec.c`, `h264_mb.c`, `h264_cavlc.c`, `h264_cabac.c`, `options_table.h`; FATE test `fate-h264-block-params` with reference file.
**Interface:** §7.6.3 "Files/functions to touch".
**Steps:** 1. Table allocation in `alloc_picture` under the flag. 2. Writes at MB end in CAVLC and CABAC paths (intra modes, cbp, qp_delta, mvd). 3. `h264_export_block_params()` in `finalize_frame`. 4. Frame-threading safety (tables are per `H264Picture`). 5. FATE test using `ffprobe -show_frames` side data dump on a conformance sample.
**Acceptance tests:** 1. Given `CAMA1_Sony_C` (MBAFF) decoded with the flag, When side data is inspected, Then `mb_type_class`/`sub_mb_type`/`ref_idx` equal JM's `trace_dec.txt` for every MB (`ffmpeg_mbparams_vs_jm`). 2. Given the flag unset, When decoding, Then no tables are allocated and speed is unchanged (±1 %). 3. Given frame threading (`-threads 4`), When decoding, Then output equals single-threaded output.
**Size:** L.
**Notes / pitfalls:** `intra4x4_pred_mode_cache` is overwritten per MB; copy at the end of `ff_h264_decode_mb_*`. `mvd_cache` exists only in the CABAC path; CAVLC must record mvd when it reads `mvd_l0/l1`.

### WI-4.102 — `ovt-plugin-ffmpeg`: consume block params into `AvcMb`
**Goal:** Map the new side data into the full H.264 extension table; replace pseudo refs with real ref lists; remove n/a diagnostics when present.
**Depends on:** WI-4.101, WI-2.170.
**Produces:** `plugins/ovt-plugin-ffmpeg/src/map_h264_blockparams.cpp`, tests.
**Interface:** §7.6.3 `AvcMb` mapping paragraph.
**Steps:** 1. Detect side data presence; 2. Map all fields; `grid.intra_mode_y/c`, `ref0/ref1`, `tx_log2`, `cbf`, `partition_id`; 3. `MotionVector.mvd_x/mvd_y`; 4. Tests vs JM trace (intra modes, cbp, ref_idx) on 20 JVT streams.
**Acceptance tests:** 1. Given `CI1_FT_B` (CABAC intra), When mapped, Then `intra4x4[16]` per MB equals JM's `intra4x4_pred_mode` derivations. 2. Given `CVSE3_Sony_H`, When mapped, Then `ref0` resolves to the same POCs as JM's `ref_idx_l0` + RefPicList0.
**Size:** M.
**Notes / pitfalls:** Keep the phase-1 path working when the side data is absent (unpatched FFmpeg).

### WI-4.103 — Submit the FFmpeg patch set to ffmpeg-devel
**Goal:** Upstream WI-4.100/4.101 as a 3-patch series; track review.
**Depends on:** WI-4.101.
**Produces:** patchwork URLs and review status in the "Upstreaming" section of `plugins/ovt-plugin-ffmpeg/README.md`.
**Interface:** n/a.
**Steps:** 1. `git format-patch` series with cover letter citing the 2020 `AVEncodeInfo` thread. 2. Post; iterate for at least three review rounds. 3. Rebase on each FFmpeg bump.
**Acceptance tests:** 1. Given the series, When posted, Then URLs recorded; `git apply --check` passes on the pinned tag in CI.
**Size:** S (submission; review time excluded).
**Notes / pitfalls:** Expect requests to generalise beyond H.264; keep `type` enum extensible.

### WI-4.104 — `third_party/hm.cmake` (HM-18.0 static libraries)
**Goal:** Build HM libraries without patches for macOS/Linux (Android optional).
**Depends on:** WI-0.010 (`ovt_tp_declare`).
**Produces:** `third_party/hm.cmake`, `VERSIONS.md` entry, smoke program decoding `WPP_A_ericsson_MAIN_2` and printing POCs.
**Interface:** §7.6.4 build row.
**Steps:** 1. ExternalProject over OVT's own `third_party/hm/CMakeLists.txt` (HM's CMake is not usable unpatched, §7.6.4); the source is a shallow git fetch of the `HM-18.0` tag pinned to its commit (`third_party/hm/fetch.cmake` fails unless the tag resolves to the pinned commit; `versions_to_cmake.py` emits `OVT_TP_hm_COMMIT`, chapter 13 §13.3.7). 2. Flags (`RExt__HIGH_BIT_DEPTH_SUPPORT`). 3. Smoke program using `TDecTop`.
**Acceptance tests:** 1. Given the build, When the smoke program runs, Then POCs 0..N print and MD5 SEI check reports OK for every picture.
**Size:** M.
**Notes / pitfalls:** HM uses `Int`/`UInt` typedefs and some `-Wall` noise; compile with warnings disabled for third-party code only. **Overlap:** absorbs WI-0.016 (`-fPIC` patch note); scheduled in phase 4 (M4.1). See chapter 16 §16.3.

### WI-4.105 — `ovt-plugin-hm`: decoder wrapper and `TComDataCU` mapping
**Goal:** Loadable HM plugin with the full §7.6.4 mapping.
**Depends on:** WI-4.104, WI-2.150.
**Produces:** `plugins/ovt-plugin-hm/CMakeLists.txt`, `plugins/ovt-plugin-hm/src/plugin.cpp`, `plugins/ovt-plugin-hm/src/decoder.cpp` (TAppDecTop control flow), `plugins/ovt-plugin-hm/src/map_cu.cpp`, `plugins/ovt-plugin-hm/tests/test_hm_plugin.cpp`.
**Interface:** §0.6.2; §7.6.4.
**Steps:** 1. `caps`, `create`. 2. NAL feeding + `bNewPicture` re-feed loop + `executeLoopFilters`. 3. POC → decode index map. 4. CU/PU/TU mapping via getters (Z-scan ↔ raster). 5. SAO, slices, planes, coefficients. 6. Tests vs the `hm_cu_dump` JSON goldens (`tests/golden/h265/<id>.hm-grid.json`, chapter 14 §14.4.4; self-consistency of the getter mapping) and vs the de265 plugin on HEVC_v1 (should be identical for all announced fields).
**Acceptance tests:** 1. Given HEVC_v1, When analysed with `hm`, Then reconstruction MD5 equals the SEI hash for every picture. 2. Given `AMP_A_Samsung_7`, When `hm` and `de265` grids are compared (WI-4.106 comparator), Then zero differences in `pred/qp/intra_mode_*/mv*/tx_log2_*/cbf/filter_idx`. 3. Given an RExt 4:4:4 12-bit stream, When analysed with `hm`, Then a `FrameAnalysis` is produced (de265 returns unsupported).
**Size:** L.
**Notes / pitfalls:** `TComDataCU::getIntraDir(CHANNEL_TYPE_CHROMA)` returns `DM_CHROMA_IDX` (36) for derived mode; resolve to the luma mode (and apply the 4:2:2 Table 8-3 remap via `TComDataCU::getIntraDir` + `getChromaFormat`).

### WI-4.106 — Verify mode: plugin-vs-plugin and decoded-picture-hash comparator
**Goal:** §7.6.4 verify mode (`VER-0001..0004`), `VerifyReport`, CLI `ovt analyze --verify-with`.
**Depends on:** WI-4.105, WI-2.164, WI-1.114 (hash SEI).
**Produces:** `core/src/analysis/verify_mode.cpp`, `core/schema/ovt_model.fbs` `VerifyReport` (coordinate with the Data Model chapter), `cli/src/cmd/analyze.cpp` (flag), GUI action hook (GUI chapter), tests.
**Interface:** §7.6.4 verify mode.
**Steps:** 1. Plane comparator with mismatch map blob. 2. Grid/tree comparators. 3. Hash SEI implementation (MD5/CRC/checksum per D.3.19). 4. Report FlatBuffer + JSON. 5. Tests: identical inputs → empty report; a deliberately corrupted plane → `VER-0001` with positions.
**Acceptance tests:** 1. Given HEVC_v1 `de265` vs `hm`, When verified, Then zero `VER-*` errors on all Main/Main10 streams. 2. Given a stream with CRC hash SEI (`PICSIZE_A_Bossen_1` uses MD5; generate CRC/checksum variants with x265 `--hash 2/3`), When verified, Then `VER-0004` is absent. 3. Given one flipped sample in a plugin's plane (test hook), When verified, Then `VER-0001` names the coordinate.
**Size:** M.
**Notes / pitfalls:** Hash is computed on the decoded picture **before** cropping; for `chroma_format_idc == 0` only one component.

### WI-4.107 — Opportunistic FFmpeg HEVC `mvs`/`venc_params` export patch
**Goal:** Implement `AV_CODEC_EXPORT_DATA_MVS | VIDEO_ENC_PARAMS` for the native `hevc` decoder (Appendix D.1) as a second upstream attempt; use it as an optional H.265 "fast" plugin path.
**Depends on:** WI-2.169.
**Produces:** `plugins/ovt-plugin-ffmpeg/patches/0002-hevc-export-mvs-venc-params.patch`, `plugins/ovt-plugin-ffmpeg/src/map_hevc_sidedata.cpp` (H265 caps with MV/QP fields only), `plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_hevc_sidedata.cpp` (vs the de265 plugin).
**Interface:** FFmpeg `AVMotionVector` (quarter-sample, `motion_scale = 4`), `AVVideoEncParams` type `AV_VIDEO_ENC_PARAMS_HEVC` (new).
**Steps:** 1. Export from `tab_mvf`/`qp_y_tab` in `hevc/refs.c` output path. 2. Options/APIchanges. 3. Plugin mapping (grid MVs/QP per 4×4 and per CU). 4. Tests: MVs/QPs equal de265 plugin's on HEVC_v1.
**Acceptance tests:** 1. Given the `fast` HEVC_v1 streams, When MV/QP grids from the patched FFmpeg are compared with the `de265` plugin's, Then they are equal. 2. Given the patch series is posted to ffmpeg-devel, When the WI is closed, Then the patchwork URLs are recorded in `plugins/ovt-plugin-ffmpeg/patches/README.md`.
**Size:** L.
**Notes / pitfalls:** This is **not** on the critical path; never make it the default H265 plugin.

### WI-4.108 — dav1d inspection API proposal (design only, no fork)
**Goal:** Write and post a design proposal for a zero-cost inspection callback in dav1d (§7.6.5).
**Depends on:** WI-2.159 (experience with the libaom fields).
**Produces:** `docs/spec/dav1d-inspection-proposal.md`, issue URL on code.videolan.org.
**Interface:** proposed `Dav1dSettings.inspect_cb` + per-frame `Dav1dInspectData` (block grid of `Av1Block` + accounting hook).
**Steps:** 1. Draft API mirroring libaom's. 2. Zero-cost argument (NULL check per block). 3. Post; record responses.
**Acceptance tests:** 1. Given the document, When reviewed by the owner, Then approved for posting; URL recorded.
**Size:** S.
**Notes / pitfalls:** No OVT code depends on this WI.

### WI-4.109 — Decoded-picture-hash and level-limit conformance checks in the index/analysis
**Goal:** Use the parsed `decoded_picture_hash` SEI (HEVC) and AV1 Argon MD5 side files to flag mismatches during normal analysis; add H.264/H.265 level limit checks (MaxDpbMbs/MaxLumaPs, bit rate) as Diagnostics.
**Depends on:** WI-1.114, WI-2.164, WI-2.156.
**Produces:** `core/src/analysis/hash_check.cpp`, `core/src/index/level_limits.cpp` (Tables A-1 H.264 / A.4 H.265 / AV1 Annex A), tests.
**Interface:** Diagnostics `VER-0004`, `SYN-CONF-0001 "picture size exceeds level %s MaxLumaPs"`, `SYN-CONF-0002 "DPB size exceeds level limit"`.
**Steps:** 1. Hash check hook in the host after `pull` (opt-in setting). 2. Level tables as `constexpr`. 3. Diagnostics in `index`.
**Acceptance tests:** 1. Given HEVC_v1 with `de265`, When analysed with hash check on, Then no `VER-0004`. 2. Given an SPS declaring level 3.0 with 1080p, When indexed, Then `SYN-CONF-0001`.
**Size:** S.
**Notes / pitfalls:** Argon MD5 files cover shown frames in output order.

### WI-4.110 — H.264 field/MBAFF completeness: field ref lists, PAFF pairing, MBAFF grid mapping
**Goal:** Close gaps in §7.4.4 field handling and define the 4×4 grid for field pictures (each field its own `FrameInfo` with half height; MBAFF frames use frame coordinates with `Block.ext AvcMb.mb_field`).
**Depends on:** WI-1.122, WI-4.102.
**Produces:** `core/src/index/h264_dpb.cpp` (fields), `plugins/ovt-plugin-ffmpeg/src/map_h264_fields.cpp`, tests on `CVFI*`, `CVPA*`, `CAMA*`, `camp_mot_*` streams.
**Interface:** §7.3.9 H.264 field pairing; §7.4.4.
**Steps:** 1. Field RefPicList alternation (§8.2.4.2.5) with JM golden. 2. PAFF pairing `derived` text and display slot merging. 3. MBAFF MB pair mapping in the plugin (top/bottom MB of a pair interleaved rows when `mb_field_decoding_flag`). 4. Tests.
**Acceptance tests:** 1. Given `CVFI1_Sony_D` and `CVPA1_TOSHIBA_B`, When indexed, Then `h264_reflist_vs_jm` passes for field pictures. 2. Given `CAMA1_Sony_C`, When analysed with the patched FFmpeg, Then field MB pairs are placed at the correct luma rows (compare `intra16x16` positions with JM trace).
**Size:** M.
**Notes / pitfalls:** For field MB pairs the top MB covers even rows of the pair's 32 lines; the 4×4 grid stores values at **frame** positions (interleaved), with `AvcMb.mb_field = 1`.

### WI-4.111 — SVC/MVC headers-only completeness and multi-layer indexing
**Goal:** Parse `slice_header_in_scalable_extension`, MVC slice headers against subset SPS, and index enhancement-layer frames with `layer_id`.
**Depends on:** WI-1.109, WI-1.121.
**Produces:** `core/src/syntax/h264/svc_slice_header.cpp`, `core/src/index/h264_layers.cpp`, `core/tests/syntax/h264/test_syntax_h264_svc_mvc.cpp` (runs on the JVT SVC and MVC conformance streams of the corpus).
**Interface:** §7.3.4 rows 1, 22, 23, 24; §7.3.9.
**Steps:** 1. G.7.3.3.4 slice header. 2. Layer-aware frame boundary (compare within the same `dependency_id`/`view_id`). 3. Index `layer_id` and skip enhancement layers in the analysis path with a Diagnostic. 4. Golden vs `trace_headers` (CBS supports SVC/MVC headers) and GStreamer.
**Acceptance tests:** 1. Given `MVC/MVCDS1`, When indexed, Then base-view frames have `layer_id 0`, non-base `layer_id = view order index`, and `trace_headers` diff is empty for all NAL 20 headers. 2. Given `SVC/SVCBST-1`, When indexed, Then `temporal_id` per frame equals the prefix NAL's `temporal_id`.
**Size:** M.
**Notes / pitfalls:** Base-view NAL 1/5 slices in MVC streams are preceded by prefix NAL 14 carrying the MVC header; attach it to the same frame.


### WI-4.112 — HRD buffer model and `HrdTrace` builder
**Goal:** Compute the CPB arrival/removal trace of §7.4.8 from a finished `StreamIndex` for H.264 and H.265 and expose it to `ovt hrd` (chapter 12 §12.2.21) and the GUI Buffer panel (FR-DEC-013, FR-GUI-032).
**Depends on:** WI-1.121 (index builder), WI-1.108 (H.264 SPS/VUI/HRD), WI-1.112 (H.265 SPS/VPS HRD), WI-1.110 (H.264 SEI: buffering period, pic timing), WI-1.114 (H.265 SEI).
**Produces:** `core/src/index/hrd_model.cpp`, `core/include/ovt/hrd.h` (C API as declared in chapter 12 §12.3: `ovt_status ovt_index_hrd_trace(ovt_Stream*, const ovt_HrdOptions* /* sched_sel_idx, nal_hrd, initial_removal_delay_override (-1 none) */, ovt_Buffer* out /* HrdTrace */)` — the first parameter is the `ovt_Stream*`, whose index is reachable from the stream handle; there is no separate `ovt_StreamIndex` handle type), `HrdTrace`/`HrdPoint` population (schema in chapter 06 §6.3), `core/tests/index/test_index_hrd.cpp`, `core/tests/golden/hrd/hand_trace_cbr.json`, synthetic corpus recipes `syn_h264_hrd_cbr`, `syn_h264_hrd_vbr`, `syn_h265_hrd_cbr` (x264 `--nal-hrd cbr`, x264 `--nal-hrd vbr`, x265 `--hrd`; added to `tests/corpora/synthetic/recipes.json` of WI-0.553).
**Interface:** §7.4.8 (equations of items 1–8, cited by clause: H.264 §C.1.1/§C.1.2, H.265 §C.2.2/§C.2.3; type I/II selection, override rule); `HrdTrace` table; chapter 12 §12.2.21 flags map 1:1 onto `ovt_HrdOptions`.
**Steps:** 1. Collect per access unit `b(n)` for type I and type II from `StreamIndex.units[].file_span` spans (start-code bytes included for Annex B input). 2. Resolve the active `hrd_parameters` set per coded video sequence and `SchedSelIdx`; compute `BitRate`, `CpbSize`, `tc` with `muldiv64`. 3. Walk buffering-period / pic-timing SEIs in decode order; implement items 1–8 of §7.4.8 in 90 kHz integer arithmetic (first AU of a subsequent buffering period: recursion on `n_b,prev`); handle `low_delay_hrd_flag` and the override. 4. Fullness, overflow/underflow detection and `DEC-HRD-00xx` diagnostics; AV1 → `OVT_E_UNSUPPORTED`. 5. Hand-computed 5-frame golden; x264/x265 conformance tests; hook the C API into `ovt hrd` (WI-4.543) and the Buffer panel model.
**Acceptance tests:** 1. Given `syn_h264_hrd_cbr` (x264 `--nal-hrd cbr --vbv-bufsize 2000 --vbv-maxrate 2000`), When `ovt hrd --type nal -f json` runs, Then `overflow_count == 0`, `underflow_count == 0` and `init_arrival_time_90k[n] == final_arrival_time_90k[n−1]` for every `n > 0` (`[index][hrd]`). 2. Given `syn_h265_hrd_cbr` (x265 `--hrd --vbv-bufsize 3000 --vbv-maxrate 3000`), When `ovt hrd --type nal -f json` runs, Then zero overflow/underflow and `bit_rate_bps == 3000000 ± 2^(6+bit_rate_scale)`. 3. Given `core/tests/golden/hrd/hand_trace_cbr.json`, When the trace of its 5 access units is computed, Then every `*_90k` and `cpb_fullness_bits` value is identical. 4. Given `syn_h264_hrd_vbr` with the first `b(n)` artificially doubled by a test mutator, When the trace is computed, Then `DEC-HRD-0010` or `DEC-HRD-0011` is reported for exactly that access unit. 5. Given `syn_av1_gop60_1080p`, When `ovt hrd` runs, Then exit 4 with `DEC-HRD-0001`. 6. Given `syn_h264_gop60_1080p` (no `hrd_parameters`), When `ovt hrd` runs, Then exit 4 with `SYN-HRD-0001`. 7. Given a synthetic H.264 index with two buffering periods (period 2 starts at AU 5, its `cpb_removal_delay` = 10 and `tc` = 3 000 ticks) built by the test, When the trace is computed, Then `removal_time_90k[5] == removal_time_90k[0] + 30 000` (recursion on the first AU of the **previous** period, not on AU 4).
**Size:** M.
**Notes / pitfalls:** `cpb_removal_delay` wraps modulo `2^(cpb_removal_delay_length_minus1 + 1)` — unwrap against the previous value. In H.265 the SEI lengths (`au_cpb_removal_delay_length_minus1`, `initial_cpb_removal_delay_length_minus1`) come from the **active** SPS/VPS `hrd_parameters`, which the index knows only after activation (§7.4.2 step ordering: parse the SEI lazily when the following slice activates its SPS). Type-II counts of Annex B input include the 3- or 4-byte start code and trailing `zero_byte`s. Do not use `double`: 90 kHz ticks and `muldiv64` everywhere.
---

## 7.9 Diagnostic registry (`SYN`, `DEC`, `IO`, `BR`, `ANA`)

Format (chapter 6 §6.9 rule V-029, chapter 5 §5.7): `<FAMILY>(-<QUALIFIER>)?-<nnnn>`; families owned by this chapter: `SYN` (syntax/index), `DEC` (decoders and plugins), `IO` (file access), `BR` (bit reader), `ANA` (analysis host). Codes are never reused or renumbered. `MDL`/`VER` are registered in chapter 6 §6.9.1, `CNT` in chapter 8 §8.5, `VQ` in chapter 9, `RND` in chapter 10, `GUI` in chapter 11, `CLI` in chapter 12. Severity is the default; a row may say when it is downgraded. Message templates use `printf` placeholders. **Renamed codes** (round-1 editorial decision D2; the old spellings MUST NOT appear in code): `IDX-GEN-000n → SYN-GEN-010n`, `IDX-H264-000n → SYN-H264-010n`, `IDX-H265-000n → SYN-H265-010n`, `IDX-AV1-000n → SYN-AV1-010n`, the container-family code 0010 formerly used by the unit splitter → `SYN-GEN-0105`, `CONF-000n → SYN-CONF-000n`, `HDR-0001 → DEC-HDR-0001`, `CCH-GEN-0002 → MDL-CCH-0002` (chapter 6).

### 7.9.1 `BR` — `BitReader` (thrown as `ParseError`, §7.2.4)

The `BR-0001 … BR-0010` table of §7.2.4 is the registry; every `BR` code has severity Error (thrown as `ParseError`) except `BR-0009` (Warning, recorded, not thrown); the spec reference is the descriptor's clause given in §7.2.2.

### 7.9.2 `IO`

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `IO-0001` | Error | `"unexpected end of file at offset %llu (%llu bytes requested)"` | a container or unit scanner reads past EOF (§7.1.4); the index continues with the complete units | — |

### 7.9.3 `SYN` — syntax parsers (§7.3) and index building (§7.4; numbers 0101–0199)

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `SYN-GEN-0001` | Error | `"%s: fixed-pattern f(%u) read %llu, expected %llu"` | `read_f` mismatch (§7.3.2); parsing continues | H.264/H.265 §7.2 `f(n)`; AV1 §4.10.2 |
| `SYN-GEN-0002` | Warning | `"emulation prevention byte followed by 0x%02X"` | EPB not followed by 0x00–0x03 (§7.2.5); byte still removed | H.264 §7.4.1.1 / H.265 §7.4.2 |
| `SYN-GEN-0003` | Error | `"%s: forbidden/reserved value %llu"` | `expect_f` (§7.2.2) | per element |
| `SYN-GEN-0101` | Error | `"no video units found"` | index finishes with zero units (§7.4.2) | — |
| `SYN-GEN-0102` | Warning | `"no parameter sets before first VCL unit"` | H.26x frames before the first SPS/PPS; indexed as `type = Unknown` | H.264 §7.4.1.2.3 / H.265 §7.4.2.4.2 |
| `SYN-GEN-0103` | Warning | `"index truncated at unit %d"` | cancelled/partial index (`OVT_INDEX_ALLOW_PARTIAL`, §7.4.6) | — |
| `SYN-GEN-0104` | Info | `"no timing information; frame-count time base"` | chapter 6 §6.2.6 fallback time base | — |
| `SYN-GEN-0105` | Error | `"length-prefixed unit overruns its sample"` | `split_length_prefixed` (§7.3.3); unit truncated to the sample | ISO/IEC 14496-15 §5.3.4.2 |
| `SYN-H264-0001` | Error | `"malformed nal_unit_header (forbidden_zero_bit = 1 or reserved nal_unit_type %d)"` | §7.3.4 row 1 | H.264 §7.4.1 |
| `SYN-H264-0010` | Error | `"slice references missing PPS %d"` | activation (§7.3.8); frame indexed with `type = Unknown` | H.264 §7.4.1.2.1 |
| `SYN-H264-0020` | Warning | `"PPS parsed before its SPS; scaling list count assumed"` | §7.3.4 row 3 | H.264 §7.3.2.2 |
| `SYN-H264-0021` | Error | `"PPS %d changed within a picture"` | §7.3.8 | H.264 §7.4.1.2.1 |
| `SYN-H264-0030` | Warning | `"buffering_period SEI before its SPS; parsed as bytes"` | §7.3.4 row 8 | H.264 D.1.2 |
| `SYN-H264-0060` | Info | `"slice with first_mb_in_slice 0 does not start a new picture"` | §7.3.9 | H.264 §7.4.1.2.4 |
| `SYN-H264-0102` | Info | `"non-existing frames inserted for frame_num gap (%d..%d)"` | §7.4.4 (`gaps_in_frame_num_value_allowed_flag = 1`) | H.264 §8.2.5.2 |
| `SYN-H264-0103` | Warning | `"frame_num gap"` | §7.4.4 (gaps not allowed) | H.264 §7.4.3 |
| `SYN-H264-0104` | Warning | `"reference list shorter than num_ref_idx_lX_active"` | §7.4.4 | H.264 §8.2.4.2 |
| `SYN-H264-0105` | Error | `"ref_pic_list_modification references a missing picture"` | §7.4.4 | H.264 §8.2.4.3 |
| `SYN-H264-0106` | Error | `"DPB overflow"` | §7.4.4; oldest short-term dropped | H.264 §8.2.5.3 |
| `SYN-H265-0001` | Error | `"nuh_temporal_id_plus1 == 0"` | §7.3.5 row 1 (ParseError) | H.265 §7.4.2.2 |
| `SYN-H265-0003` | Error | `"truncated or malformed SPS (parse error at bit %llu: %s)"` | SPS parse failure (§7.3.5 row 3); the unit becomes kind Bytes | H.265 §7.3.2.2 |
| `SYN-H265-0007` | Error | `"%s out of range (%s = %lld, allowed %s)"` | generic range violation in any H.265 header element (the example code of §0.1 rule 6) | per element |
| `SYN-H265-0010` | Error | `"pps_pic_parameter_set_id out of range [0,63]"` | §7.3.5 row 4 (ParseError) | H.265 §7.4.3.3.1 |
| `SYN-H265-0031` | Warning | `"num_entry_point_offsets %d exceeds tiles/CTB rows (%d)"` | §7.3.5 row 10 | H.265 §7.4.7.1 |
| `SYN-H265-0040` | Warning | `"RefPicList0 unknown at parse time; SCC current-picture entries ignored"` | §7.3.5 row 10 | H.265 §7.4.7.1 (`pps_curr_pic_ref_enabled_flag`) |
| `SYN-H265-0050` | Warning | `"payload type %d not allowed in %s SEI"` | §7.3.5 row 11 | H.265 D.2.1 |
| `SYN-H265-0060` | Error | `"slice segment without first slice segment"` | §7.3.9; attached to a synthetic frame | H.265 §7.4.7.1 |
| `SYN-H265-0101` | Info | `"RASL picture not output (random access at CRA)"` | analysis path only (§7.3.10) | H.265 §8.1.3 |
| `SYN-H265-0102` | Error (Info for RASL after a CRA start) | `"missing reference picture POC %d"` | §7.4.4 RPS derivation | H.265 §8.3.2 |
| `SYN-AV1-0001` | Error | `"obu_has_size_field = 0 in a position that requires a size"` | §7.3.3 | AV1 §5.3.1, Annex B |
| `SYN-AV1-0002` | Info | `"not in selected operating point"` | §7.3.6 row 1 | AV1 §7.18 |
| `SYN-AV1-0003` | Warning | `"new coded video sequence not followed by a shown KEY frame"` | §7.3.8 | AV1 §7.5 |
| `SYN-AV1-0010` | Error | `"redundant frame header differs"` | §7.3.6 row 10 | AV1 §5.9.1 / §7.21 |
| `SYN-AV1-0011` | Info | `"ref_order_hint mismatch: RefValid[%d] cleared"` | §7.3.6 row 11 | AV1 §5.9.2 |
| `SYN-AV1-0012` | Error | `"intra_only frame with refresh_frame_flags = 0xFF"` | §7.3.10 | AV1 §5.9.2 (conformance requirement) |
| `SYN-AV1-0013` | Error | `"show_existing_frame of an empty slot"` | §7.3.10 | AV1 §7.21 |
| `SYN-AV1-0014` | Error | `"frame_refs_short_signaling: LAST/GOLDEN order hint not before current"` | §7.3.10 `set_frame_refs` | AV1 §7.8 |
| `SYN-AV1-0020` | Error | `"tile size %u overruns tile group (%u bytes left)"` | §7.3.6 row 29 (ParseError) | AV1 §5.11.1 |
| `SYN-AV1-0030` | Warning | `"temporal unit with %d shown frames (expected 1)"` | §7.3.9 | AV1 §7.5 |
| `SYN-AV1-0031` | Error | `"tile group without frame header"` | §7.3.9 | AV1 §5.11.1 |
| `SYN-AV1-0032` | Warning | `"Annex B temporal_unit_size disagrees with temporal delimiters"` | §7.3.9 | AV1 Annex B |
| `SYN-AV1-0101` | Error | `"truncated frame"` | §7.4.2 S8 | AV1 §5.11.1 |
| `SYN-AV1-0102` | Error | `"ref_frame_idx points to an empty slot"` | §7.4.4 | AV1 §7.20 |
| `SYN-HRD-0001` | Error | `"stream carries no hrd_parameters"` | §7.4.8 (`ovt hrd` exit 4) | H.264 E.1.2 / H.265 E.2.2 |
| `SYN-CONF-0001` | Warning | `"picture size exceeds level %s MaxLumaPs"` | WI-4.109 level checks | H.264 Table A-1 / H.265 Table A.8 |
| `SYN-CONF-0002` | Warning | `"DPB size exceeds level limit"` | WI-4.109 | H.264 A.3.1 / H.265 A.4.2 |

### 7.9.4 `DEC` — decoders, plugins, HRD

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `DEC-PLUGIN-0001` | Error | `"decoder plugin not installed (%s)"` | no plugin for the codec (§7.5.1; chapter 4 FR on optional components) | — |
| `DEC-H265-0001` | Error | `"decoded picture corrupt or hash mismatch (plugin %s, POC %d)"` | the HEVC analysis plugin reports a corrupt picture, or the chapter 14 corrupt-set check fails on it | H.265 D.3.19 |
| `DEC-H265-0010` | Info | `"use ovt-plugin-hm"` | `de265` refuses an unsupported profile (§7.6.2) | H.265 A.3 |
| `DEC-HDR-0001` | Warning | `"colour signalling differs between container (%s) and bitstream (%s)"` | §7.6.3 colour merge | H.273 |
| `DEC-HDR-0002` | Info | `"Dolby Vision RPU present but not applied: %s"` | the playback path finds an RPU (`nal_unit_type` 62 / AV1 T.35 metadata) that the renderer does not apply (profile 7 enhancement layer, no `AVDOVIMetadata` side data from the decoder, or HDR output disabled); once per stream; chapter 4 FR-HDR-013, chapter 10 §10.3.7 | Dolby Vision profiles spec; H.265 §7.3.1 |
| `DEC-HRD-0001` | Error | `"AV1 decoder model (AV1 Annex E) not implemented; decoder_model_info is parsed and displayed only"` | `ovt hrd` on AV1 (§7.4.8) | AV1 Annex E |
| `DEC-HRD-0002` | Error | `"pic_timing SEI without preceding buffering_period SEI"` | §7.4.8 | H.264 C.1 / H.265 C.1 |
| `DEC-HRD-0003` | Info | `"sub-picture HRD (decoding-unit level) not modelled"` | §7.4.8 | H.265 E.2.2 |
| `DEC-HRD-0010` | Error | `"CPB overflow at access unit %d by %lld bits"` | §7.4.8 | H.264 C.3 / H.265 C.3 |
| `DEC-HRD-0011` | Error | `"CPB underflow at access unit %d: removal precedes final arrival by %lld ticks"` | §7.4.8 | H.264 C.3 / H.265 C.3 |

### 7.9.5 `ANA` — analysis host (§7.5)

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `ANA-0001` | Info | `"stage %s not available from plugin %s"` | requested plane stage not delivered (§7.5.4) | — |
| `ANA-H264-0001` | Info | `"field MV semantics approximate (FFmpeg side data)"` | §7.6.3 | — |
| `ANA-H264-0002` | Info | `"intra MB: prediction modes not available without the FFmpeg patch"` | §7.6.3 | — |
| `ANA-PLG-0001` | Error | `"plugin %s rejected: abi_version %u, host expects %u"` | plugin loading (§7.5.1, chapter 12 §12.3.11 loading rules): the exported table's `abi_version` differs from the host constant; the plugin is not registered and the call fails with `OVT_E_PLUGIN` | — |
| `ANA-PLG-0002` | Warning | `"plugin %s ignored: id %s already registered from %s"` | plugin loading: a second plugin with the same id/codec registration; the first one found on the search path wins | — |
| `ANA-PLG-0003` | Error | `"codec pack %s rejected: signing certificate differs from the app's"` | Android codec-pack discovery (chapter 13 §13.4.6); nothing from that package is loaded | — |
| `ANA-PLG-0004` | Error | `"plugin %s rejected: file is world-writable or not owned by the current user"` | macOS/Linux plugin loading (chapter 13 §13.5.3) | — |
| `ANA-LRU-0001` | Error | pointer only — the row is owned by chapter 12 §12.3.10 (`ovt_analysis_plane` on an evicted blob; message and conditions there) | chapter 12 §12.3.10 | — |

### Glossary additions (chapter 7)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **Av1DecodeReturn** — libaom's `{buf, idx, show_existing}` struct (declared in `aom/aomdx.h`) that `aom_codec_decode` fills through `user_priv`; a non-null `user_priv` routes the call to `decoder_inspect()`, which decodes one frame per call and is the only path on which the inspection callback fires; `buf` is where the one-frame decode stopped (7.6.1).
- **Inspection mode (libaom)** — the decode mode entered by `AV1_SET_INSPECTION_CALLBACK` in which each `aom_codec_decode` call decodes exactly one frame (7.6.1).
- **`ovt::jobs::Pool` / `Cancel`** — the process thread pool and the cooperative cancellation token passed by reference through every long call chain (7.1.6).
- **`ovt::Result<T>`** — status-plus-value return type used at module boundaries instead of exceptions (7.1.6).
- **ParseError** — the exception type thrown by `BitReader` and the syntax parsers carrying a `BR-*`/`SYN-*` code and the bit position; never leaves the `syntax` module (7.2.4).
- **PredMode (libde265)** — libde265's `MODE_INTER`/`MODE_INTRA`/`MODE_SKIP` CU prediction mode read through `de265_internals_get_CB_info` (7.6.2); maps to the neutral `pred` codes.
- **deblk_bS_v** — the per-4×4 vertical-edge boundary strength array that libde265 patch 0001 adds, because stock libde265 keeps one bS per 4×4 overwritten by the horizontal pass (7.6.2).
- **VerifyReport** — the FlatBuffer result of verify mode: plane mismatches, grid differences, block-tree differences and picture-hash results (7.6.4).

- **Emulation prevention byte (EPB) position** — the raw byte offset of a removed `0x03` within a NAL unit (`Unit.epb_positions`).
- **RAP map** — `rap_before[d]`: nearest random access point at or before decode index `d` (7.4.2 S11).
