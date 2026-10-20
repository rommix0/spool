#include "io/fault_file_system.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace spool {

namespace {

// Case-insensitive identity: lower-case, '/' folded to '\'.
std::wstring norm(const std::wstring& p) {
  std::wstring k = p;
  for (wchar_t& c : k) c = (c == L'/') ? L'\\' : static_cast<wchar_t>(std::towlower(c));
  return k;
}

// Directory key: normalized without trailing separators ("d:\rec\" -> "d:\rec").
std::wstring dir_key(const std::wstring& p) {
  std::wstring k = norm(p);
  while (!k.empty() && k.back() == L'\\') k.pop_back();
  return k;
}

bool starts_with(const std::wstring& s, const std::wstring& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// A directory "matches" a prefix as if it ended with a separator, so the
// prefix "d:\rec\" covers the directory "d:\rec".
bool dir_matches(const std::wstring& dkey, const std::wstring& prefix_key) {
  return starts_with(dkey + L"\\", prefix_key);
}

std::wstring parent_key(const std::wstring& key) {
  const size_t pos = key.find_last_of(L'\\');
  return pos == std::wstring::npos ? std::wstring() : key.substr(0, pos);
}

}  // namespace

struct FaultState {
  struct Node {
    std::wstring path;  // as created (original spelling)
    std::wstring key;   // normalized
    std::vector<uint8_t> data;
    std::map<uint64_t, uint64_t> counted;  // rule id -> bytes written since the rule was added
  };
  struct StoredRule {
    FaultFileSystem::Rule rule;
    std::wstring prefix_key;
    uint64_t id;
  };

  mutable std::mutex mu;
  std::condition_variable cv;
  std::map<std::wstring, std::shared_ptr<Node>> files;
  std::set<std::wstring> dirs;
  std::vector<StoredRule> rules;
  uint64_t next_rule_id = 1;
  uint64_t hang_generation = 0;
  std::vector<std::pair<std::wstring, uint64_t>> free_bytes;
  std::vector<std::pair<std::wstring, std::wstring>> volumes;

  // All below: caller holds mu.
  bool open_blocked_file(const std::wstring& key) const {
    for (const auto& r : rules)
      if (r.rule.kind == FaultFileSystem::Kind::FailOpen && starts_with(key, r.prefix_key)) return true;
    return false;
  }
  bool open_blocked_dir(const std::wstring& dkey) const {
    for (const auto& r : rules)
      if (r.rule.kind == FaultFileSystem::Kind::FailOpen && dir_matches(dkey, r.prefix_key)) return true;
    return false;
  }
  void add_dir_and_parents(std::wstring dkey) {
    while (!dkey.empty()) {
      dirs.insert(dkey);
      dkey = parent_key(dkey);
    }
  }
  bool dir_exists(const std::wstring& dkey) const {
    // Drive roots ("d:") and the empty root always exist, like on Win32.
    return dkey.find(L'\\') == std::wstring::npos || dirs.count(dkey) != 0;
  }
  // True if the rule applies to a write of n bytes to node (prefix and threshold).
  bool write_rule_active(const StoredRule& r, Node& node, size_t n) const {
    if (!starts_with(node.key, r.prefix_key)) return false;
    return node.counted[r.id] + n > r.rule.after_bytes;
  }
};

namespace {

using Kind = FaultFileSystem::Kind;
using Node = FaultState::Node;

class FaultFile : public IFile {
 public:
  FaultFile(std::shared_ptr<FaultState> st, std::shared_ptr<Node> node)
      : st_(std::move(st)), node_(std::move(node)) {}

  bool write(const void* p, size_t n) override { return do_write(p, n, nullptr); }
  bool write_at(uint64_t off, const void* p, size_t n) override { return do_write(p, n, &off); }

  bool read_at(uint64_t off, void* p, size_t n) override {
    std::lock_guard<std::mutex> lk(st_->mu);
    if (!node_) return false;
    if (off > node_->data.size() || n > node_->data.size() - off) return false;
    if (n) std::memcpy(p, node_->data.data() + off, n);
    return true;
  }

  bool flush() override {
    std::lock_guard<std::mutex> lk(st_->mu);
    if (!node_) return false;
    for (const auto& r : st_->rules)
      if (r.rule.kind == Kind::FailFlush && starts_with(node_->key, r.prefix_key)) return false;
    return true;
  }

  uint64_t size() const override {
    std::lock_guard<std::mutex> lk(st_->mu);
    return node_ ? node_->data.size() : 0;
  }

  void close() override {
    std::lock_guard<std::mutex> lk(st_->mu);
    node_.reset();
  }

 private:
  bool do_write(const void* p, size_t n, const uint64_t* off) {
    uint64_t delay_ms = 0;
    {
      std::lock_guard<std::mutex> lk(st_->mu);
      if (!node_) return false;
      for (const auto& r : st_->rules)
        if (r.rule.kind == Kind::SlowWrite && st_->write_rule_active(r, *node_, n))
          delay_ms += r.rule.delay_ms;
    }
    if (delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    std::unique_lock<std::mutex> lk(st_->mu);
    if (!node_) return false;
    for (const auto& r : st_->rules) {
      if (r.rule.kind == Kind::FailWrite && st_->write_rule_active(r, *node_, n)) return false;
      if (r.rule.kind == Kind::HangWrite && st_->write_rule_active(r, *node_, n)) {
        // Blocks only this thread; the mutex is released while waiting.
        const uint64_t gen = st_->hang_generation;
        st_->cv.wait(lk, [&] { return st_->hang_generation != gen; });
        return false;
      }
    }
    auto& data = node_->data;
    const uint64_t at = off ? *off : data.size();
    if (at > data.max_size() || n > data.max_size() - at) return false;
    if (data.size() < at + n) data.resize(static_cast<size_t>(at + n), 0);
    if (n) std::memcpy(data.data() + at, p, n);
    for (const auto& r : st_->rules) {
      const bool write_kind =
          r.rule.kind == Kind::FailWrite || r.rule.kind == Kind::HangWrite || r.rule.kind == Kind::SlowWrite;
      if (write_kind && starts_with(node_->key, r.prefix_key)) node_->counted[r.id] += n;
    }
    return true;
  }

  std::shared_ptr<FaultState> st_;
  std::shared_ptr<Node> node_;  // guarded by st_->mu; null once closed
};

void set_err(std::wstring* err, const wchar_t* msg) {
  if (err) *err = msg;
}

}  // namespace

FaultFileSystem::FaultFileSystem() : st_(std::make_shared<FaultState>()) {}

FaultFileSystem::~FaultFileSystem() { release_hangs(); }

void FaultFileSystem::add_rule(Rule r) {
  std::lock_guard<std::mutex> lk(st_->mu);
  std::wstring pk = norm(r.prefix);
  st_->rules.push_back({std::move(r), std::move(pk), st_->next_rule_id++});
}

void FaultFileSystem::clear_rules() {
  std::lock_guard<std::mutex> lk(st_->mu);
  st_->rules.clear();
  for (auto& f : st_->files) f.second->counted.clear();
}

void FaultFileSystem::release_hangs() {
  {
    std::lock_guard<std::mutex> lk(st_->mu);
    ++st_->hang_generation;
  }
  st_->cv.notify_all();
}

void FaultFileSystem::set_free_bytes(const std::wstring& prefix, uint64_t bytes) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring k = norm(prefix);
  for (auto& e : st_->free_bytes)
    if (e.first == k) { e.second = bytes; return; }
  st_->free_bytes.emplace_back(k, bytes);
}

void FaultFileSystem::set_volume(const std::wstring& prefix, const std::wstring& id) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring k = norm(prefix);
  for (auto& e : st_->volumes)
    if (e.first == k) { e.second = id; return; }
  st_->volumes.emplace_back(k, id);
}

std::vector<uint8_t> FaultFileSystem::contents(const std::wstring& path) const {
  std::lock_guard<std::mutex> lk(st_->mu);
  auto it = st_->files.find(norm(path));
  return it == st_->files.end() ? std::vector<uint8_t>() : it->second->data;
}

std::unique_ptr<IFile> FaultFileSystem::create_new(const std::wstring& path, std::wstring* err) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring key = norm(path);
  if (st_->open_blocked_file(key)) {
    set_err(err, L"injected failure: cannot create file");
    return nullptr;
  }
  if (!st_->dir_exists(parent_key(key))) {
    set_err(err, L"parent directory does not exist");
    return nullptr;
  }
  if (st_->files.count(key) || st_->dirs.count(dir_key(path))) {
    set_err(err, L"file already exists");
    return nullptr;
  }
  auto node = std::make_shared<Node>();
  node->path = path;
  node->key = key;
  st_->files.emplace(key, node);
  return std::make_unique<FaultFile>(st_, std::move(node));
}

std::unique_ptr<IFile> FaultFileSystem::open_existing(const std::wstring& path, std::wstring* err) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring key = norm(path);
  if (st_->open_blocked_file(key)) {
    set_err(err, L"injected failure: cannot open file");
    return nullptr;
  }
  auto it = st_->files.find(key);
  if (it == st_->files.end()) {
    set_err(err, L"file not found");
    return nullptr;
  }
  return std::make_unique<FaultFile>(st_, it->second);
}

bool FaultFileSystem::exists(const std::wstring& path) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring key = norm(path);
  const std::wstring dkey = dir_key(path);
  if (st_->open_blocked_file(key) || st_->open_blocked_dir(dkey)) return false;  // vanished volume
  return st_->files.count(key) != 0 || (!dkey.empty() && st_->dirs.count(dkey) != 0);
}

bool FaultFileSystem::ensure_dir(const std::wstring& dir, std::wstring* err) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring dkey = dir_key(dir);
  if (st_->open_blocked_dir(dkey)) {
    set_err(err, L"injected failure: cannot create directory");
    return false;
  }
  if (st_->files.count(dkey)) {
    set_err(err, L"a file with that name exists");
    return false;
  }
  st_->add_dir_and_parents(dkey);
  return true;
}

bool FaultFileSystem::remove(const std::wstring& path) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring key = norm(path);
  if (st_->open_blocked_file(key)) return false;
  return st_->files.erase(key) != 0;  // open handles keep their node alive
}

std::vector<std::wstring> FaultFileSystem::list(const std::wstring& dir, const std::wstring& suffix) {
  std::vector<std::wstring> out;
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring dkey = dir_key(dir);
  if (st_->open_blocked_dir(dkey)) return out;
  std::wstring base = dir;
  while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
  const std::wstring skey = norm(suffix);
  // Sorted like Win32 (CompareStringOrdinal ignore-case folds to upper case).
  std::vector<std::pair<std::wstring, std::wstring>> found;  // (upper-case name, full path)
  for (const auto& f : st_->files) {
    const Node& n = *f.second;
    if (parent_key(n.key) != dkey) continue;
    const size_t cut = n.path.find_last_of(L"\\/");
    const std::wstring name = n.path.substr(cut == std::wstring::npos ? 0 : cut + 1);
    const std::wstring nkey = norm(name);
    if (nkey.size() < skey.size() || nkey.compare(nkey.size() - skey.size(), skey.size(), skey) != 0) continue;
    std::wstring upper = name;
    for (wchar_t& c : upper) c = static_cast<wchar_t>(std::towupper(c));
    found.emplace_back(std::move(upper), base + L"\\" + name);
  }
  std::sort(found.begin(), found.end());
  for (auto& e : found) out.push_back(std::move(e.second));
  return out;
}

std::optional<uint64_t> FaultFileSystem::free_bytes(const std::wstring& dir) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring dkey = dir_key(dir);
  if (st_->open_blocked_dir(dkey)) return std::nullopt;
  size_t best = 0;
  uint64_t value = uint64_t(1) << 40;  // default 1 TB
  bool found = false;
  for (const auto& e : st_->free_bytes) {
    if (!dir_matches(dkey, e.first)) continue;
    if (!found || e.first.size() > best) { best = e.first.size(); value = e.second; found = true; }
  }
  return value;
}

std::wstring FaultFileSystem::volume_id(const std::wstring& dir) {
  std::lock_guard<std::mutex> lk(st_->mu);
  const std::wstring dkey = dir_key(dir);
  if (st_->open_blocked_dir(dkey)) return {};
  size_t best = 0;
  std::wstring value;
  bool found = false;
  for (const auto& e : st_->volumes) {
    if (!dir_matches(dkey, e.first)) continue;
    if (!found || e.first.size() > best) { best = e.first.size(); value = e.second; found = true; }
  }
  return value;
}

}  // namespace spool
