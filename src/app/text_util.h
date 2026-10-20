#pragma once
// Small string helpers shared by the settings code and the window code.
#include <string>
#include <vector>

namespace spool {

std::wstring trim(const std::wstring& s);                       // surrounding spaces and tabs removed
bool eq_nocase(const std::wstring& a, const std::wstring& b);   // ordinal, case-insensitive
std::wstring join_lines(const std::vector<std::wstring>& lines);  // joined with "\n"

}  // namespace spool
