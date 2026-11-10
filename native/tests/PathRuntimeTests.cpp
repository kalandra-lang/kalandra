#include "../runtime/Path.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
using namespace kalandra;
using namespace kalandra::runtime;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::fprintf(stderr, "path assertion %d: %s\n", __LINE__, #x);                               \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
struct Result {
  std::string text;
  std::uint64_t flag;
};
static StringDescriptor string(Invocation &ctx, const std::string &value) {
  StringDescriptor out;
  kalandra_string_literal_v1(&ctx, &out, value.data(), value.size());
  return out;
}
static StringArrayDescriptor strings(Invocation &ctx, const std::vector<std::string> &values) {
  if (values.empty())
    return {};
  auto *data =
      static_cast<StringDescriptor *>(ctx.allocateRecord(values.size() * sizeof(StringDescriptor)));
  CHECK(data);
  for (std::size_t i = 0; i < values.size(); ++i)
    data[i] = string(ctx, values[i]);
  return {data, data, 0, std::int64_t(values.size()), 1};
}
static Result evaluate(PathStyle style, PathAction action, const std::vector<std::string> &values) {
  Invocation ctx;
  StringDescriptor out;
  std::uint64_t flag;
  if (action == PathAction::Join) {
    auto input = strings(ctx, values);
    flag = pathOperation(ctx, out, action, &input, style);
  } else {
    CHECK(values.size() == 1);
    auto input = string(ctx, values[0]);
    flag = pathOperation(ctx, out, action, &input, style);
  }
  CHECK(!ctx.status() && validateString(ctx, out));
  Result result{out.data ? std::string(out.data, out.byteLength) : std::string{}, flag};
  ctx.settle();
  CHECK(!ctx.live() && ctx.allocations() == ctx.releases());
  return result;
}
static void path(PathStyle style, const std::string &input, const std::string &name,
                 const std::string &parent, const std::string &extension, bool absolute) {
  CHECK(evaluate(style, PathAction::FileName, {input}).text == name);
  CHECK(evaluate(style, PathAction::ParentPath, {input}).text == parent);
  CHECK(evaluate(style, PathAction::Extension, {input}).text == extension);
  CHECK(evaluate(style, PathAction::IsAbsolute, {input}).flag == absolute);
}
static void joined(PathStyle style, const std::vector<std::string> &parts,
                   const std::string &expected) {
  const auto actual = evaluate(style, PathAction::Join, parts).text;
  if (actual != expected) {
    std::fprintf(stderr, "join got [%s] expected [%s]\n", actual.c_str(), expected.c_str());
    std::abort();
  }
}
static std::string unhex(const std::string &text) {
  if (text == "-")
    return {};
  CHECK(text.size() % 2 == 0);
  std::string out;
  for (std::size_t i = 0; i < text.size(); i += 2)
    out += char(std::stoul(text.substr(i, 2), nullptr, 16));
  return out;
}
int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--batch") {
    std::string line;
    while (std::getline(std::cin, line)) {
      std::istringstream input(line);
      unsigned style, action, count;
      input >> style >> action >> count;
      std::vector<std::string> values;
      std::string text;
      for (unsigned i = 0; i < count; ++i) {
        input >> text;
        values.push_back(unhex(text));
      }
      const auto result = evaluate(style ? PathStyle::Windows : PathStyle::Posix,
                                   static_cast<PathAction>(action), values);
      std::printf("%llu ", (unsigned long long)result.flag);
      if (result.text.empty())
        std::printf("-");
      else
        for (unsigned char ch : result.text)
          std::printf("%02x", unsigned(ch));
      std::printf("\n");
    }
    return 0;
  }
  constexpr auto p = PathStyle::Posix, w = PathStyle::Windows;
  for (auto style : {p, w}) {
    path(style, "", "", ".", "", false);
    path(style, "file.txt", "file.txt", ".", ".txt", false);
    path(style, ".gitignore", ".gitignore", ".", "", false);
    path(style, "..hidden", "..hidden", ".", "", false);
    path(style, ".config.json", ".config.json", ".", ".json", false);
    path(style, "archive.tar.gz", "archive.tar.gz", ".", ".gz", false);
    path(style, "file.", "file.", ".", ".", false);
    path(style, "...", "...", ".", "", false);
    path(style, ".", ".", ".", "", false);
    path(style, "..", "..", ".", "", false);
    path(style, "dir/../file.txt", "file.txt", "dir/..", ".txt", false);
    path(style, "dir/", "dir", ".", "", false);
    path(style, "κόσμος/سلام.txt", "سلام.txt", "κόσμος", ".txt", false);
    joined(style, {}, "");
    joined(style, {"", ""}, "");
  }
  path(p, "/", "", "/", "", true);
  path(p, "//", "", "//", "", true);
  path(p, "///a///", "a", "///", "", true);
  path(p, "//host/share/file", "file", "//host/share", "", true);
  path(p, "a\\b.txt", "a\\b.txt", ".", ".txt", false);
  path(p, "C:\\a\\b", "C:\\a\\b", ".", "", false);
  joined(p, {"data", "runs", "output.csv"}, "data/runs/output.csv");
  joined(p, {"a", ".", "..", "b/"}, "a/./../b/");
  joined(p, {"a/", "", "b"}, "a/b");
  joined(p, {"a///", "b"}, "a///b");
  joined(p, {"old", "//host/share", "x"}, "//host/share/x");
  joined(p, {"old", "/new", "leaf"}, "/new/leaf");
  joined(p, {"/root", "..", "file"}, "/root/../file");
  path(w, "\\", "", "\\", "", false);
  path(w, "C:", "", "C:", "", false);
  path(w, "C:foo.txt", "foo.txt", "C:", ".txt", false);
  path(w, "C:\\", "", "C:\\", "", true);
  path(w, "C:/dir/file.txt", "file.txt", "C:/dir", ".txt", true);
  path(w, "\\root\\file", "file", "\\root", "", false);
  path(w, "\\\\server\\share", "", "\\\\server\\share", "", true);
  path(w, "\\\\server\\share\\", "", "\\\\server\\share\\", "", true);
  path(w, "\\\\server\\share\\dir\\file.txt", "file.txt", "\\\\server\\share\\dir", ".txt", true);
  path(w, "//server/share/dir/file", "file", "//server/share/dir", "", true);
  path(w, "\\\\server", "", "\\\\server", "", false);
  path(w, "\\\\server\\", "", "\\\\server\\", "", false);
  path(w, "\\\\?\\C:\\dir\\file.txt", "file.txt", "\\\\?\\C:\\dir", ".txt", true);
  path(w, "\\\\?\\C:\\dir/file.txt", "dir/file.txt", "\\\\?\\C:\\", ".txt", true);
  path(w, "\\\\?\\UNC\\server\\share\\file.txt", "file.txt", "\\\\?\\UNC\\server\\share\\", ".txt",
       true);
  path(w, "\\\\.\\COM1", "", "\\\\.\\COM1", "", true);
  path(w, "\\\\?\\Volume{abc}\\dir", "dir", "\\\\?\\Volume{abc}\\", "", true);
  path(w, "\\\\?\\", "", "\\\\?\\", "", false);
  joined(w, {"data", "runs", "output.csv"}, "data\\runs\\output.csv");
  joined(w, {"C:", "foo", "bar"}, "C:foo\\bar");
  joined(w, {"C:\\base", "c:child"}, "C:\\base\\child");
  joined(w, {"C:\\base", "c:"}, "C:\\base");
  joined(w, {"C:\\base", "D:child"}, "D:child");
  joined(w, {"C:\\base", "\\child"}, "C:\\child");
  joined(w, {"C:\\base", "/child"}, "C:/child");
  joined(w, {"base", "\\child"}, "\\child");
  joined(w, {"\\\\server\\share\\base", "\\child"}, "\\\\server\\share\\child");
  joined(w, {"\\\\", "server", "share", "dir", "\\next"}, "\\\\server\\share\\next");
  joined(w, {"\\\\?\\", "UNC", "server", "share", "dir", "\\next"},
         "\\\\?\\UNC\\server\\share\\next");
  joined(w, {"\\\\?\\C:\\base", "a/b"}, "\\\\?\\C:\\base\\a/b");
  joined(w, {"\\\\?\\C:\\base", "/child"}, "\\\\?\\C:\\base\\/child");
  joined(w, {"old", "C:\\new", "..", "file"}, "C:\\new\\..\\file");
  joined(w, {"\\\\server\\\\", "\\", "share", "\\leaf"}, "\\\\server\\share\\leaf");
  // Embedded NUL is text, not a terminator. Filesystem validation happens in std/fs.
  joined(p, {std::string("a\0b", 3), "c"}, std::string("a\0b/c", 5));
  path(p, std::string("a\0b.txt", 7), std::string("a\0b.txt", 7), ".", ".txt", false);
  {
    Invocation ctx;
    auto input = string(ctx, "a/b.txt");
    pathOperation(ctx, input, PathAction::ParentPath, &input, p);
    CHECK(std::string(input.data, input.byteLength) == "a");
    ctx.settle();
    CHECK(!ctx.live());
  }
  for (auto action : {0u, 6u, UINT32_MAX}) {
    Invocation ctx;
    StringDescriptor out, input{};
    CHECK(kalandra_path_v1(&ctx, &out, action, &input) == 0 && ctx.status() == 3 && !out.allocated);
  }
  {
    Invocation ctx;
    StringDescriptor out;
    StringArrayDescriptor invalid{nullptr, nullptr, 0, INT64_MAX, 1};
    pathOperation(ctx, out, PathAction::Join, &invalid, p);
    CHECK(ctx.status() == 3 && !out.allocated);
  }
  {
    Invocation ctx;
    auto input = string(ctx, "/foo");
    StringDescriptor out;
    std::atomic<bool> token{true};
    ctx.bindCancellation(&token);
    pathOperation(ctx, out, PathAction::ParentPath, &input, p);
    CHECK(ctx.poll() && !out.allocated);
    ctx.fault(1);
    pathOperation(ctx, out, PathAction::IsAbsolute, &input, p);
    CHECK(ctx.status() == 1 && !out.allocated);
  }
  {
    Invocation ctx;
    const std::string longName(2 * 1024 * 1024, 'x');
    auto parts = strings(ctx, {"\\\\" + longName + "\\share", "\\one", "\\two", "leaf"});
    StringDescriptor out;
    std::atomic<std::uint64_t> polls{0};
    ctx.watchPolls(&polls);
    pathOperation(ctx, out, PathAction::Join, &parts, w);
    CHECK(!ctx.status() && out.byteLength == std::int64_t(longName.size() + 17));
    CHECK(polls > longName.size() / 1024 && polls < 20 * longName.size() / 1024);
    ctx.settle();
    CHECK(!ctx.live() && ctx.allocations() == ctx.releases());
  }
  // A long UNC anchor followed by many tiny components must not be rescanned
  // at every append/reset. Poll counts establish a linear bound without timing.
  for (bool rooted : {false, true}) {
    Invocation ctx;
    const std::string name(2 * 1024 * 1024, 's');
    std::vector<std::string> values{rooted ? "\\\\" + name + "\\share" : "\\\\" + name + "\\\\"};
    for (unsigned n = 0; n < 500; ++n)
      values.push_back(rooted ? "\\leaf" : "tail");
    auto input = strings(ctx, values);
    StringDescriptor out;
    std::atomic<std::uint64_t> polls{0};
    ctx.watchPolls(&polls);
    pathOperation(ctx, out, PathAction::Join, &input, w);
    CHECK(!ctx.status() && validateString(ctx, out));
    CHECK(polls < 30 * (name.size() / 1024 + values.size()));
    if (rooted)
      CHECK(std::string(out.data, out.byteLength) == "\\\\" + name + "\\share\\leaf");
    else
      CHECK(out.byteLength == std::int64_t(name.size() + 4 + 4 + 499 * 5));
    ctx.settle();
    CHECK(!ctx.live() && ctx.allocations() == ctx.releases());
  }
  std::puts("Lexical paths: POSIX/Windows roots, joins, components, UTF-8, ownership and "
            "cancellation passed");
}
