#include "session/naming.h"

#include <cstdio>
#include <cwctype>

#include "core/clock.h"

namespace sl {

namespace {
// CON, PRN, AUX, NUL, COM1-COM9, LPT1-LPT9 (any case): Windows device names.
bool reserved_name(std::wstring stem) {
  for (auto& c : stem) c = wchar_t(std::towupper(c));
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") return true;
  return stem.size() == 4 && (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) &&
         stem[3] >= L'1' && stem[3] <= L'9';
}
}  // namespace

std::wstring sanitize_component(std::wstring s) {
  for (auto& c : s) {
    if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
        c == L'\\' || c == L'|' || c == L'?' || c == L'*')
      c = L'_';
  }
  while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
  if (s.empty()) s = L"_";
  if (reserved_name(s.substr(0, s.find(L'.')))) s.insert(0, 1, L'_');  // "nul.wav" is the NUL device
  return s;
}

namespace {
std::wstring fmt(const wchar_t* f, int a, int b = 0, int c = 0) {
  wchar_t buf[32];
  std::swprintf(buf, 32, f, a, b, c);
  return buf;
}
}  // namespace

std::wstring expand_name(const std::wstring& tmpl, const NameInputs& in) {
  const LocalParts p = local_parts(in.first_sample);
  std::wstring seg;
  if (in.seg > 0) {
    seg = in.seg < 26 ? std::wstring(L"_") + static_cast<wchar_t>(L'a' + in.seg)
                      : L"_" + std::to_wstring(in.seg);
  }
  std::wstring out;
  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == L'{') {
      const size_t close = tmpl.find(L'}', i);
      if (close != std::wstring::npos) {
        const std::wstring tok = tmpl.substr(i + 1, close - i - 1);
        bool known = true;
        if (tok == L"session") out += sanitize_component(in.session);
        else if (tok == L"dest") out += sanitize_component(in.dest);
        else if (tok == L"date") out += fmt(L"%04d-%02d-%02d", p.year, p.month, p.day);
        else if (tok == L"time") out += fmt(L"%02d%02d%02d", p.hour, p.minute, p.second);
        else if (tok == L"n") out += fmt(L"%03d", static_cast<int>(in.n));
        else if (tok == L"seg") out += seg;
        else known = false;
        if (known) {
          i = close + 1;
          continue;
        }
      }
    }
    out += tmpl[i++];
  }
  return sanitize_component(out);
}

std::wstring unique_path(IFileSystem& fs, const std::wstring& dir, const std::wstring& stem,
                         const std::wstring& ext) {
  const std::wstring base =
      dir.empty() || dir.back() == L'\\' || dir.back() == L'/' ? dir : dir + L"\\";
  std::wstring path = base + stem + ext;
  for (unsigned i = 1; fs.exists(path); ++i) path = base + stem + L"_" + std::to_wstring(i) + ext;
  return path;
}

SampleIndex first_rotation_boundary(SampleIndex now, WallTime now_wall, uint32_t rate,
                                    uint32_t period_min) {
  if (period_min == 0) return kNoRotation;
  const int64_t period_ms = static_cast<int64_t>(period_min) * 60000;
  const int64_t local_ms =
      now_wall.utc_ms + static_cast<int64_t>(now_wall.local_offset_min) * 60000;
  int64_t since_midnight = local_ms % 86400000;
  if (since_midnight < 0) since_midnight += 86400000;
  const int64_t delta_ms = (since_midnight / period_ms + 1) * period_ms - since_midnight;  // > 0
  return now + (delta_ms * static_cast<int64_t>(rate) + 999) / 1000;  // ceil: strictly after now
}

SampleIndex next_rotation_boundary(SampleIndex prev_boundary, uint32_t rate, uint32_t period_min) {
  return prev_boundary + static_cast<int64_t>(period_min) * 60 * static_cast<int64_t>(rate);
}

}  // namespace sl
