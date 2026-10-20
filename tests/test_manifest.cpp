#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "io/fault_file_system.h"
#include "session/event_log.h"
#include "session/manifest.h"
#include "test.h"

using namespace spool;

namespace {
// 2026-10-16 14:30:00 UTC, local = UTC-4 (10:30:00 local).
WallTime wall() { return WallTime{1792161000000LL, -240}; }

std::string text_of(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

size_t count_lines(const std::string& s) {
  size_t n = 0;
  for (char c : s) n += (c == '\n');
  return n;
}
}  // namespace

TEST(manifest_json_line_base_fields) {
  JsonLine l("marker", 48000, wall());
  l.str("name", L"intro").num("n", -7);
  CHECK_EQ(l.finish(),
           std::string("{\"ev\":\"marker\",\"sample\":48000,\"utc\":\"") + iso_utc(wall()) +
               "\",\"local_offset\":-240,\"name\":\"intro\",\"n\":-7}");
}

TEST(manifest_escapes_and_roundtrips) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto m = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  const std::wstring path = L"D:\\Gigs\\\"Live\" \u00fc.wav";
  JsonLine l("file_open", 0, wall());
  l.str("path", path).str("ctl", L"a\tb\nc\x01").str("emoji", L"\U0001F600").num("n", 3);
  CHECK(m->append(l));
  CHECK(m->flush());

  auto recs = read_manifest(fs, L"D:\\rec\\m.jsonl");
  CHECK_EQ(recs.size(), size_t(1));
  if (recs.size() != 1) return;
  CHECK_EQ(recs[0]["ev"], std::string("file_open"));
  CHECK_EQ(recs[0]["path"], std::string("D:\\Gigs\\\"Live\" \xC3\xBC.wav"));
  CHECK_EQ(recs[0]["ctl"], std::string("a\tb\nc\x01"));
  CHECK_EQ(recs[0]["emoji"], std::string("\xF0\x9F\x98\x80"));
  CHECK_EQ(recs[0]["n"], std::string("3"));
  CHECK_EQ(recs[0]["sample"], std::string("0"));
  CHECK_EQ(recs[0]["utc"], iso_utc(wall()));
  CHECK_EQ(recs[0]["local_offset"], std::string("-240"));
  // Control characters are \u00XX on disk; non-ASCII stays raw UTF-8.
  const std::string raw = text_of(fs.contents(L"D:\\rec\\m.jsonl"));
  CHECK(raw.find("\\u0001") != std::string::npos);
  CHECK(raw.find("\\t") != std::string::npos);
  CHECK(raw.find("\xC3\xBC") != std::string::npos);
}

TEST(manifest_reader_unescapes_all_forms) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto f = fs.create_new(L"D:\\rec\\m.jsonl", &err);
  CHECK(f != nullptr);
  if (!f) return;
  const std::string s =
      "{\"a\":\"q\\\" b\\\\ s\\/ \\b\\f\\n\\r\\t \\u00e9 \\ud83d\\ude00\",\"k\":12,\"neg\":-3}\n";
  CHECK(f->write(s.data(), s.size()));
  auto recs = read_manifest(fs, L"D:\\rec\\m.jsonl");
  CHECK_EQ(recs.size(), size_t(1));
  if (recs.size() != 1) return;
  CHECK_EQ(recs[0]["a"], std::string("q\" b\\ s/ \b\f\n\r\t \xC3\xA9 \xF0\x9F\x98\x80"));
  CHECK_EQ(recs[0]["k"], std::string("12"));
  CHECK_EQ(recs[0]["neg"], std::string("-3"));
}

TEST(manifest_buffered_until_flush) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto m = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(m->append(JsonLine("session_start", 0, wall())));
  CHECK(m->append(JsonLine("session_end", 100, wall())));
  CHECK(fs.contents(L"D:\\rec\\m.jsonl").empty());
  CHECK(m->flush());
  const std::string raw = text_of(fs.contents(L"D:\\rec\\m.jsonl"));
  CHECK_EQ(count_lines(raw), size_t(2));
  CHECK(!raw.empty() && raw.back() == '\n');
  CHECK_EQ(read_manifest(fs, L"D:\\rec\\m.jsonl").size(), size_t(2));
}

TEST(manifest_flush_failure_keeps_buffer) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto m = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  fs.add_rule({L"D:\\rec", FaultFileSystem::Kind::FailWrite, 0, 0});
  CHECK(m->append(JsonLine("marker", 1, wall())));
  CHECK(!m->flush());
  CHECK(fs.contents(L"D:\\rec\\m.jsonl").empty());
  fs.clear_rules();
  CHECK(m->flush());  // the buffered line was kept and is written now
  CHECK_EQ(read_manifest(fs, L"D:\\rec\\m.jsonl").size(), size_t(1));
}

TEST(manifest_skips_torn_last_line) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto f = fs.create_new(L"D:\\rec\\m.jsonl", &err);
  CHECK(f != nullptr);
  if (!f) return;
  const std::string s = "{\"ev\":\"session_start\",\"sample\":0}\n{\"ev\":\"fi";
  CHECK(f->write(s.data(), s.size()));
  auto recs = read_manifest(fs, L"D:\\rec\\m.jsonl");
  CHECK_EQ(recs.size(), size_t(1));
  CHECK(read_manifest(fs, L"D:\\rec\\missing.jsonl").empty());
}

TEST(manifest_create_refuses_existing) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto a = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(a != nullptr);
  auto b = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(b == nullptr);
  CHECK(!err.empty());
}

TEST(manifest_open_append_repairs_torn_line) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  {
    auto f = fs.create_new(L"D:\\rec\\m.jsonl", &err);
    CHECK(f != nullptr);
    if (!f) return;
    const std::string s = "{\"ev\":\"session_start\",\"sample\":0}\n{\"ev\":\"fi";
    CHECK(f->write(s.data(), s.size()));
  }
  auto m = Manifest::open_append(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(m->append(JsonLine("gap_start", 5, wall()).str("reason", L"crash")));
  CHECK(m->flush());
  auto recs = read_manifest(fs, L"D:\\rec\\m.jsonl");
  CHECK_EQ(recs.size(), size_t(2));  // torn line is skipped, new record intact
  if (recs.size() == 2) CHECK_EQ(recs[1]["reason"], std::string("crash"));
  CHECK(Manifest::open_append(fs, L"D:\\rec\\nope.jsonl", &err) == nullptr);
}

TEST(manifest_open_append_clean_file_adds_no_blank_line) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  {
    auto m = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(m->append(JsonLine("marker", 1, wall())));
    CHECK(m->flush());
  }
  auto m = Manifest::open_append(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  CHECK(m->append(JsonLine("marker", 2, wall())));
  CHECK(m->flush());
  const std::string raw = text_of(fs.contents(L"D:\\rec\\m.jsonl"));
  CHECK(raw.find("\n\n") == std::string::npos);
  CHECK_EQ(read_manifest(fs, L"D:\\rec\\m.jsonl").size(), size_t(2));
}

TEST(event_log_recent_newest_first) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\logs", &err));
  EventLog log(fs, L"D:\\logs\\events.log");
  log.add(Severity::Info, wall(), L"main", L"a");
  log.add(Severity::Warning, wall(), L"main", L"b");
  log.add(Severity::Error, wall(), L"main", L"c");
  auto r = log.recent(2);
  CHECK_EQ(r.size(), size_t(2));
  if (r.size() == 2) {
    CHECK(r[0].text == L"c");
    CHECK(r[1].text == L"b");
    CHECK(r[0].sev == Severity::Error);
    CHECK(r[0].dest == L"main");
  }
  CHECK_EQ(log.recent(100).size(), size_t(3));
  CHECK_EQ(count_lines(text_of(fs.contents(L"D:\\logs\\events.log"))), size_t(3));
}

TEST(event_log_line_format) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\logs", &err));
  EventLog log(fs, L"D:\\logs\\events.log");
  log.add(Severity::Warning, wall(), L"main", L"disk low");
  log.add(Severity::Info, wall(), L"", L"started");
  log.add(Severity::Error, wall(), L"b", L"\u00fc");
  CHECK_EQ(text_of(fs.contents(L"D:\\logs\\events.log")),
           std::string("2026-10-16 10:30:00  WARN  [main] disk low\r\n"
                       "2026-10-16 10:30:00  INFO  started\r\n"
                       "2026-10-16 10:30:00  ERROR  [b] \xC3\xBC\r\n"));
}

TEST(event_log_works_without_file) {
  FaultFileSystem fs;  // directory never created: create_new fails
  EventLog log(fs, L"D:\\nodir\\events.log");
  log.add(Severity::Info, wall(), L"", L"hello");
  CHECK_EQ(log.recent(5).size(), size_t(1));
  CHECK(fs.contents(L"D:\\nodir\\events.log").empty());
}

TEST(manifest_append_does_not_block_on_hung_flush) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto m = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  fs.add_rule({L"D:\\rec\\m.jsonl", FaultFileSystem::Kind::HangWrite, 0, 0});
  CHECK(m->append(JsonLine("marker", 1, wall())));
  bool flush_ok = true;
  std::thread t([&] { flush_ok = m->flush(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let flush enter the hung write
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(m->append(JsonLine("marker", 2, wall())));
  const auto took = std::chrono::steady_clock::now() - t0;
  CHECK(took < std::chrono::milliseconds(500));
  fs.release_hangs();
  t.join();
  CHECK(!flush_ok);
  fs.clear_rules();
  CHECK(m->flush());  // the failed bytes were re-queued ahead of the newer append
  auto recs = read_manifest(fs, L"D:\\rec\\m.jsonl");
  CHECK_EQ(recs.size(), size_t(2));
  if (recs.size() == 2) {
    CHECK_EQ(recs[0]["sample"], std::string("1"));
    CHECK_EQ(recs[1]["sample"], std::string("2"));
  }
}

TEST(event_log_appends_to_existing_file) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\logs", &err));
  {
    auto f = fs.create_new(L"D:\\logs\\events.log", &err);
    CHECK(f != nullptr);
    if (!f) return;
    const std::string s = "earlier line\r\n";
    CHECK(f->write(s.data(), s.size()));
  }
  EventLog log(fs, L"D:\\logs\\events.log");
  log.add(Severity::Info, wall(), L"", L"resumed");
  CHECK_EQ(text_of(fs.contents(L"D:\\logs\\events.log")),
           std::string("earlier line\r\n2026-10-16 10:30:00  INFO  resumed\r\n"));
}

// The app logs to app.log until a session starts, then to that session's log, and back.
TEST(event_log_switches_file) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\logs", &err));
  EventLog log(fs, L"D:\\logs\\app.log");
  log.add(Severity::Info, wall(), L"", L"a");
  log.set_file(L"D:\\logs\\s1.log");
  log.add(Severity::Info, wall(), L"", L"b");
  log.set_file(L"D:\\logs\\s1.log");  // already open: nothing changes
  log.add(Severity::Info, wall(), L"", L"c");
  log.set_file(L"D:\\logs\\app.log");  // appends to the earlier file
  log.add(Severity::Info, wall(), L"", L"d");
  CHECK_EQ(text_of(fs.contents(L"D:\\logs\\app.log")),
           std::string("2026-10-16 10:30:00  INFO  a\r\n2026-10-16 10:30:00  INFO  d\r\n"));
  CHECK_EQ(text_of(fs.contents(L"D:\\logs\\s1.log")),
           std::string("2026-10-16 10:30:00  INFO  b\r\n2026-10-16 10:30:00  INFO  c\r\n"));
  log.set_file(L"D:\\nodir\\x.log");  // cannot be opened: memory only
  log.add(Severity::Info, wall(), L"", L"e");
  CHECK_EQ(log.recent(10).size(), size_t(5));
  CHECK_EQ(count_lines(text_of(fs.contents(L"D:\\logs\\app.log"))), size_t(2));
}

TEST(event_log_file_line_has_no_embedded_newlines) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\logs", &err));
  EventLog log(fs, L"D:\\logs\\events.log");
  log.add(Severity::Info, wall(), L"a\nb", L"x\r\ny");
  CHECK_EQ(text_of(fs.contents(L"D:\\logs\\events.log")),
           std::string("2026-10-16 10:30:00  INFO  [a b] x  y\r\n"));
  auto r = log.recent(1);
  CHECK_EQ(r.size(), size_t(1));
  if (r.size() == 1) CHECK(r[0].text == L"x\r\ny");  // in-memory entry unchanged
}

TEST(event_log_recent_does_not_block_on_hung_write) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\logs", &err));
  EventLog log(fs, L"D:\\logs\\events.log");
  fs.add_rule({L"D:\\logs\\events.log", FaultFileSystem::Kind::HangWrite, 0, 0});
  std::thread t([&] { log.add(Severity::Info, wall(), L"", L"stuck"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const auto t0 = std::chrono::steady_clock::now();
  const size_t n = log.recent(10).size();
  const auto took = std::chrono::steady_clock::now() - t0;
  CHECK(took < std::chrono::milliseconds(500));
  CHECK_EQ(n, size_t(1));
  fs.release_hangs();
  t.join();
}

TEST(event_log_caps_memory_at_1000) {
  FaultFileSystem fs;
  EventLog log(fs, L"D:\\nodir\\events.log");
  for (int i = 0; i < 1005; ++i) log.add(Severity::Info, wall(), L"", std::to_wstring(i));
  auto r = log.recent(5000);
  CHECK_EQ(r.size(), size_t(1000));
  CHECK(r.front().text == L"1004");
  CHECK(r.back().text == L"5");
}

// Nothing new since the last successful flush: no device flush (a writer parked in a gap
// stays off the device). Bytes whose device flush failed are flushed again next time.
TEST(manifest_flush_skips_device_when_clean) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto m = Manifest::create(fs, L"D:\\rec\\m.jsonl", &err);
  CHECK(m != nullptr);
  if (!m) return;
  fs.add_rule({L"D:\\rec", FaultFileSystem::Kind::FailFlush, 0, 0});
  CHECK(m->flush());  // nothing written yet: the failing device flush is not called
  CHECK(m->append(JsonLine("marker", 1, wall())));
  CHECK(!m->flush());  // written, but not durable
  CHECK(!m->flush());  // still dirty: the device flush is tried again
  fs.clear_rules();
  CHECK(m->flush());
  fs.add_rule({L"D:\\rec", FaultFileSystem::Kind::FailFlush, 0, 0});
  CHECK(m->flush());  // clean again
  CHECK_EQ(read_manifest(fs, L"D:\\rec\\m.jsonl").size(), size_t(1));
}
