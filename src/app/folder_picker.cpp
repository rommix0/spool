#include "app/folder_picker.h"

#include <shobjidl.h>
#include <wrl/client.h>

namespace spool {

std::wstring pick_folder(HWND owner) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dlg;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
    return {};
  FILEOPENDIALOGOPTIONS opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  if (FAILED(dlg->Show(owner))) return {};
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(dlg->GetResult(&item))) return {};
  PWSTR p = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return {};
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

}  // namespace spool
