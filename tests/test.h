#pragma once
// Minimal test harness: no third-party code.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace sltest {

struct TestCase {
  const char* name;
  void (*fn)();
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> r;
  return r;
}

struct State {
  bool failed = false;
  bool skipped = false;
};
inline State& state() {
  static State s;
  return s;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
  state().failed = true;
}

template <class T>
std::string to_str(const T& v) {
  if constexpr (std::is_arithmetic_v<T>) return std::to_string(v);
  else if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<long long>(v));
  else return "<value>";
}
inline std::string to_str(const std::string& v) { return "\"" + v + "\""; }
inline std::string to_str(const char* v) { return std::string("\"") + v + "\""; }
inline std::string to_str(bool v) { return v ? "true" : "false"; }

}  // namespace sltest

#define TEST(name)                                                  \
  static void test_##name();                                        \
  static sltest::Registrar registrar_##name(#name, &test_##name);   \
  static void test_##name()

#define CHECK(expr)                                                 \
  do {                                                              \
    if (!(expr)) sltest::fail(__FILE__, __LINE__, #expr);           \
  } while (0)

#define CHECK_EQ(a, b)                                                           \
  do {                                                                           \
    const auto& sl_a_ = (a);                                                     \
    const auto& sl_b_ = (b);                                                     \
    if (!(sl_a_ == sl_b_))                                                       \
      sltest::fail(__FILE__, __LINE__,                                           \
                   std::string(#a " == " #b " (") + sltest::to_str(sl_a_) +      \
                       " vs " + sltest::to_str(sl_b_) + ")");                    \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
  do {                                                                           \
    const double sl_a_ = static_cast<double>(a);                                 \
    const double sl_b_ = static_cast<double>(b);                                 \
    if (!(std::fabs(sl_a_ - sl_b_) <= static_cast<double>(eps)))                 \
      sltest::fail(__FILE__, __LINE__,                                           \
                   std::string(#a " ~= " #b " (") + std::to_string(sl_a_) +      \
                       " vs " + std::to_string(sl_b_) + ", eps " #eps ")");      \
  } while (0)

// Mark the current test skipped (not failed) and return from it.
#define SKIP(msg)                                                   \
  do {                                                              \
    std::printf("  SKIP %s\n", msg);                                \
    sltest::state().skipped = true;                                 \
    return;                                                         \
  } while (0)
