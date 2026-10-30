#include "Completion.h"
#include "kalandra/Utf8.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <type_traits>
namespace kalandra::lsp {
namespace {
CompletionInfo completeImportPaths(const std::shared_ptr<const SourceFile> &source,
                                   const Token &token, std::size_t offset,
                                   const std::vector<SourceOverlay> &overlays) {
  namespace fs = std::filesystem;
  CompletionInfo result{{offset, offset}, {}, false};
  const auto &text = source->text();
  if (token.span.end < token.span.begin + 2 || text[token.span.end - 1] != '"' ||
      offset <= token.span.begin || offset >= token.span.end)
    return result;
  const auto syntax = parse(source);
  if (!syntax.ok() || !std::ranges::any_of(syntax.module.imports, [&](const auto &import) {
        return import.path.begin == token.span.begin && import.path.end == token.span.end;
      }))
    return result;
  const auto content =
      std::string_view(text).substr(token.span.begin + 1, token.span.end - token.span.begin - 2);
  if (content.find_first_of(std::string("\0\\\"", 3)) != content.npos)
    return result;
  const auto prefix = content.substr(0, offset - token.span.begin - 1);
  if (!prefix.starts_with("./") && !prefix.starts_with("../"))
    return result;
  const auto directory = prefix.substr(0, prefix.rfind('/') + 1);
  std::error_code error;
  auto parent = fs::weakly_canonical(fs::path(source->path()).parent_path() / directory, error);
  if (error)
    return result;
  if (parent.filename().empty())
    parent = parent.parent_path();
  std::map<std::string, ScopeNameKind> candidates;
  const auto add = [&](const fs::path &path, bool folder) {
    const auto filename = path.filename().string();
    if (filename.empty() || filename.front() == '.' || !validUtf8(filename) ||
        filename.find_first_of(std::string("\0\r\n\\\"", 5)) != filename.npos ||
        (!folder && path.extension() != ".k") || path == fs::path(source->path()))
      return;
    const auto label = std::string(directory) + filename + (folder ? "/" : "");
    if (label.starts_with(prefix))
      candidates.try_emplace(label, folder ? ScopeNameKind::Folder : ScopeNameKind::File);
  };
  fs::directory_iterator iterator(parent, error), end;
  // An unsaved dependency may live in a directory that does not exist yet.
  if (error && error != std::errc::no_such_file_or_directory)
    return result;
  error.clear();
  std::size_t entries = 0;
  for (; iterator != end; iterator.increment(error)) {
    if (error)
      return result;
    if (++entries > 20000) {
      result.incomplete = true;
      return result;
    }
    const auto status = iterator->symlink_status(error);
    if (error)
      return result;
    if (fs::is_directory(status))
      add(iterator->path(), true);
    else if (fs::is_regular_file(status))
      add(iterator->path(), false);
  }
  if (error || overlays.size() > ModuleLimits{}.maxFiles)
    return result;
  for (const auto &overlay : overlays) {
    const auto path = fs::weakly_canonical(overlay.path, error);
    if (error || path.parent_path() != parent)
      continue;
    const auto status = fs::symlink_status(overlay.path, error);
    if (error && error != std::errc::no_such_file_or_directory)
      continue;
    if (!error && status.type() != fs::file_type::not_found && !fs::is_regular_file(status))
      continue;
    add(path, false);
  }
  result.replacement = {token.span.begin + 1, token.span.end - 1};
  result.incomplete = candidates.size() > 256;
  for (const auto &[name, kind] : candidates) {
    if (result.names.size() == 256)
      break;
    result.names.push_back({name, kind});
  }
  return result;
}
} // namespace
CompletionInfo completeNames(const std::string &path, const std::string &text, std::size_t offset,
                             const std::vector<ModulePackage> &packages,
                             std::vector<SourceOverlay> overlays, CheckOptions options,
                             const SourceGraphLoader &loader) {
  CompletionInfo result{{offset, offset}, {}, false};
  if (offset > text.size())
    return result;
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error).string();
  if (error)
    return result;
  auto source = std::make_shared<const SourceFile>(canonical, text);
  const auto tokens = lex(source);
  if (!std::ranges::all_of(tokens.diagnostics,
                           [](const auto &d) { return d.code == "L005" || d.code == "L011"; }))
    return result;
  for (const auto &token : tokens.tokens) {
    if (token.kind == TokenKind::String && token.span.begin < offset && offset < token.span.end)
      return tokens.diagnostics.empty() ? completeImportPaths(source, token, offset, overlays)
                                        : result;
    if (token.kind == TokenKind::Identifier && token.span.begin <= offset &&
        offset <= token.span.end) {
      result.replacement = token.span;
      break;
    }
    if (token.span.begin <= offset && offset < token.span.end &&
        (token.kind == TokenKind::String || token.kind == TokenKind::Number))
      return result;
  }
  // Repair only one label/value slot inside balanced construction/pattern delimiters.
  // The parsed node and checker must still identify a valid nominal owner.
  const auto trivia = [](TokenKind kind) {
    return kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent ||
           kind == TokenKind::End;
  };
  std::vector<TokenKind> delimiters;
  std::optional<TokenKind> previous;
  std::vector<const Token *> following;
  for (const auto &token : tokens.tokens) {
    if (trivia(token.kind))
      continue;
    if (token.span.end <= result.replacement.begin) {
      previous = token.kind;
      if (token.kind == TokenKind::LeftBrace || token.kind == TokenKind::LeftParen ||
          token.kind == TokenKind::LeftBracket)
        delimiters.push_back(token.kind);
      else if (token.kind == TokenKind::RightBrace || token.kind == TokenKind::RightParen ||
               token.kind == TokenKind::RightBracket) {
        if (!delimiters.empty())
          delimiters.pop_back();
      }
    } else if (token.span.begin >= result.replacement.end && following.size() < 2)
      following.push_back(&token);
  }
  const bool labelSlot = !delimiters.empty() && delimiters.back() == TokenKind::LeftBrace &&
                         (previous == TokenKind::LeftBrace || previous == TokenKind::Comma);
  const auto boundary = [](TokenKind kind) {
    return kind == TokenKind::RightBrace || kind == TokenKind::Comma;
  };
  const bool bareLabel = labelSlot && !following.empty() && boundary(following[0]->kind);
  const bool missingValue = labelSlot && following.size() == 2 &&
                            following[0]->kind == TokenKind::Colon && boundary(following[1]->kind);
  const auto prefix = text.substr(result.replacement.begin, offset - result.replacement.begin);
  constexpr std::string_view probe = "__kalandra_completion_probe";
  // An identifier can stand in for either a constructor value or a pattern binder.
  constexpr std::string_view filler = "__kalandra_completion_binding";
  if (text.size() - (result.replacement.end - result.replacement.begin) + probe.size() +
          (bareLabel      ? filler.size() + 2
           : missingValue ? filler.size() + 1
                          : 0) >
      SyntaxLimits{}.maxSourceBytes)
    return result;
  auto replacement = text;
  replacement.replace(result.replacement.begin, result.replacement.end - result.replacement.begin,
                      std::string(probe) + (bareLabel ? ": " + std::string(filler) : ""));
  if (missingValue) {
    const auto at =
        result.replacement.begin + probe.size() + following[0]->span.end - result.replacement.end;
    replacement.insert(at, " " + std::string(filler));
  }
  std::erase_if(overlays, [&](const auto &entry) {
    std::error_code ec;
    return std::filesystem::weakly_canonical(entry.path, ec).string() == canonical && !ec;
  });
  overlays.push_back({canonical, std::move(replacement)});
  SyntaxLimits limits;
  limits.editorRecovery = true;
  auto syntax =
      loader ? loader(path, overlays, true) : loadModules(path, limits, {}, packages, overlays);
  if (!syntax || !syntax->module.editorOnly)
    return result;
  // Locate the probe through original-file identities so this does not depend
  // on how the module loader arranges the composed source.
  std::optional<SourceSpan> site;
  bool labelSite = false;
  const auto locate = [&](SourceSpan candidate) {
    const auto [original, local] = syntax->module.source->origin(candidate.begin);
    if (original->path() == canonical && local == result.replacement.begin &&
        syntax->module.source->slice(candidate) == probe)
      site = candidate;
  };
  for (const auto &import : syntax->module.imports)
    for (const auto &imported : import.names)
      locate(imported.name);
  for (const auto &expression : syntax->module.expressions) {
    if (const auto *name = std::get_if<NameExpr>(&expression.value)) {
      locate(name->name);
      // Device selectors have one qualified NameExpr rather than a field node.
      if (syntax->module.source->slice(name->name) == "devices." + std::string(probe))
        locate({name->name.end - probe.size(), name->name.end});
    } else if (const auto *field = std::get_if<FieldExpr>(&expression.value))
      locate(field->field);
    else if (const auto *record = std::get_if<RecordExpr>(&expression.value)) {
      if (record->family)
        locate(record->name);
      for (const auto &field : record->fields) {
        if (site)
          break;
        locate(field.name);
        if (site) {
          labelSite = true;
          break;
        }
      }
    } else if (const auto *match = std::get_if<MatchExpr>(&expression.value))
      for (const auto &arm : match->arms) {
        if (arm.variant)
          locate(*arm.variant);
        for (const auto &field : arm.fields)
          locate(field.name);
      }
    if (site)
      break;
  }
  if (!site) {
    // Type annotations are not expressions. Walk their owned syntax explicitly,
    // including nested callable types and annotations in nested suites.
    std::vector<const TypeSyntax *> types;
    const auto statements = [&](const std::vector<Statement> &body) {
      for (const auto &statement : body)
        if (statement.annotation)
          types.push_back(&*statement.annotation);
    };
    for (const auto &function : syntax->module.functions) {
      for (const auto channel : function.errorTypes)
        locate(channel);
      if (function.errorType)
        locate(*function.errorType);
      for (const auto &parameter : function.parameters)
        types.push_back(&parameter.type);
      if (function.result)
        types.push_back(&*function.result);
      statements(function.body);
    }
    for (const auto &record : syntax->module.records) {
      for (const auto member : record.errorAlias)
        locate(member);
      for (const auto &field : record.fields)
        types.push_back(&field.type);
      for (const auto &variant : record.variants)
        for (const auto &field : variant.fields)
          types.push_back(&field.type);
    }
    for (const auto &expression : syntax->module.expressions) {
      std::visit(
          [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, LambdaExpr>) {
              for (const auto channel : value.errorTypes)
                locate(channel);
              for (const auto &parameter : value.parameters)
                if (parameter.annotation)
                  types.push_back(&*parameter.annotation);
              if (value.result)
                types.push_back(&*value.result);
            }
            if constexpr (requires { value.body; })
              statements(value.body);
            if constexpr (std::is_same_v<T, ConditionalExpr>) {
              statements(value.whenTrue);
              statements(value.whenFalse);
            }
            if constexpr (std::is_same_v<T, MatchExpr>)
              for (const auto &arm : value.arms)
                statements(arm.body);
          },
          expression.value);
    }
    while (!site && !types.empty()) {
      const auto *type = types.back();
      types.pop_back();
      if (type->function) {
        for (const auto channel : type->function->errorTypes)
          locate(channel);
        for (const auto &parameter : type->function->parameters)
          types.push_back(&parameter);
        types.push_back(&type->function->result);
      } else {
        locate(type->name);
        if (type->element)
          locate(*type->element);
      }
    }
  }
  if (!site)
    return result;
  ScopeCompletion query{*site, prefix, {}, false, false};
  options.completion = &query;
  (void)check(std::move(*syntax), {}, options);
  result.appendFieldColon = labelSite && bareLabel;
  result.names = std::move(query.names);
  result.incomplete = query.truncated;
  return result;
}
} // namespace kalandra::lsp
