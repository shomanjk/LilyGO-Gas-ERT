#pragma once

// Minimal test harness shared by the host unit tests (no external framework).

#include <cstdio>
#include <string>

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    g_checks++;                                                         \
    if (!(cond)) {                                                      \
      g_failures++;                                                     \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    g_checks++;                                                                                     \
    auto va_ = (a);                                                                                 \
    auto vb_ = (b);                                                                                 \
    if (!(va_ == vb_)) {                                                                            \
      g_failures++;                                                                                 \
      std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b,            \
                  static_cast<long long>(va_), static_cast<long long>(vb_));                        \
    }                                                                                               \
  } while (0)

#define CHECK_STR(a, b)                                                                             \
  do {                                                                                              \
    g_checks++;                                                                                     \
    std::string va_ = (a);                                                                          \
    std::string vb_ = (b);                                                                          \
    if (va_ != vb_) {                                                                               \
      g_failures++;                                                                                 \
      std::printf("  FAIL %s:%d: %s == %s (\"%s\" vs \"%s\")\n", __FILE__, __LINE__, #a, #b,        \
                  va_.c_str(), vb_.c_str());                                                        \
    }                                                                                               \
  } while (0)

struct TestCase {
  const char *name;
  void (*fn)();
};

template<size_t N> int run_tests(const TestCase (&tests)[N]) {
  for (const auto &t : tests) {
    int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
