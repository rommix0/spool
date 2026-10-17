#include "session/preflight.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

#include "session/destination.h"

namespace sl {

namespace {

constexpr int kProbeAttempts = 5;
constexpr DWORD kProbeRetryMs = 10;
constexpr uint64_t kMinPrimarySeconds = 3600;

std::wstring join(const std::wstring& dir, const wchar_t* name) {
  if (dir.empty() || dir.back() == L'\\' || dir.back() == L'/') return dir + name;
  return dir + L"\\" + name;
}

std::wstring gigabytes(uint64_t bytes) {
  wchar_t buf[32];
  std::swprintf(buf, 32, L"%.1f GB", double(bytes) / double(1ull << 30));
  return buf;
}

}  // namespace

uint64_t low_water_bytes(const SessionParams& p, uint64_t data_rate) {
  return std::max<uint64_t>(uint64_t(p.low_water_minutes) * 60 * data_rate, p.low_water_min_bytes);
}

bool folder_healthy(IFileSystem& fs, const std::wstring& dir, uint64_t low_water) {
  std::wstring err;
  if (dir.empty() || !fs.ensure_dir(dir, &err)) return false;
  const std::wstring probe = join(dir, L".sl_probe.tmp");
  std::unique_ptr<IFile> f;
  for (int i = 0; i < kProbeAttempts && !f; ++i) {
    f = fs.create_new(probe, &err);
    if (f) break;
    if (!fs.exists(probe)) return false;  // files cannot be created here
    // A leftover from a crash, or another destination probing this folder right now.
    fs.remove(probe);
    Sleep(kProbeRetryMs);
  }
  if (!f) return false;
  static const char kBytes[] = "probe";
  const bool wrote = f->write(kBytes, sizeof kBytes - 1) && f->flush();
  f->close();
  fs.remove(probe);  // a probe that cannot be removed does not make the folder unhealthy
  if (!wrote) return false;
  const std::optional<uint64_t> free = fs.free_bytes(dir);
  return free && *free >= low_water;
}

PreflightResult preflight(const std::vector<DestinationConfig>& dests, const SessionParams& sp, IFileSystem& fs) {
  PreflightResult r{false, {}, {}};
  for (const auto& d : dests) {
    const size_t channels = d.channels.empty() ? sp.input_channels : d.channels.size();
    const uint64_t rate = data_rate(d, sp.rate, channels);
    const uint64_t low_water = low_water_bytes(sp, rate);
    const std::wstring tag = L" (" + d.name + L")";
    bool writable = false;
    std::vector<std::pair<std::wstring, std::wstring>> volumes;  // (volume id, folder), chain order
    for (size_t i = 0; i < d.folders.size(); ++i) {
      const std::wstring& dir = d.folders[i];
      if (folder_healthy(fs, dir, 0)) {
        writable = true;
        const std::optional<uint64_t> free = fs.free_bytes(dir);
        if (free && *free < low_water)
          r.warnings.push_back(L"Only " + gigabytes(*free) + L" free in " + dir + L", below the low-water mark" + tag);
        if (i == 0 && free && rate && *free / rate < kMinPrimarySeconds)
          r.warnings.push_back(L"Only about " + std::to_wstring(*free / rate / 60) + L" min of recording space in " +
                               dir + tag);
      } else {
        r.warnings.push_back(L"Cannot write to " + dir + tag);
      }
      const std::wstring vol = fs.volume_id(dir);
      if (vol.empty()) continue;
      for (const auto& [id, earlier] : volumes)
        if (id == vol) {
          r.warnings.push_back(dir + L" is on the same drive as " + earlier + tag);
          break;
        }
      volumes.emplace_back(vol, dir);
    }
    if (writable) {
      r.can_start = true;
    } else {
      r.errors.push_back(L"No folder can be written" + tag + L": it starts in last resort");
    }
  }
  return r;
}

}  // namespace sl
