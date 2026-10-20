#pragma once
// Thin helpers over Get/WritePrivateProfile*W for UTF-16 INI files (Unicode survives).
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace spool::ini {

// GetPrivateProfileString strips one pair of surrounding quotes, so quoting keeps leading
// and trailing spaces (and inner quotes) intact.
std::wstring quoted(const std::wstring& v);

// WritePrivateProfileString writes ANSI unless the file already starts with a UTF-16LE BOM.
// False if the file neither exists nor can be created.
bool ensure_unicode_file(const std::wstring& path);

// One whole section, replacing what was there.
bool write_section(const std::wstring& path, const std::wstring& section,
                   const std::vector<std::pair<std::wstring, std::wstring>>& kv);

// nullopt if the key is missing.
std::optional<std::wstring> get(const std::wstring& path, const std::wstring& section, const wchar_t* key);

// 0 if missing or not a number.
int64_t get_i64(const std::wstring& path, const std::wstring& section, const wchar_t* key);

}  // namespace spool::ini
