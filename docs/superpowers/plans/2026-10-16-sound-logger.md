# Sound Logger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Win32 program that logs audio from a WASAPI input to WAV (BWF) and Opus files for many hours. It survives failing drives, a lost input device, and crashes without silently losing audio.

**Architecture:** One float32 capture ring is fed by the capture thread. Each destination runs its own writer thread that reads the ring through its own cursor and writes to a folder chain, with flush-before-confirm, failover, and rotation. A session manager thread owns the destinations, the watchdog, device-gap handling, and crash state. A plain Win32 dashboard polls status at 30 fps. All platform I/O sits behind interfaces, so the whole pipeline can be tested with an in-memory fault-injecting file system, a fake clock, and a synthetic input source.

**Tech Stack:** C++20, MSVC (VS 2022 Build Tools 17.14 is installed), CMake 3.31, Win32/WASAPI/GDI/comctl32, `libopus` 1.5.2, `libopusenc` 0.2.1 (both via CMake FetchContent). Tests use a ~60-line in-house harness, not a test framework, so no extra dependency.

**Spec:** `docs/specs/2026-10-16-sound-logger-design.md` (read it with this plan; § numbers below refer to it). Background: `HANDOFF.md`.

## Global Constraints

- Windows 10/11 x64 only. Build with MSVC, static CRT (`MultiThreaded$<$<CONFIG:Debug>:Debug>`), `/W4 /permissive- /utf-8`, `UNICODE`, `_UNICODE`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`.
- No third-party code except `libopus` and `libopusenc`. Use nothing else outside the Windows SDK, in tests too.
- Plain Win32 GUI. Meters are drawn with GDI, double-buffered.
- Up to 8 input channels, up to 192 kHz. Ring default 30 s, rounded up to a power-of-two frame count, pre-faulted at start.
- The capture thread never allocates, locks, or does file I/O.
- WAV: BWF, 16-bit int **default**, 24-bit int, 32-bit float. TPDF dither only when converting to 16-bit.
- Opus: Ogg Opus, 1–2 channels per destination, VBR, default 128 kbps stereo / 64 kbps mono, always 48 kHz output.
- Rotation default 30 min, aligned to the wall clock (:00/:30), or off. Name template default `{session}_{date}_{time}_{n}{seg}`. Never overwrite files.
- Flush interval 2 s. Low-water mark = max(10 min of audio, 1 GB), checked every 10 s. Stall watchdog fires at 50% of ring lag.
- Settings live in `%APPDATA%\SoundLogger\settings.ini`. Logs go to `%LOCALAPPDATA%\SoundLogger\logs\`.
- UI copy is sentence case. Stop has no single-key shortcut and always asks for confirmation.
- Namespace `sl`. `SampleIndex` is `int64_t`, a frame index that is monotonic for the session.

## Review Focus

1. **Stop or exit while a writer is hung.** `stop()` must return within 3 s and finalize every healthy destination. It abandons the stuck thread. Test in Task 10.
2. **A destination drive vanishes mid-run** (USB unplugged, so open and write both fail on that path prefix). Failover to the next folder with no lost or duplicated samples. Test in Task 10.
3. **Session or destination names with filename-illegal characters** (`Set 1: Main/Stage`). Sanitize to `_`, never fail to open. Test in Task 7.
4. **The wall clock jumps after recording starts** (DST, or the user sets the clock). Only the first rotation boundary is wall-clock aligned; after that, boundaries are exactly `period × 60 × rate` samples apart, so no zero-length or duplicate files appear. Test in Task 9.
5. **"Marker 30 s ago" pressed 5 s into a session.** Clamp to the session's first sample, never a negative or pre-session index. Test in Task 12.

---

## File structure

```
CMakeLists.txt                 top-level: options, deps, targets sl_core / sound_logger / sl_tests
cmake/deps.cmake               FetchContent for opus + hand-made libopusenc target
src/core/types.h               SampleIndex, StreamFormat, WallTime
src/core/sample_ring.{h,cpp}   capture ring
src/core/spsc_queue.h          UI → manager command queue
src/core/sample_convert.{h,cpp} format conversion, TPDF, channel extraction
src/core/clock.{h,cpp}         IClock, system clock, FakeClock, time formatting
src/io/file_system.h           IFile / IFileSystem
src/io/win_file_system.cpp     Win32 implementation
src/io/fault_file_system.{h,cpp} in-memory fault-injecting test double
src/format/encoder.h           IEncoderFile, Marker
src/format/wav_writer.{h,cpp}  BWF/RF64 writer
src/format/wav_reader.{h,cpp}  header parser (tests + repair)
src/format/wav_repair.{h,cpp}  crash repair
src/format/opus_writer.{h,cpp} libopusenc wrapper
src/session/naming.{h,cpp}     name template, sanitize, unique path, rotation boundaries
src/session/manifest.{h,cpp}   JSON Lines writer + flat reader
src/session/event_log.{h,cpp}  human-readable log + recent list for UI
src/session/config.h           DestinationConfig, SessionParams
src/session/preflight.{h,cpp}
src/session/destination.{h,cpp} writer thread, rotation, failover, last resort
src/session/meter.{h,cpp}
src/session/session_manager.{h,cpp}
src/session/recovery.{h,cpp}   StateStore, repair scan, resume
src/audio/capture_source.h     ICaptureSource, CaptureCallbacks, CaptureConfig
src/audio/ramp.h               test-pattern sample values
src/audio/synthetic_source.{h,cpp}
src/audio/wasapi_source.{h,cpp}
src/app/settings.{h,cpp}       INI load/save/validate (in sl_core, tested)
src/app/format_text.{h,cpp}    display strings (in sl_core, tested)
src/app/main.cpp, main_window.{h,cpp}, meter_control.{h,cpp}, dest_panel.{h,cpp},
        settings_dialog.{h,cpp}, resource.h, sound_logger.rc, app.manifest
tests/test.h, tests/test_main.cpp, tests/test_<module>.cpp
```

`sl_core` is a static library containing everything except `src/app/*` window code. `sound_logger` (WIN32 exe) and `sl_tests` (console exe) link it. Executables go to `build/bin/<Config>/`.

Common commands (Git Bash or PowerShell, from the repo root):
- Configure: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`
- Build: `cmake --build build --config Debug`
- Run tests matching a filter: `build/bin/Debug/sl_tests.exe <substring>` (no argument runs all). It exits 0 and prints `N passed, 0 failed` on success.

---

### Task 1: Project skeleton, dependencies, test harness

**Files:**
- Create: `.gitignore` (`build/`), `CMakeLists.txt`, `cmake/deps.cmake`, `tests/test.h`, `tests/test_main.cpp`, `tests/test_smoke.cpp`, `src/core/types.h`

**Interfaces:**
- Produces: `TEST(name)`, `CHECK(expr)`, `CHECK_EQ(a, b)`, `CHECK_NEAR(a, b, eps)` macros. On failure they print the file, line, and expression, then mark the test failed and continue. `test_main` runs every test whose name contains `argv[1]`.
- Produces in `src/core/types.h`:
```cpp
namespace sl {
using SampleIndex = int64_t;
struct StreamFormat { uint32_t sample_rate = 0; uint16_t channels = 0;
                      bool operator==(const StreamFormat&) const = default; };
struct WallTime { int64_t utc_ms = 0; int32_t local_offset_min = 0; }; // local = utc + offset
}
```

- [ ] **Step 1:** `git init`, then add `.gitignore` containing `build/`.
- [ ] **Step 2:** Write `cmake/deps.cmake`. FetchContent `opus` from `https://github.com/xiph/opus` tag `v1.5.2` with `OPUS_BUILD_TESTING OFF`, `OPUS_BUILD_PROGRAMS OFF`. FetchContent `libopusenc` from `https://github.com/xiph/libopusenc` tag `v0.2.1` with `SOURCE_SUBDIR` pointing to a nonexistent directory, so only the source is fetched. Define `add_library(opusenc STATIC ${libopusenc_SOURCE_DIR}/src/*.c)` (glob), include `${libopusenc_SOURCE_DIR}/include`, link `Opus::opus`, and compile definitions `OUTSIDE_SPEEX RANDOM_PREFIX=libopusenc PACKAGE_NAME="libopusenc" PACKAGE_VERSION="0.2.1"`. If the build complains about other defines, copy them from libopusenc's `win32/VS2015` project files. Set `CMAKE_MSVC_RUNTIME_LIBRARY` before the FetchContent calls.
- [ ] **Step 3:** Write the top-level `CMakeLists.txt` (min 3.25, C++20, global flags from Global Constraints, `CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin`), the `sl_core` static library (empty source list plus `types.h` for now), and `sl_tests` from `tests/*.cpp`. Leave the `sound_logger` target commented out until Task 16.
- [ ] **Step 4:** Write the smoke test:
```cpp
TEST(smoke_opus_links) {
  CHECK(std::string(opus_get_version_string()).find("libopus 1.5") != std::string::npos);
  CHECK(ope_get_version_string() != nullptr);
}
```
- [ ] **Step 5:** Configure, build, and run `sl_tests.exe smoke`. Expected: `1 passed, 0 failed`.
- [ ] **Step 6:** Commit with `git commit -m "build: skeleton, opus deps, test harness"`.

---

### Task 2: Capture ring and SPSC queue

**Files:**
- Create: `src/core/sample_ring.{h,cpp}`, `src/core/spsc_queue.h`, `src/audio/ramp.h`
- Test: `tests/test_ring.cpp`

**Interfaces:**
- Produces:
```cpp
namespace sl {
class SampleRing {
 public:
  SampleRing(uint32_t channels, size_t min_frames, SampleIndex start = 0); // capacity = next pow2 ≥ min_frames; VirtualAlloc + touch every page + best-effort VirtualLock
  uint32_t channels() const;  size_t capacity() const;
  void write(const float* interleaved, size_t frames);   // single producer; RT-safe; overwrites oldest
  void jump_to(SampleIndex next);                        // producer only, forward only; frames in the skipped range are invalid
  SampleIndex write_index() const;                       // acquire load
  SampleIndex oldest_valid() const;                      // max(write_index - capacity, last jump target, start)
  struct ReadResult { size_t frames; bool overrun; };
  ReadResult read(SampleIndex from, float* out, size_t max_frames) const;
};
template <class T, size_t N> class SpscQueue {           // N power of two
 public: bool push(const T&); std::optional<T> pop();
};
inline float ramp_value(SampleIndex frame, uint32_t ch)  // exact in float32, period 2^17 frames
  { return float(((uint64_t(frame) * 8u) + ch) & 0xFFFFFu) / 1048576.0f; }
}
```
- Overrun detection is reader-side (my decision; the spec leaves the ring to the builder). `read` copies `[from, min(from+max, write_index))`. It returns `{0, true}` if `from < oldest_valid()` either before or after the copy (re-check after copying, seqlock style, so torn data is never returned). Reading at or past `write_index` returns `{0, false}`.

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(ring_capacity_rounds_up) { SampleRing r(2, 1000); CHECK_EQ(r.capacity(), 1024u); }
TEST(ring_roundtrip_across_wrap) { /* cap 1024, 2ch: write 700 ramp frames twice; read(700, buf, 700) -> frames 700, !overrun, buf == ramp 700..1399 */ }
TEST(ring_overrun_when_reader_too_far_behind) { /* write 3000 frames into cap 1024; read(0,..) -> {0,true}; read(2000,..) -> 1000 frames */ }
TEST(ring_jump_invalidates_skipped) { /* write 100; jump_to(5000); oldest_valid()==5000; read(50,..)->{0,true}; write 10; read(5000,..)->10 frames */ }
TEST(ring_read_future_is_empty) { /* read(write_index()+5,..) -> {0,false} */ }
TEST(ring_start_index) { SampleRing r(1, 16, 1'000'000); CHECK_EQ(r.write_index(), 1'000'000); }
TEST(ring_threaded_no_tearing) { /* producer thread writes 5M ramp frames in 480-frame chunks; reader reads chunks of ≤4096; every returned frame f satisfies buf == ramp_value(f, c); overruns allowed but never torn data */ }
TEST(spsc_fifo_and_full) { SpscQueue<int, 4> q; CHECK(q.push(1)); CHECK(q.push(2)); CHECK(q.push(3)); CHECK(q.push(4)); CHECK(!q.push(5)); CHECK_EQ(*q.pop(), 1); }
```
- [ ] **Step 2:** Run `sl_tests.exe ring` and `sl_tests.exe spsc`. Expected: compile failure (types not defined).
- [ ] **Step 3:** Implement. Index with `frame & (capacity - 1)`. `write_index_` and `jump_floor_` are `std::atomic<int64_t>`.
- [ ] **Step 4:** Run the same tests. Expected: all pass. Run `ring_threaded_no_tearing` 5 times in a row.
- [ ] **Step 5:** Commit with `git commit -m "core: capture ring and spsc queue"`.

---

### Task 3: Sample conversion

**Files:**
- Create: `src/core/sample_convert.{h,cpp}`
- Test: `tests/test_convert.cpp`

**Interfaces:**
- Produces (scales: int16 ÷ 32768, int24 ÷ 8388608, int32 ÷ 2147483648; float → int rounds to nearest, then clamps):
```cpp
namespace sl {
void int16_to_float(const int16_t* in, float* out, size_t n);
void int24_packed_to_float(const uint8_t* in, float* out, size_t n);   // 3-byte little-endian
void int32_to_float(const int32_t* in, float* out, size_t n);          // also covers 24-in-32 left-justified
class Tpdf { public: explicit Tpdf(uint32_t seed = 0x9E3779B9u); float next(); }; // xorshift32; triangular in (-1, 1) LSB
void float_to_int16_tpdf(const float* in, int16_t* out, size_t n, Tpdf& d);
void float_to_int24_packed(const float* in, uint8_t* out, size_t n);
void extract_channels(const float* in, uint32_t in_ch, const uint16_t* map, uint32_t out_ch, float* out, size_t frames);
}
```

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(convert_int24_roundtrip_exact) { /* values -8388608,-1,0,1,8388607 -> float -> int24 identical */ }
TEST(convert_int24_clamps) { /* 1.0f -> 8388607; -1.5f -> -8388608 */ }
TEST(convert_int16_tpdf_silence_small) { /* 10000 zeros -> every output in {-1,0,1} */ }
TEST(convert_int16_tpdf_unbiased) { /* 10000 x 0.25f -> mean within 8192 ± 1 */ }
TEST(convert_int16_clamps) { /* 2.0f -> 32767; -2.0f -> -32768 */ }
TEST(convert_extract_channels) { /* 8ch frames, map {6,1} -> out[0]=in[6], out[1]=in[1] per frame */ }
```
- [ ] **Step 2:** Run `sl_tests.exe convert`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `sl_tests.exe convert`. Expected: 6 passed.
- [ ] **Step 5:** Commit with `git commit -m "core: sample conversion and TPDF dither"`.

---

### Task 4: File layer, Win32 implementation, fault-injection double

**Files:**
- Create: `src/io/file_system.h`, `src/io/win_file_system.cpp`, `src/io/fault_file_system.{h,cpp}`
- Test: `tests/test_files.cpp`

**Interfaces:**
- Produces:
```cpp
namespace sl {
class IFile { public: virtual ~IFile() = default;
  virtual bool write(const void* p, size_t n) = 0;                  // append at end
  virtual bool write_at(uint64_t off, const void* p, size_t n) = 0;
  virtual bool read_at(uint64_t off, void* p, size_t n) = 0;
  virtual bool flush() = 0;                                          // data on stable storage (FlushFileBuffers)
  virtual uint64_t size() const = 0;
  virtual void close() = 0; };
class IFileSystem { public: virtual ~IFileSystem() = default;
  virtual std::unique_ptr<IFile> create_new(const std::wstring& path, std::wstring* err) = 0;  // fails if it exists
  virtual std::unique_ptr<IFile> open_existing(const std::wstring& path, std::wstring* err) = 0; // read/write
  virtual bool exists(const std::wstring& path) = 0;
  virtual bool ensure_dir(const std::wstring& dir, std::wstring* err) = 0;                     // creates parents
  virtual bool remove(const std::wstring& path) = 0;
  virtual std::vector<std::wstring> list(const std::wstring& dir, const std::wstring& suffix) = 0;
  virtual std::optional<uint64_t> free_bytes(const std::wstring& dir) = 0;
  virtual std::wstring volume_id(const std::wstring& dir) = 0; };                               // "" if unknown
IFileSystem& real_fs();
class FaultFileSystem : public IFileSystem {   // in-memory; paths compared case-insensitively
 public:
  enum class Kind { FailOpen, FailWrite, FailFlush, HangWrite, SlowWrite };
  struct Rule { std::wstring prefix; Kind kind; uint64_t after_bytes = 0; uint32_t delay_ms = 0; };
  void add_rule(Rule r); void clear_rules();
  void release_hangs();                         // hung writes return false
  void set_free_bytes(const std::wstring& prefix, uint64_t bytes);   // default 1 TB
  void set_volume(const std::wstring& prefix, const std::wstring& id);
  std::vector<uint8_t> contents(const std::wstring& path) const;
  /* + IFileSystem overrides */ };
}
```
- Win32: `CreateFileW` (`CREATE_NEW` / `OPEN_EXISTING`), `WriteFile`, `SetFilePointerEx`, `FlushFileBuffers`, `GetDiskFreeSpaceExW`, `GetVolumePathNameW` + `GetVolumeNameForVolumeMountPointW`, `SHCreateDirectoryExW`. Prefix paths longer than `MAX_PATH` with `\\?\`.

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(fault_fail_write_after_bytes) { /* rule FailWrite prefix L"D:\\" after 100: write 60 ok, write 60 -> false */ }
TEST(fault_hang_until_release) { /* HangWrite: write on thread blocks; release_hangs(); thread's write returns false within 1 s */ }
TEST(fault_free_bytes_and_volume) { /* set_free_bytes(L"E:\\",5) -> free_bytes(L"E:\\x")==5; set_volume both D:\\a and D:\\b "v1" -> equal ids */ }
TEST(win_create_new_refuses_existing) { /* temp dir via GetTempPathW + unique name: create_new ok, again -> nullptr with err */ }
TEST(win_write_at_flush_size) { /* write 10 bytes, write_at(2,"XY",2), flush, size==10, read_at back */ }
TEST(win_free_bytes_and_volume) { /* free_bytes(temp) > 0; volume_id(temp) == volume_id(temp\\sub) */ }
```
- [ ] **Step 2:** Run `sl_tests.exe fault` and `sl_tests.exe win_`. Expected: compile failure.
- [ ] **Step 3:** Implement both file systems. `SlowWrite` sleeps `delay_ms` per write call.
- [ ] **Step 4:** Run the same tests. Expected: 6 passed.
- [ ] **Step 5:** Commit with `git commit -m "io: file system interface, win32 and fault-injection implementations"`.

---

### Task 5: BWF/RF64 WAV writer, reader, repair

**Files:**
- Create: `src/format/encoder.h`, `src/format/wav_writer.{h,cpp}`, `src/format/wav_reader.{h,cpp}`, `src/format/wav_repair.{h,cpp}`
- Test: `tests/test_wav.cpp`

**Interfaces:**
- Consumes: `IFileSystem`/`IFile` (Task 4), conversion functions (Task 3).
- Produces:
```cpp
namespace sl {
struct Marker { uint64_t frame_in_file; std::wstring name; };
class IEncoderFile { public: virtual ~IEncoderFile() = default;
  virtual bool write(const float* interleaved, size_t frames) = 0;   // destination channel count
  virtual bool flush() = 0;                 // on disk + header updated; sets confirmed_frames()
  virtual uint64_t confirmed_frames() const = 0;
  virtual uint64_t frames_written() const = 0;
  virtual bool finalize(const std::vector<Marker>& markers, std::optional<uint64_t> truncate_to) = 0; // truncate_to: header claims only this many frames (failover)
  virtual void abandon() = 0; };            // close the handle without touching the file (stalled)
enum class WavDepth { Int16, Int24, Float32 };
struct BextInfo { std::string description, originator = "SoundLogger", originator_reference;
                  std::string date /*yyyy-mm-dd*/, time /*hh:mm:ss*/; uint64_t time_reference = 0; };
struct WavParams { uint32_t sample_rate; uint16_t channels; WavDepth depth = WavDepth::Int16; BextInfo bext;
                   uint64_t rf64_threshold = 0xFFFFFFFFull - 1024 * 1024; };  // tests lower it
std::unique_ptr<IEncoderFile> open_wav(IFileSystem&, const std::wstring& path, const WavParams&, std::wstring* err);
struct WavInfo { std::string riff_id; uint16_t format_tag, channels, bits, block_align; uint32_t sample_rate;
                 uint64_t data_offset, data_bytes, frames; BextInfo bext; std::vector<Marker> markers; };
std::optional<WavInfo> read_wav_info(IFile&);
std::optional<uint64_t> repair_wav(IFileSystem&, const std::wstring& path, std::wstring* err); // returns repaired frame count
}
```
- Layout, in order: `RIFF` | `JUNK` (28-byte payload, reserved for `ds64`) | `fmt ` | `bext` (602-byte fixed part, version 2, empty coding history) | `data` | at finalize: `cue ` + `LIST`/`adtl` with `labl` per marker. Insert a pad byte after odd-length data. Format tag rules: `WAVE_FORMAT_EXTENSIBLE` (channel mask 0) when channels > 2, otherwise 1 (PCM) or 3 (IEEE float, plus a `fact` chunk). `flush()` writes the RIFF and data sizes, then `IFile::flush()`. When data passes `rf64_threshold`, rewrite `JUNK` → `ds64` (riff size, data size, sample count), set the id to `RF64`, and set both 32-bit sizes to `0xFFFFFFFF`.
- `repair_wav`: data bytes = file size − data offset, rounded down to the block align. Then rewrite the sizes (RF64 if needed).

- [ ] **Step 1: Write the failing tests** (all on `FaultFileSystem`)
```cpp
TEST(wav_16bit_stereo_basic) { /* 48k/2ch/Int16, write 48000 frames, finalize -> riff "RIFF", tag 1, bits 16, frames 48000,
                                  bext.originator "SoundLogger", bext.time_reference & date/time as given */ }
TEST(wav_flush_updates_header_before_finalize) { /* write 1000 frames, flush -> read_wav_info on contents: data_bytes 4000; confirmed_frames()==1000 */ }
TEST(wav_markers_to_cue_labl) { /* markers {100,"intro"},{2000,"set 2"} -> read back identical */ }
TEST(wav_8ch_24bit_extensible) { /* tag 0xFFFE, block_align 24 */ }
TEST(wav_float32_ramp_exact) { /* write ramp_value frames, read samples back bit-identical */ }
TEST(wav_rf64_upgrade) { /* rf64_threshold 1000; 1000 frames 16-bit stereo -> riff_id "RF64", data_bytes 4000, frames 1000 */ }
TEST(wav_truncate_on_finalize) { /* write 1000, flush, write 500, finalize({}, 1000) -> frames 1000 */ }
TEST(wav_repair_after_crash) { /* write 1000, flush, write 333 more, abandon(); repair_wav -> 1333; read_wav_info frames 1333 */ }
TEST(wav_odd_data_pads_before_cue) { /* 24-bit mono 3 frames + 1 marker -> cue chunk at even offset, marker read back */ }
```
- [ ] **Step 2:** Run `sl_tests.exe wav`. Expected: compile failure.
- [ ] **Step 3:** Implement the writer, reader, and repair.
- [ ] **Step 4:** Run `sl_tests.exe wav`. Expected: 9 passed.
- [ ] **Step 5:** Commit with `git commit -m "format: BWF/RF64 writer, reader, crash repair"`.

---

### Task 6: Opus writer

**Files:**
- Create: `src/format/opus_writer.{h,cpp}`
- Test: `tests/test_opus.cpp`

**Interfaces:**
- Consumes: `IEncoderFile`, `Marker` (Task 5), `IFileSystem` (Task 4).
- Produces:
```cpp
namespace sl {
struct OpusParams { uint32_t input_rate; uint16_t channels; uint32_t bitrate = 0; }; // 0 -> 128000 stereo / 64000 mono
std::unique_ptr<IEncoderFile> open_opus(IFileSystem&, const std::wstring& path, const OpusParams&, std::wstring* err); // channels must be 1 or 2
}
```
- Use `ope_encoder_create_callbacks` with write/close callbacks into `IFile`, mapping family 0, comment `ENCODER=SoundLogger`, `OPUS_SET_BITRATE`, `OPUS_SET_VBR(1)`, `OPE_SET_MUXING_DELAY(24000)`. Feed audio with `ope_encoder_write_float`.
- **Confirmed frames:** the write callback parses every Ogg page it receives (`"OggS"`, granule = little-endian int64 at byte 6) and keeps the last granule. `flush()` = `IFile::flush()`, then confirmed = `floor(max(0, granule − preskip) × input_rate / 48000)`, where preskip comes from the `OpusHead` packet. This rounds down, so a failover seam in an Opus file can repeat up to one page of audio but never drops any. This is accepted for Opus and recorded exactly in the manifest.
- `finalize` = `ope_encoder_drain` + destroy. Markers and `truncate_to` are ignored (the spec says Opus markers go in the manifest only).

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(opus_valid_stream_and_size) { /* 10 s 1 kHz sine, 48k stereo, 128k: contents start "OggS", contain "OpusHead"; size 120 KB..200 KB */ }
TEST(opus_final_granule_matches_length) { /* last page granule - preskip == 480000 */ }
TEST(opus_confirmed_after_flush) { /* write 5 s, flush: confirmed <= 240000 && confirmed >= 240000 - 48000 */ }
TEST(opus_44100_input_resampled) { /* 44.1k input 441000 frames: final granule - preskip == 480000 ± 960 */ }
TEST(opus_rejects_3_channels) { /* open_opus channels 3 -> nullptr, err non-empty */ }
```
- [ ] **Step 2:** Run `sl_tests.exe opus`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `sl_tests.exe opus`. Expected: 5 passed (plus the smoke test).
- [ ] **Step 5:** Commit with `git commit -m "format: ogg opus writer with confirmed-frame tracking"`.

---

### Task 7: Clock, naming, rotation boundaries

**Files:**
- Create: `src/core/clock.{h,cpp}`, `src/session/naming.{h,cpp}`
- Test: `tests/test_naming.cpp`

**Interfaces:**
- Consumes: `WallTime`, `SampleIndex` (Task 1), `IFileSystem` (Task 4).
- Produces:
```cpp
namespace sl {
class IClock { public: virtual ~IClock() = default; virtual WallTime now() const = 0; virtual uint64_t steady_ms() const = 0; };
IClock& system_clock();
class FakeClock : public IClock { public: void set(WallTime); void advance_ms(int64_t); /* advances both */ };
std::string iso_utc(WallTime);                          // "2026-10-16T14:00:00.000Z"
struct LocalParts { int year, month, day, hour, minute, second, ms; };
LocalParts local_parts(WallTime);
std::wstring sanitize_component(std::wstring s);        // <>:"/\|?* and chars < 0x20 -> '_'; trim trailing '.'/' '; empty -> "_"
struct NameInputs { std::wstring session, dest; WallTime first_sample; uint32_t n; uint32_t seg; };
std::wstring expand_name(const std::wstring& tmpl, const NameInputs&);  // tokens per spec §7; {n} 3-digit; seg 0 -> "", 1 -> "_b", 2 -> "_c"…; session/dest sanitized
std::wstring unique_path(IFileSystem&, const std::wstring& dir, const std::wstring& stem, const std::wstring& ext); // stem.ext, stem_1.ext, …
constexpr SampleIndex kNoRotation = INT64_MAX;
SampleIndex first_rotation_boundary(SampleIndex now, WallTime now_wall, uint32_t rate, uint32_t period_min); // next local-time multiple of period since local midnight, strictly after now
SampleIndex next_rotation_boundary(SampleIndex prev_boundary, uint32_t rate, uint32_t period_min);          // prev + period*60*rate
}
```

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(name_default_template) { /* session "gig", dest "main", local 2026-10-16 14:30:00, n 3, seg 0 -> L"gig_2026-10-16_143000_003" */ }
TEST(name_segment_letter) { /* seg 1 -> ends with L"_003_b" */ }
TEST(name_sanitizes_illegal_chars) { CHECK(sanitize_component(L"Set 1: Main/Stage") == L"Set 1_ Main_Stage"); CHECK(sanitize_component(L"x. ") == L"x"); }
TEST(name_unique_path_on_collision) { /* existing a.wav and a_1.wav -> a_2.wav */ }
TEST(rotation_aligned_to_half_hour) { /* now_wall local 14:12:00.000, rate 48000, 30 min -> now + 18*60*48000 */ }
TEST(rotation_exactly_on_boundary_goes_to_next) { /* 14:30:00.000 -> now + 30*60*48000 */ }
TEST(rotation_mid_second) { /* 14:12:00.500 -> now + (17*60+59.5)*48000 */ }
TEST(rotation_next_is_sample_based) { CHECK_EQ(next_rotation_boundary(1000, 48000, 30), 1000 + 30LL*60*48000); }
TEST(rotation_off) { CHECK_EQ(first_rotation_boundary(0, {}, 48000, 0), kNoRotation); }
```
- [ ] **Step 2:** Run `sl_tests.exe name` and `sl_tests.exe rotation`. Expected: compile failure.
- [ ] **Step 3:** Implement. The system clock uses `GetSystemTimePreciseAsFileTime` and the current `GetTimeZoneInformation` bias; steady time uses `QueryPerformanceCounter`.
- [ ] **Step 4:** Run the same tests. Expected: 9 passed.
- [ ] **Step 5:** Commit with `git commit -m "session: clock, name templates, wall-clock aligned rotation"`.

---

### Task 8: Manifest (JSON Lines) and event log

**Files:**
- Create: `src/session/manifest.{h,cpp}`, `src/session/event_log.{h,cpp}`
- Test: `tests/test_manifest.cpp`

**Interfaces:**
- Consumes: `IFileSystem` (Task 4), `IClock`/`iso_utc` (Task 7).
- Produces:
```cpp
namespace sl {
class JsonLine { public:
  JsonLine(std::string_view ev, SampleIndex sample, WallTime wall);   // writes "ev","sample","utc","local_offset"
  JsonLine& str(std::string_view key, std::wstring_view v);           // UTF-8, escaped
  JsonLine& num(std::string_view key, int64_t v);
  std::string finish() const; };                                       // one line, no trailing newline
class Manifest { public:
  static std::unique_ptr<Manifest> create(IFileSystem&, const std::wstring& path, std::wstring* err);
  static std::unique_ptr<Manifest> open_append(IFileSystem&, const std::wstring& path, std::wstring* err);
  bool append(const JsonLine&);   // buffered
  bool flush(); };                // writes buffered lines + IFile::flush
using ManifestRecord = std::map<std::string, std::string>;  // values unescaped; numbers as text
std::vector<ManifestRecord> read_manifest(IFileSystem&, const std::wstring& path); // flat objects only; skips a torn last line
enum class Severity { Info, Warning, Error };
struct LogEntry { WallTime wall; Severity sev; std::wstring dest, text; };
class EventLog { public:
  EventLog(IFileSystem&, std::wstring path);  // thread-safe; each add appends "2026-10-16 14:30:00  WARN  [main] text\r\n" and flushes
  void add(Severity, WallTime, std::wstring dest, std::wstring text);
  std::vector<LogEntry> recent(size_t max) const; };  // newest first, in-memory cap 1000
}
```
- Event vocabulary. Every line carries `ev`, `sample`, `utc`, `local_offset`. Extra fields by event:

| ev | extra fields |
|---|---|
| `session_start` | `session`, `session_id`, `dest`, `format`, `rate`, `channels`, `prev_manifest`?, `prev_session`? |
| `file_open` | `path`, `folder_index`, `n`, `seg` |
| `file_close` | `path`, `start`, `end` (exclusive), `bytes` |
| `rotation` | `path` (new file) |
| `marker` | `name` |
| `gap_start` | `reason` = `device_lost` / `overrun` / `last_resort` / `crash` |
| `gap_end` | `reason` |
| `failover` | `from`, `to`, `trigger` = `error` / `low_space` / `stall`, `confirmed` |
| `clock_pair` | (base fields only) |
| `warning` | `msg` |
| `session_end` | `reason` = `stop` / `format_change` |

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(manifest_escapes_and_roundtrips) { /* path L"D:\\Gigs\\\"Live\" ü.wav" -> read_manifest gives identical UTF-8 of that string */ }
TEST(manifest_buffered_until_flush) { /* append 2 lines, contents empty; flush -> 2 lines */ }
TEST(manifest_skips_torn_last_line) { /* contents with valid line + "{\"ev\":\"fi" -> 1 record */ }
TEST(manifest_create_refuses_existing) { /* second create -> nullptr */ }
TEST(event_log_recent_newest_first) { /* add a, b, c -> recent(2) == {c, b}; file has 3 lines */ }
```
- [ ] **Step 2:** Run `sl_tests.exe manifest` and `sl_tests.exe event_log`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the same tests. Expected: 5 passed.
- [ ] **Step 5:** Commit with `git commit -m "session: json-lines manifest and event log"`.

---

### Task 9: Destination, normal path (write, flush, rotate, markers, stop)

**Files:**
- Create: `src/session/config.h`, `src/session/destination.{h,cpp}`
- Test: `tests/test_destination.cpp`

**Interfaces:**
- Consumes: ring (Task 2), conversion/`extract_channels` (Task 3), file system (Task 4), `open_wav`/`open_opus` (Tasks 5, 6), naming/rotation (Task 7), `Manifest`/`EventLog` (Task 8).
- Produces:
```cpp
namespace sl {
enum class OutFormat { Wav, Opus };
struct DestinationConfig { std::wstring name = L"main"; OutFormat format = OutFormat::Wav; WavDepth wav_depth = WavDepth::Int16;
  uint32_t opus_bitrate = 0; std::vector<uint16_t> channels;   // empty = all input channels
  std::vector<std::wstring> folders; uint32_t rotation_minutes = 30;
  std::wstring name_template = L"{session}_{date}_{time}_{n}{seg}"; };
struct SessionParams { std::wstring session_name = L"session", session_id; uint32_t rate = 48000; uint16_t input_channels = 2;
  SampleIndex start_sample = 0; WallTime start_wall;            // wall_for(s) = start_wall + (s - start_sample)*1000/rate
  uint32_t flush_ms = 2000, low_water_minutes = 10; uint64_t low_water_min_bytes = 1ull << 30; uint32_t stall_percent = 50;
  std::wstring prev_session; };
struct CmdMarker { SampleIndex at; std::wstring name; };  struct CmdSplit { SampleIndex at; };
struct CmdAppendFolder { std::wstring path; };            struct CmdGapStart { SampleIndex at; std::string reason; };
struct CmdGapEnd { SampleIndex at; std::string reason; }; struct CmdCloseForGap { SampleIndex at; };
struct CmdResumeAt { SampleIndex at; };                   struct CmdClockPair { SampleIndex at; WallTime wall; };
struct CmdAcknowledge {};
using DestCommand = std::variant<CmdMarker, CmdSplit, CmdAppendFolder, CmdGapStart, CmdGapEnd,
                                 CmdCloseForGap, CmdResumeAt, CmdClockPair, CmdAcknowledge>;
enum class DestState { Idle, Writing, FailingOver, LastResort, GapClosed, Stopped, Disabled };
enum class FolderHealth { Active, Standby, Failed };
struct FolderStatus { std::wstring path; FolderHealth health; std::optional<uint64_t> free_bytes; double hours_left; };
struct DestStatus { std::wstring name, current_file; DestinationConfig config; DestState state; bool needs_ack;
                    std::vector<FolderStatus> folders; uint64_t gaps; SampleIndex cursor, confirmed;
                    uint32_t n; std::wstring manifest_path; };   // current file index + manifest, for crash state (Task 13)
struct DestResumeSeed { uint32_t first_n = 1; std::wstring prev_manifest; };
class Destination { public:
  Destination(DestinationConfig, const SessionParams&, SampleRing&, IFileSystem&, IClock&, EventLog&, DestResumeSeed = {});
  void start(SampleIndex at);
  void post(DestCommand);                 // any thread; mutex-protected deque drained by the worker
  void stop(SampleIndex end);             // writes through end, finalizes, waits ≤ 3 s, else abandons the worker
  SampleIndex progress() const;           // atomic cursor
  void declare_stalled();                 // Task 10
  void disable(std::wstring reason);      // state Disabled, nothing written
  DestStatus status() const;
  uint64_t data_rate() const; };          // bytes/s: WAV rate*ch*bytes; Opus bitrate/8
bool is_stalled(SampleIndex write_index, SampleIndex progress, size_t capacity, uint32_t stall_percent);
}
```
- Worker thread priority: `THREAD_PRIORITY_ABOVE_NORMAL`.
- Worker loop: read ≤ 4096 frames from the cursor, extract channels, then split the chunk at the next rotation boundary or split point. At a boundary: finalize the current file with its markers, then open the next file (`n+1`, `seg 0`) and write `rotation` + `file_close` + `file_open`. If no data is available, sleep 5 ms. Every `flush_ms` of steady time: `encoder->flush()`, then `manifest->flush()`.
- Markers: a marker goes into the file whose `[start, end)` contains its sample, if that file is still open. It always goes into the manifest. Files open in `folders[0]` (failover comes in Task 10). The manifest is `<session>_<dest>.jsonl` in the active folder, and its first line is `session_start`.
- WAV bext: `time_reference` = samples since local midnight at the file's first sample; date and time from `wall_for(first)`; `originator_reference` = session_id.

- [ ] **Step 1: Write the failing tests.** Use `FaultFileSystem`, `FakeClock` starting at local 2026-10-16 14:29:58, rate 8000, 1 channel, WAV `Float32`, rotation 1 min. A helper `feed(ring, from, frames)` writes `ramp_value`, and `wait_progress(dest, target)` polls with a 5 s timeout.
```cpp
TEST(dest_continuous_across_rotations) { /* feed 150 s; stop -> 4 files: 2 s, 60 s, 60 s, 28 s; concatenated samples == ramp 0..1.2M with no dup/gap */ }
TEST(dest_manifest_open_close_ranges) { /* file_close start/end per file contiguous; first line session_start; last session_end */ }
TEST(dest_marker_into_correct_file) { /* files span 0–2 s, 2–62 s, 62–122 s…; CmdMarker at 70*8000 -> file 3 cue at frame 8*8000 = 64000 */ }
TEST(dest_split_command) { /* CmdSplit at 10*8000 -> extra file boundary at exactly that sample */ }
TEST(dest_rotation_ignores_clock_jump) { /* after first rotation, FakeClock.set back 1 h; next boundary still +60*8000 samples (Review Focus 4) */ }
TEST(dest_flush_confirms_every_interval) { /* feed 5 s, advance steady 2.1 s -> status().confirmed > start; WAV header in contents matches confirmed */ }
TEST(dest_opus_smoke) { /* Opus mono, rate 48000, rotation 1 min, 130 s -> 3 files, each valid OggS */ }
```
- [ ] **Step 2:** Run `sl_tests.exe dest_`. Expected: compile failure.
- [ ] **Step 3:** Implement the normal path. `declare_stalled` can be a stub for now; Task 10 fills it in.
- [ ] **Step 4:** Run `sl_tests.exe dest_`. Expected: 7 passed. Run it 3 times to check for flakiness.
- [ ] **Step 5:** Commit with `git commit -m "session: destination writer with rotation, markers, manifest"`.

---

### Task 10: Failover, stall watchdog, last resort, overrun, preflight

**Files:**
- Create: `src/session/preflight.{h,cpp}`
- Modify: `src/session/destination.{h,cpp}`
- Test: `tests/test_failover.cpp`, `tests/test_preflight.cpp`

**Interfaces:**
- Consumes: everything from Task 9.
- Produces:
```cpp
namespace sl {
struct PreflightResult { bool can_start; std::vector<std::wstring> warnings, errors; };
PreflightResult preflight(const std::vector<DestinationConfig>&, const SessionParams&, IFileSystem&);
bool folder_healthy(IFileSystem&, const std::wstring& dir, uint64_t low_water_bytes); // ensure_dir + create/remove ".sl_probe.tmp" + free ≥ low water
uint64_t low_water_bytes(const SessionParams&, uint64_t data_rate);                   // max(minutes*60*rate, min_bytes)
}
```
- Preflight (§8.3): warn on the same `volume_id` as an earlier folder in the chain, and warn if the primary's `free/data_rate < 1 h`. A destination with no writable folder is an error and starts in `LastResort`. `can_start` is false only if **no** destination has a writable folder.
- Failover procedure (§4.3): increment the worker generation. A stalled worker is abandoned (`CancelSynchronousIo` best effort on its handle, then detach; it checks the generation and exits without touching shared state). Otherwise the old encoder is finalized with `truncate_to = confirmed`, on a detached thread with a 2 s budget. Open the next healthy folder with `seg+1` and cursor = file start + confirmed. Write `failover` to the old manifest (best effort), then create the new manifest with `prev_manifest`. Set `needs_ack` true.
- Triggers: an encoder write/flush/open error; free space below low water (checked every 10 s of steady time); `declare_stalled()`.
- No healthy folder → `LastResort`: write `gap_start(last_resort)`, keep the cursor following `write_index`, and retry every 30 s. `CmdAppendFolder` pushes onto the chain and retries immediately. On recovery: the new file starts at `write_index`, plus `gap_end`.
- Overrun (ring read returns `overrun`): cursor = `write_index − capacity/2`, `gap_start`/`gap_end(overrun)`, `gaps++`.
- At each rotation boundary, open the next file in the highest-priority folder for which `folder_healthy` passes (`seg` resets to 0).

- [ ] **Step 1: Write the failing tests** (setup as in Task 9, folders `D:\p`, `E:\s`)
```cpp
TEST(failover_on_write_error_no_loss_no_dup) { /* FailWrite D:\ after 100 KB -> files D:\p\..._001.wav and E:\s\..._001_b.wav;
                                                 frames(D file) == its confirmed; D ++ E samples == continuous ramp */ }
TEST(failover_on_low_space) { /* set_free_bytes D:\ below low water, advance 10 s -> active folder E:\s, failover trigger "low_space" */ }
TEST(failover_on_stall_then_stop_returns_fast) { /* HangWrite D:\ ; declare_stalled() -> continues on E:\s;
                                                   stop() returns < 3000 ms (Review Focus 1); then release_hangs() */ }
TEST(failover_drive_vanishes) { /* FailOpen+FailWrite on D:\ mid-run -> E:\s continues, no loss (Review Focus 2) */ }
TEST(last_resort_then_append_folder) { /* FailOpen on D:\ and E:\ -> LastResort, gap_start; CmdAppendFolder F:\new -> writing, gap_end, gaps==1 */ }
TEST(return_to_primary_at_rotation) { /* one-shot FailWrite on D:\ then clear_rules; after next rotation boundary the active folder is D:\p again */ }
TEST(overrun_records_gap) { /* ring 1 s, SlowWrite 500 ms on D:\ -> gap_start reason "overrun"; gaps ≥ 1 */ }
TEST(is_stalled_threshold) { CHECK(!is_stalled(1000, 500, 1024, 50)); CHECK(is_stalled(1000, 400, 1024, 50)); }
TEST(preflight_same_volume_warns) { /* set_volume D:\p and D:\q same id -> warning mentions both */ }
TEST(preflight_one_dest_unwritable_can_start) { /* dest A ok, dest B all FailOpen -> can_start true, 1 error */ }
TEST(preflight_all_unwritable_blocks) { /* -> can_start false */ }
```
- [ ] **Step 2:** Run `sl_tests.exe failover`, `last_resort`, `overrun`, `preflight`, `is_stalled`. Expected: failures.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the same filters, then the whole `dest_` suite again. Expected: all pass, 3 runs in a row.
- [ ] **Step 5:** Commit with `git commit -m "session: failover, stall watchdog, last resort, preflight"`.

---

### Task 11: Level meters

**Files:**
- Create: `src/session/meter.{h,cpp}`
- Test: `tests/test_meter.cpp`

**Interfaces:**
- Produces:
```cpp
namespace sl {
struct ChannelMeter { float peak_db, hold_db, rms_db; uint64_t clips; };
class MeterReader { public:
  MeterReader(SampleRing&, uint32_t rate, IClock&);
  void update();                          // UI thread, ~30 Hz: new frames since last update (if > capacity behind, skip to write_index - rate/10)
  std::vector<ChannelMeter> get() const;
  void reset_clips(); };
}
```
- Peak = max |x| over new frames. Hold for 2 s, then it falls back to the current peak. RMS over the last 300 ms. dB floor is −60 (anything lower reports −60). A clip is |x| ≥ 0.988553 (−0.1 dBFS). Each consecutive run of clipped samples counts once.

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(meter_full_scale_sine) { /* 1 kHz amplitude 1.0 for 0.5 s -> peak 0 ±0.1 dB, rms -3.01 ±0.1 */ }
TEST(meter_silence_floor) { /* zeros -> peak -60, rms -60 */ }
TEST(meter_clip_runs) { /* two runs of 10 samples at 1.0 separated by 0.5 -> clips 2 */ }
TEST(meter_hold_2s) { /* peak 0 dB then silence; at +1.9 s hold 0 dB; at +2.1 s hold -60 */ }
```
- [ ] **Step 2:** Run `sl_tests.exe meter`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `sl_tests.exe meter`. Expected: 4 passed.
- [ ] **Step 5:** Commit with `git commit -m "session: level meters with hold and clip counts"`.

---

### Task 12: Capture interface, synthetic source, session manager

**Files:**
- Create: `src/audio/capture_source.h`, `src/audio/synthetic_source.{h,cpp}`, `src/session/session_manager.{h,cpp}`
- Test: `tests/test_session.cpp`

**Interfaces:**
- Consumes: Tasks 2–11.
- Produces:
```cpp
namespace sl {
enum class CaptureMode { Raw, Exclusive };
struct CaptureConfig { std::wstring device_id; CaptureMode mode = CaptureMode::Raw; uint32_t sample_rate = 0; }; // rate used only for Exclusive
struct CaptureCallbacks { virtual ~CaptureCallbacks() = default;
  virtual void on_frames(const float* interleaved, size_t frames, bool discontinuity) = 0; // capture thread
  virtual void on_device_lost() = 0;
  virtual void on_device_returned(StreamFormat fmt) = 0; };  // the source is open again but not started
class ICaptureSource { public: virtual ~ICaptureSource() = default;
  virtual bool open(const CaptureConfig&, std::wstring* err) = 0;
  virtual StreamFormat format() const = 0;
  virtual bool start(CaptureCallbacks*) = 0;   // after loss, the manager calls start() again
  virtual void stop() = 0; };
class SyntheticSource : public ICaptureSource { public:
  enum class Pattern { Ramp, Sine, Silence };
  SyntheticSource(StreamFormat, Pattern, bool manual);   // manual: frames only via pump(); else a real-time thread, 10 ms blocks
  void pump(size_t frames);
  void simulate_loss(); void simulate_return(StreamFormat); };
struct SessionConfig { SessionParams params; std::vector<DestinationConfig> dests; uint32_t ring_seconds = 30; };
enum class SessionState { Idle, Recording, InputLost };
struct SessionStats { SessionState state; std::wstring session_id; StreamFormat fmt; SampleIndex position;
  double elapsed_s; uint64_t gaps, discontinuities; double ring_fill; bool recovered_from_crash; };
struct ResumeInfo;   // Task 13
class SessionManager : CaptureCallbacks { public:
  SessionManager(ICaptureSource&, IFileSystem&, IClock&, EventLog&);
  PreflightResult preflight(const SessionConfig&);
  bool start(const SessionConfig&, const ResumeInfo* resume, std::wstring* err);
  void stop();
  void marker(uint32_t seconds_ago);      // UI thread: stamps max(start_sample, write_index - s*rate), auto name "Marker N"
  void split();                           // UI thread: stamps write_index
  void append_folder(size_t dest, std::wstring path);
  void acknowledge();
  SessionStats stats() const; std::vector<DestStatus> dest_status() const; MeterReader* meter(); };
}
```
- `on_frames` writes to the ring and does nothing else. UI calls push onto `SpscQueue<UiCommand, 256>`, which the manager thread drains every 10 ms and turns into `DestCommand`s. The manager thread also runs the watchdog (`is_stalled`, every 250 ms) and posts `CmdClockPair` every 5 min.
- Device lost → `CmdGapStart(device_lost)` to all destinations, and a silence filler thread writes zeros into the ring at real-time pace (steady clock), becoming the producer. The cap is the rotation period (30 min if rotation is off). At the cap the filler stops and sends `CmdCloseForGap`.
- Device returned with the same format → stop the filler. If capped, first `ring.jump_to(start_sample + elapsed_wall × rate)` and send `CmdResumeAt`. Then `source.start(this)` and `CmdGapEnd(device_lost)`.
- Device returned with a different format → stop every destination (`session_end reason format_change`), then start a **linked session**: new `session_id`, `prev_session` set, the same destination configs, and a new ring. A destination whose channel map exceeds the new channel count is `disable()`d and logged as an error.
- `discontinuity` true → increment `discontinuities` and log a warning.

- [ ] **Step 1: Write the failing tests** (manual `SyntheticSource` Ramp, rate 8000, 1 ch, `FaultFileSystem`, `FakeClock`)
```cpp
TEST(session_basic_record_stop) { /* start, pump 10 s, stop -> one file, continuous ramp; manifest session_start..session_end */ }
TEST(session_marker_clamped_to_start) { /* pump 5 s; marker(30) -> manifest marker sample == start_sample (Review Focus 5) */ }
TEST(session_device_loss_fills_silence) { /* pump 2 s; simulate_loss; advance 3 s; simulate_return(same); pump 2 s; stop ->
                                            file = ramp(2 s) + zeros(3 s ±10 ms) + ramp; gap_start/gap_end device_lost */ }
TEST(session_long_loss_closes_and_jumps) { /* rotation 1 min; loss 90 s -> file closed at cap; on return new file starts at start + 92 s*8000 ±80 */ }
TEST(session_format_change_links_new_session) { /* simulate_return({16000,1}) -> new session_start with prev_session == old id */ }
TEST(session_watchdog_fails_over_stalled) { /* ring_seconds 1, HangWrite on primary -> within 2 s active folder is secondary, trigger "stall" */ }
TEST(session_channel_map_disabled_after_format_change) { /* dest channels {1} on 2ch; return with 1ch -> that dest Disabled */ }
```
- [ ] **Step 2:** Run `sl_tests.exe session_`. Expected: compile failure.
- [ ] **Step 3:** Implement `SyntheticSource` and `SessionManager`.
- [ ] **Step 4:** Run `sl_tests.exe session_` 3 times, then the full suite. Expected: all pass.
- [ ] **Step 5:** Commit with `git commit -m "session: manager, synthetic source, device-gap handling"`.

---

### Task 13: Crash state, repair scan, resume

**Files:**
- Create: `src/session/recovery.{h,cpp}`
- Modify: `src/session/session_manager.{h,cpp}` (save state every `flush_ms`, clear on `stop`, honor `ResumeInfo`)
- Test: `tests/test_recovery.cpp`

**Interfaces:**
- Produces:
```cpp
namespace sl {
struct DestResume { std::wstring name, manifest_path; uint32_t next_n; };
struct RecordingState { bool recording = false; std::wstring session_id, session_name; SampleIndex start_sample = 0, last_sample = 0;
  int64_t start_utc_ms = 0, last_utc_ms = 0; uint32_t rate = 0; uint16_t channels = 0; std::vector<DestResume> dests; };
class StateStore { public: explicit StateStore(std::wstring ini_path);  // Get/WritePrivateProfileStringW, section [State], [Dest0..]
  void save(const RecordingState&); std::optional<RecordingState> load() const; void clear(); };
struct RepairReport { std::vector<std::wstring> repaired, failed; };
RepairReport repair_open_files(IFileSystem&, const RecordingState&);  // follow each manifest's prev_manifest chain; file_open without file_close -> repair_wav (Opus: none), append file_close + gap_start(crash)
struct ResumeInfo { RecordingState prior; SampleIndex resume_sample; };
ResumeInfo make_resume(const RecordingState&, WallTime now);        // resume_sample = last_sample + (now.utc_ms - last_utc_ms) * rate / 1000
}
```
- `SessionManager` gains `void set_state_store(StateStore*)` and a test hook `void abandon_for_test()` (stops threads without finalizing anything, simulating a crash). With a resume: the ring starts at `resume_sample`, `SessionParams` keeps the old `session_id`, `start_sample`, and `start_wall`, each destination's `DestResumeSeed.first_n` = `next_n` and `prev_manifest` = the old manifest, a `gap_end(crash)` is written, and `stats().recovered_from_crash` is true.

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(state_store_roundtrip) { /* temp ini; save with 2 dests incl. Unicode path -> load identical; clear -> load().recording false */ }
TEST(recovery_repairs_crashed_wav) { /* session pump 10 s with flush; abandon_for_test(); repair_open_files -> 1 repaired;
                                       WAV frames == contents frames; manifest now has file_close + gap_start crash */ }
TEST(recovery_resume_continues_session) { /* after crash advance FakeClock 60 s; make_resume -> resume_sample == last + 60*8000;
                                             start(cfg,&resume) -> new file n == previous n + 1, same session_id, gap_end crash */ }
```
- [ ] **Step 2:** Run `sl_tests.exe state_store` and `sl_tests.exe recovery`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the same tests and the full suite. Expected: all pass.
- [ ] **Step 5:** Commit with `git commit -m "session: crash state, repair scan, resume"`.

---

### Task 14: WASAPI capture source

**Files:**
- Create: `src/audio/wasapi_source.{h,cpp}`
- Test: `tests/test_wasapi.cpp`

**Interfaces:**
- Consumes: `ICaptureSource`, `CaptureCallbacks`, `CaptureConfig` (Task 12), conversion functions (Task 3).
- Produces:
```cpp
namespace sl {
struct DeviceInfo { std::wstring id, name; StreamFormat mix_format; };
std::vector<DeviceInfo> list_capture_devices();
std::wstring default_capture_device_id();
std::vector<uint32_t> exclusive_rates(const std::wstring& id);   // probes 44100, 48000, 88200, 96000, 176400, 192000
class WasapiSource : public ICaptureSource { /* overrides */ };
}
```
- Raw mode: `IAudioClient2::SetClientProperties` with `AUDCLNT_STREAMOPTIONS_RAW`, shared mode at the mix format, `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, 100 ms buffer. Exclusive mode: try int32, then 24-in-32, then packed 24, then int16 at the chosen rate via `IsFormatSupported`, with the device's default period. Reject channels > 8.
- Capture thread: `CoInitializeEx(MTA)`, `AvSetMmThreadCharacteristicsW(L"Pro Audio")`, wait on the event with a 200 ms timeout, `GetBuffer`, convert into a buffer preallocated at `open()` (sized to the buffer frame count), `on_frames`, `ReleaseBuffer`. `AUDCLNT_BUFFERFLAGS_SILENT` → zeros; `DATA_DISCONTINUITY` → `discontinuity = true`. `AUDCLNT_E_DEVICE_INVALIDATED` → release the client, `on_device_lost()`, and the thread exits.
- `IMMNotificationClient`: when the configured device returns to `DEVICE_STATE_ACTIVE` (or is re-added), reopen it and call `on_device_returned(format())`.

- [ ] **Step 1: Write the tests** (they skip, printing `SKIP`, when no capture device exists)
```cpp
TEST(wasapi_list_devices_does_not_throw) { auto d = list_capture_devices(); CHECK(d.size() < 64); }
TEST(wasapi_raw_capture_one_second) { /* open default raw, start, 1 s, stop -> frames within ±10% of mix rate; channels == mix channels */ }
```
- [ ] **Step 2:** Run `sl_tests.exe wasapi`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `sl_tests.exe wasapi`. Expected: 2 passed (or skipped without a device).
- [ ] **Step 5: Manual check.** With a USB interface, run the app in Task 16 (or a temporary loop in the test) and unplug the interface mid-capture. `on_device_lost` must fire. Replug it: `on_device_returned` must fire with the same format.
- [ ] **Step 6:** Commit with `git commit -m "audio: WASAPI capture source (raw/exclusive, device loss)"`.

---

### Task 15: Settings (INI) and display text

**Files:**
- Create: `src/app/settings.{h,cpp}`, `src/app/format_text.{h,cpp}` (both compiled into `sl_core`)
- Test: `tests/test_settings.cpp`

**Interfaces:**
- Produces:
```cpp
namespace sl {
struct AppSettings { CaptureConfig input; uint32_t ring_seconds = 30; std::wstring session_name = L"session";
  uint32_t flush_seconds = 2, low_water_minutes = 10, low_water_min_gb = 1, stall_percent = 50;
  bool start_on_launch = false, global_hotkey = false; std::vector<DestinationConfig> dests; };
std::wstring settings_path();                                  // %APPDATA%\SoundLogger\settings.ini
AppSettings default_settings();                               // one WAV dest "main", 16-bit, all channels, %USERPROFILE%\Music\SoundLogger, 30 min
AppSettings load_settings(const std::wstring& ini);           // missing file -> default_settings()
bool save_settings(const std::wstring& ini, const AppSettings&);
std::vector<std::wstring> validate(const AppSettings&, uint16_t device_channels);
std::wstring format_elapsed(double seconds);                  // "02:14:37"
std::wstring format_time_left(double hours);                  // "31 h left", "45 min left" (< 1 h)
std::wstring format_summary(const DestinationConfig&, uint16_t input_channels); // L"WAV 16-bit · ch 1–2", L"Opus 128k · ch 1–2"
}
```
- INI sections: `[Input]`, `[General]`, `[Destination0]`…. Folders are joined with `|` and channels with `,` (1-based in the file, 0-based in memory).
- Validation messages (exact): `Add at least one destination.`, `"<name>" needs at least one folder.`, `"<name>": Opus records 1 or 2 channels.`, `"<name>": channel <k> doesn't exist on this device.`, `"<name>": Opus bitrate must be 32–256 kbps for stereo or 16–128 kbps for mono.`, `Destination names must be unique.`

- [ ] **Step 1: Write the failing tests**
```cpp
TEST(settings_roundtrip_unicode) { /* 2 dests, folders L"D:\\Gigs|E:\\Bäckup" -> save/load identical */ }
TEST(settings_defaults_when_missing) { /* load nonexistent -> 1 dest, WavDepth::Int16, rotation 30 */ }
TEST(settings_validate_opus_channels) { /* opus with channels {0,1,2} -> contains L"\"x\": Opus records 1 or 2 channels." */ }
TEST(settings_validate_channel_range) { /* channel 5 on a 2-ch device -> L"\"x\": channel 6 doesn't exist on this device." */ }
TEST(text_elapsed_and_time_left) { CHECK(format_elapsed(8077) == L"02:14:37"); CHECK(format_time_left(31.4) == L"31 h left"); CHECK(format_time_left(0.75) == L"45 min left"); }
TEST(text_summary) { /* WAV Int16 channels {} on 2 ch -> L"WAV 16-bit · ch 1–2"; Opus 128000 {0,1} -> L"Opus 128k · ch 1–2" */ }
```
- [ ] **Step 2:** Run `sl_tests.exe settings` and `sl_tests.exe text_`. Expected: compile failure.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the same tests. Expected: 6 passed.
- [ ] **Step 5:** Commit with `git commit -m "app: INI settings, validation, display text"`.

---

### Task 16: Dashboard window

**Files:**
- Create: `src/app/main.cpp`, `src/app/main_window.{h,cpp}`, `src/app/meter_control.{h,cpp}`, `src/app/dest_panel.{h,cpp}`, `src/app/resource.h`, `src/app/sound_logger.rc`, `src/app/app.manifest`
- Modify: `CMakeLists.txt` (enable the `sound_logger` WIN32 target; link `comctl32`, `ole32`, `avrt`, `uuid`)

**Interfaces:**
- Consumes: `SessionManager`, `MeterReader`, `EventLog`, `AppSettings`, `format_*` (Tasks 8–15), `WasapiSource`/`SyntheticSource`.
- `main.cpp` command line: `--synthetic` uses `SyntheticSource(48000 Hz, 2 ch, Sine, real-time)` in place of WASAPI, so the UI can run without hardware.
- Manifest: common controls v6, `PerMonitorV2` DPI awareness.

Layout, per spec §10 and the approved mockup A:
- **Top bar:** `Record` and `Stop` buttons, then the elapsed clock (`format_elapsed`, large font), then the line `<device> · <rate/1000> kHz · <ch> ch · <Raw|Exclusive>`. `Marker` is a split button (`BS_SPLITBUTTON`) whose menu offers `Now`, `5 s ago`, `10 s ago`, `30 s ago`. `Split file` and `Settings…` follow.
- **Levels:** `MeterControl` (window class `SLMeter`, GDI double-buffered). One horizontal bar per channel on a −60…0 dBFS scale, with an RMS bar, a peak line, a hold tick, and a clip lamp. A per-channel clip count sits at the right. Clicking the counts calls `reset_clips()`. The status line below reads `Buffer <fill>% · Gaps <n> · Discontinuities <n>`.
- **Destinations:** `DestPanel` (window class `SLDestPanel`). Each destination block shows `format_summary`, the current file name, and one row per folder: path, then `active`/`standby`/`failed`, then `format_time_left`. The block background is neutral normally, amber when `needs_ack`, and red for `LastResort` or `Disabled`. One `Acknowledge` button shows when any block needs it. Right-clicking a block opens a menu with `Add folder to "<name>"…`, which opens an `IFileDialog` with `FOS_PICKFOLDERS` and then calls `append_folder`.
- **Event list:** a `ListView` in report mode, newest first, with `Time | Destination | Event` columns, filled from `EventLog::recent(500)`.
- **Timer:** 33 ms. Call `meter->update()`, then repaint the meters. Refresh stats, destinations, and the event list at 4 Hz.
- **Copy (exact):** the Stop confirmation is `MessageBox` with `Stop recording?` and Yes/No. Closing while recording asks `Recording is in progress. Stop recording and exit?` with Yes/No. Settings while recording shows `Stop recording to change settings.` A preflight block shows its errors joined by newlines, titled `Can't start recording`. Preflight warnings show in a Yes/No box ending with `Start anyway?`.
- **Accelerators:** `M` = marker now, `S` = split. No Stop shortcut. Optional global `Ctrl+Alt+M` via `RegisterHotKey` when `global_hotkey` is set; on failure, log the warning `Couldn't register Ctrl+Alt+M. Another program may be using it.`

- [ ] **Step 1:** Implement the window, controls, and resources.
- [ ] **Step 2:** Build: `cmake --build build --config Debug`. Expected: `sound_logger.exe` with no warnings from `src/app`.
- [ ] **Step 3: Manual check with `build/bin/Debug/sound_logger.exe --synthetic`.**
  - Record starts and the clock advances.
  - Meters move at about −3 dB RMS.
  - `M` adds `Marker 1` to the event list.
  - `S` logs a rotation.
  - Stop asks first, and No keeps recording.
  - Closing asks first.
  - A WAV appears in the default folder and plays in Windows Media Player.
- [ ] **Step 4:** Commit with `git commit -m "app: dashboard window, meters, destination panel, event list"`.

---

### Task 17: Settings dialog and live folder append

**Files:**
- Create: `src/app/settings_dialog.{h,cpp}`
- Modify: `src/app/sound_logger.rc`, `src/app/resource.h`, `src/app/main_window.cpp`

Behavior (spec §10.5). It's a modal dialog with three groups.
- **Input:** device combo (`list_capture_devices`), mode combo (`Raw (no Windows processing)`, `Exclusive`), sample rate combo from `exclusive_rates` (Exclusive only; in Raw mode a read-only label `<rate> Hz, set in Windows sound settings`), ring length (seconds).
- **Destinations:** a list with `Add…`, `Edit…`, `Remove`. The editor has:
  - name
  - format (`WAV`, `Opus`)
  - for WAV, bit depth (`16-bit`, `24-bit`, `32-bit float`); for Opus, bitrate (`32k, 48k, 64k, 96k, 128k, 160k, 192k, 256k`)
  - a channel checklist (ListView checkboxes, `Ch 1…Ch N`)
  - a folder list with `Add…` (folder picker), `Remove`, `Up`, `Down`
  - rotation (`Off`, `5 min`, `10 min`, `15 min`, `30 min`, `60 min`)
  - a name template edit with a live preview label (`expand_name` with the current time, n=1, plus the extension)
- **General:** session name, flush interval (s), low-water mark (min), stall threshold (%), `Start recording when the program opens`, `Global marker hotkey (Ctrl+Alt+M)`.

OK runs `validate()`. If there are errors, show them in a `MessageBox` titled `Check these settings` and keep the dialog open. Otherwise `save_settings` and apply.

- [ ] **Step 1:** Implement.
- [ ] **Step 2:** Build. Expected: success, with no warnings from `src/app`.
- [ ] **Step 3: Manual check (`--synthetic`).**
  - Add an Opus destination on channels 1–2 at 64k with a second folder. Record 2 min. The `.opus` plays in a browser, and the WAV still records.
  - Choose 3 channels for Opus: the validation message appears.
  - While recording, right-click a destination and add a folder. It shows as `standby`.
  - Point a destination's only folder at a missing drive letter: the block goes red (`LastResort`). Add a working folder: it turns active, and the event list shows the gap end.
- [ ] **Step 4:** Commit with `git commit -m "app: settings dialog and live folder append"`.

---

### Task 18: App lifecycle: power, restart, recovery, auto-start; soak

**Files:**
- Modify: `src/app/main.cpp`, `src/app/main_window.cpp`

Behavior:
- **Single instance:** a named mutex `Local\SoundLogger.SingleInstance`. A second launch brings the existing window to the front and exits.
- **Restart registration:** at startup call `RegisterApplicationRestart(L"--recover", 0)`. Windows only restarts processes that ran for at least 60 s; note this in the log on the first crash test.
- **Power:** on record start, call `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)`. On stop, call `SetThreadExecutionState(ES_CONTINUOUS)`.
- **Recovery:** on launch, if `StateStore` (`%APPDATA%\SoundLogger\state.ini`) has `recording=1`, run `repair_open_files`, log each repaired file, then `start(cfg, &make_resume(...))` without asking. Show an amber banner `Recovered from a crash. Recording resumed.` with `Acknowledge`. This applies whether or not `--recover` was passed.
- **Auto-start:** if `start_on_launch` is set and there's no recovery, run preflight and start. If preflight warns, start anyway and log the warnings. If it blocks, show the block message.
- **Logs:** the event log file is `%LOCALAPPDATA%\SoundLogger\logs\<session_id>.log`, with `session_id` = `yyyymmdd-hhmmss` local time at start.
- **Debug-only crash flag:** `--crash-after <s>` (compiled only in Debug) writes through a null pointer after `<s>` seconds, to test WER restart.

- [ ] **Step 1:** Implement.
- [ ] **Step 2:** Build Release: `cmake --build build --config Release`. Expected: success.
- [ ] **Step 3: Manual crash check.** Run `build/bin/Debug/sound_logger.exe --synthetic --crash-after 90`, then Record.
  - After the crash, Windows relaunches the app within about 60 s.
  - The banner shows, and recording continues with `{n}` incremented.
  - The crashed WAV opens with the right length.
  - The manifest shows `gap_start crash` / `gap_end crash`.
- [ ] **Step 4: Manual sleep check.** With the power plan's sleep set to 1 min, record for 5 min. The PC doesn't sleep.
- [ ] **Step 5: Soak (spec §11).** Real interface, rotation 5 min, two destinations (WAV 16-bit plus Opus) on different drives, 8 h. Pass criteria:
  - every manifest has zero `gap_start` events
  - file ranges are contiguous
  - private bytes at hour 8 are within 10% of hour 1
  - the clip and discontinuity counters match expectations
- [ ] **Step 6:** Commit with `git commit -m "app: power, crash restart and recovery, auto-start, single instance"`.
