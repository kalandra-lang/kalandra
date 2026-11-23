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
  INDEX_FN(isAttribute);
  INDEX_FN(getCursorLinkage);
  INDEX_FN(Cursor_isFunctionInlined);
  INDEX_FN(getFunctionTypeCallingConv);
  INDEX_FN(isConstQualifiedType);
  INDEX_FN(isVolatileQualifiedType);
  INDEX_FN(Type_getNullability);
  INDEX_FN(createIndex);
  INDEX_FN(disposeIndex);
  INDEX_FN(parseTranslationUnit2);
  INDEX_FN(saveTranslationUnit);
  INDEX_FN(disposeTranslationUnit);
  INDEX_FN(getTranslationUnitCursor);
  INDEX_FN(visitChildren);
  INDEX_FN(getCString);
  INDEX_FN(disposeString);
  INDEX_FN(getCursorSpelling);
  INDEX_FN(getCursorType);
  INDEX_FN(getCursorResultType);
  INDEX_FN(getCanonicalType);
  INDEX_FN(getTypeSpelling);
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
#undef INDEX_FN
  std::string str(CXString cx) {
    const char *p = getCString(cx);
    std::string result = p ? p : "";
    disposeString(cx);
    return result;
  }
};
struct Unit {
  IndexAPI &api;
  CXTranslationUnit tu = nullptr;
  ~Unit() {
    if (tu)
      api.disposeTranslationUnit(tu);
  }
  Unit(const Unit &) = delete;
  explicit Unit(IndexAPI &api) : api(api) {}
  std::string parse(CXIndex index, const std::vector<const char *> &args, const char *file,
                    const std::string *source = nullptr) {
    CXUnsavedFile overlay{file, source ? source->data() : nullptr, source ? source->size() : 0};
    unsigned flags = CXTranslationUnit_IncludeAttributedTypes;
    flags |= source ? CXTranslationUnit_SkipFunctionBodies : CXTranslationUnit_ForSerialization;
    if (api.parseTranslationUnit2(index, file, args.data(), args.size(),
                                  source ? &overlay : nullptr, source ? 1 : 0, flags, &tu) ||
        !tu)
      return "could not parse the native header contract";
    for (unsigned i = 0; i < api.getNumDiagnostics(tu); ++i) {
      auto diagnostic = api.getDiagnostic(tu, i);
      const auto severity = api.getDiagnosticSeverity(diagnostic);
      auto message = api.str(api.getDiagnosticSpelling(diagnostic));
      api.disposeDiagnostic(diagnostic);
      if (severity >= CXDiagnostic_Error)
        return message;
    }
    return {};
  }
};
struct NativeParameter {
  std::string cType, kType;
  bool buffer = false, writable = false;
};
std::string number(IndexAPI &api, CXType type) {
  switch (type.kind) {
  case CXType_Float:
    return "f32";
  case CXType_Double:
    return "f64";
  case CXType_Int:
  case CXType_Long:
  case CXType_LongLong:
  case CXType_UInt:
  case CXType_ULong:
  case CXType_ULongLong: {
    bool sign = type.kind == CXType_Int || type.kind == CXType_Long || type.kind == CXType_LongLong;
    auto size = api.Type_getSizeOf(type);
    if (size == 4 || size == 8)
      return std::string(sign ? "i" : "u") + std::to_string(size * 8);
    return {};
  }
  case CXType_Void:
    return "unit";
  default:
    return {};
  }
}
std::vector<CXCursor> children(IndexAPI &api, CXCursor cursor) {
  std::vector<CXCursor> out;
  api.visitChildren(
      cursor,
      [](CXCursor c, CXCursor, CXClientData data) {
        static_cast<std::vector<CXCursor> *>(data)->push_back(c);
        return CXChildVisit_Continue;
      },
      &out);
  return out;
}
std::expected<std::vector<int>, std::string>
countRelations(IndexAPI &api, CXIndex index, const std::vector<const char *> &args,
               const std::string &name, const std::vector<NativeParameter> &params) {
  std::vector<int> scalars, pointers, counts(params.size(), -1);
  for (unsigned i = 0; i < params.size(); ++i) {
    if (params[i].buffer)
      pointers.push_back(i);
    else if (params[i].kType == "u64" || params[i].kType == "u32")
      scalars.push_back(i);
  }
  if (scalars.empty())
    return std::unexpected(name + ": counted buffers require an unsigned count parameter");
  // Sema compares bounds using resolved parameter declarations, even when the
  // two prototypes use different names. A plain compatible function pointer
  // alone would *not* check the count relationship. Conversely, redeclaring
  // noescape alone can add the attribute; test substitution before redeclaration.
  size_t alternatives = 1;
  for (size_t i = 0; i < pointers.size(); ++i) {
    if (alternatives > 64 / scalars.size())
      return std::unexpected(name + ": too many distinct candidate count relationships");
    alternatives *= scalars.size();
  }
  std::optional<std::vector<int>> match;
  for (size_t alternative = 0; alternative < alternatives; ++alternative) {
    size_t indexValue = alternative;
    for (int pointer : pointers) {
      counts[pointer] = scalars[indexValue % scalars.size()];
      indexValue /= scalars.size();
    }
    std::string signature;
    for (unsigned i = 0; i < params.size(); ++i) {
      if (i)
        signature += ", ";
      signature += params[i].cType;
      if (params[i].buffer)
        signature += " _Nonnull __attribute__((counted_by(karg" + std::to_string(counts[i]) + ")))";
      signature += " karg" + std::to_string(i);
      if (params[i].buffer)
        signature += " __attribute__((noescape))";
    }
    // Protect the proof declaration from SDK convenience macros.
    std::string probe =
        "#undef __attribute__\n#undef _Nonnull\n#undef counted_by\n#undef noescape\n"
        "#pragma clang diagnostic error \"-Wincompatible-function-pointer-types\"\n"
        "#pragma clang diagnostic error \"-Wignored-attributes\"\n"
        "#pragma clang diagnostic error \"-Wunknown-attributes\"\n";
    probe += "static void (*const kalandra_contract_check)(" + signature + ") = &" + name +
             ";\nextern void " + name + "(" + signature + ");\n";
    Unit unit(api);
    if (!unit.parse(index, args, "kalandra_import_probe.c", &probe).empty())
      continue;
    if (match)
      return std::unexpected(name + ": ambiguous counted buffer relationship");
    match = counts;
  }
  if (match)
    return *match;
  return std::unexpected(
      name + ": cannot verify a direct counted_by parameter and noescape for every buffer");
}
} // namespace
std::expected<std::string, std::string> importAppleCHeader(const std::string &path,
                                                           const std::set<std::string> &wanted,
                                                           HeaderContracts &bounds,
                                                           bool framework) {
  namespace fs = std::filesystem;
  try {
    const auto sdk = xcrun("--show-sdk-path");
    const auto compiler = fs::path(xcrun("--find", "clang"));
    const auto library = compiler.parent_path().parent_path() / "lib/libclang.dylib";
    void *lib = dlopen(library.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!lib)
      return std::unexpected("cannot load libclang from " + library.string());
    struct Close {
      void *lib;
      ~Close() { dlclose(lib); }
    } close{lib};
    IndexAPI api(lib);
    CXIndex index = api.createIndex(0, 0);
    struct Index {
      IndexAPI &api;
      CXIndex index;
      ~Index() { api.disposeIndex(index); }
    } guard{api, index};
    std::vector<const char *> args{"-x",       framework ? "objective-c-header" : "c-header",
                                   "-std=c17", "-fblocks",
                                   "-Werror",  "-isysroot",
                                   sdk.c_str()};
    if (!framework)
      args.push_back("-fbounds-safety");
    Unit unit(api);
    auto error = unit.parse(index, args, path.c_str());
    llvm::SmallString<256> pch;
    llvm::FileRemover cleanup;
    if (!error.empty())
      return std::unexpected(path + ": " + error);
    std::map<std::string, CXCursor> functions;
    for (auto c : children(api, api.getTranslationUnitCursor(unit.tu))) {
      if (c.kind != CXCursor_FunctionDecl)
        continue;
      auto name = api.str(api.getCursorSpelling(c));
      if (wanted.contains(name))
        functions[name] = c;
    }
    std::string generated;
    for (const auto &name : wanted) {
      if (!functions.contains(name))
        return std::unexpected(path + ": no supported C declaration for " + name);
      auto c = functions[name];
      if (api.Cursor_isVariadic(c) || api.getCursorLinkage(c) != CXLinkage_External ||
          api.Cursor_isFunctionInlined(c) ||
          api.getFunctionTypeCallingConv(api.getCursorType(c)) != CXCallingConv_C)
        return std::unexpected(name + ": unsupported native linkage or calling convention");
      for (auto a : children(api, c))
        if (api.isAttribute(a.kind) && a.kind != CXCursor_VisibilityAttr)
          return std::unexpected(name + ": unsupported function attribute in native contract");
      std::vector<NativeParameter> params;
      bool buffers = false;
      const auto result = number(api, api.getCanonicalType(api.getCursorResultType(c)));
      if (result.empty())
        return std::unexpected(name + ": unsupported native result");
      for (int i = 0; i < api.Cursor_getNumArguments(c); ++i) {
        auto param = api.Cursor_getArgument(c, i);
        auto t = api.getCursorType(param);
        auto canon = api.getCanonicalType(t);
        NativeParameter p;
        if (canon.kind == CXType_Pointer) {
          auto elem = api.getCanonicalType(api.getPointeeType(canon));
          if (elem.kind != CXType_UChar ||
              api.Type_getNullability(t) != CXTypeNullability_NonNull ||
              api.isVolatileQualifiedType(elem))
            return std::unexpected(name + ": expected a non-null counted unsigned-byte pointer");
          p.buffer = true;
          buffers = true;
          p.writable = !api.isConstQualifiedType(elem);
          p.cType = p.writable ? "unsigned char *" : "const unsigned char *";
          p.kType = "ByteBuffer";
        } else {
          p.kType = number(api, canon);
          if (p.kType.empty() || p.kType == "unit")
            return std::unexpected(name + ": unsupported C parameter");
          p.cType = api.str(api.getTypeSpelling(canon));
        }
        // With implicit attributes excluded, a borrowed pointer has one
        // parameter attribute: noescape, independently required by Sema below.
        // In particular, never treat cf_consumed + noescape as a borrow.
        const auto attributes = std::ranges::count_if(
            children(api, param), [&](CXCursor child) { return api.isAttribute(child.kind); });
        if (attributes != (p.buffer ? 1 : 0))
          return std::unexpected(name + ": unsupported or missing parameter ownership attribute");
        params.push_back(std::move(p));
      }
      if (api.getCursorType(c).kind != CXType_FunctionProto)
        return std::unexpected(name + ": prototype required");
      if (buffers) {
        if (framework)
          return std::unexpected(name + ": a checked C buffer contract is required");
        if (result != "unit" || std::ranges::any_of(params, [](const auto &p) {
              return !p.buffer && p.kType != "u32" && p.kType != "u64";
            }))
          return std::unexpected(
              name + ": counted calls currently accept unsigned counts and return void");
        // Check every proposed contract against this exact AST, including its
        // transitive headers and macros. Never reopen and reinterpret the header.
        if (pch.empty()) {
          if (auto ec = llvm::sys::fs::createTemporaryFile("kalandra-header", "pch", pch))
            return std::unexpected(ec.message());
          cleanup.setFile(pch);
          if (api.saveTranslationUnit(unit.tu, pch.c_str(), 0) != CXSaveError_None)
            return std::unexpected(name + ": could not serialize header for contract verification");
          args[1] = "c";
          args.insert(args.end(), {"-include-pch", pch.c_str()});
        }
        auto counts = countRelations(api, index, args, name, params);
        if (!counts)
          return std::unexpected(counts.error());
        bounds[name].bufferCounts = std::move(*counts);
      }
      generated += "export extern \"C\" fn " + name + "(";
      for (size_t i = 0; i < params.size(); ++i) {
        if (i)
          generated += ", ";
        if (params[i].writable)
          generated += "mut ";
        generated += "arg" + std::to_string(i) + ": " + params[i].kType;
      }
      generated += ") -> " + result + (buffers ? " ! BufferError" : "") + "\n";
    }
    if (!bounds.empty())
      generated = "from \"std/buffer\" import ByteBuffer, BufferError\n" + generated;
    return generated;
  } catch (const std::exception &e) {
    return std::unexpected(e.what());
  }
}
std::expected<std::optional<std::string>, std::string>
importAppleFramework(const std::string &name, const std::set<std::string> &symbols,
                     HeaderContracts &bounds) {
  try {
    const auto header = std::filesystem::path(xcrun("--show-sdk-path")) /
                        "System/Library/Frameworks" / (name + ".framework") / "Headers" /
                        (name + ".h");
    if (!std::filesystem::is_regular_file(header))
      return std::optional<std::string>{};
    auto source = importAppleCHeader(header.string(), symbols, bounds, true);
    if (!source)
      return std::unexpected(source.error());
    return *source;
  } catch (const std::exception &error) {
    return std::unexpected(error.what());
  }
}
} // namespace kalandra
#endif
