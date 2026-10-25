#include "Rename.h"
#include "Workspace.h"
#include <algorithm>
#include <compare>
#include <map>
#include <set>
#include <tuple>
namespace kalandra::lsp {
namespace {
using SpanKey = std::pair<std::size_t, std::size_t>;
struct Key {
  std::string path;
  std::size_t begin, end;
  auto operator<=>(const Key &) const = default;
};
struct Edge {
  Key use, target;
  bool secondary;
};
struct Graph {
  std::string root;
  std::vector<Edge> edges;
  std::map<Key, std::string> identifiers;
  std::set<Key> shorthand;
  std::set<std::string> sources;
  std::set<Key> expressionNames, fixedDeclarations;
};
using Snapshots = std::map<std::string, std::shared_ptr<const SourceFile>>;
Key key(const SourceLocation &location) {
  return {location.source->path(), location.span.begin, location.span.end};
}
std::optional<Key> key(const std::shared_ptr<const SourceFile> &source, SourceSpan span) {
  auto location = sourceLocation(source, span);
  return location ? std::optional{key(*location)} : std::nullopt;
}
bool same(SourceSpan a, SourceSpan b) { return a.begin == b.begin && a.end == b.end; }
std::expected<void, std::string> snapshot(std::shared_ptr<const SourceFile> source,
                                          Snapshots &files, std::set<std::string> &paths,
                                          unsigned depth = 0) {
  if (depth > 32)
    return std::unexpected("Rename source nesting exceeds limit");
  if (source->parts().empty()) {
    paths.insert(source->path());
    auto [found, inserted] = files.try_emplace(source->path(), source);
    if (!inserted && found->second->text() != source->text())
      return std::unexpected("Sources changed during rename analysis");
  } else
    for (const auto &part : source->parts()) {
      auto result = snapshot(part.source, files, paths, depth + 1);
      if (!result)
        return result;
    }
  return {};
}
std::expected<Graph, std::string> indexGraph(ParseResult syntax, const std::string &root,
                                             Snapshots &files, CheckOptions options) {
  const auto source = syntax.module.source;
  if (!syntax.ok() || !source)
    return std::unexpected("Cannot rename an invalid source graph");
  // Surface identities stop at explicit aliases. Ordinary imports/re-exports
  // transparently follow their provider, using the compiler-validated AST edges.
  using Name = std::pair<std::size_t, std::string>;
  std::map<Name, SourceSpan> exports;
  struct Forward {
    std::size_t target;
    SourceSpan name;
  };
  std::map<Name, Forward> forwards;
  const auto word = [&](SourceSpan span) { return std::string(source->slice(span)); };
  for (const auto &function : syntax.module.functions)
    if (function.exported)
      exports.emplace(Name{function.moduleIndex, word(function.name)}, function.name);
  for (const auto &record : syntax.module.records)
    if (record.exported)
      exports.emplace(Name{source->partIndex(record.name.begin), word(record.name)}, record.name);
  for (const auto &import : syntax.module.imports)
    if (import.exported && import.target)
      for (const auto &name : import.names) {
        const Name label{import.moduleIndex, word(name.alias.value_or(name.name))};
        if (name.alias)
          exports.emplace(label, *name.alias);
        else
          forwards.emplace(label, Forward{*import.target, name.name});
      }
  const auto resolve = [&](std::size_t module, SourceSpan token) -> std::optional<SourceSpan> {
    Name label{module, word(token)};
    std::set<Name> seen;
    while (seen.size() < 128 && seen.insert(label).second) {
      if (auto found = exports.find(label); found != exports.end())
        return found->second;
      const auto next = forwards.find(label);
      if (next == forwards.end())
        return {};
      label = {next->second.target, word(next->second.name)};
    }
    return {};
  };
  std::set<Key> expressionNames, fixedDeclarations;
  for (const auto &expression : syntax.module.expressions)
    if (const auto *name = std::get_if<NameExpr>(&expression.value))
      if (auto location = key(source, name->name))
        expressionNames.insert(*location);
  for (const auto &function : syntax.module.functions)
    if (word(function.name) == "main")
      if (auto location = key(source, function.name))
        fixedDeclarations.insert(*location);
  const auto imports = syntax.module.imports;
  auto index = sourceIndex(std::move(syntax), options);
  if (!index)
    return std::unexpected("Cannot rename: a searched source graph has errors");
  Graph graph{root, {}, {}, {}, {}, std::move(expressionNames), std::move(fixedDeclarations)};
  auto stored = snapshot(source, files, graph.sources);
  if (!stored)
    return std::unexpected(stored.error());
  struct Local {
    SourceSpan original, surface;
  };
  std::map<Name, Local> locals;
  std::map<SpanKey, SourceSpan> importTokens;
  std::map<SpanKey, const ResolvedImport *> checkedImports;
  for (const auto &import : index->module.imports)
    checkedImports.emplace(SpanKey{import.name.begin, import.name.end}, &import);
  for (const auto &import : imports) {
    if (!import.target)
      continue;
    for (const auto &name : import.names) {
      const auto provider = resolve(*import.target, name.name);
      const auto checked = checkedImports.find({name.name.begin, name.name.end});
      if (!provider || checked == checkedImports.end())
        return std::unexpected("Cannot resolve rename import surface identity");
      const auto surface = name.alias.value_or(*provider);
      locals.emplace(Name{import.moduleIndex, word(name.alias.value_or(name.name))},
                     Local{checked->second->declaration, surface});
      importTokens.emplace(SpanKey{name.name.begin, name.name.end}, *provider);
      if (name.alias)
        importTokens.emplace(SpanKey{name.alias->begin, name.alias->end}, *name.alias);
    }
  }
  auto lexed = lex(source);
  if (!lexed.diagnostics.empty())
    return std::unexpected("Cannot index rename source tokens");
  for (const auto &token : lexed.tokens)
    if (token.kind == TokenKind::Identifier) {
      auto location = key(source, token.span);
      if (location)
        graph.identifiers.try_emplace(*location, word(token.span));
    }
  for (const auto &token : index->module.sourceTokens)
    if (token.secondary) {
      if (auto location = key(source, token.span))
        graph.shorthand.insert(*location);
    }
  for (const auto &occurrence : index->occurrences) {
    auto target = occurrence.declaration;
    if (auto token = importTokens.find({occurrence.span.begin, occurrence.span.end});
        token != importTokens.end())
      target = token->second;
    else if (auto local =
                 locals.find({source->partIndex(occurrence.span.begin), word(occurrence.span)});
             local != locals.end() && same(local->second.original, target))
      target = local->second.surface;
    auto use = key(source, occurrence.span), destination = key(source, target);
    if (use && destination)
      graph.edges.push_back({*use, *destination, occurrence.secondary});
  }
  return graph;
}
struct Edit {
  Key use;
  std::string text;
  SourceSpan primary, secondary; // Relative ranges within replacement text.
};
using Edits = std::map<Key, Edit>;
struct OffsetMap {
  struct Mapping {
    const Edit *edit;
    std::int64_t before;
  };
  std::map<std::string, std::vector<Mapping>> files;
  explicit OffsetMap(const Edits &edits) {
    for (const auto &[key, edit] : edits) {
      auto &list = files[key.path];
      const auto before =
          list.empty() ? 0
                       : list.back().before + std::int64_t(list.back().edit->text.size()) -
                             std::int64_t(list.back().edit->use.end - list.back().edit->use.begin);
      list.push_back({&edit, before});
    }
  }
  std::optional<Key> shifted(const Key &original, bool secondary = false) const {
    const auto file = files.find(original.path);
    if (file == files.end())
      return original;
    const auto &list = file->second;
    auto next = std::lower_bound(
        list.begin(), list.end(), original.begin,
        [](const auto &mapping, std::size_t at) { return mapping.edit->use.begin < at; });
    std::int64_t delta = 0;
    if (next != list.begin()) {
      const auto &previous = *std::prev(next);
      if (previous.edit->use.end > original.begin)
        return {};
      delta = previous.before + std::int64_t(previous.edit->text.size()) -
              std::int64_t(previous.edit->use.end - previous.edit->use.begin);
    }
    if (next != list.end() && next->edit->use == original) {
      const auto part = secondary ? next->edit->secondary : next->edit->primary;
      const auto begin = static_cast<std::size_t>(std::int64_t(original.begin) + next->before);
      return Key{original.path, begin + part.begin, begin + part.end};
    }
    if (next != list.end() && next->edit->use.begin < original.end)
      return {};
    return Key{original.path, static_cast<std::size_t>(std::int64_t(original.begin) + delta),
               static_cast<std::size_t>(std::int64_t(original.end) + delta)};
  }
};
using Relation = std::pair<Key, Key>;
std::set<Relation> relations(const Graph &graph) {
  std::set<Relation> result;
  for (const auto &edge : graph.edges)
    result.emplace(edge.use, edge.target);
  return result;
}
} // namespace
static bool identifier(std::string_view name) {
  if (name.empty() || name.size() > 128 || name == "_")
    return false;
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (!letter(name.front()) ||
      !std::ranges::all_of(name, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); }))
    return false;
  return true;
}
static bool grammarWord(std::string_view name) {
  constexpr std::string_view words[]{"fn",    "export", "from",  "import", "as",    "type",
                                     "enum",  "error",  "with",  "if",     "else",  "match",
                                     "catch", "fail",   "try",   "await",  "spawn", "parallel",
                                     "on",    "true",   "false", "and",    "or",    "not"};
  return std::ranges::find(words, name) != std::end(words);
}
bool validRenameName(std::string_view name) {
  if (!identifier(name))
    return false;
  constexpr std::string_view reserved[] = {
      "fn",          "export",    "from", "import", "as",        "type",       "enum",    "error",
      "with",        "if",        "else", "match",  "catch",     "fail",       "try",     "await",
      "spawn",       "parallel",  "on",   "true",   "false",     "and",        "or",      "not",
      "main",        "Tensor",    "Bool", "String", "TaskError", "FaultKind",  "devices", "map",
      "parallelMap", "reduceSum", "relu", "matmul", "length",    "byteLength", "shape",   "array",
      "tensor",      "int",       "uint", "float"};
  if (std::ranges::find(reserved, name) != std::end(reserved))
    return false;
  return std::ranges::none_of(numericTypes, [&](const auto &type) { return type.name == name; });
}
std::expected<RenameResult, std::string>
planRename(const std::vector<std::string> &roots, const std::string &workspace,
           std::span<const ModulePackage> packages, std::span<const SourceOverlay> overlays,
           const std::string &path, std::size_t offset, std::optional<std::string> newName,
           CheckOptions options, const SourceGraphLoader &loader, const WritableSource &writable) {
  if (newName && !validRenameName(*newName))
    return std::unexpected("Invalid or reserved rename identifier");
  if (!containsPath(workspace, path))
    return std::unexpected("Rename requires a containing workspace");
  Snapshots files;
  std::vector<Graph> graphs;
  std::size_t bytes = 0, edges = 0, identifiers = 0;
  for (const auto &root : roots) {
    auto syntax =
        loader ? loader(root, overlays, false) : loadModules(root, {}, {}, packages, overlays);
    if (!syntax || !syntax->module.source)
      return std::unexpected("Cannot load rename source graph");
    bytes += syntax->module.source->text().size();
    if (bytes > 64 * 1024 * 1024)
      return std::unexpected("Rename exceeds the 64 MiB graph budget");
    auto graph = indexGraph(std::move(*syntax), root, files, options);
    if (!graph)
      return std::unexpected(graph.error());
    edges += graph->edges.size();
    identifiers += graph->identifiers.size();
    if (edges > 300000 || identifiers > 500000)
      return std::unexpected("Rename source-index budget exceeded");
    graphs.push_back(std::move(*graph));
  }
  std::optional<Edge> selected;
  for (const auto &graph : graphs)
    for (const auto &edge : graph.edges)
      if (!edge.secondary && edge.use.path == path && edge.use.begin <= offset &&
          offset < edge.use.end) {
        if (selected && selected->target != edge.target)
          return std::unexpected("Rename source has inconsistent bindings across graphs");
        selected = edge;
        break;
      }
  if (!selected)
    return RenameResult{};
  if (writable && !writable(selected->target.path))
    return std::unexpected("Cannot rename a read-only installed package declaration");
  if (!containsPath(workspace, selected->target.path))
    return std::unexpected("Cannot rename a declaration outside the selected workspace");
  const auto selectionFile = files.find(path);
  if (selectionFile == files.end())
    return std::unexpected("Missing rename source snapshot");
  RenameResult result{
      {SourceLocation{selectionFile->second, {selected->use.begin, selected->use.end}}},
      std::string(selectionFile->second->slice({selected->use.begin, selected->use.end})),
      {}};
  if (!identifier(result.placeholder) || std::ranges::any_of(graphs, [&](const auto &graph) {
        return graph.fixedDeclarations.contains(selected->target);
      }))
    return RenameResult{};
  if (!newName || *newName == result.placeholder)
    return result;
  std::set<Key> shorthand;
  for (const auto &graph : graphs) {
    shorthand.insert(graph.shorthand.begin(), graph.shorthand.end());
    std::set<Key> covered;
    for (const auto &edge : graph.edges)
      covered.insert(edge.use);
    for (const auto &[use, word] : graph.identifiers)
      if ((word == result.placeholder || word == *newName) && !covered.contains(use) &&
          !(grammarWord(word) && !graph.expressionNames.contains(use)))
        return std::unexpected(
            "Rename encounters an unmapped identifier with the affected spelling");
  }
  Edits edits;
  for (const auto &graph : graphs)
    for (const auto &edge : graph.edges)
      if (edge.target == selected->target) {
        if (writable && !writable(edge.use.path))
          return std::unexpected("Rename would edit a read-only installed package source");
        if (!containsPath(workspace, edge.use.path))
          return std::unexpected("Rename would edit a source outside the selected workspace");
        const auto old = files.at(edge.use.path)->slice({edge.use.begin, edge.use.end});
        if (old != result.placeholder)
          return std::unexpected("Rename encounters an inconsistent surface spelling");
        Edit edit{edge.use, *newName, {0, newName->size()}, {0, newName->size()}};
        if (shorthand.contains(edge.use)) {
          if (edge.secondary) {
            edit.text = *newName + ":" + std::string(old);
            edit.primary = {newName->size() + 1, edit.text.size()};
            edit.secondary = {0, newName->size()};
          } else {
            edit.text = std::string(old) + ":" + *newName;
            edit.primary = {old.size() + 1, edit.text.size()};
            edit.secondary = {0, old.size()};
          }
        }
        auto [found, inserted] = edits.try_emplace(edge.use, edit);
        if (!inserted && found->second.text != edit.text)
          return std::unexpected("Conflicting rename roles");
      }
  if (edits.size() > 100000)
    return std::unexpected("Rename exceeds edit budget");
  std::map<std::string, std::string> replacements;
  for (auto first = edits.begin(); first != edits.end();) {
    const auto &file = first->first.path;
    auto last = first;
    auto size = std::int64_t(files.at(file)->text().size());
    while (last != edits.end() && last->first.path == file) {
      size += std::int64_t(last->second.text.size()) -
              std::int64_t(last->first.end - last->first.begin);
      ++last;
    }
    if (size < 0 || size >= std::int64_t(SyntaxLimits{}.maxSourceBytes))
      return std::unexpected("Renamed source exceeds the source byte budget");
    auto &output = replacements[file];
    output.reserve(static_cast<std::size_t>(size));
    const auto source = files.at(file)->text();
    std::size_t cursor = 0;
    for (auto at = first; at != last; ++at) {
      if (at->first.begin < cursor)
        return std::unexpected("Overlapping rename replacements");
      output.append(source.substr(cursor, at->first.begin - cursor));
      output += at->second.text;
      cursor = at->first.end;
    }
    output.append(source.substr(cursor));
    first = last;
  }
  std::map<std::string, std::string> proposed;
  for (const auto &overlay : overlays)
    proposed.emplace(overlay.path, overlay.text);
  for (const auto &[file, text] : replacements)
    proposed.insert_or_assign(file, text);
  const OffsetMap offsets(edits);
  Snapshots afterFiles;
  bytes = edges = identifiers = 0;
  for (const auto &before : graphs) {
    // Each graph needs only its own replacements; workspace-wide edits must not
    // consume one module graph's overlay file/byte budget.
    std::vector<SourceOverlay> updated;
    for (const auto &file : before.sources)
      if (const auto overlay = proposed.find(file); overlay != proposed.end())
        updated.push_back({file, overlay->second});
    auto syntax = loader ? loader(before.root, updated, false)
                         : loadModules(before.root, {}, {}, packages, updated);
    if (!syntax || !syntax->module.source)
      return std::unexpected("Cannot load proposed rename");
    bytes += syntax->module.source->text().size();
    if (bytes > 64 * 1024 * 1024)
      return std::unexpected("Proposed rename exceeds graph budget");
    auto after = indexGraph(std::move(*syntax), before.root, afterFiles, options);
    if (!after)
      return std::unexpected("Proposed rename does not type-check: " + after.error());
    edges += after->edges.size();
    identifiers += after->identifiers.size();
    if (edges > 300000 || identifiers > 500000)
      return std::unexpected("Proposed rename source-index budget exceeded");
    std::set<Relation> expected;
    for (const auto &edge : before.edges) {
      auto use = offsets.shifted(edge.use, edge.secondary), target = offsets.shifted(edge.target);
      if (!use || !target)
        return std::unexpected("Rename overlaps a semantic source range");
      expected.emplace(*use, *target);
    }
    if (expected != relations(*after))
      return std::unexpected("Rename would change name resolution or capture another binding");
  }
  if (afterFiles.size() != files.size())
    return std::unexpected("Rename changed the source graph");
  for (const auto &[file, source] : files) {
    const auto after = afterFiles.find(file);
    const auto changed = replacements.find(file);
    const auto expected =
        changed == replacements.end() ? source->text() : std::string_view(changed->second);
    if (after == afterFiles.end() || after->second->text() != expected)
      return std::unexpected("Sources changed during rename validation");
    if (!proposed.contains(file) || replacements.contains(file)) {
      const bool open =
          std::ranges::any_of(overlays, [&](const auto &overlay) { return overlay.path == file; });
      if (!open) {
        auto disk = loadSource(file);
        if (!disk || (*disk)->text() != source->text())
          return std::unexpected("Disk source changed during rename");
      }
    }
  }
  for (const auto &[use, edit] : edits)
    result.edits.push_back({{files.at(use.path), {use.begin, use.end}}, edit.text});
  return result;
}
} // namespace kalandra::lsp
