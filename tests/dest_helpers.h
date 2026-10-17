#pragma once
// Helpers shared by the destination and failover tests.
#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/ramp.h"
#include "core/sample_ring.h"
#include "format/wav_reader.h"
#include "io/fault_file_system.h"
#include "session/destination.h"
#include "session/manifest.h"

namespace dest_test {

// Writes ramp frames [from, from + frames) into the ring.
inline void feed(sl::SampleRing& ring, sl::SampleIndex from, size_t frames) {
  const uint32_t ch = ring.channels();
  std::vector<float> buf(4096 * ch);
  for (size_t done = 0; done < frames;) {
    const size_t n = std::min<size_t>(4096, frames - done);
    for (size_t i = 0; i < n; ++i)
      for (uint32_t c = 0; c < ch; ++c) buf[i * ch + c] = sl::ramp_value(from + sl::SampleIndex(done + i), c);
    ring.write(buf.data(), n);
    done += n;
  }
}

// Polls p() for up to 5 s.
template <class Pred>
bool poll(Pred p) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!p()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

inline bool wait_progress(const sl::Destination& d, sl::SampleIndex target) {
  return poll([&] { return d.progress() >= target; });
}

inline std::optional<sl::WavInfo> wav_info(sl::FaultFileSystem& fs, const std::wstring& path) {
  std::wstring err;
  auto f = fs.open_existing(path, &err);
  if (!f) return std::nullopt;
  auto info = sl::read_wav_info(*f);
  f->close();
  return info;
}

// The frames the header claims (32-bit float files only).
inline std::vector<float> wav_samples(sl::FaultFileSystem& fs, const std::wstring& path) {
  std::wstring err;
  auto f = fs.open_existing(path, &err);
  if (!f) return {};
  auto info = sl::read_wav_info(*f);
  std::vector<float> out;
  if (info && info->bits == 32) {
    out.resize(size_t(info->frames) * info->channels);
    if (!f->read_at(info->data_offset, out.data(), out.size() * 4)) out.clear();
  }
  f->close();
  return out;
}

// Mono: number of samples that differ from the ramp starting at `first`.
inline size_t ramp_errors(const std::vector<float>& v, sl::SampleIndex first) {
  size_t bad = 0;
  for (size_t i = 0; i < v.size(); ++i) bad += (v[i] != sl::ramp_value(first + sl::SampleIndex(i), 0));
  return bad;
}

inline int64_t num(sl::ManifestRecord& r, const char* key) {
  const auto it = r.find(key);
  return it == r.end() ? -1 : std::stoll(it->second);
}

inline std::vector<sl::ManifestRecord> events(const std::vector<sl::ManifestRecord>& recs, const std::string& ev) {
  std::vector<sl::ManifestRecord> out;
  for (const auto& r : recs)
    if (r.count("ev") && r.at("ev") == ev) out.push_back(r);
  return out;
}

inline bool ends_with(const std::wstring& s, const std::wstring& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

inline std::string narrow(const std::wstring& w) {  // ASCII
  std::string s;
  for (wchar_t c : w) s += static_cast<char>(c);
  return s;
}

}  // namespace dest_test
