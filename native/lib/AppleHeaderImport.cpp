#include "AppleHeaderImport.h"
#ifdef __APPLE__
#include <algorithm>
#include <array>
#include <clang-c/Index.h>
#include <dlfcn.h>
#include <filesystem>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/FileUtilities.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>
#include <map>
#include <stdexcept>
#include <vector>

namespace kalandra {
namespace {
std::string xcrun(llvm::StringRef arg, llvm::StringRef value = {}) {
  llvm::SmallString<256> output;
  if (llvm::sys::fs::createTemporaryFile("kalandra-sdk", "txt", output))
    throw std::runtime_error("cannot create SDK discovery output");
  llvm::FileRemover clean(output);
  std::vector<llvm::StringRef> args{"/usr/bin/xcrun", "--sdk", "macosx", arg};
  if (!value.empty())
    args.push_back(value);
  std::array<std::optional<llvm::StringRef>, 3> io{llvm::StringRef{}, output.str(), std::nullopt};
  if (llvm::sys::ExecuteAndWait(args.front(), args, std::nullopt, io, 30))
    throw std::runtime_error("xcrun could not locate the selected macOS developer tools");
  auto data = llvm::MemoryBuffer::getFile(output);
  if (!data)
    throw std::runtime_error(data.getError().message());
  return (*data)->getBuffer().trim().str();
}

// Load the indexing library belonging to the selected SDK toolchain. Only the
// compiler needs it. The compiled application links directly to the system API.
struct IndexAPI {
  void *lib;
  explicit IndexAPI(void *lib) : lib(lib) {}
  template <class T> T symbol(const char *name) {
    auto fn = reinterpret_cast<T>(dlsym(lib, name));
    if (!fn)
      throw std::runtime_error(std::string("missing libclang function ") + name);
    return fn;
  }
#define INDEX_FN(f) decltype(&clang_##f) f = symbol<decltype(&clang_##f)>("clang_" #f)
  INDEX_FN(createIndex);
  INDEX_FN(disposeIndex);
  INDEX_FN(parseTranslationUnit2);
  INDEX_FN(disposeTranslationUnit);
  INDEX_FN(getTranslationUnitCursor);
  INDEX_FN(visitChildren);
  INDEX_FN(getCString);
  INDEX_FN(disposeString);
  INDEX_FN(getCursorSpelling);
  INDEX_FN(getCursorType);
  INDEX_FN(getCursorResultType);
  INDEX_FN(getCanonicalType);
  INDEX_FN(getTypeDeclaration);
  INDEX_FN(getEnumDeclIntegerType);
  INDEX_FN(getTypeSpelling);
  INDEX_FN(Type_getObjCEncoding);
  INDEX_FN(Cursor_getNumArguments);
  INDEX_FN(Cursor_getArgument);
  INDEX_FN(getPointeeType);
  INDEX_FN(getNumDiagnostics);
  INDEX_FN(getDiagnostic);
  INDEX_FN(getDiagnosticSeverity);
  INDEX_FN(getDiagnosticSpelling);
  INDEX_FN(disposeDiagnostic);
  INDEX_FN(Cursor_isVariadic);
  INDEX_FN(Type_getSizeOf);
  INDEX_FN(Cursor_getOffsetOfField);
#undef INDEX_FN
  std::string str(CXString cx) {
    const char *p = getCString(cx);
    std::string result = p ? p : "";
    disposeString(cx);
    return result;
  }
};
struct Surface {
  IndexAPI &api;
  const std::set<std::string> &wanted;
  std::map<std::string, std::string> methods, records;
  std::set<std::string> conflicts;
  std::string owner;

  std::vector<CXCursor> children(CXCursor cursor) {
    std::vector<CXCursor> list;
    api.visitChildren(
        cursor,
        [](CXCursor c, CXCursor, CXClientData p) {
          static_cast<std::vector<CXCursor> *>(p)->push_back(c);
          return CXChildVisit_Continue;
        },
        &list);
    return list;
  }
  bool fields(CXType type, std::vector<std::string> &names, unsigned depth = 0) {
    if (depth > 4)
      return false;
    size_t offset = 0;
    for (const auto child : children(api.getTypeDeclaration(type))) {
      if (child.kind != CXCursor_FieldDecl)
        continue;
      if (api.Cursor_getOffsetOfField(child) != static_cast<long long>(offset * 8))
        return false;
      auto field = api.getCanonicalType(api.getCursorType(child));
      if (field.kind == CXType_Record) {
        if (!fields(field, names, depth + 1))
          return false;
      } else if (field.kind == CXType_Double) {
        auto name = api.str(api.getCursorSpelling(child));
        if (name.empty() || std::find(names.begin(), names.end(), name) != names.end())
          return false;
        names.push_back(name);
      } else
        return false;
      if (names.size() > 4)
        return false;
      offset += api.Type_getSizeOf(field);
    }
    return offset && static_cast<long long>(offset) == api.Type_getSizeOf(type);
  }
  std::string type(CXType input, bool result) {
    auto t = api.getCanonicalType(input);
    if (t.kind == CXType_Enum)
      t = api.getCanonicalType(api.getEnumDeclIntegerType(api.getTypeDeclaration(t)));
    if (t.kind == CXType_Record && !result) {
      auto name = api.str(api.getCursorSpelling(api.getTypeDeclaration(t)));
      std::vector<std::string> names;
      if (name.empty() || !fields(t, names))
        return {};
      std::string rec = "export type " + name + ":\n";
      for (auto &field : names)
        rec += "  " + field + ": f64\n";
      records[name] = rec;
      return name;
    }
    if (t.kind == CXType_Pointer) {
      const auto pointee = api.getPointeeType(t);
      const auto spelling = api.str(api.getTypeSpelling(pointee));
      if (spelling == "const char" && !result)
        return "cstr";
      if (pointee.kind == CXType_ObjCInterface)
        return "cptr";
      return {};
    }
    const auto enc = api.str(api.Type_getObjCEncoding(input));
    if (enc == "@" || enc == "#" || enc.starts_with("@\""))
      return "cptr";
    if (enc == "v" && result)
      return "unit";
    if (enc == "B")
      return "bool";
    if (enc == "d")
      return "f64";
    if (enc == "f")
      return "f32";
    if (enc == "i")
      return "i32";
    if (enc == "I")
      return "u32";
    if (enc == "q")
      return "i64";
    if (enc == "Q")
      return "u64";
    return {};
  }
  void inspect(CXCursor c) {
    if (c.kind == CXCursor_StructDecl && wanted.contains(api.str(api.getCursorSpelling(c)))) {
      type(api.getCursorType(c), false);
      return;
    }
    const bool method =
        c.kind == CXCursor_ObjCInstanceMethodDecl || c.kind == CXCursor_ObjCClassMethodDecl;
    if (method || c.kind == CXCursor_FunctionDecl) {
      const auto selector = api.str(api.getCursorSpelling(c));
      std::string name = method ? owner + "_" + selector : selector;
      if (name.ends_with(':'))
        name.pop_back();
      std::replace(name.begin(), name.end(), ':', '_');
      if (!wanted.contains(name) || api.Cursor_isVariadic(c))
        return;
      auto result = type(api.getCursorResultType(c), true);
      if (result.empty())
        return;
      std::string params = method ? "receiver: cptr" : "";
      for (int i = 0; i < api.Cursor_getNumArguments(c); ++i) {
        auto arg = type(api.getCursorType(api.Cursor_getArgument(c, i)), false);
        if (arg.empty())
          return;
        if (!params.empty())
          params += ", ";
        params += "arg" + std::to_string(i) + ": " + arg;
      }
      auto line = "export extern \"" + std::string(method ? "objc" : "C") + "\" fn " + name + "(" +
                  params + ") -> " + result;
      if (method)
        line += " = \"" + selector + "\"";
      line += "\n";
      auto [it, inserted] = methods.emplace(name, line);
      if (!inserted && it->second != line)
        conflicts.insert(name);
      return;
    }
    if (c.kind != CXCursor_ObjCInterfaceDecl && c.kind != CXCursor_ObjCProtocolDecl &&
        c.kind != CXCursor_ObjCCategoryDecl)
      return;
    owner = api.str(api.getCursorSpelling(c));
    auto members = children(c);
    if (c.kind == CXCursor_ObjCCategoryDecl)
      for (auto m : members)
        if (m.kind == CXCursor_ObjCClassRef) {
          owner = api.str(api.getCursorSpelling(m));
          break;
        }
    for (auto m : members)
      if (m.kind == CXCursor_ObjCInstanceMethodDecl || m.kind == CXCursor_ObjCClassMethodDecl)
        inspect(m);
  }
};
} // namespace

std::expected<std::optional<std::string>, std::string>
importAppleFramework(const std::string &name, const std::set<std::string> &symbols) {
  namespace fs = std::filesystem;
  try {
    const auto sdk = xcrun("--show-sdk-path");
    const auto folder = fs::path(sdk) / "System/Library/Frameworks" / (name + ".framework");
    const auto header = folder / "Headers" / (name + ".h");
    if (!fs::is_regular_file(header))
      return std::optional<std::string>{};
#ifndef __aarch64__
    return std::unexpected("Objective-C framework imports currently require Apple Silicon");
#endif
    const auto clang = fs::path(xcrun("--find", "clang"));
    const auto library = clang.parent_path().parent_path() / "lib/libclang.dylib";
    void *lib = dlopen(library.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!lib)
      return std::unexpected("cannot load the selected developer toolchain's " + library.string());
    struct Close {
      void *lib;
      ~Close() { dlclose(lib); }
    } close{lib};
    IndexAPI api(lib);
    auto index = api.createIndex(0, 0);
    struct Index {
      IndexAPI &api;
      CXIndex i;
      ~Index() { api.disposeIndex(i); }
    } indexGuard{api, index};
    std::string src = "#import <" + name + "/" + name + ".h>\n#import <objc/runtime.h>\n";
    const char *file = "kalandra-apple-import.m";
    CXUnsavedFile input{file, src.c_str(), src.size()};
    const char *args[] = {"-x",        "objective-c", "-isysroot",
                          sdk.c_str(), "-fblocks",    "-fno-modules"};
    CXTranslationUnit tu = nullptr;
    if (api.parseTranslationUnit2(index, file, args, 6, &input, 1,
                                  CXTranslationUnit_SkipFunctionBodies, &tu) ||
        !tu)
      return std::unexpected("cannot index the SDK header " + header.string());
    struct TU {
      IndexAPI &api;
      CXTranslationUnit tu;
      ~TU() { api.disposeTranslationUnit(tu); }
    } unit{api, tu};
    for (unsigned i = 0; i < api.getNumDiagnostics(tu); ++i) {
      auto diag = api.getDiagnostic(tu, i);
      auto severity = api.getDiagnosticSeverity(diag);
      auto message = api.str(api.getDiagnosticSpelling(diag));
      api.disposeDiagnostic(diag);
      if (severity >= CXDiagnostic_Error)
        return std::unexpected(header.string() + ": " + message);
    }
    Surface surface{api, symbols, {}, {}, {}, {}};
    for (auto c : surface.children(api.getTranslationUnitCursor(tu)))
      surface.inspect(c);
    std::string source;
    for (auto &symbol : symbols) {
      if (surface.conflicts.contains(symbol) ||
          (!surface.records.contains(symbol) && !surface.methods.contains(symbol)))
        return std::unexpected(name + ": no unambiguous supported SDK declaration for " + symbol);
    }
    for (auto &[key, value] : surface.records)
      source += value;
    for (auto &[key, value] : surface.methods)
      source += value;
    return source;
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
} // namespace kalandra
#endif
