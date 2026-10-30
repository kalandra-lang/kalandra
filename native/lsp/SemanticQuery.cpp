#include "SemanticQuery.h"
#include <algorithm>
#include <map>
namespace kalandra::lsp {
namespace {
using SpanKey = std::pair<std::size_t, std::size_t>;
std::shared_ptr<const SourceFile> originalSource(std::shared_ptr<const SourceFile> source,
                                                 std::string_view path, unsigned depth = 0) {
  if (!source || depth > 32)
    return {};
  if (source->parts().empty())
    return source->path() == path ? source : nullptr;
  for (const auto &part : source->parts())
    if (auto original = originalSource(part.source, path, depth + 1))
      return original;
  return {};
}
std::map<SpanKey, SourceSpan> callSites(const Module &module) {
  std::map<SpanKey, SourceSpan> callees;
  for (const auto &expression : module.expressions) {
    if (const auto *call = std::get_if<CallExpr>(&expression.value))
      callees.emplace(SpanKey{expression.span.begin, expression.span.end},
                      module.expressions[call->callee].span);
    if (const auto *pipeline = std::get_if<BinaryExpr>(&expression.value);
        pipeline && pipeline->operation == TokenKind::Arrow) {
      const auto &right = module.expressions[pipeline->right];
      if (std::holds_alternative<NameExpr>(right.value))
        callees.emplace(SpanKey{expression.span.begin, expression.span.end}, right.span);
      else if (const auto *call = std::get_if<CallExpr>(&right.value))
        callees.emplace(SpanKey{expression.span.begin, expression.span.end},
                        module.expressions[call->callee].span);
    }
  }
  return callees;
}
} // namespace
std::optional<SourceLocation> sourceLocation(std::shared_ptr<const SourceFile> source,
                                             SourceSpan span) {
  if (!source || span.end <= span.begin || span.end > source->text().size())
    return {};
  const auto [file, begin] = source->origin(span.begin);
  const auto [endFile, end] = source->origin(span.end);
  if (file != endFile || end <= begin)
    return {};
  auto original = originalSource(source, file->path());
  if (!original)
    return {};
  return SourceLocation{std::move(original), {begin, end}};
}
std::optional<SourceIndex> sourceIndex(ParseResult syntax, CheckOptions options) {
  if (!syntax.ok() || !syntax.module.source)
    return {};
  const auto callees = callSites(syntax.module);
  auto checked = check(std::move(syntax), {}, options);
  if (!checked.ok())
    return {};
  const auto &module = *checked.module;
  // Global byte spans are unique within this composed graph; spelling is only a
  // guard against synthetic symbols, never the identity used to find references.
  const auto lexed = lex(module.source);
  if (!lexed.diagnostics.empty())
    return {};
  std::map<SpanKey, std::string_view> identifiers;
  std::map<std::size_t, SourceSpan> ending;
  for (const auto &token : lexed.tokens)
    if (token.kind == TokenKind::Identifier) {
      identifiers.emplace(SpanKey{token.span.begin, token.span.end},
                          module.source->slice(token.span));
      ending.emplace(token.span.end, token.span);
    }
  std::map<SpanKey, SourceSpan> occurrences;
  const auto add = [&](SourceSpan use, SourceSpan target, std::string_view name) {
    const SpanKey key{use.begin, use.end};
    const auto token = identifiers.find(key);
    if (token != identifiers.end() && token->second == name)
      occurrences.try_emplace(key, target);
  };
  // Imported nominal/function names may be privately qualified by the checker.
  // Their declaration spans still identify the original source token.
  const auto addDeclaration = [&](SourceSpan span) {
    const auto token = identifiers.find({span.begin, span.end});
    if (token != identifiers.end())
      add(span, span, token->second);
  };
  std::vector<std::pair<SpanKey, SourceSpan>> secondary;
  for (const auto &token : module.sourceTokens)
    if (token.declaration) {
      addDeclaration(*token.declaration);
      const auto word = identifiers.find({token.span.begin, token.span.end});
      if (word != identifiers.end()) {
        if (token.secondary)
          secondary.push_back({{token.span.begin, token.span.end}, *token.declaration});
        else
          add(token.span, *token.declaration, word->second);
      }
    }
  for (const auto &import : module.imports) {
    const auto addImport = [&](SourceSpan span) {
      const auto token = identifiers.find({span.begin, span.end});
      if (token != identifiers.end())
        add(span, import.declaration, token->second);
    };
    addDeclaration(import.declaration);
    addImport(import.name);
    if (import.alias)
      addImport(*import.alias);
  }
  for (const auto &symbol : module.symbols)
    add(symbol.declaration, symbol.definition.value_or(symbol.declaration), symbol.name);
  for (const auto &function : module.functions)
    if (!function.anonymous)
      addDeclaration(function.declaration);
  for (const auto &record : module.records) {
    addDeclaration(record.declaration);
    for (const auto &field : record.fields)
      add(field.declaration, field.declaration, field.name);
  }
  for (const auto &expression : module.expressions) {
    if (const auto *read = std::get_if<SymbolRead>(&expression.operation)) {
      const auto &symbol = module.symbols[read->symbol];
      add(expression.span, symbol.definition.value_or(symbol.declaration), symbol.name);
    } else if (const auto *reference = std::get_if<FunctionReference>(&expression.operation)) {
      const auto &function = module.functions[reference->function];
      if (!function.anonymous) {
        const auto token = identifiers.find({expression.span.begin, expression.span.end});
        if (token != identifiers.end())
          add(expression.span, function.declaration, token->second);
      }
    } else if (const auto *call = std::get_if<FunctionCall>(&expression.operation)) {
      const auto &function = module.functions[call->function];
      const auto callee = callees.find({expression.span.begin, expression.span.end});
      if (!function.anonymous && callee != callees.end()) {
        const auto token = identifiers.find({callee->second.begin, callee->second.end});
        if (token != identifiers.end())
          add(callee->second, function.declaration, token->second);
      }
    } else if (const auto *read = std::get_if<RecordFieldRead>(&expression.operation)) {
      const auto &receiver = module.expressions[read->input].type;
      const auto token = ending.find(expression.span.end);
      if (receiver.record && token != ending.end() &&
          token->second.begin >= expression.span.begin) {
        const auto &field = module.records[*receiver.record].fields[read->field];
        add(token->second, field.declaration, field.name);
      }
    }
  }
  SourceIndex index{std::move(*checked.module), {}};
  for (const auto &[use, target] : occurrences)
    index.occurrences.push_back({{use.first, use.second}, target, false});
  for (const auto &[use, target] : secondary)
    index.occurrences.push_back({{use.first, use.second}, target, true});
  return index;
}
std::optional<std::vector<SourceLocation>> references(ParseResult syntax,
                                                      const SourceLocation &declaration,
                                                      bool includeDeclaration,
                                                      CheckOptions options) {
  auto index = sourceIndex(std::move(syntax), options);
  if (!index)
    return {};
  const auto &module = index->module;
  const auto same = [](const SourceLocation &a, const SourceLocation &b) {
    return a.source->path() == b.source->path() && a.span.begin == b.span.begin &&
           a.span.end == b.span.end;
  };
  std::map<SpanKey, SourceLocation> matches;
  const auto collect = [&](SpanKey use, SourceSpan target) {
    auto destination = sourceLocation(module.source, target);
    if (!destination || !same(*destination, declaration))
      return;
    auto location = sourceLocation(module.source, {use.first, use.second});
    if (location && (includeDeclaration || !same(*location, declaration)))
      matches.try_emplace(use, std::move(*location));
  };
  for (const auto &occurrence : index->occurrences)
    collect({occurrence.span.begin, occurrence.span.end}, occurrence.declaration);
  std::vector<SourceLocation> result;
  for (auto &[span, location] : matches)
    result.push_back(std::move(location));
  return result;
}
std::optional<SemanticToken> semanticToken(ParseResult syntax, std::string_view path,
                                           std::size_t offset, CheckOptions options) {
  if (!syntax.ok() || !syntax.module.source)
    return {};
  auto original = originalSource(syntax.module.source, path);
  if (!original || offset >= original->text().size())
    return {};
  auto tokens = lex(original);
  const auto token = std::ranges::find_if(tokens.tokens, [&](const auto &token) {
    return token.span.begin <= offset && offset < token.span.end;
  });
  if (token == tokens.tokens.end() ||
      (token->kind != TokenKind::Identifier && token->kind != TokenKind::String &&
       token->kind != TokenKind::Number))
    return {};
  const auto word = original->slice(token->span);
  const auto callees = callSites(syntax.module);
  auto checked = check(std::move(syntax), {}, options);
  if (!checked.ok())
    return {};
  SemanticToken selected{std::move(*checked.module), {original, token->span}, {}, {}, {}, {}};
  const auto &module = selected.module;
  const auto exact = [&](SourceSpan span) {
    auto mapped = sourceLocation(module.source, span);
    return mapped && mapped->source->path() == path && mapped->span.begin == token->span.begin &&
           mapped->span.end == token->span.end;
  };
  for (const auto &token : module.sourceTokens)
    if (!token.secondary && exact(token.span)) {
      selected.declaration = token.declaration;
      selected.type = token.type;
      selected.record = token.record;
      return selected;
    }
  for (const auto &import : module.imports)
    if (exact(import.name) || (import.alias && exact(*import.alias))) {
      selected.declaration = import.declaration;
      selected.function = import.function;
      selected.record = import.record;
      return selected;
    }
  for (const auto &symbol : module.symbols)
    if (symbol.name == word && exact(symbol.declaration)) {
      selected.type = symbol.type;
      selected.declaration = symbol.definition.value_or(symbol.declaration);
      return selected;
    }
  for (FunctionId i = 0; i < module.functions.size(); ++i)
    if (!module.functions[i].anonymous && exact(module.functions[i].declaration)) {
      selected.function = i;
      selected.declaration = module.functions[i].declaration;
      return selected;
    }
  for (std::size_t i = 0; i < module.records.size(); ++i) {
    const auto &record = module.records[i];
    if (exact(record.declaration)) {
      selected.record = i;
      selected.declaration = record.declaration;
      return selected;
    }
    for (const auto &field : record.fields)
      if (exact(field.declaration)) {
        selected.type = field.type;
        selected.declaration = field.declaration;
        return selected;
      }
  }
  for (const auto &expression : module.expressions) {
    if (exact(expression.span)) {
      selected.type = expression.type;
      if (const auto *symbol = std::get_if<SymbolRead>(&expression.operation)) {
        const auto &binding = module.symbols[symbol->symbol];
        if (binding.name != word)
          continue;
        selected.declaration = binding.definition.value_or(binding.declaration);
      } else if (const auto *reference = std::get_if<FunctionReference>(&expression.operation)) {
        selected.function = reference->function;
        if (!module.functions[reference->function].anonymous)
          selected.declaration = module.functions[reference->function].declaration;
      } else if (const auto *field = std::get_if<RecordFieldRead>(&expression.operation)) {
        const auto &receiver = module.expressions[field->input].type;
        if (receiver.record)
          selected.declaration = module.records[*receiver.record].fields[field->field].declaration;
      }
      return selected;
    }
    if (const auto *field = std::get_if<RecordFieldRead>(&expression.operation)) {
      auto mapped = sourceLocation(module.source, expression.span);
      if (mapped && mapped->source->path() == path && mapped->span.end == token->span.end &&
          mapped->span.begin <= token->span.begin) {
        const auto &receiver = module.expressions[field->input].type;
        if (receiver.record && *receiver.record < module.records.size() &&
            module.records[*receiver.record].fields[field->field].name == word) {
          selected.type = expression.type;
          selected.declaration = module.records[*receiver.record].fields[field->field].declaration;
          return selected;
        }
      }
    }
    if (const auto *call = std::get_if<FunctionCall>(&expression.operation)) {
      const auto found = callees.find({expression.span.begin, expression.span.end});
      if (found != callees.end() && exact(found->second)) {
        selected.function = call->function;
        if (!module.functions[call->function].anonymous)
          selected.declaration = module.functions[call->function].declaration;
        return selected;
      }
    }
  }
  return {};
}
std::optional<SourceLocation> definition(ParseResult syntax, std::string_view path,
                                         std::size_t offset, CheckOptions options) {
  auto selected = semanticToken(std::move(syntax), path, offset, options);
  if (!selected || !selected->declaration)
    return {};
  auto location = sourceLocation(selected->module.source, *selected->declaration);
  if (!location)
    return {};
  // Do not publish generated lambda bodies or compiler-internal spans as definitions.
  auto tokens = lex(location->source);
  const auto named = std::ranges::any_of(tokens.tokens, [&](const auto &token) {
    return token.kind == TokenKind::Identifier && token.span.begin == location->span.begin &&
           token.span.end == location->span.end;
  });
  return named ? location : std::nullopt;
}
} // namespace kalandra::lsp
