#include "kalandra/Diagnostics.h"
#include "kalandra/Utf8.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace kalandra;
static void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
static std::string report(const SourceFile &source, SourceSpan span) {
  const Diagnostic diagnostic{"E001", "test", span};
  return renderDiagnosticsJson(&source, {&diagnostic, 1});
}
int main() {
  try {
    SourceFile unicode("unicode.k", "a😀é\nx");
    auto text = report(unicode, {1, 5});
    require(
        text.find("\"start\":{\"line\":0,\"character\":1},\"end\":{\"line\":0,\"character\":3}") !=
            std::string::npos,
        "supplementary scalar did not occupy two UTF-16 units");
    text = report(unicode, {5, 9});
    require(
        text.find("\"start\":{\"line\":0,\"character\":3},\"end\":{\"line\":1,\"character\":1}") !=
            std::string::npos,
        "multiline UTF-16 range incorrect");
    text = report(unicode, {2, 4});
    require(
        text.find("\"start\":{\"line\":0,\"character\":1},\"end\":{\"line\":0,\"character\":1}") !=
            std::string::npos,
        "split scalar offset was not snapped to its start");
    text = report(unicode, {9, 3});
    require(text.find("\"byteRange\":{\"start\":9,\"end\":9}") != std::string::npos,
            "backwards span was not clamped");
    text = report(unicode, {std::numeric_limits<std::size_t>::max(),
                            std::numeric_limits<std::size_t>::max()});
    require(text.find("\"byteRange\":{\"start\":9,\"end\":9}") != std::string::npos,
            "oversized span was not clamped");
    SourceFile endings("lines.k", "x\r\ny\n");
    text = report(endings, {1, 3});
    require(
        text.find("\"start\":{\"line\":0,\"character\":1},\"end\":{\"line\":1,\"character\":0}") !=
            std::string::npos,
        "CRLF range incorrect");
    SourceFile carriageReturn("carriage.k", "x\ry");
    text = report(carriageReturn, {2, 3});
    require(
        text.find("\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":1}") !=
            std::string::npos,
        "standalone CR editor range incorrect");
    auto first = std::make_shared<SourceFile>("first.k", "one");
    auto second = std::make_shared<SourceFile>("lib\"\\é.k", "😀\nmissing");
    SourceFile graph("first.k", first->text() + "\n" + second->text(), {{0, first}, {4, second}});
    text = report(graph, {9, 16});
    require(
        text.find("\"file\":\"lib\\\"\\\\é.k\"") != std::string::npos &&
            text.find("\"byteRange\":{\"start\":5,\"end\":12}") != std::string::npos &&
            text.find(
                "\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":7}") !=
                std::string::npos,
        "original module location or escaped path lost");
    text = report(graph, {2, 999});
    require(text.find("\"byteRange\":{\"start\":2,\"end\":3}") != std::string::npos,
            "range crossed original source boundary");
    std::string bytes;
    for (unsigned i = 0; i < 256; ++i)
      bytes += char(i);
    const Diagnostic escaped{"quoted\"code", bytes, {0, 1}};
    text = renderDiagnosticsJson(nullptr, {&escaped, 1}, CheckSummary{2, 3}, "a\n\tb.k");
    require(validUtf8(text) && text.find('\0') == std::string::npos &&
                text.find("\\u0000") != std::string::npos &&
                text.find("\"byteRange\":null,\"range\":null") != std::string::npos &&
                text.find("\"summary\":null") != std::string::npos,
            "escaping, missing ranges or failure summary incorrect");
    SourceFile invalid("invalid.k", std::string("a\xff", 2));
    text = report(invalid, {1, 2});
    require(validUtf8(text) &&
                text.find("\"end\":{\"line\":0,\"character\":2}") != std::string::npos,
            "invalid UTF-8 position was not safely counted");
    std::vector<Diagnostic> many;
    SourceFile longLine("long.k", std::string(1024 * 1024, 'x'));
    for (unsigned i = 0; i < 100; ++i)
      many.push_back({"E", "many", {longLine.text().size() - i, longLine.text().size()}});
    text = renderDiagnosticsJson(&longLine, many);
    require(text.find("1048576") != std::string::npos, "long-line batch positions lost");
    text = renderDiagnosticsJson(nullptr, {}, CheckSummary{2, 42});
    require(text == "{\"formatVersion\":1,\"positionEncoding\":\"utf-16\",\"ok\":true,"
                    "\"diagnostics\":[],\"summary\":{\"functions\":2,\"values\":42}}\n",
            "success response schema changed");
    std::cout << "Structured diagnostics: Unicode/CRLF, module origins, bounded spans, escaping, "
                 "invalid UTF-8 and batch positions passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
