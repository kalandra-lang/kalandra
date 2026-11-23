#include "PortableHeaderImport.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Expr.h>
#include <clang/Basic/Builtins.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <map>

namespace kalandra {
namespace {
class HeaderDiagnostics : public clang::DiagnosticConsumer {
public:
  std::string error;
  void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                        const clang::Diagnostic &diag) override {
    clang::DiagnosticConsumer::HandleDiagnostic(level, diag);
    if (level >= clang::DiagnosticsEngine::Error && error.empty()) {
      llvm::SmallString<128> text;
      diag.FormatDiagnostic(text);
      error = text.str().str();
    }
  }
};

std::string scalar(clang::ASTContext &ctx, clang::QualType t) {
  const auto *builtin = t->getAs<clang::BuiltinType>();
  if (!builtin)
    return {};
  using K = clang::BuiltinType;
  switch (builtin->getKind()) {
  case K::Void:
    return "unit";
  case K::Float:
    return "f32";
  case K::Double:
    return "f64";
  case K::Int:
  case K::Long:
  case K::LongLong:
  case K::UInt:
  case K::ULong:
  case K::ULongLong: {
    const auto width = ctx.getTypeSize(t);
    if (width != 32 && width != 64)
      return {};
    return std::string(t->isUnsignedIntegerType() ? "u" : "i") + std::to_string(width);
  }
  default:
    return {};
  }
}

// Only direct references to this function's parameters carry a byte extent.
// Inherited attributes may still point into an earlier declaration of the same function.
const clang::ParmVarDecl *parameter(const clang::Expr *expr, const clang::FunctionDecl &fn) {
  const auto *ref = llvm::dyn_cast<clang::DeclRefExpr>(expr->IgnoreParenImpCasts());
  const auto *p = ref ? llvm::dyn_cast<clang::ParmVarDecl>(ref->getDecl()) : nullptr;
  const auto *owner = p ? llvm::dyn_cast<clang::FunctionDecl>(p->getDeclContext()) : nullptr;
  if (!owner || owner->getCanonicalDecl() != fn.getCanonicalDecl() ||
      p->getFunctionScopeIndex() >= fn.getNumParams())
    return nullptr;
  return fn.getParamDecl(p->getFunctionScopeIndex());
}

bool recordBound(const clang::DiagnoseIfAttr &attr, const clang::FunctionDecl &fn,
                 std::vector<int> &counts) {
  if (attr.getDefaultSeverity() != clang::DiagnoseIfAttr::DS_error)
    return false;
  const auto *test = llvm::dyn_cast<clang::BinaryOperator>(attr.getCond()->IgnoreParenImpCasts());
  if (!test || test->getOpcode() != clang::BO_GT)
    return false;
  const auto *count = parameter(test->getLHS(), fn);
  const auto *call = llvm::dyn_cast<clang::CallExpr>(test->getRHS()->IgnoreParenImpCasts());
  if (!count || !count->getType()->isUnsignedIntegerType() || !call ||
      call->getBuiltinCallee() != clang::Builtin::BI__builtin_object_size ||
      call->getNumArgs() != 2)
    return false;
  const auto *ptr = parameter(call->getArg(0), fn);
  const auto *mode = llvm::dyn_cast<clang::IntegerLiteral>(call->getArg(1)->IgnoreParenImpCasts());
  if (!ptr || !ptr->getType()->isPointerType() || !mode || !mode->getValue().isZero())
    return false;
  auto &slot = counts[ptr->getFunctionScopeIndex()];
  const int index = int(count->getFunctionScopeIndex());
  if (slot != -1 && slot != index)
    return false;
  slot = index;
  return true;
}

// A direct application callback has process-lifetime code and no captured state. Its ABI uses the same
// scalar subset as direct imports. Reject adjusted or ABI-annotated parameters.
std::optional<std::string> callbackType(clang::ASTContext &ctx, const clang::ParmVarDecl &arg,
                                        unsigned hidden = 0) {
  auto t = arg.getType();
  const auto *p = t->getAs<clang::PointerType>();
  const auto *block = t->getAs<clang::BlockPointerType>();
  if ((!p && !block) || t->getNullability() != clang::NullabilityKind::NonNull)
    return {};
  auto pointee = p ? p->getPointeeType() : block->getPointeeType();
  if (pointee.hasAddressSpace())
    return {};
  for (const auto *attr : arg.attrs())
    if (!llvm::isa<clang::NoEscapeAttr>(attr))
      return {};
  auto *fn = pointee->getAs<clang::FunctionProtoType>();
  if (!fn || fn->isVariadic() || fn->getCallConv() != clang::CC_C || fn->hasExtParameterInfos() ||
      fn->getExtInfo() != clang::FunctionType::ExtInfo())
    return {};
  auto result = scalar(ctx, fn->getReturnType());
  if (result.empty())
    return {};
  std::string spelling = "fn(";
  unsigned index = 0;
  if (hidden > fn->getNumParams())
    return {};
  for (auto param : fn->param_types().drop_back(hidden)) {
    auto s = scalar(ctx, param);
    if (s.empty() || s == "unit")
      return {};
    if (index++ != 0)
      spelling += ", ";
    spelling += s;
  }
  return spelling + ")->" + result;
}

// An adapter opts into scope cleanup with paired native handle annotations.
const clang::AcquireHandleAttr *acquisition(const clang::FunctionDecl &fn) {
  auto t = fn.getType();
  while (!t.isCanonical()) {
    if (auto *a = llvm::dyn_cast<clang::AttributedType>(t.getTypePtr()))
      if (auto *owned = llvm::dyn_cast_or_null<clang::AcquireHandleAttr>(a->getAttr()))
        return owned;
    auto next = t.getSingleStepDesugaredType(fn.getASTContext());
    if (next == t)
      break;
    t = next;
  }
  return nullptr;
}
// Handle families are chosen by the adapter author. Each distinct family
// is a nominal Kalandra resource, local to this imported module.
std::string resourceName(llvm::StringRef family) {
  std::string result;
  bool start = true;
  for (char ch : family) {
    if (ch == '_') {
      start = true;
      continue;
    }
    if (!llvm::isAlnum(ch) || (result.empty() && !llvm::isAlpha(ch)))
      return {};
    result += start ? llvm::toUpper(ch) : ch;
    start = false;
  }
  return result;
}
const clang::ReleaseHandleAttr *subscriptionRelease(const clang::FunctionDecl &fn) {
  if (fn.getNumParams() != 1 || !fn.getReturnType()->isVoidType() || fn.isVariadic() ||
      fn.isInlined() || !fn.isExternallyVisible() ||
      scalar(fn.getASTContext(), fn.getParamDecl(0)->getType()) != "u64")
    return nullptr;
  // Ownership does not imply that release is valid on a cleanup worker.
  // Accept an explicit nonisolated declaration, including SDK macro expansion.
  bool nonisolated = false;
  for (const auto *attr : fn.attrs()) {
    if (llvm::isa<clang::VisibilityAttr>(attr))
      continue;
    const auto *swift = llvm::dyn_cast<clang::SwiftAttrAttr>(attr);
    if (!swift || swift->getAttribute() != "nonisolated")
      return nullptr;
    nonisolated = true;
  }
  if (!nonisolated)
    return nullptr;
  auto *p = fn.getParamDecl(0);
  const auto *release = p->getAttr<clang::ReleaseHandleAttr>();
  auto *proto = fn.getType()->getAs<clang::FunctionProtoType>();
  return proto && proto->getCallConv() == clang::CC_C &&
                 proto->getExtInfo() == clang::FunctionType::ExtInfo() &&
                 std::distance(p->attr_begin(), p->attr_end()) == 1 && release &&
                 !resourceName(release->getHandleType()).empty()
             ? release
             : nullptr;
}

std::expected<std::string, std::string> declaration(const clang::FunctionDecl &fn,
                                                    HeaderContracts &bounds) {
  const auto name = fn.getNameAsString();
  auto reject = [&](const char *reason) { return std::unexpected(name + ": " + reason); };
  // acquire_handle on a return can decorate the function *type*, so fn.attrs()
  // alone would publish an owned integer handle as an ordinary integer.
  if (const auto *release = subscriptionRelease(fn)) {
    bounds[name].subscriptionFamily = release->getHandleType().str();
    bounds[name].subscriptionRelease = name;
    return "export extern \"C\" fn " + name +
           "(own subscription: " + resourceName(release->getHandleType()) + ") -> unit\n";
  }
  const auto *acquire = acquisition(fn);
  auto declaredType = fn.getType();
  while (!declaredType.isCanonical()) {
    if (const auto *attr = llvm::dyn_cast<clang::AttributedType>(declaredType.getTypePtr());
        attr &&
        (!acquire || resourceName(acquire->getHandleType()).empty() || attr->getAttr() != acquire))
      return reject("unsupported attributed native function type");
    auto next = declaredType.getSingleStepDesugaredType(fn.getASTContext());
    if (next == declaredType)
      break;
    declaredType = next;
  }
  const auto *proto = fn.getType()->getAs<clang::FunctionProtoType>();
  if (!proto || !fn.isExternallyVisible() || fn.isInlined() || fn.isVariadic() ||
      proto->getCallConv() != clang::CC_C)
    return reject("unsupported C linkage or prototype");
  std::vector<int> counts(fn.getNumParams(), -1);
  bool borrowed = false;
  bool mainThreadOnly = false;
  for (const auto *attr : fn.attrs()) {
    if (llvm::isa<clang::VisibilityAttr>(attr))
      continue;
    if (const auto *isolation = llvm::dyn_cast<clang::SwiftAttrAttr>(attr);
        isolation && isolation->getAttribute() == "@MainActor") {
      mainThreadOnly = true;
      continue;
    }
    if (const auto *broker = llvm::dyn_cast<clang::CallbackAttr>(attr)) {
      // A portable adapter receives one native function and the values it will
      // pass through to the trailing arguments of each delivery, all by value.
      auto encoding = llvm::ArrayRef(broker->encoding_begin(), broker->encoding_size());
      if (!acquire || fn.getNumParams() < 2 || encoding.empty() || encoding.front() != 0 ||
          !fn.getParamDecl(0)->getType()->isFunctionPointerType() || bounds.contains(name))
        return reject("unsupported callback forwarding contract");
      auto *signature =
          fn.getParamDecl(0)->getType()->getPointeeType()->getAs<clang::FunctionProtoType>();
      const auto captures = fn.getNumParams() - 1;
      if (!signature || signature->getNumParams() + 1 != encoding.size() ||
          signature->getNumParams() < captures)
        return reject("callback forwarding arity mismatch");
      const auto explicitCount = signature->getNumParams() - captures;
      for (unsigned i = 0; i < signature->getNumParams(); ++i) {
        if (i < explicitCount) {
          if (encoding[i + 1] != -1)
            return reject("callback delivery arguments must come from the adapter");
          continue;
        }
        const auto index = i - explicitCount + 1;
        auto *param = fn.getParamDecl(index);
        auto type = scalar(fn.getASTContext(), param->getType());
        if (encoding[i + 1] != int(index) || param->hasAttrs() || type.empty() || type == "unit" ||
            !fn.getASTContext().hasSameUnqualifiedType(param->getType(),
                                                       signature->getParamType(i)))
          return reject("callback capture needs a matching scalar passed by value");
        bounds[name].forwardedCaptures.push_back(type);
      }
      bounds[name].blockParameter = 0;
      continue;
    }
    const auto *extent = llvm::dyn_cast<clang::DiagnoseIfAttr>(attr);
    if (!extent || !recordBound(*extent, fn, counts))
      return reject("unsupported native function contract");
    borrowed = true;
  }
  auto &ctx = fn.getASTContext();
  auto result = scalar(ctx, fn.getReturnType());
  if (result.empty() || (borrowed && result != "unit"))
    return reject("unsupported native result");
  bool callbacks = false;
  std::string source = "export extern \"C\" fn " + name + "(";
  unsigned hidden = bounds.contains(name) ? bounds.at(name).forwardedCaptures.size() : 0;
  for (unsigned i = 0; i < fn.getNumParams() - hidden; ++i) {
    const auto *arg = fn.getParamDecl(i);
    auto t = arg->getType();
    std::string type;
    if (i)
      source += ", ";
    if (t->isFunctionPointerType() || t->isBlockPointerType()) {
      auto cb = callbackType(ctx, *arg, hidden);
      if (!cb || borrowed)
        return reject("unsupported native callback signature or lifetime");
      if (t->isBlockPointerType()) {
        if (callbacks)
          return reject("a block registration requires one callback");
        bounds[name].blockParameter = i;
      } else if (!hidden && bounds.contains(name) && bounds.at(name).blockParameter) {
        return reject("a block registration requires one callback");
      }
      type = *cb;
      callbacks = true;
    } else if (const auto *ptr = t->getAs<clang::PointerType>()) {
      auto elem = ptr->getPointeeType().getCanonicalType();
      auto byte = elem.isConstQualified() ? ctx.UnsignedCharTy.withConst() : ctx.UnsignedCharTy;
      if (!ctx.hasSameUnqualifiedType(t, ctx.getPointerType(byte)) || counts[i] < 0 ||
          t->getNullability() != clang::NullabilityKind::NonNull ||
          !elem->isSpecificBuiltinType(clang::BuiltinType::UChar) || elem.isVolatileQualified() ||
          elem.hasAddressSpace() || !arg->hasAttr<clang::NoEscapeAttr>())
        return reject("byte pointers require _Nonnull, noescape and a checked byte extent");
      for (const auto *attr : arg->attrs())
        if (!llvm::isa<clang::NoEscapeAttr>(attr))
          return reject("unsupported parameter ownership or ABI");
      if (!elem.isConstQualified())
        source += "mut ";
      type = "ByteBuffer";
    } else {
      type = scalar(ctx, t);
      if (arg->hasAttrs() || type.empty() || type == "unit" ||
          (borrowed && type != "u32" && type != "u64"))
        return reject("unsupported C parameter");
    }
    source += "arg" + std::to_string(i) + ": " + type;
  }
  if (acquire) {
    if (result != "u64" || !bounds.contains(name) || !bounds.at(name).blockParameter ||
        mainThreadOnly)
      return reject("subscription acquisition requires a block and an unsigned 64-bit token");
    std::string close;
    for (auto *decl : ctx.getTranslationUnitDecl()->decls()) {
      auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl);
      auto *release = candidate ? subscriptionRelease(*candidate) : nullptr;
      if (!release || release->getHandleType() != acquire->getHandleType())
        continue;
      auto next = candidate->getNameAsString();
      if (!close.empty() && next != close)
        return reject("ambiguous subscription release operation");
      close = next;
    }
    if (close.empty())
      return reject("missing subscription release operation");
    bounds[name].subscriptionRelease = close;
    bounds[name].subscriptionFamily = acquire->getHandleType().str();
    result = resourceName(acquire->getHandleType());
  }
  if (mainThreadOnly)
    bounds[name].mainThreadOnly = true;
  if (borrowed)
    bounds[name].bufferCounts = std::move(counts);
  if (callbacks)
    bounds[name].hasCallbacks = true;
  return source + ") -> " + result + (borrowed ? " ! BufferError\n" : "\n");
}
} // namespace

std::expected<std::string, std::string>
importPortableCHeader(const std::string &path, const std::string &driver,
                      const std::optional<std::filesystem::path> &resources,
                      const std::set<std::string> &symbols, HeaderContracts &bounds, const HeaderTarget *target) {
  std::vector<std::string> args{"-x",
                                "c",
                                "-std=c17",
                                "-fblocks",
                                "-Werror=unknown-attributes",
                                "-Werror=ignored-attributes",
                                "-include",
                                path};
  if (resources)
    args.insert(args.end(), {"-resource-dir", resources->string()});
  if (target) {
    args.insert(args.end(), {"--no-default-config", "--target=" + target->triple,
                             "--sysroot=" + target->sysroot.string(), "-nostdinc"});
    for (const auto &dir : target->includes)
      args.insert(args.end(), {"-isystem", dir.string()});
  }
  HeaderDiagnostics diagnostics;
  auto ast = clang::tooling::buildASTFromCodeWithArgs(
      "", args, "kalandra-import.c", driver, std::make_shared<clang::PCHContainerOperations>(),
      clang::tooling::getClangStripDependencyFileAdjuster(), {}, &diagnostics);
  if (!ast || !diagnostics.error.empty() || ast->getDiagnostics().hasErrorOccurred())
    return std::unexpected(
        path + ": " +
        (diagnostics.error.empty() ? "cannot read native header" : diagnostics.error));
  auto &ctx = ast->getASTContext();
  HeaderContracts verified;
  std::string source;
  std::map<std::string, std::string> families;
  std::set<std::string> released;
  for (auto *decl : ctx.getTranslationUnitDecl()->decls()) {
    auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl);
    if (!candidate)
      continue;
    llvm::StringRef family;
    if (auto *release = subscriptionRelease(*candidate)) {
      family = release->getHandleType();
      released.insert(family.str());
    } else if (auto *acquire = acquisition(*candidate))
      family = acquire->getHandleType();
    auto key = resourceName(family);
    if (key.empty())
      continue;
    auto [entry, inserted] = families.emplace(key, family.str());
    if (!inserted && entry->second != family)
      return std::unexpected("ambiguous subscription family name: " + key);
  }
  for (const auto &name : symbols) {
    if (families.contains(name)) {
      if (!released.contains(families.at(name)))
        return std::unexpected("missing subscription release operation for " + name);
      verified[name].subscriptionFamily = families.at(name);
      continue;
    }
    const clang::FunctionDecl *fn = nullptr;
    for (auto *decl : ctx.getTranslationUnitDecl()->lookup(&ctx.Idents.get(name))) {
      const auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (!candidate || (fn && fn->getCanonicalDecl() != candidate->getCanonicalDecl()))
        return std::unexpected(path + ": ambiguous native declaration for " + name);
      fn = candidate->getMostRecentDecl();
    }
    if (!fn || fn->isImplicit())
      return std::unexpected(path + ": missing C prototype for " + name);
    auto line = declaration(*fn, verified);
    if (!line)
      return std::unexpected(line.error());
    source += *line;
  }
  if (std::ranges::any_of(verified,
                          [](const auto &item) { return !item.second.bufferCounts.empty(); }))
    source = "from \"std/buffer\" import ByteBuffer, BufferError\n" + source;
  for (const auto &[type, family] : families)
    if (std::ranges::any_of(verified, [&](const auto &entry) {
          return entry.second.subscriptionFamily == family;
        })) {
      source = "export type " + type + ":\n  __storage: [u8]\n" + source;
      verified[type].subscriptionFamily = family;
    }
  bounds = std::move(verified);
  return source;
}
} // namespace kalandra
