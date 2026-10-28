#pragma once
#include "kalandra/NumericType.h"
#include "kalandra/TensorTransform.h"
#include "kalandra/Syntax.h"

namespace kalandra {
using ValueId = std::size_t;
using SymbolId = std::size_t;
using FunctionId = std::size_t;
using ExtentId = std::uint64_t;
struct Extent {
  std::optional<std::uint64_t> constant;
  ExtentId identity =
      0; // Nonzero identity for a dynamic extent, never an implicit equality of '?'.
};
enum class BuiltinValue { Numeric, TaskError, FaultKind, String, Device, Bool, Record, Function };
struct FunctionSignature;
struct ValueType {
  // Ranked immutable storage; arrays use rank one with a separate source category.
  bool tensor = false;
  std::vector<Extent> shape;
  bool task = false, fallible = false;
  std::size_t identity = 0;
  BuiltinValue builtin = BuiltinValue::Numeric;
  // Tensor residence, or the selection carried by a Device value. Null is CPU.
  // Scalar numeric results remain host values, including GPU reductions.
  std::optional<std::uint32_t> device = {};
  // Scalar numeric identity, or the tensor element type; irrelevant to other builtins.
  NumericType element = NumericType::Float32;
  bool array = false; // Source [T], distinct from Tensor[T,...]; never an implicit conversion.
  std::optional<std::size_t> record = {}; // Nominal module-local declaration identity.
  std::optional<std::size_t> errorRecord =
      {};                      // Expected application-error family; null means TaskError.
  std::string recordName = {}; // User-facing spelling; identity is the table index above.
  std::shared_ptr<const FunctionSignature> callable = {};
};
struct FunctionSignature {
  std::vector<ValueType> parameters;
  ValueType result;
};
// Whole-module may-call information, indexed by expression ID. Unknown sources
// remain explicit even when compatible known targets are also present.
struct FunctionInvocationFlow {
  std::vector<std::vector<FunctionId>> targets;
  std::vector<bool> unknown;
  // Expressions potentially executed from this graph's root main. Uncalled
  // library helpers retain type checking without inventing incoming values.
  std::vector<bool> reachable;
};
[[nodiscard]] std::optional<FunctionInvocationFlow>
functionInvocationFlow(const struct TypedModule &, std::size_t budget = 300000);
// Structural signatures ignore dynamic shape identities, never nominal record identities.
[[nodiscard]] bool sameFunctionValueType(const ValueType &, const ValueType &,
                                         std::size_t depth = 0);
[[nodiscard]] std::optional<std::string> functionValueCapabilityError(const struct TypedModule &,
                                                                      NumericTarget);
[[nodiscard]] bool containsFunctionStorage(const struct TypedModule &, const ValueType &);
// CPU data captures supported by the first worker closure-environment copy.
[[nodiscard]] bool workerClosureCaptureTypeSupported(const struct TypedModule &, const ValueType &);
// Permit proven supported function arguments; reject other function transport.
[[nodiscard]] std::optional<std::string> functionValueTransportError(const struct TypedModule &,
                                                                     const struct TypedExpr &);
struct FunctionReference {
  FunctionId function;
  std::vector<ValueId> captures = {};
};
struct FunctionInvoke {
  ValueId callee;
  std::vector<ValueId> arguments;
};
inline bool isRecordArray(const ValueType &type) {
  return type.builtin == BuiltinValue::Record && type.array;
}
inline bool isBoolArray(const ValueType &type) {
  return type.builtin == BuiltinValue::Bool && type.array;
}
inline bool isStringArray(const ValueType &type) {
  return type.builtin == BuiltinValue::String && type.array;
}
inline bool isFunctionArray(const ValueType &type) {
  return type.builtin == BuiltinValue::Function && type.array;
}
inline bool isOwnedArray(const ValueType &type) {
  return isRecordArray(type) || isBoolArray(type) || isStringArray(type) || isFunctionArray(type);
}
inline ValueType recordArrayElement(ValueType type) {
  type.tensor = type.array = type.task = type.fallible = false;
  type.shape.clear();
  type.identity = 0;
  type.errorRecord.reset();
  return type;
}
struct Symbol {
  std::string name;
  ValueType type;
  SourceSpan declaration;
  FunctionId owner;
  bool parameter;
  // Source definition of a lifted capture/narrowed alias. Diagnostics retain declaration.
  std::optional<SourceSpan> definition = {};
};
struct Int32Literal {
  std::int32_t value;
};
struct Int64Literal {
  std::int64_t value;
};
struct UInt64Literal {
  std::uint64_t value;
};
struct UInt32Literal {
  std::uint32_t value;
};
struct Float64Literal {
  std::uint64_t bits;
};
struct Float32Literal {
  std::uint32_t bits;
};
// Checked projection of a completed error union to a strict family subset.
// Introduced for the matched binding inside a selected match branch.
struct ErrorNarrow {
  ValueId input;
};
struct SymbolRead {
  SymbolId symbol;
};
struct ScalarUnary {
  TokenKind operation;
  ValueId operand;
};
// Shape-preserving elementwise unary arithmetic on immutable float32 tensors.
struct TensorUnary {
  TokenKind operation;
  ValueId operand;
};
enum class StringQueryKind { Length, ByteLength };
struct StringSlice {
  ValueId input, start, end;
};
struct StringIndex {
  ValueId input, index;
};
struct StringQuery {
  StringQueryKind kind;
  ValueId input;
};
struct StringConcat {
  ValueId left, right;
};
struct StringLiteral {
  std::string value;
};
struct BoolLiteral {
  bool value;
};
struct ScalarCompare {
  ComparisonKind operation;
  ValueId left, right;
};
struct ScalarBinary {
  TokenKind operation;
  ValueId left;
  ValueId right;
};
// Elementwise strict float32 arithmetic. Scalar operands lift across the tensor;
// tensor operands require identical ranks and extents (no array broadcasting).
struct TensorBinary {
  TokenKind operation;
  ValueId left, right;
};
struct NumericConvert {
  ValueId input;
};
struct TensorLiteral {
  std::vector<ValueId> elements;
}; // Flattened row-major scalar values.
struct FunctionCall {
  FunctionId function;
  std::vector<ValueId> arguments;
};
// Explicit immutable flat-array to CPU tensor construction.
struct ArrayToTensor {
  ValueId input;
  std::optional<std::vector<ValueId>> dimensions =
      {}; // Absent preserves rank one; empty is rank zero.
};
struct TensorReshape {
  ValueId input;
  std::vector<ValueId> dimensions;
};
struct TensorToArray {
  ValueId input;
}; // Flatten logical row-major elements into an immutable numeric array.
struct ArrayConcat {
  ValueId left, right;
};
struct ArraySlice {
  ValueId input, start, end;
};
struct ArrayIndex {
  ValueId input, index;
};
struct TensorIndex {
  ValueId input;
  std::vector<ValueId> indices;
};
enum class CollectionQueryKind { Length, Shape };
struct CollectionQuery {
  CollectionQueryKind kind;
  ValueId input;
};
struct ArrayMap {
  ValueId input;
  FunctionId function; // One explicit element parameter, followed by lifted captures.
  std::vector<ValueId> captures = {};
  bool parallel = false;
};
enum class ReductionOrder { BalancedAdjacent };
struct Matmul {
  ValueId left;
  ValueId right;
  bool checkInnerExtent;
  bool checkAllocationSize; // Validate before allocation; never rely on wrapped dimension products.
  ReductionOrder order = ReductionOrder::BalancedAdjacent;
  bool separatelyRoundedProducts = true;
};
struct ReduceSum {
  ValueId input;
  ReductionOrder order = ReductionOrder::BalancedAdjacent;
  bool rowMajor = true;
};
// Non-pointwise immutable rank-two operations. Keeping this distinct from
// TensorUnary prevents elementwise/reduction fusion from discarding row or
// layout dependencies.
struct TensorTransform {
  TensorTransformKind kind;
  ValueId input;
  std::optional<ValueId> epsilon = {};
};
struct ExtentRequirement {
  std::size_t axis;
  std::uint64_t extent;
};
struct ShapeCheck {
  ValueId input;
  std::vector<ExtentRequirement> requirements;
};
struct TypedStatement {
  SourceSpan span;
  std::optional<SymbolId> binding;
  ValueId value;
};
struct DeviceSelection {
  std::optional<std::uint32_t> device; // Null selects CPU; otherwise an exact GPU index.
};
struct Transfer {
  ValueId input, target;
};
struct TaskOperation {
  TaskPrefix prefix;
  ValueId operand;
  std::optional<ValueId> placement = {};
};
struct ParallelOperation {
  std::vector<TypedStatement> body;
  ValueId result;
  std::size_t group;
};
struct ConditionalOperation {
  ValueId condition;
  std::vector<TypedStatement> whenTrue, whenFalse;
  ValueId trueResult, falseResult;
};
struct GuardOperation {
  ValueId condition;
  std::vector<TypedStatement> body;
  ValueId result; // The body ends in an explicit fail; the guard has no source value.
};
struct CatchOperation {
  ValueId operand;
  SymbolId parameter;
  std::vector<TypedStatement> body;
  ValueId result;
};
struct MatchCase {
  std::optional<unsigned> tag;
  std::vector<TypedStatement> body;
  ValueId result;
};
struct MatchOperation {
  ValueId subject;
  std::vector<MatchCase> cases;
};
enum class ErrorField { Kind, Message, Reason };
struct ErrorFieldRead {
  ValueId subject;
  ErrorField field;
  std::optional<std::size_t> taskSlot = {}; // Private TaskError payload inside a mixed union.
};
struct FailOperation {
  ValueId error;
};
struct RecordFieldValue {
  std::size_t field;
  ValueId value;
};
struct RecordValue {
  std::optional<ValueId> base;
  std::vector<RecordFieldValue> fields;    // Written evaluation order, canonical field indices.
  std::optional<std::size_t> variant = {}; // Selected enum case; inactive slots stay zero.
};
struct RecordFieldRead {
  ValueId input;
  std::size_t field;
};
struct TypedRecordField {
  std::string name;
  ValueType type;
  SourceSpan declaration;
};
struct TypedVariant {
  std::string name;
  SourceSpan declaration;
  std::vector<std::size_t> fields;
};
struct TypedRecord {
  std::string name;
  SourceSpan declaration;
  std::vector<TypedRecordField> fields;
  std::vector<TypedVariant> variants = {};
  bool error = false;           // Only declared errors may appear in expected-error channels.
  bool taskErrorFamily = false; // Private canonical identity for built-in TaskError in unions.
  std::vector<std::size_t> errorMembers =
      {}; // Canonical original families for a transparent union.
  std::vector<std::pair<std::size_t, std::size_t>> errorOrigins =
      {}; // Original family/variant per case.
};
struct TypedExpr {
  SourceSpan span;
  ValueType type; // Concrete scalar/tensor numeric identity; builtins retain explicit tags.
  std::variant<Float64Literal, Float32Literal, Int32Literal, Int64Literal, UInt32Literal,
               UInt64Literal, StringSlice, StringIndex, StringQuery, StringConcat, StringLiteral,
               BoolLiteral, ScalarCompare, SymbolRead, ScalarUnary, TensorUnary, ScalarBinary,
               TensorBinary, TensorLiteral, NumericConvert, FunctionCall, Matmul, ReduceSum,
               TensorTransform,
               ArrayMap, ArrayToTensor, TensorReshape, TensorToArray, ArrayConcat, ArraySlice,
               ArrayIndex, TensorIndex, CollectionQuery, ShapeCheck, DeviceSelection, Transfer,
               TaskOperation, ParallelOperation, ConditionalOperation, CatchOperation,
               MatchOperation, ErrorFieldRead, FailOperation, GuardOperation, RecordValue,
               RecordFieldRead, ErrorNarrow, FunctionReference, FunctionInvoke>
      operation;
};
struct ShapeEquality {
  Extent left;
  Extent right;
};
struct TypedFunction {
  std::string name;
  SourceSpan declaration;
  std::vector<SymbolId> parameters;
  std::optional<ValueType> declaredResult;
  ValueType resultType;
  ValueId result = 0;
  std::vector<TypedStatement> body;
  std::vector<ValueId> expressions;
  std::vector<ShapeEquality> preconditions;
  // Lifted anonymous functions keep explicit parameters first, lexical captures last.
  std::size_t captureCount = 0;
  bool anonymous = false;
};
// Original import tokens and compiler-resolved targets, retained for source tools.
// An explicit alias is recorded separately so future rename can preserve it.
struct ResolvedImport {
  SourceSpan name;
  std::optional<SourceSpan> alias;
  SourceSpan declaration;
  std::optional<FunctionId> function;
  std::optional<std::size_t> record;
  bool exported = false;
};
struct ResolvedSourceToken {
  SourceSpan span;
  std::optional<SourceSpan> declaration;
  std::optional<ValueType> type;
  // Nominal type tokens use a declaration header on hover; fields use their value type.
  std::optional<std::size_t> record;
  // Shorthand pattern labels also declare a local binding at this same token.
  // Keep the field-reference role without overriding primary binding navigation.
  bool secondary = false;
};
struct TypedModule {
  std::shared_ptr<const SourceFile> source;
  std::vector<Symbol> symbols;
  std::vector<TypedExpr> expressions;
  std::vector<TypedFunction> functions;
  std::vector<TypedRecord> records;
  std::vector<ResolvedImport> imports = {};
  std::vector<ResolvedSourceToken> sourceTokens = {};
};
struct SemanticLimits {
  std::size_t maxDepth = 128;
  std::size_t maxDiagnostics = 32;
  std::size_t maxValues = 300000;
  std::size_t maxTensorElements = 200000;
  std::size_t maxLiteralBytes = 4096;
  std::size_t maxShapeRequirements = 100000;
};
struct CheckResult {
  std::shared_ptr<const SourceFile> source;
  std::optional<TypedModule> module; // Never publish partial IR on an error.
  std::vector<Diagnostic> diagnostics;
  [[nodiscard]] bool ok() const { return module.has_value(); }
};
// Source Strings, excluding private borrowed task diagnostics.
[[nodiscard]] bool hasOrdinaryStrings(const TypedModule &module);
// True for device/transfer/placed-call IR or resident/selector types.
[[nodiscard]] bool hasPlacement(const TypedModule &module);
// Editor-only scope/member snapshot. Names are captured at the queried name;
// no partial typed IR or inferred type information escapes a failed check.
enum class ScopeNameKind {
  Variable,
  Parameter,
  Function,
  Type,
  Field,
  EnumCase,
  File,
  Folder,
  Keyword
};
struct ScopeName {
  std::string name;
  ScopeNameKind kind;
};
struct ScopeCompletion {
  SourceSpan site;
  std::string prefix;
  std::vector<ScopeName> names;
  bool reached = false, truncated = false;
};
// Editor-only snapshot of a validated callee, not a partial module or executable IR.
struct SignatureErrorFamily {
  std::string name;
  std::vector<std::size_t> members;
  bool taskError = false;
};
struct CallableSignatureSnapshot {
  FunctionSignature signature;
  std::vector<std::string> parameterNames;
  std::vector<SignatureErrorFamily> errorFamilies;
};
struct CallableSignatureQuery {
  SourceSpan site;
  std::optional<CallableSignatureSnapshot> snapshot;
  // Optional receiver of a projected field argument, in this syntax module.
  std::optional<ExprId> argumentReceiver = {};
  // Earlier editing slot, counting an implicit pipeline argument when present.
  std::optional<std::size_t> argumentIndex = {};
  // Validate a compound edited argument up to a selected unresolved name/member hole.
  std::optional<ExprId> editingExpression = {};
  std::optional<SourceSpan> editingSite = {};
};
struct CheckOptions {
  // Build placement-aware IR without probing hardware or selecting a backend.
  // The ordinary CPU entry keeps rejecting placement syntax by default.
  bool placement = false;
  ScopeCompletion *completion = nullptr;       // Caller-owned, optional editor query.
  CallableSignatureQuery *signature = nullptr; // Checked callable guidance only.
};
[[nodiscard]] CheckResult check(ParseResult syntax, SemanticLimits limits = {},
                                CheckOptions options = {});
[[nodiscard]] std::string describeType(const ValueType &type);
[[nodiscard]] std::string dumpTypedIR(const TypedModule &module);
// Reject unsupported numeric IR before a backend can apply float32-specific lowering.
[[nodiscard]] std::optional<std::string> numericCapabilityError(const TypedModule &module,
                                                                NumericTarget target);
} // namespace kalandra
