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

// A borrowed callback carries executable code only. Its ABI uses the same
// scalar subset as direct imports. Reject adjusted or ABI-annotated parameters.
std::optional<std::string> callbackType(clang::ASTContext &ctx, const clang::ParmVarDecl &arg) {
  auto t = arg.getType();
  const auto *p = t->getAs<clang::PointerType>();
  if (!p || t->getNullability() != clang::NullabilityKind::NonNull ||
      !arg.hasAttr<clang::NoEscapeAttr>() || p->getPointeeType().hasAddressSpace())
    return {};
  for (const auto *attr : arg.attrs())
    if (!llvm::isa<clang::NoEscapeAttr>(attr))
      return {};
  auto *fn = p->getPointeeType()->getAs<clang::FunctionProtoType>();
  if (!fn || fn->isVariadic() || fn->getCallConv() != clang::CC_C || fn->hasExtParameterInfos() ||
      fn->getExtInfo() != clang::FunctionType::ExtInfo())
    return {};
  auto result = scalar(ctx, fn->getReturnType());
  if (result.empty())
    return {};
  std::string spelling = "fn(";
  unsigned index = 0;
  for (auto param : fn->param_types()) {
    auto s = scalar(ctx, param);
    if (s.empty() || s == "unit")
      return {};
    if (index++ != 0)
      spelling += ", ";
    spelling += s;
  }
  return spelling + ")->" + result;
}

std::expected<std::string, std::string> declaration(const clang::FunctionDecl &fn,
                                                    HeaderContracts &bounds) {
  const auto name = fn.getNameAsString();
  auto reject = [&](const char *reason) { return std::unexpected(name + ": " + reason); };
  // acquire_handle on a return can decorate the function *type*, so fn.attrs()
  // alone would publish an owned integer handle as an ordinary integer.
  auto declaredType = fn.getType();
  while (!declaredType.isCanonical()) {
    if (llvm::isa<clang::AttributedType>(declaredType.getTypePtr()))
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
  for (const auto *attr : fn.attrs()) {
    if (llvm::isa<clang::VisibilityAttr>(attr))
      continue;
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
  for (unsigned i = 0; i < fn.getNumParams(); ++i) {
    const auto *arg = fn.getParamDecl(i);
    auto t = arg->getType();
    std::string type;
    if (i)
      source += ", ";
    if (t->isFunctionPointerType()) {
      auto cb = callbackType(ctx, *arg);
      if (!cb || borrowed)
        return reject("unsupported native callback signature or lifetime");
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
                      const std::set<std::string> &symbols, HeaderContracts &bounds) {
  std::vector<std::string> args{
      "-x",       "c", "-std=c17", "-Werror=unknown-attributes", "-Werror=ignored-attributes",
      "-include", path};
  if (resources)
    args.insert(args.end(), {"-resource-dir", resources->string()});
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
  for (const auto &name : symbols) {
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
  bounds = std::move(verified);
  return source;
}
} // namespace kalandra
