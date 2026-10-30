#include "Edits.h"
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace kalandra::lsp;
static void require(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
static TextChange edit(std::size_t line, std::size_t start, std::size_t end, std::string text) {
  return {Range{{line, start}, {line, end}}, std::move(text), {}};
}
static void expect(std::string_view original, std::vector<TextChange> changes,
                   std::string expected) {
  auto result = applyChanges(original, changes);
  require(result && *result == expected, "unexpected edited text");
}
static void reject(std::string_view original, std::vector<TextChange> changes,
                   EditLimits limits = {}) {
  require(!applyChanges(original, changes, limits), "invalid edit was accepted");
}
int main() {
  try {
    expect("a😀éz", {edit(0, 1, 3, "猫")}, "a猫éz");
    expect("a😀éz", {edit(0, 3, 4, "")}, "a😀z");
    reject("a😀éz", {edit(0, 2, 3, "x")});
    reject("a😀éz", {edit(0, 1, 2, "x")});
    expect("abc", {edit(0, 100, 100, "!")}, "abc!");
    reject("abc", {edit(0, 100, 99, "!")});
    reject("abc", {edit(1, 0, 0, "!")});
    reject("abc", {{{Range{{1, 0}, {0, 0}}}, "x", {}}});
    expect("abc\n", {edit(1, 0, 0, "!")}, "abc\n!");
    reject("abc\n", {edit(2, 0, 0, "!")});
    for (auto ending : {"\n", "\r\n", "\r"}) {
      std::string source = std::string("a") + ending + "b";
      expect(source, {{Range{{0, 1}, {1, 0}}, "-", {}}}, "a-b");
      expect(source, {edit(0, 200, 200, "!")}, std::string("a!") + ending + "b");
      expect(source, {edit(1, 1, 1, "!")}, source + "!");
    }
    expect("a\r\nb\rc\n", {{Range{{0, 1}, {3, 0}}, "!", {}}}, "a!");
    expect("", {edit(0, 0, 0, "😀\n"), edit(1, 0, 0, "x")}, "😀\nx");
    expect("abc", {edit(0, 1, 2, "long"), edit(0, 5, 6, "!")}, "along!");
    expect("abc", {{{}, "new\ntext", {}}, edit(1, 0, 4, "😀")}, "new\n😀");
    expect("abc", {edit(0, 1, 2, "x"), {{}, "reset", {}}}, "reset");
    expect("abc", {}, "abc");
    auto withLength = edit(0, 1, 3, "x");
    withLength.rangeLength = 2;
    expect("a😀z", {withLength}, "axz");
    withLength.rangeLength = 4;
    reject("a😀z", {withLength});
    expect("a\r\nb", {{Range{{0, 1}, {1, 0}}, "", 2}}, "ab");
    reject("abc", {{{}, "replacement", 3}});
    reject(std::string("a\xff", 2), {});
    reject("abc", {edit(0, 0, 0, std::string(1, char(255)))});
    reject("abc", {edit(0, 0, 1, "x"), edit(9, 0, 0, "x")});
    EditLimits small;
    small.maxBytes = 3;
    expect("abc", {edit(0, 0, 3, "abc")}, "abc");
    reject("abc", {edit(0, 0, 0, "x")}, small);
    auto shrunk = applyChanges("abc", std::vector{edit(0, 0, 3, "x")}, small);
    require(shrunk && *shrunk == "x", "shrinking at byte limit failed");
    small.maxChanges = 1;
    reject("abc", {edit(0, 0, 0, ""), edit(0, 0, 0, "")}, small);
    small.maxWorkBytes = 3;
    reject("abc", {edit(0, 0, 0, "")}, small);
    std::cout << "Incremental edits: UTF-16, surrogate boundaries, LF/CRLF/CR, sequential "
                 "ranges, clamping, rangeLength, atomic failure and limits passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
