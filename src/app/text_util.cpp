#include "app/text_util.h"

#include <windows.h>

namespace spool {

std::wstring trim(const std::wstring& s) {
  const size_t a = s.find_first_not_of(L" \t");
  if (a == std::wstring::npos) return {};
  return s.substr(a, s.find_last_not_of(L" \t") - a + 1);
}

bool eq_nocase(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring join_lines(const std::vector<std::wstring>& lines) {
  std::wstring out;
  for (size_t i = 0; i < lines.size(); ++i) out += (i ? L"\n" : L"") + lines[i];
  return out;
}

}  // namespace spool
