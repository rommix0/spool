#pragma once
// The shell's modal folder picker (IFileOpenDialog with FOS_PICKFOLDERS).
#include <windows.h>

#include <string>

namespace sl {

// The folder the user picked, or empty if cancelled or failed. Needs COM on the calling thread.
std::wstring pick_folder(HWND owner);

}  // namespace sl
