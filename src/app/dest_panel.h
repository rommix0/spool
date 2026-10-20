#pragma once
// Destinations panel for the dashboard (window class SLDestPanel, spec 10.3): one block
// per destination with its format summary, current file and folder chain (path, state,
// time left). Neutral normally, amber while a failover awaits acknowledgment, red in
// LastResort or Disabled. Blocks that need attention (red, then amber) come first; when the
// blocks don't fit, a vertical scrollbar scrolls one block at a time. Right-click a block to
// append a folder to its chain.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/types.h"
#include "session/destination.h"

namespace spool {

class DestPanel {
 public:
  static bool register_class(HINSTANCE);

  DestPanel() = default;
  ~DestPanel();
  DestPanel(const DestPanel&) = delete;
  DestPanel& operator=(const DestPanel&) = delete;

  // A child of parent; takes its font from WM_SETFONT.
  HWND create(HWND parent, int id, HINSTANCE);
  HWND hwnd() const { return hwnd_; }

  // input: the session's input format (for the summaries and data rates). live: folders
  // can be appended (a session is recording). Repaints only when something visible changed.
  void set_status(const std::vector<DestStatus>&, StreamFormat input, bool live);
  // Called with the destination's index in the status list and the chosen folder.
  void on_add_folder(std::function<void(size_t, std::wstring)> f) { on_add_ = std::move(f); }
  // Height that fits every block at the control's DPI.
  int preferred_height() const;

 private:
  enum class Tone { Neutral, Amber, Red };
  struct FolderRow {
    std::wstring path, health, left;
    bool operator==(const FolderRow&) const = default;
  };
  struct Block {
    size_t index = 0;  // position in the status list (blocks are shown attention-first)
    std::wstring name, summary, file;
    std::vector<FolderRow> folders;
    Tone tone = Tone::Neutral;
    bool operator==(const Block&) const = default;
  };

  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT, WPARAM, LPARAM);
  int px(int v) const;
  int line_height() const;
  int block_height(const Block&) const;
  int hit_test(POINT client) const;  // block index or -1
  size_t max_top() const;            // the last first-visible block that still fills the view
  void update_scroll();
  void scroll_to(size_t top);
  void on_vscroll(int code);
  void context_menu(POINT screen);
  void paint(HDC, const RECT& client) const;
  void set_font(HFONT);  // also derives bold_ and line_h_
  void free_buffer();

  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;       // not owned
  HFONT bold_ = nullptr;       // owned, derived from font_
  int line_h_ = 0;             // text line height for font_, set by set_font
  std::vector<Block> blocks_;
  size_t top_ = 0;    // first visible block
  int wheel_ = 0;     // mouse wheel delta not yet turned into a scroll
  bool live_ = false;
  std::function<void(size_t, std::wstring)> on_add_;
  HDC mem_dc_ = nullptr;
  HBITMAP mem_bmp_ = nullptr, old_bmp_ = nullptr;
  SIZE mem_size_{};
};

}  // namespace spool
