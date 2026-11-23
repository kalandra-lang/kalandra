#include "PortableHeaderImport.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecordLayout.h>
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

// Widen the private ABI for narrow scalar calls. Clang then supplies the
// extension required by the destination C calling convention.
std::string narrowScalar(clang::ASTContext &ctx, clang::QualType type) {
  if (type->isBooleanType())
    return "bool";
  if (!type->isIntegerType() || type->isEnumeralType() || !type->isBuiltinType())
    return {};
  const auto bits = ctx.getTypeSize(type);
  if (bits != 8 && bits != 16)
    return {};
  return std::string(type->isSignedIntegerType() ? "i" : "u") + std::to_string(bits);
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
// Native pointer handles are references to named, incomplete C structs. No
// pointee layout or address is ever exposed to the Kalandra program.
bool opaquePointer(clang::QualType t, bool allowConst = false) {
  const auto *ptr = t->getAs<clang::PointerType>();
  if (!ptr || t->getNullability() != clang::NullabilityKind::NonNull)
    return false;
  auto target = ptr->getPointeeType();
  const auto *record = target->getAsRecordDecl();
  return record && record->isStruct() && record->getIdentifier() && !record->getDefinition() &&
         (!target.hasQualifiers() ||
          (allowConst &&
           target.getQualifiers() == clang::Qualifiers::fromCVRMask(clang::Qualifiers::Const))) &&
         !target.hasAddressSpace();
}
bool handleType(clang::ASTContext &ctx, clang::QualType t) {
  return scalar(ctx, t) == "u64" || opaquePointer(t);
}
const clang::ReleaseHandleAttr *subscriptionRelease(const clang::FunctionDecl &fn) {
  if (fn.getNumParams() != 1 || !fn.getReturnType()->isVoidType() || fn.isVariadic() ||
      fn.isInlined() || !fn.isExternallyVisible() ||
      !handleType(fn.getASTContext(), fn.getParamDecl(0)->getType()))
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

// Verify both the nominal family and the target C type before borrowing or
// transferring a handle. A shared family spelling cannot coerce pointer types.
std::expected<std::string, std::string> disposer(clang::ASTContext &ctx, llvm::StringRef family,
                                                 clang::QualType type) {
  std::string name;
  for (auto *decl : ctx.getTranslationUnitDecl()->decls()) {
    const auto *fn = llvm::dyn_cast<clang::FunctionDecl>(decl);
    const auto *release = fn ? subscriptionRelease(*fn) : nullptr;
    if (!release || release->getHandleType() != family)
      continue;
    if (!ctx.hasSameUnqualifiedType(type, fn->getParamDecl(0)->getType()) ||
        (!name.empty() && name != fn->getNameAsString()))
      return std::unexpected("incompatible or ambiguous native handle release");
    name = fn->getNameAsString();
  }
  if (name.empty())
    return std::unexpected("missing subscription release operation");
  return name;
}
using NativeRecords = std::map<std::string, std::string>;
std::expected<std::string, std::string>
recordArgument(const clang::RecordDecl &decl, NativeRecords &records, NativeParameterLayout &abi) {
  const auto *record = decl.getDefinition();
  if (!record || !record->isStruct() || !record->getIdentifier() || record->field_empty() ||
      record->hasAttrs() || record->hasFlexibleArrayMember())
    return std::unexpected("expected a named, fixed-layout C struct");
  auto &ctx = record->getASTContext();
  const auto &layout = ctx.getASTRecordLayout(record);
  abi.abi = NativeParameterABI::Record;
  abi.bytes = layout.getSize().getQuantity();
  abi.alignment = layout.getAlignment().getQuantity();
  std::string name = record->getNameAsString(), source = "export type " + name + ":\n";
  for (const auto *field : record->fields()) {
    auto type = scalar(ctx, field->getType());
    if (type.empty())
      type = narrowScalar(ctx, field->getType());
    if (!field->getIdentifier() || field->hasAttrs() || field->isBitField() ||
        field->getType().hasQualifiers() || type.empty() || type == "unit")
      return std::unexpected("unsupported native struct field in " + name);
    abi.offsets.push_back(layout.getFieldOffset(field->getFieldIndex()) / ctx.getCharWidth());
    source += "  " + field->getNameAsString() + ": " + type + "\n";
  }
  records.emplace(name, std::move(source));
  return name;
}

std::expected<std::string, std::string>
declaration(const clang::FunctionDecl &fn, HeaderContracts &bounds, NativeRecords &records) {
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
  std::vector<NativeParameterLayout> native(fn.getNumParams());
  bool adapted = false;
  bool byteBuffers = false;
  for (unsigned i = 0; i < counts.size(); ++i) {
    if (counts[i] < 0)
      continue;
    auto *arg = fn.getParamDecl(i);
    auto t = arg->getType();
    if (!ctx.hasSameType(t->getPointeeType().getCanonicalType(), ctx.CharTy.withConst())) {
      byteBuffers = true;
      continue;
    }
    auto count = unsigned(counts[i]);
    auto size = scalar(ctx, fn.getParamDecl(count)->getType());
    if ((size != "u32" && size != "u64") ||
        t->getNullability() != clang::NullabilityKind::NonNull ||
        !arg->hasAttr<clang::NoEscapeAttr>() ||
        std::distance(arg->attr_begin(), arg->attr_end()) != 1 ||
        fn.getParamDecl(count)->hasAttrs() || std::ranges::count(counts, counts[i]) != 1)
      return reject(
          "text requires a dedicated unsigned byte count and a nonescaping non-null pointer");
    native[i].abi = NativeParameterABI::Text;
    native[count].abi = NativeParameterABI::TextLength;
    native[count].argument = i;
    native[count].width = size == "u32" ? 32 : 64;
    adapted = true;
  }
  if (byteBuffers && adapted)
    return reject("mixed text and ByteBuffer contracts are unsupported");
  borrowed = byteBuffers;
  auto result = scalar(ctx, fn.getReturnType());
  bool bridge = false;
  if (result.empty()) {
    auto narrow = narrowScalar(ctx, fn.getReturnType());
    if (!narrow.empty()) {
      result = narrow;
      bridge = bounds[name].nativeABI.narrowResult = true;
    }
  }
  std::vector<bool> indirect(fn.getNumParams());
  if (const auto *record = fn.getReturnType()->getAsRecordDecl()) {
    NativeParameterLayout layout;
    auto type = recordArgument(*record, records, layout);
    if (!type)
      return reject(type.error().c_str());
    result = *type;
    bounds[name].nativeABI.recordResult = std::move(layout);
    bridge = true;
  }
  if ((result.empty() && !(acquire && opaquePointer(fn.getReturnType()))) ||
      (borrowed && result != "unit"))
    return reject("unsupported native result");
  bool callbacks = false;
  std::string source = "export extern \"C\" fn " + name + "(";
  unsigned hidden = bounds.contains(name) ? bounds.at(name).forwardedCaptures.size() : 0;
  unsigned argument = 0;
  for (unsigned i = 0; i < fn.getNumParams() - hidden; ++i) {
    if (native[i].abi == NativeParameterABI::TextLength)
      continue;
    native[i].argument = argument++;
    const auto *arg = fn.getParamDecl(i);
    auto t = arg->getType();
    std::string type;
    if (native[i].argument)
      source += ", ";
    if (native[i].abi == NativeParameterABI::Text) {
      type = "String";
    } else if (const auto *array = ctx.getAsConstantArrayType(arg->getOriginalType());
               array && array->getSizeModifier() == clang::ArraySizeModifier::Static &&
               array->getSize() == 1 && array->getElementType().isConstQualified() &&
               array->getElementType()->getAsRecordDecl()) {
      if (borrowed || !arg->hasAttr<clang::NoEscapeAttr>() ||
          std::distance(arg->attr_begin(), arg->attr_end()) != 1 ||
          array->getElementType().isVolatileQualified())
        return reject("record requires a nonescaping read-only single-element borrow");
      auto name = recordArgument(*array->getElementType()->getAsRecordDecl(), records, native[i]);
      if (!name)
        return reject(name.error().c_str());
      type = *name;
      adapted = true;
    } else if (const auto *record = t->getAsRecordDecl()) {
      if (arg->hasAttrs())
        return reject("unsupported native record parameter contract");
      auto imported = recordArgument(*record, records, native[i]);
      if (!imported)
        return reject(imported.error().c_str());
      type = *imported;
      indirect[i] = true;
      adapted = bridge = true;
    } else if (auto *use = arg->getAttr<clang::UseHandleAttr>()) {
      const bool pointer = opaquePointer(t, true);
      const bool readOnly = pointer && t->getPointeeType().isConstQualified();
      if ((!pointer && scalar(ctx, t) != "u64") || resourceName(use->getHandleType()).empty() ||
          (pointer && !arg->hasAttr<clang::NoEscapeAttr>()) || borrowed ||
          std::ranges::any_of(arg->attrs(), [](auto *a) {
            return !llvm::isa<clang::UseHandleAttr, clang::NoEscapeAttr>(a);
          }))
        return reject("unsupported native handle borrow");
      auto close =
          disposer(ctx, use->getHandleType(),
                   readOnly ? ctx.getPointerType(t->getPointeeType().getUnqualifiedType()) : t);
      if (!close)
        return std::unexpected(name + ": " + close.error());
      adapted = true;
      native[i].abi = readOnly  ? NativeParameterABI::ReadPointer
                      : pointer ? NativeParameterABI::Pointer
                                : NativeParameterABI::Token;
      type = resourceName(use->getHandleType());
      bounds[type].subscriptionFamily = use->getHandleType().str();
    } else if (t->isFunctionPointerType() || t->isBlockPointerType()) {
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
      if (type.empty()) {
        type = narrowScalar(ctx, t);
        if (!type.empty()) {
          native[i].abi = t->isSignedIntegerType() ? NativeParameterABI::SignedNarrow
                                                   : NativeParameterABI::UnsignedNarrow;
          adapted = bridge = true;
        }
      }
      if (arg->hasAttrs() || type.empty() || type == "unit" ||
          (borrowed && type != "u32" && type != "u64"))
        return reject("unsupported C parameter");
    }
    source += "arg" + std::to_string(i) + ": " + type;
  }
  if (adapted) {
    if (borrowed || callbacks)
      return reject("unsupported combination of native call contracts");
    for (auto &param : native)
      if (param.abi == NativeParameterABI::TextLength)
        param.argument = native[param.argument].argument;
    bounds[name].nativeABI.parameters = std::move(native);
  }
  if (bridge) {
    if (callbacks || borrowed)
      return reject("unsupported native record call contract");
    const auto symbol = "__kalandra_native_call_" + name;
    auto &contract = bounds[name];
    contract.nativeABI.bridge = symbol;
    // Only Clang classifies a struct passed or returned by value. The compiler
    // and bridge share a private pointer ABI, independent of the target's
    // register classification, hidden result pointers and stack arguments.
    const bool out = contract.nativeABI.recordResult.has_value();
    const auto cResult =
        contract.nativeABI.narrowResult
            ? (fn.getReturnType()->isSignedIntegerType() ? std::string("int")
                                                         : std::string("unsigned int"))
            : fn.getReturnType().getUnqualifiedType().getAsString();
    std::string params = out ? ("struct " + result + " *output") : "";
    std::string args;
    for (unsigned i = 0; i < fn.getNumParams(); ++i) {
      if (!params.empty())
        params += ", ";
      if (i)
        args += ", ";
      auto pname = "p" + std::to_string(i);
      auto t = fn.getParamDecl(i)->getType().getCanonicalType().getUnqualifiedType();
      // Array parameters decay at the C function boundary.
      if (t->isArrayType())
        t = ctx.getArrayDecayedType(t);
      if (!narrowScalar(ctx, t).empty())
        params += (t->isSignedIntegerType() ? "int " : "unsigned int ") + pname;
      else
        params +=
            (indirect[i] ? "const " : "") + t.getAsString() + (indirect[i] ? " *" : " ") + pname;
      args += (indirect[i] ? "*" : "") + pname;
    }
    if (params.empty())
      params = "void";
    contract.bridgeDefinition = (out ? "void" : cResult) + " " + symbol + "(" + params + ") { " +
                                (out                ? "*output = "
                                 : result == "unit" ? ""
                                                    : "return ") +
                                name + "(" + args + "); }\n";
  }
  if (acquire) {
    bool pointer = opaquePointer(fn.getReturnType());
    if (!handleType(ctx, fn.getReturnType()) || borrowed ||
        (callbacks &&
         (pointer || !bounds[name].blockParameter || !bounds[name].nativeABI.parameters.empty())))
      return reject("unsupported native handle acquisition");
    auto close = disposer(ctx, acquire->getHandleType(), fn.getReturnType());
    if (!close)
      return std::unexpected(name + ": " + close.error());
    bounds[name].nativeABI.pointerResult = pointer;
    bounds[name].subscriptionRelease = *close;
    bounds[name].subscriptionFamily = acquire->getHandleType().str();
    result = resourceName(acquire->getHandleType());
  }
  if (mainThreadOnly)
    bounds[name].mainThreadOnly = true;
  if (borrowed)
    bounds[name].bufferCounts = std::move(counts);
  if (callbacks && bounds.contains(name) && !bounds[name].nativeABI.parameters.empty())
    return reject("callback registration with a borrowed native handle is unsupported");
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
  NativeRecords records;
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
    const clang::RecordDecl *requestedRecord = nullptr;
    for (auto *decl : ctx.getTranslationUnitDecl()->lookup(&ctx.Idents.get(name))) {
      if (const auto *r = llvm::dyn_cast<clang::RecordDecl>(decl)) {
        requestedRecord = r;
        continue;
      }
      if (const auto *alias = llvm::dyn_cast<clang::TypedefNameDecl>(decl);
          alias && alias->getUnderlyingType()->getAsRecordDecl() &&
          alias->getUnderlyingType()->getAsRecordDecl()->getName() == name) {
        requestedRecord = alias->getUnderlyingType()->getAsRecordDecl();
        continue;
      }
      const auto *candidate = llvm::dyn_cast<clang::FunctionDecl>(decl);
      if (!candidate || (fn && fn->getCanonicalDecl() != candidate->getCanonicalDecl()))
        return std::unexpected(path + ": ambiguous native declaration for " + name);
      fn = candidate->getMostRecentDecl();
    }
    if (requestedRecord && !fn) {
      NativeParameterLayout layout;
      auto r = recordArgument(*requestedRecord, records, layout);
      if (!r)
        return std::unexpected(r.error());
      continue;
    }
    if (!fn || fn->isImplicit())
      return std::unexpected(path + ": missing C prototype for " + name);
    auto line = declaration(*fn, verified, records);
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
  for (const auto &[name, definition] : records) {
    if (families.contains(name))
      return std::unexpected("native record and handle use the same name: " + name);
    source = definition + source;
  }
  bounds = std::move(verified);
  return source;
}
} // namespace kalandra
