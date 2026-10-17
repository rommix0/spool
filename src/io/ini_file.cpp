#include "io/ini_file.h"

#include <windows.h>

#include <cstdlib>

namespace sl::ini {

namespace {
constexpr DWORD kValueChars = 32768;  // a long path plus quotes
constexpr wchar_t kMissing[] = L"\x01";
}  // namespace

std::wstring quoted(const std::wstring& v) { return L"\"" + v + L"\""; }

bool ensure_unicode_file(const std::wstring& path) {
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
  const uint8_t bom[2] = {0xFF, 0xFE};
  DWORD done = 0;
  const bool ok = WriteFile(h, bom, 2, &done, nullptr) && done == 2;
  CloseHandle(h);
  return ok;
}

bool write_section(const std::wstring& path, const std::wstring& section,
                   const std::vector<std::pair<std::wstring, std::wstring>>& kv) {
  std::wstring block;
  for (const auto& [k, v] : kv) {
    block += k + L"=" + v;
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return WritePrivateProfileSectionW(section.c_str(), block.c_str(), path.c_str()) != 0;
}

std::optional<std::wstring> get(const std::wstring& path, const std::wstring& section, const wchar_t* key) {
  std::wstring buf(kValueChars, L'\0');
  const DWORD n = GetPrivateProfileStringW(section.c_str(), key, kMissing, buf.data(), kValueChars, path.c_str());
  buf.resize(n);
  if (buf == kMissing) return std::nullopt;
  return buf;
}

int64_t get_i64(const std::wstring& path, const std::wstring& section, const wchar_t* key) {
  const auto v = get(path, section, key);
  return v ? std::wcstoll(v->c_str(), nullptr, 10) : 0;
}

}  // namespace sl::ini
