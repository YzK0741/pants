#include <cstdio>
#include <string>

import utility.text;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

} // namespace

int main() {
  check(pants::trim("  hello\t\n") == "hello", "trim should remove surrounding whitespace");
  check(pants::trim("xxhelloxx", "x") == "hello", "trim should honour delimiters");
  check(pants::trim_left("  hello") == "hello", "trim_left should remove leading whitespace");
  check(pants::trim_right("hello  ") == "hello", "trim_right should remove trailing whitespace");
  check(pants::trim("   ").empty(), "all-whitespace string should trim to empty");

  check(pants::wstring_to_string(L"hello") == "hello", "wide ASCII should convert");
  check(pants::string_to_wstring("hello") == L"hello", "ASCII should convert to wide");
  const std::string utf8 = "中文";
  check(pants::wstring_to_string(pants::string_to_wstring(utf8)) == utf8, "UTF-8 round trip should be stable");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("text ok");
  return 0;
}
