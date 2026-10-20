#include "session/manifest.h"

#include <windows.h>

#include <cstdio>

#include "core/clock.h"

namespace spool {

std::string utf8_from_wide(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

namespace {

void escape_into(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(char(cp));
  } else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

// Minimal parser for one flat JSON object on one line.
class LineParser {
 public:
  explicit LineParser(std::string_view s) : s_(s) {}

  bool parse(ManifestRecord& out) {
    skip_ws();
    if (!eat('{')) return false;
    skip_ws();
    if (eat('}')) return at_end();
    for (;;) {
      skip_ws();
      std::string key, val;
      if (!read_string(key)) return false;
      skip_ws();
      if (!eat(':')) return false;
      skip_ws();
      if (peek() == '"') {
        if (!read_string(val)) return false;
      } else if (!read_bare(val)) {
        return false;
      }
      out[key] = std::move(val);
      skip_ws();
      if (eat(',')) continue;
      if (eat('}')) return at_end();
      return false;
    }
  }

 private:
  char peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }
  bool eat(char c) {
    if (p_ < s_.size() && s_[p_] == c) {
      ++p_;
      return true;
    }
    return false;
  }
  void skip_ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r')) ++p_;
  }
  bool at_end() {
    skip_ws();
    return p_ == s_.size();
  }
  bool hex4(uint32_t& v) {
    if (p_ + 4 > s_.size()) return false;
    v = 0;
    for (size_t i = 0; i < 4; ++i) {
      const char c = s_[p_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    p_ += 4;
    return true;
  }
  bool read_string(std::string& out) {
    if (!eat('"')) return false;
    while (p_ < s_.size()) {
      const char c = s_[p_++];
      if (c == '"') return true;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ >= s_.size()) return false;
      const char e = s_[p_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {
            uint32_t lo = 0;
            const size_t save = p_;
            if (eat('\\') && eat('u') && hex4(lo) && lo >= 0xDC00 && lo < 0xE000) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              p_ = save;
              cp = 0xFFFD;
            }
          } else if (cp >= 0xDC00 && cp < 0xE000) {
            cp = 0xFFFD;
          }
          append_utf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;  // unterminated
  }
  // A number (or true/false/null) kept as text.
  bool read_bare(std::string& out) {
    const size_t start = p_;
    while (p_ < s_.size() && s_[p_] != ',' && s_[p_] != '}') ++p_;
    size_t end = p_;
    while (end > start && (s_[end - 1] == ' ' || s_[end - 1] == '\t' || s_[end - 1] == '\r')) --end;
    if (end == start) return false;
    out.assign(s_.substr(start, end - start));
    return true;
  }

  std::string_view s_;
  size_t p_ = 0;
};

}  // namespace

JsonLine::JsonLine(std::string_view ev, SampleIndex sample, WallTime wall) {
  s_ = "{\"ev\":";
  escape_into(s_, ev);
  s_ += ",\"sample\":" + std::to_string(sample) + ",\"utc\":";
  escape_into(s_, iso_utc(wall));
  s_ += ",\"local_offset\":" + std::to_string(wall.local_offset_min);
}

JsonLine& JsonLine::str(std::string_view key, std::wstring_view v) {
  s_.push_back(',');
  escape_into(s_, key);
  s_.push_back(':');
  escape_into(s_, utf8_from_wide(v));
  return *this;
}

JsonLine& JsonLine::num(std::string_view key, int64_t v) {
  s_.push_back(',');
  escape_into(s_, key);
  s_.push_back(':');
  s_ += std::to_string(v);
  return *this;
}

std::string JsonLine::finish() const { return s_ + "}"; }

std::unique_ptr<Manifest> Manifest::create(IFileSystem& fs, const std::wstring& path, std::wstring* err) {
  auto f = fs.create_new(path, err);
  if (!f) return nullptr;
  return std::unique_ptr<Manifest>(new Manifest(std::move(f)));
}

std::unique_ptr<Manifest> Manifest::open_append(IFileSystem& fs, const std::wstring& path,
                                                std::wstring* err) {
  auto f = fs.open_existing(path, err);
  if (!f) return nullptr;
  std::unique_ptr<Manifest> m(new Manifest(std::move(f)));
  const uint64_t size = m->file_->size();
  if (size > 0) {
    char last = 0;
    // Unreadable tail: be safe; a blank line is harmless to the reader.
    m->lead_newline_ = !m->file_->read_at(size - 1, &last, 1) || last != '\n';
  }
  return m;
}

bool Manifest::append(const JsonLine& l) {
  const std::string line = l.finish();
  std::lock_guard<std::mutex> lk(buf_m_);
  buf_ += line;
  buf_.push_back('\n');
  return true;
}

bool Manifest::flush() {
  std::lock_guard<std::mutex> flk(flush_m_);
  std::string taken;
  {
    std::lock_guard<std::mutex> lk(buf_m_);
    taken.swap(buf_);
  }
  if (!taken.empty()) {
    std::string out;
    if (lead_newline_) out.push_back('\n');
    out += taken;
    if (!file_->write(out.data(), out.size())) {
      lead_newline_ = true;  // a partial write may have left a torn line behind
      std::lock_guard<std::mutex> lk(buf_m_);
      buf_.insert(0, taken);  // failed bytes stay ahead of anything appended meanwhile
      return false;
    }
    lead_newline_ = false;
    dirty_ = true;
  }
  if (!dirty_) return true;  // nothing changed: no device flush (a parked writer stays idle)
  if (!file_->flush()) return false;
  dirty_ = false;
  return true;
}

std::vector<ManifestRecord> read_manifest(IFileSystem& fs, const std::wstring& path) {
  std::vector<ManifestRecord> out;
  auto f = fs.open_existing(path, nullptr);
  if (!f) return out;
  const uint64_t size = f->size();
  std::string data(size_t(size), '\0');
  if (size > 0 && !f->read_at(0, data.data(), data.size())) return out;
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    ManifestRecord rec;
    if (nl > pos && LineParser(std::string_view(data).substr(pos, nl - pos)).parse(rec))
      out.push_back(std::move(rec));
    pos = nl + 1;
  }
  return out;
}

}  // namespace spool
