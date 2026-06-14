#pragma once

#include <avbridge.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace avb::test {

constexpr int skip = 77;

class Context {
public:
  void section(const char *name) { std::printf("[%s]\n", name); }

  void check(bool condition, const char *message) {
    if (condition)
      return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures_;
  }

  template <typename Actual, typename Expected>
  void equal(const Actual &actual, const Expected &expected,
             const char *message) {
    check(actual == expected, message);
  }

  void near(double actual, double expected, double tolerance,
            const char *message) {
    if (std::fabs(actual - expected) <= tolerance)
      return;
    std::fprintf(stderr, "FAIL: %s (got %.6f, expected %.6f +/- %.6f)\n",
                 message, actual, expected, tolerance);
    ++failures_;
  }

  void string(const char *actual, const char *expected, const char *message) {
    if (actual && expected && std::strcmp(actual, expected) == 0)
      return;
    std::fprintf(stderr, "FAIL: %s (got \"%s\", expected \"%s\")\n", message,
                 actual ? actual : "(null)", expected ? expected : "(null)");
    ++failures_;
  }

  int finish(const char *suite) const {
    if (failures_ == 0) {
      std::printf("%s: passed\n", suite);
      return 0;
    }
    std::fprintf(stderr, "%s: %d failure%s\n", suite, failures_,
                 failures_ == 1 ? "" : "s");
    return 1;
  }

private:
  int failures_ = 0;
};

inline bool parse_backend(const char *name, avb_backend &backend) {
  return name && avb_backend_from_name(name, &backend) == AVB_OK;
}

inline bool backend_is_built(avb_backend backend) {
  return avb_backend_is_available(backend) != 0;
}

template <typename T, size_t N>
bool contains(const T (&values)[N], int count, T value) {
  if (count < 0 || count > static_cast<int>(N))
    return false;
  for (int i = 0; i < count; ++i) {
    if (values[i] == value)
      return true;
  }
  return false;
}

template <typename T, size_t N> bool unique(const T (&values)[N], int count) {
  if (count < 0 || count > static_cast<int>(N))
    return false;
  for (int i = 0; i < count; ++i) {
    for (int j = i + 1; j < count; ++j) {
      if (values[i] == values[j])
        return false;
    }
  }
  return true;
}

inline const char *decoder_error(avb_decoder *decoder) {
  const char *error = avb_decoder_get_last_error(decoder);
  return error ? error : "(no error message)";
}

inline const char *encoder_error(avb_encoder *encoder) {
  const char *error = avb_encoder_get_last_error(encoder);
  return error ? error : "(no error message)";
}

} // namespace avb::test
