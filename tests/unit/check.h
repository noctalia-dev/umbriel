#pragma once

// Minimal zero-dependency test harness. UMBRIEL_TEST(descriptiveName) { CHECK(condition); CHECK_EQ(actual, expected); }
// int main() { return RUN_TESTS(); } Cases self-register, every failure is reported (a failing CHECK does not stop the
// case), and failures print file, line, the expressions, and both values.

#include <cstddef>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel::test {

  struct Case {
    std::string_view name;
    void (*run)();
  };

  inline std::vector<Case>& cases() {
    static std::vector<Case> registry;
    return registry;
  }

  inline int& failureCount() {
    static int count = 0;
    return count;
  }

  struct Registrar {
    Registrar(std::string_view name, void (*run)()) { cases().push_back({name, run}); }
  };

  inline void reportFailure(const char* file, int line, const std::string& message) {
    std::println(stderr, "  {}:{}: {}", file, line, message);
    ++failureCount();
  }

  // Values without a std::formatter (wlr_box and friends) still produce a
  // located failure, just without the value dump.
  template <typename T> std::string describe(const T& value) {
    if constexpr (std::formattable<T, char>) {
      return std::format("{}", value);
    } else {
      return "<unprintable>";
    }
  }

  inline int runAll() {
    size_t failedCases = 0;
    for (const auto& testCase : cases()) {
      const int before = failureCount();
      testCase.run();
      if (failureCount() != before) {
        std::println(stderr, "FAIL {}", testCase.name);
        ++failedCases;
      }
    }
    if (failedCases == 0) {
      std::println("ok: {} case(s)", cases().size());
      return 0;
    }
    std::println(stderr, "{} of {} case(s) failed", failedCases, cases().size());
    return 1;
  }

} // namespace umbriel::test

#define UMBRIEL_TEST(name)                                                                                             \
  static void name();                                                                                                  \
  static const ::umbriel::test::Registrar kRegistrar##name(#name, &name);                                              \
  static void name()

#define CHECK(expr)                                                                                                    \
  do {                                                                                                                 \
    if (!static_cast<bool>(expr)) {                                                                                    \
      ::umbriel::test::reportFailure(__FILE__, __LINE__, "CHECK(" #expr ") failed");                                   \
    }                                                                                                                  \
  } while (false)

#define CHECK_EQ(actualExpr, expectedExpr)                                                                             \
  do {                                                                                                                 \
    const auto& umbrielCheckActual = (actualExpr);                                                                     \
    const auto& umbrielCheckExpected = (expectedExpr);                                                                 \
    if (!(umbrielCheckActual == umbrielCheckExpected)) {                                                               \
      ::umbriel::test::reportFailure(                                                                                  \
          __FILE__, __LINE__,                                                                                          \
          std::format(                                                                                                 \
              "CHECK_EQ({}, {}) failed: got {}, want {}", #actualExpr, #expectedExpr,                                  \
              ::umbriel::test::describe(umbrielCheckActual), ::umbriel::test::describe(umbrielCheckExpected)           \
          )                                                                                                            \
      );                                                                                                               \
    }                                                                                                                  \
  } while (false)

#define RUN_TESTS() ::umbriel::test::runAll()
