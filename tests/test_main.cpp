#include <cstring>

#include "test.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : "";
  int passed = 0, failed = 0, skipped = 0;
  for (const auto& t : sltest::registry()) {
    if (*filter && std::strstr(t.name, filter) == nullptr) continue;
    sltest::state() = {};
    std::printf("[ RUN  ] %s\n", t.name);
    std::fflush(stdout);
    t.fn();
    if (sltest::state().failed) {
      ++failed;
      std::printf("[ FAIL ] %s\n", t.name);
    } else if (sltest::state().skipped) {
      ++skipped;
      std::printf("[ SKIP ] %s\n", t.name);
    } else {
      ++passed;
      std::printf("[  OK  ] %s\n", t.name);
    }
  }
  std::printf("%d passed, %d failed", passed, failed);
  if (skipped) std::printf(", %d skipped", skipped);
  std::printf("\n");
  return failed == 0 ? 0 : 1;
}
