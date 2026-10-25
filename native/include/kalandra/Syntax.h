#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace kalandra {
struct SourceSpan {
  std::size_t begin = 0;
  std::size_t end = 0;
};
class SourceFile {
public:
  struct Part {
    std::size_t begin;
    std::shared_ptr<const SourceFile> source;
  };
  SourceFile(std::string path, std::string text);
  SourceFile(std::string path, std::string text, std::vector<Part> parts);
  [[nodiscard]] const std::string &path() const { return path_; }
  [[nodiscard]] const std::string &text() const { return text_; }
  [[nodiscard]] std::string_view slice(SourceSpan span) const;
  // One-based lines and byte columns; offsets and spans are UTF-8 byte offsets.
  [[nodiscard]] std::pair<std::size_t, std::size_t> location(std::size_t offset) const;
  [[nodiscard]] std::string_view line(std::size_t number) const;
  [[nodiscard]] std::pair<const SourceFile *, std::size_t> origin(std::size_t offset) const;
  [[nodiscard]] std::size_t partIndex(std::size_t offset) const;
  [[nodiscard]] const std::vector<Part> &parts() const { return parts_; }

private:
  std::string path_;
  std::string text_;
  std::vector<std::size_t> lines_;
  std::vector<Part> parts_;
};
struct Diagnostic {
  std::string code;
  std::string message;
  SourceSpan span;
};
[[nodiscard]] std::string renderDiagnostic(const SourceFile &source, const Diagnostic &diagnostic);

enum class TokenKind {
  Identifier,
  Number,
  String,
  Newline,
  Indent,
  Dedent,
  End,
  LeftParen,
  RightParen,
  LeftBracket,
  RightBracket,
  LeftBrace,
  RightBrace,
  Colon,
  Comma,
  Question,
  Equal,
  Arrow,
  FatArrow,
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Dot,
  Comparison,
  Bang,
  Pipe,
  Relu // Typed tensor builtin; never produced by the lexer.
};
struct Token {
  TokenKind kind;
  SourceSpan span;
};
struct SyntaxLimits {
  std::size_t maxSourceBytes = 16 * 1024 * 1024;
  std::size_t maxTokens = 500000;
  std::size_t maxNodes = 200000;
  std::size_t maxDepth = 128;
  std::size_t maxDiagnostics = 32;
  bool editorRecovery = false; // Query-only syntax; never executable typed IR.
};
[[nodiscard]] std::expected<std::shared_ptr<const SourceFile>, std::string>
loadSource(std::string path, std::size_t maxBytes = SyntaxLimits{}.maxSourceBytes);

struct LexResult {
  std::shared_ptr<const SourceFile> source;
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;
};
[[nodiscard]] LexResult lex(std::shared_ptr<const SourceFile> source, SyntaxLimits limits = {});

struct Dimension {
  std::optional<std::uint64_t> extent;
  SourceSpan span;
};
struct FunctionTypeSyntax;
struct TypeSyntax {
  SourceSpan name;
  SourceSpan span;
  bool tensor = false;
  std::optional<SourceSpan> element;
  std::vector<Dimension> dimensions;
  bool array = false;
  // Immutable recursive syntax; copies preserve ownership without borrowing parser storage.
  std::shared_ptr<const FunctionTypeSyntax> function = {};
};
struct FunctionTypeSyntax {
  std::vector<TypeSyntax> parameters;
  TypeSyntax result;
  // Errors of this callable's invocation, not errors constructing a returned callable.
  std::vector<SourceSpan> errorTypes;
};
using ExprId = std::size_t;
struct NameExpr {
  SourceSpan name;
};
struct StringExpr {
  SourceSpan literal;
};
struct NumberExpr {
  SourceSpan literal;
};
struct UnaryExpr {
  TokenKind operation;
  ExprId operand;
};
struct BinaryExpr {
  TokenKind operation;
  ExprId left;
  ExprId right;
};
enum class ComparisonKind { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
struct ComparisonExpr {
  ComparisonKind operation;
  ExprId left, right;
};
struct CallExpr {
  ExprId callee;
  std::vector<ExprId> arguments;
};
struct IndexExpr {
  ExprId input;
  std::vector<ExprId> indices;
};
struct FieldInitializer {
  SourceSpan name;
  ExprId value;
};
struct RecordExpr {
  // Construction names a type; updates evaluate a base immutable value first.
  std::optional<ExprId> base;
  SourceSpan name;
  std::vector<FieldInitializer> fields;
  std::optional<SourceSpan> family = {}; // Qualified enum construction.
};
struct FieldExpr {
  ExprId input;
  SourceSpan field;
};
struct ArrayExpr {
  std::vector<ExprId> elements;
};
struct Statement;
struct LambdaParameter {
  SourceSpan name;
  std::optional<TypeSyntax> annotation;
};
struct LambdaExpr {
  std::vector<LambdaParameter> parameters;
  std::optional<TypeSyntax> result;
  std::vector<Statement> body;
  std::vector<SourceSpan> errorTypes = {};
};
enum class TaskPrefix { Spawn, Await, Try };
struct TaskExpr {
  TaskPrefix prefix;
  ExprId operand;
  std::optional<ExprId> placement;
};
struct ParallelExpr {
  std::vector<Statement> body;
};
struct ConditionalExpr {
  ExprId condition;
  std::vector<Statement> whenTrue, whenFalse;
};
struct LogicalExpr {
  bool conjunction;
  ExprId left, right;
};
struct CatchExpr {
  ExprId operand;
  SourceSpan parameter;
  std::vector<Statement> body;
};
struct PatternField {
  SourceSpan name, binding;
};
struct MatchArm {
  SourceSpan span;
  std::optional<SourceSpan> family, variant;
  std::vector<PatternField> fields;
  std::vector<Statement> body;
};
struct MatchExpr {
  ExprId subject;
  std::vector<MatchArm> arms;
};
struct FailExpr {
  ExprId operand;
};
struct Expr {
  SourceSpan span;
  std::variant<NameExpr, NumberExpr, StringExpr, UnaryExpr, BinaryExpr, ComparisonExpr, CallExpr,
               ArrayExpr, IndexExpr, TaskExpr, ParallelExpr, ConditionalExpr, LogicalExpr,
               CatchExpr, MatchExpr, FailExpr, LambdaExpr, RecordExpr, FieldExpr>
      value;
};
struct Statement {
  SourceSpan span;
  std::optional<SourceSpan> binding;
  std::optional<TypeSyntax> annotation;
  ExprId expression;
};
struct Parameter {
  SourceSpan name;
  TypeSyntax type;
};
struct Function {
  SourceSpan span;
  SourceSpan name;
  std::vector<Parameter> parameters;
  std::optional<TypeSyntax> result;
  std::vector<Statement> body;
  bool taskError = false;
  bool exported = false;
  std::size_t moduleIndex = 0;
  std::optional<SourceSpan> errorType = {};
  std::vector<SourceSpan> errorTypes = {};
};
struct ImportedName {
  SourceSpan name;
  std::optional<SourceSpan> alias;
};
struct Import {
  SourceSpan path;
  std::vector<ImportedName> names;
  std::size_t moduleIndex = 0;
  std::optional<std::size_t> target = {};
  bool exported = false;
};
struct VariantDeclaration {
  SourceSpan name;
  std::vector<Parameter> fields;
};
struct RecordDeclaration {
  SourceSpan name;
  std::vector<Parameter> fields;
  bool exported = false;
  std::vector<VariantDeclaration> variants = {};
  bool error = false;
  std::vector<SourceSpan> errorAlias = {};
};
struct Module {
  // Source storage outlives all borrowed slices/byte spans. Expressions use flat
  // arena IDs so a long left-associative chain cannot recursively destroy nodes.
  std::shared_ptr<const SourceFile> source;
  std::vector<Expr> expressions;
  std::vector<Function> functions;
  std::vector<Import> imports;
  std::vector<RecordDeclaration> records;
  bool editorOnly = false; // Structural recovery and unresolved expression holes.
};
struct ParseResult {
  Module module;
  std::vector<Diagnostic> diagnostics;
  [[nodiscard]] bool ok() const { return diagnostics.empty(); }
};
[[nodiscard]] ParseResult parse(std::shared_ptr<const SourceFile> source, SyntaxLimits limits = {});
struct ModuleLimits {
  std::size_t maxFiles = 128;
  std::size_t maxDepth = 32;
};
struct ModulePackage {
  std::string name;
  std::string entry;
};
// An already-resolved offline graph. Node zero owns the workspace entry; other
// nodes are confined to their canonical source roots. Reuse one node for aliases
// of the same resolved identity. Identity is supplied by the resolver, not inferred
// from a library's metadata name/version. No fetching or manifest discovery occurs.
// Paths are explicit filesystem paths, relative to the process working directory
// when not absolute (the same convention as loadModules/local mappings).
struct PackageDependency {
  std::string name;
  std::size_t target;
};
struct PackageNode {
  std::string identity;
  std::string root;
  std::string entry;
  std::vector<PackageDependency> dependencies;
  std::vector<ModulePackage> localPackages;
};
// Immutable per-analysis replacement contents, including files not yet saved.
// Paths use the same filesystem identity as imports; no contents are written.
struct SourceOverlay {
  std::string path;
  std::string text;
};
// Explicit offline package entry mappings; no filesystem or network work while parsing.
[[nodiscard]] std::expected<ModulePackage, std::string> parseModulePackage(std::string_view value);
// CPU local-module loading: explicit relative .k files and supplied package entries.
[[nodiscard]] std::expected<ParseResult, std::string>
loadModules(std::string path, SyntaxLimits syntaxLimits = {}, ModuleLimits moduleLimits = {},
            std::span<const ModulePackage> packages = {},
            std::span<const SourceOverlay> overlays = {});
// Each node resolves bare imports through its own edges/local mappings. Relative
// imports retain their owner. Graph structure is validated before reading sources.
[[nodiscard]] std::expected<ParseResult, std::string>
loadPackageModules(std::span<const PackageNode> graph, SyntaxLimits syntaxLimits = {},
                   ModuleLimits moduleLimits = {}, std::span<const SourceOverlay> overlays = {});
} // namespace kalandra
