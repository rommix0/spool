#include "session/preflight.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

#include "session/destination.h"
#include "session/naming.h"

namespace spool {

namespace {

constexpr uint64_t kMinPrimarySeconds = 3600;

std::wstring join(const std::wstring& dir, const std::wstring& name) {
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

bool folder_healthy(IFileSystem& fs, const std::wstring& dir, uint64_t low_water, const std::wstring& prober) {
  std::wstring err;
  if (dir.empty() || !fs.ensure_dir(dir, &err)) return false;
  // A name of its own per probe (R12): two destinations sharing a folder, or a leftover
  // from a crash, never make each other fail.
  static std::atomic<uint64_t> counter{0};
  const std::wstring name = L".spool_probe_" + sanitize_component(prober) + L"_" + std::to_wstring(GetCurrentProcessId()) +
                            L"_" + std::to_wstring(++counter) + L".tmp";
  const std::wstring probe = join(dir, name);
  std::unique_ptr<IFile> f = fs.create_new(probe, &err);
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
    bool usable = false;  // a folder the writer would accept (writable and above low water)
    std::vector<std::pair<std::wstring, std::wstring>> volumes;  // (volume id, folder), chain order
    for (size_t i = 0; i < d.folders.size(); ++i) {
      const std::wstring& dir = d.folders[i];
      if (folder_healthy(fs, dir, 0, d.name)) {
        const std::optional<uint64_t> free = fs.free_bytes(dir);
        if (free && *free >= low_water) usable = true;
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
    if (usable) {
      r.can_start = true;
    } else {
      r.errors.push_back(L"No folder can be written with enough free space" + tag + L": it starts in last resort");
    }
  }
  return r;
}

}  // namespace spool
