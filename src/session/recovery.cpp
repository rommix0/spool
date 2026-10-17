#include "session/recovery.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <deque>
#include <map>
#include <set>
#include <utility>

#include "format/wav_repair.h"
#include "io/ini_file.h"
#include "session/manifest.h"

namespace sl {

namespace {

// ---- StateStore helpers ----

using ini::ensure_unicode_file;
using ini::get;
using ini::get_i64;
using ini::quoted;
using ini::write_section;

std::wstring dest_section(size_t i) { return L"Dest" + std::to_wstring(i); }

size_t saved_dest_count(const std::wstring& path) {
  return size_t(std::clamp<int64_t>(get_i64(path, L"State", L"dests"), 0, 1024));
}

bool delete_dests(const std::wstring& path, size_t from, size_t to) {
  bool ok = true;
  for (size_t i = from; i < to; ++i)
    ok = WritePrivateProfileStringW(dest_section(i).c_str(), nullptr, nullptr, path.c_str()) && ok;
  return ok;
}

// ---- repair helpers ----

std::wstring wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

std::string field(const ManifestRecord& r, const char* key) {
  const auto it = r.find(key);
  return it == r.end() ? std::string() : it->second;
}

std::optional<int64_t> number(const ManifestRecord& r, const char* key) {
  const std::string s = field(r, key);
  int64_t v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
  return v;
}

// Case-insensitive path identity.
std::wstring path_key(std::wstring p) {
  for (wchar_t& c : p) c = c == L'/' ? L'\\' : wchar_t(std::towlower(c));
  return p;
}

bool is_opus(const std::wstring& path) {
  const std::wstring k = path_key(path);
  return k.size() >= 5 && k.compare(k.size() - 5, 5, L".opus") == 0;
}

std::optional<uint64_t> file_size(IFileSystem& fs, const std::wstring& path) {
  auto f = fs.open_existing(path, nullptr);
  if (!f) return std::nullopt;
  const uint64_t n = f->size();
  f->close();
  return n;
}

bool append_lines(IFileSystem& fs, const std::wstring& path, const std::vector<JsonLine>& lines) {
  std::wstring err;
  auto m = Manifest::open_append(fs, path, &err);
  if (!m) return false;
  for (const auto& l : lines) m->append(l);
  return m->flush();
}

struct ManifestFile {
  std::wstring path;
  std::vector<ManifestRecord> recs;
  std::vector<JsonLine> add;  // written by the repair
  bool readable = true;
};

struct OpenFile {
  std::wstring path;
  SampleIndex start;
  size_t manifest;  // index into the chain
};

// True if the session's last session_start in these records is followed by session_end: it
// was stopped cleanly and only the crash state was left behind.
bool ended_cleanly(const std::vector<ManifestRecord>& recs, const std::wstring& session_id) {
  bool ended = false, started = false;
  for (const auto& r : recs) {
    const std::string ev = field(r, "ev");
    if (ev == "session_start" && (session_id.empty() || wide(field(r, "session_id")) == session_id)) {
      started = true;
      ended = false;
    } else if (ev == "session_end" && started) {
      ended = true;
    }
  }
  return ended;
}

enum class HeadState { Unreadable, Live, Ended };

// A sample's wall time on the crashed session's clock.
WallTime session_wall(const RecordingState& st, SampleIndex s) {
  const int64_t utc = st.rate ? st.start_utc_ms + (s - st.start_sample) * 1000 / int64_t(st.rate) : st.last_utc_ms;
  return WallTime{utc, st.local_offset_min};
}

// Repairs one destination; returns the state of its current manifest. *audio_end is raised
// to where its repaired audio ends.
HeadState repair_dest(IFileSystem& fs, const RecordingState& st, const DestResume& d, RepairReport& rep,
                      SampleIndex* audio_end) {
  const auto wall = [&](SampleIndex s) { return session_wall(st, s); };

  // The chain: the current manifest first, then every prev_manifest it names, and so on.
  std::vector<ManifestFile> chain;
  std::set<std::wstring> seen;
  std::deque<std::wstring> todo{d.manifest_path};
  while (!todo.empty() && chain.size() < 10000) {
    const std::wstring path = todo.front();
    todo.pop_front();
    if (path.empty() || !seen.insert(path_key(path)).second) continue;
    if (!file_size(fs, path)) {  // vanished drive or deleted manifest
      rep.failed.push_back(path);
      if (chain.empty()) chain.push_back(ManifestFile{path, {}, {}, false});  // keeps index 0 = current
      continue;
    }
    ManifestFile m{path, read_manifest(fs, path), {}, true};
    for (const auto& r : m.recs)
      if (field(r, "ev") == "session_start" && !field(r, "prev_manifest").empty())
        todo.push_back(wide(field(r, "prev_manifest")));
    chain.push_back(std::move(m));
  }
  const bool head_ok = !chain.empty() && chain[0].readable;

  SampleIndex gap_at = st.last_sample;  // where the recorded audio ends, if later
  std::vector<OpenFile> opens;
  std::set<std::wstring> closed;
  std::map<std::wstring, SampleIndex> failed_over;  // old_path -> confirmed (absolute sample)
  for (size_t i = 0; i < chain.size(); ++i)
    for (const auto& r : chain[i].recs) {
      const std::string ev = field(r, "ev");
      const std::wstring path = wide(field(r, "path"));
      if (ev == "file_open" && !path.empty()) {
        opens.push_back(OpenFile{path, number(r, "sample").value_or(0), i});
      } else if (ev == "file_close" && !path.empty()) {
        closed.insert(path_key(path));
      } else if (ev == "failover") {
        const std::wstring old = wide(field(r, "old_path"));
        if (const auto c = number(r, "confirmed"); c && !old.empty()) failed_over[path_key(old)] = *c;
      }
    }

  for (const auto& o : opens) {
    const std::wstring k = path_key(o.path);
    if (closed.count(k)) continue;
    closed.insert(k);  // listed twice: handled once
    const auto fo = failed_over.find(k);
    SampleIndex end = o.start;
    if (is_opus(o.path)) {
      // Ogg pages are self-delimiting: nothing to repair. The end is the failover's
      // confirmed sample if it failed over, else only the open is known.
      if (!file_size(fs, o.path)) {
        rep.failed.push_back(o.path);
        continue;
      }
      if (fo != failed_over.end()) end = std::max(o.start, fo->second);
    } else {
      // A file that failed over ends at the confirmed sample: the rest is in the next file.
      std::optional<uint64_t> cap;
      if (fo != failed_over.end()) cap = uint64_t(std::max<SampleIndex>(0, fo->second - o.start));
      std::wstring err;
      const auto frames = repair_wav(fs, o.path, &err, cap);
      if (!frames) {
        rep.failed.push_back(o.path);
        continue;
      }
      end = o.start + SampleIndex(*frames);
    }
    const auto bytes = file_size(fs, o.path);
    JsonLine l("file_close", end, wall(end));
    l.str("path", o.path).num("start", o.start).num("end", end).num("bytes", int64_t(bytes.value_or(0)));
    chain[o.manifest].add.push_back(l);
    rep.repaired.push_back(o.path);
    gap_at = std::max(gap_at, end);
  }

  // Older manifests first; what cannot go where it belongs goes to the current one.
  std::vector<JsonLine> spill;
  for (size_t i = chain.size(); i-- > 1;)
    if (!chain[i].add.empty() && !append_lines(fs, chain[i].path, chain[i].add))
      spill.insert(spill.end(), chain[i].add.begin(), chain[i].add.end());
  *audio_end = std::max(*audio_end, gap_at);
  if (!head_ok) return HeadState::Unreadable;  // already reported
  std::vector<JsonLine> head = chain[0].add;
  head.insert(head.end(), spill.begin(), spill.end());
  // No gap after a clean stop (a stale state), nor twice (a crash during the last scan).
  const bool ended = ended_cleanly(chain[0].recs, st.session_id);
  bool have_gap = false;
  for (const auto& r : chain[0].recs)
    have_gap |= field(r, "ev") == "gap_start" && field(r, "reason") == "crash" && number(r, "sample") >= st.last_sample;
  if (!have_gap && !ended) head.push_back(JsonLine("gap_start", gap_at, wall(gap_at)).str("reason", L"crash"));
  if (!head.empty() && !append_lines(fs, chain[0].path, head)) rep.failed.push_back(chain[0].path);
  return ended ? HeadState::Ended : HeadState::Live;
}

}  // namespace

StateStore::StateStore(std::wstring ini_path) : path_(std::move(ini_path)) {}

bool StateStore::save(const RecordingState& s) {
  if (!ensure_unicode_file(path_)) return false;
  const size_t old = saved_dest_count(path_);
  bool ok = true;
  for (size_t i = 0; i < s.dests.size(); ++i) {
    const DestResume& d = s.dests[i];
    ok = write_section(path_, dest_section(i),
                       {{L"name", quoted(d.name)}, {L"manifest_path", quoted(d.manifest_path)},
                        {L"next_n", std::to_wstring(d.next_n)}}) &&
         ok;
  }
  ok = delete_dests(path_, s.dests.size(), old) && ok;
  return write_section(path_, L"State",
                {{L"recording", s.recording ? L"1" : L"0"},
                 {L"session_id", quoted(s.session_id)},
                 {L"session_name", quoted(s.session_name)},
                 {L"start_sample", std::to_wstring(s.start_sample)},
                 {L"last_sample", std::to_wstring(s.last_sample)},
                 {L"start_utc_ms", std::to_wstring(s.start_utc_ms)},
                 {L"last_utc_ms", std::to_wstring(s.last_utc_ms)},
                 {L"local_offset_min", std::to_wstring(s.local_offset_min)},
                 {L"rate", std::to_wstring(s.rate)},
                 {L"channels", std::to_wstring(s.channels)},
                 {L"device_id", quoted(s.device_id)},
                 {L"dests", std::to_wstring(s.dests.size())}}) &&
         ok;
}

std::optional<RecordingState> StateStore::load() const {
  const auto rec = get(path_, L"State", L"recording");
  if (!rec) return std::nullopt;
  RecordingState s;
  s.recording = *rec == L"1";
  s.session_id = get(path_, L"State", L"session_id").value_or(L"");
  s.session_name = get(path_, L"State", L"session_name").value_or(L"");
  s.device_id = get(path_, L"State", L"device_id").value_or(L"");
  s.start_sample = get_i64(path_, L"State", L"start_sample");
  s.last_sample = get_i64(path_, L"State", L"last_sample");
  s.start_utc_ms = get_i64(path_, L"State", L"start_utc_ms");
  s.last_utc_ms = get_i64(path_, L"State", L"last_utc_ms");
  s.local_offset_min = int32_t(get_i64(path_, L"State", L"local_offset_min"));
  s.rate = uint32_t(get_i64(path_, L"State", L"rate"));
  s.channels = uint16_t(get_i64(path_, L"State", L"channels"));
  const size_t n = saved_dest_count(path_);
  for (size_t i = 0; i < n; ++i) {
    const std::wstring sec = dest_section(i);
    s.dests.push_back(DestResume{get(path_, sec, L"name").value_or(L""), get(path_, sec, L"manifest_path").value_or(L""),
                                 uint32_t(std::max<int64_t>(1, get_i64(path_, sec, L"next_n")))});
  }
  return s;
}

bool StateStore::clear() {
  if (!ensure_unicode_file(path_)) return false;
  const bool ok = delete_dests(path_, 0, saved_dest_count(path_));
  return write_section(path_, L"State", {{L"recording", L"0"}}) && ok;
}

RepairReport repair_open_files(IFileSystem& fs, const RecordingState& st) {
  RepairReport rep;
  rep.audio_end = st.last_sample;
  size_t readable = 0, ended = 0;
  for (const auto& d : st.dests) {
    if (d.manifest_path.empty()) continue;  // never opened a manifest
    try {
      const HeadState h = repair_dest(fs, st, d, rep, &rep.audio_end);
      readable += h != HeadState::Unreadable;
      ended += h == HeadState::Ended;
      if (h == HeadState::Live) rep.live_heads.push_back(d.manifest_path);
    } catch (...) {  // out of memory on a huge manifest: report it, go on with the others
      rep.failed.push_back(d.manifest_path);
    }
  }
  rep.session_ended = readable > 0 && ended == readable;
  return rep;
}

std::vector<std::wstring> end_crashed_session(IFileSystem& fs, const RecordingState& st, const RepairReport& rep,
                                              const std::wstring& reason) {
  std::vector<std::wstring> failed;
  const SampleIndex at = rep.audio_end;
  for (const std::wstring& m : rep.live_heads) {
    try {
      if (!append_lines(fs, m, {JsonLine("session_end", at, session_wall(st, at)).str("reason", reason)}))
        failed.push_back(m);
    } catch (...) {  // out of memory: report it, go on with the others
      failed.push_back(m);
    }
  }
  return failed;
}

ResumeInfo make_resume(const RecordingState& st, WallTime now, SampleIndex audio_end) {
  const int64_t down_ms = std::max<int64_t>(0, now.utc_ms - st.last_utc_ms);
  return ResumeInfo{st, std::max(st.last_sample + down_ms * int64_t(st.rate) / 1000, audio_end)};
}

}  // namespace sl
