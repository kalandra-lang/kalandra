#include "Completion.h"
#include "Edits.h"
#include "Hover.h"
#include "Rename.h"
#include "SemanticQuery.h"
#include "Signature.h"
#include "Workspace.h"
#include "kalandra/Diagnostics.h"
#include "kalandra/Formatter.h"
#include "kalandra/PackageInstall.h"
#include "kalandra/Project.h"
#include "kalandra/Semantics.h"
#include "kalandra/Utf8.h"
#include "kalandra/Version.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace j = llvm::json;
namespace fs = std::filesystem;
using namespace kalandra;
namespace {
constexpr std::size_t maxFrame = 32 * 1024 * 1024, maxHeader = 8192;
std::string encode(const j::Value &value) {
  std::string text;
  llvm::raw_string_ostream stream(text);
  stream << value;
  return text;
}
void send(j::Value value) {
  auto text = encode(value);
  std::cout << "Content-Length: " << text.size() << "\r\n\r\n" << text << std::flush;
}
void error(j::Value id, int code, std::string message) {
  send(j::Object{{"jsonrpc", "2.0"},
                 {"id", std::move(id)},
                 {"error", j::Object{{"code", code}, {"message", std::move(message)}}}});
}
void log(std::string message) {
  if (!j::isUTF8(message))
    message = j::fixUTF8(message);
  send(j::Object{{"jsonrpc", "2.0"},
                 {"method", "window/logMessage"},
                 {"params", j::Object{{"type", 2}, {"message", std::move(message)}}}});
}
// Empty optional is clean EOF. Invalid framing is fatal: no reliable next boundary.
std::optional<std::string> readFrame() {
  std::string header;
  char byte;
  while (std::cin.get(byte)) {
    if (static_cast<unsigned char>(byte) > 127 || header.size() == maxHeader)
      throw std::runtime_error("invalid or oversized protocol header");
    header += byte;
    if (header.ends_with("\r\n\r\n"))
      break;
  }
  if (header.empty() && std::cin.eof())
    return {};
  if (!header.ends_with("\r\n\r\n"))
    throw std::runtime_error("truncated protocol header");
  std::optional<std::size_t> length;
  for (std::size_t at = 0; at + 2 < header.size();) {
    const auto end = header.find("\r\n", at);
    auto line = std::string_view(header).substr(at, end - at);
    at = end + 2;
    if (line.empty())
      break;
    const auto colon = line.find(':');
    if (colon == line.npos)
      throw std::runtime_error("invalid protocol header field");
    std::string name(line.substr(0, colon));
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
      return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c);
    });
    auto value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
      value.remove_suffix(1);
    if (name == "content-length") {
      std::size_t number = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      if (length || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          !number || number > maxFrame)
        throw std::runtime_error("invalid protocol content length");
      length = number;
    } else if (name == "content-type") {
      std::string normalized;
      for (unsigned char c : value)
        if (c != ' ' && c != '\t')
          normalized += c >= 'A' && c <= 'Z' ? char(c + 32) : char(c);
      const auto charset = normalized.find("charset=");
      if (charset != normalized.npos) {
        auto encoding = std::string_view(normalized).substr(charset + 8);
        encoding = encoding.substr(0, encoding.find(';'));
        if (encoding.size() >= 2 && encoding.front() == '"' && encoding.back() == '"') {
          encoding.remove_prefix(1);
          encoding.remove_suffix(1);
        }
        if (encoding != "utf-8" && encoding != "utf8")
          throw std::runtime_error("protocol charset must be UTF-8");
      }
    }
  }
  if (!length)
    throw std::runtime_error("missing protocol content length");
  std::string body(*length, '\0');
  if (!std::cin.read(body.data(), static_cast<std::streamsize>(*length)))
    throw std::runtime_error("truncated protocol body");
  return body;
}
bool boundedJson(std::string_view text) {
  std::size_t depth = 0;
  bool quoted = false, escape = false;
  for (char c : text) {
    if (quoted) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '[' || c == '{') {
      if (++depth > 128)
        return false;
    } else if ((c == ']' || c == '}') && depth)
      --depth;
  }
  return true;
}
int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
std::optional<std::string> pathFromUri(llvm::StringRef ref, bool source = true) {
  std::string_view uri(ref.data(), ref.size());
  if (!uri.starts_with("file://"))
    return {};
  uri.remove_prefix(7);
  if (uri.starts_with("localhost/"))
    uri.remove_prefix(9);
  if (uri.empty() || uri.front() != '/' || uri.find_first_of("?#") != uri.npos)
    return {};
  std::string path;
  for (std::size_t at = 0; at < uri.size(); ++at) {
    if (uri[at] == '%') {
      if (uri.size() - at < 3 || hex(uri[at + 1]) < 0 || hex(uri[at + 2]) < 0)
        return {};
      path += char(hex(uri[at + 1]) * 16 + hex(uri[at + 2]));
      at += 2;
    } else
      path += uri[at];
  }
  if (!validUtf8(path) || path.find_first_of(std::string("\0\r\n", 3)) != path.npos ||
      (source && fs::path(path).extension() != ".k"))
    return {};
#ifdef _WIN32
  if (path.size() < 4 || path[0] != '/' || path[2] != ':' || path[3] != '/' ||
      !((path[1] >= 'a' && path[1] <= 'z') || (path[1] >= 'A' && path[1] <= 'Z')) ||
      path.find('\\') != path.npos || path.find(':', 3) != path.npos)
    return {};
  path.erase(0, 1);
#endif
  std::error_code ec;
  const auto canonical = fs::weakly_canonical(fs::path(path), ec);
  if (ec || canonical.empty())
    return {};
  return canonical.string();
}
std::string uriFromPath(std::string_view path) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string uri = "file://";
#ifdef _WIN32
  const auto portable = fs::path(path).generic_string();
  path = portable;
  uri += '/';
#endif
  for (unsigned char c : path) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == ':')
      uri += char(c);
    else {
      uri += '%';
      uri += digits[c >> 4];
      uri += digits[c & 15];
    }
  }
  return uri;
}
std::optional<std::int64_t> protocolInteger(const j::Value *value) {
  // Bound before LLVM's floating-number-to-integer conversion.
  const auto number = value ? value->getAsNumber() : std::nullopt;
  if (!number || !std::isfinite(*number) || *number < std::numeric_limits<std::int32_t>::min() ||
      *number > std::numeric_limits<std::int32_t>::max())
    return {};
  return value->getAsInteger();
}
std::optional<lsp::Position> editPosition(const j::Object *position) {
  const auto line = position ? protocolInteger(position->get("line")) : std::nullopt;
  const auto character = position ? protocolInteger(position->get("character")) : std::nullopt;
  if (!line || !character || *line < 0 || *character < 0)
    return {};
  return lsp::Position{static_cast<std::size_t>(*line), static_cast<std::size_t>(*character)};
}
struct Document {
  std::string uri, text;
  std::int64_t version;
};
class Session {
  std::map<std::string, Document> documents;
  std::map<std::string, std::string> published;
  std::size_t bytes = 0;
  std::optional<NativeProject> project;
  CheckOptions options;
  std::vector<std::string> workspaces;

public:
  bool initialized = false, shutdown = false, hoverMarkdown = false, signatureOffsets = false;
  bool renameDocuments = false, renamePrepare = false;
  Session(std::optional<NativeProject> p, bool placement) : project(std::move(p)) {
    options.placement = placement;
  }
  bool writable(std::string_view path) const {
    return !project ||
           !lsp::containsPath((fs::path(project->manifest).parent_path() / ".kalandra").string(),
                              path);
  }
  std::expected<ParseResult, std::string> loadGraph(const std::string &root,
                                                    std::span<const SourceOverlay> overlays,
                                                    bool editorRecovery = false) const {
    SyntaxLimits syntaxLimits;
    syntaxLimits.editorRecovery = editorRecovery;
    if (!project || project->dependencies.empty())
      return loadModules(root, syntaxLimits, {},
                         project ? project->packages : std::vector<ModulePackage>{}, overlays);
    // The configured manifest is a startup snapshot. A changed file requires a
    // restart; locks and source integrity are verified afresh, with no installer.
    auto current = loadNativeProject(project->manifest);
    if (!current || current->manifestContents != project->manifestContents)
      return std::unexpected(
          "Project manifest changed or is unavailable; restart the language server");
    auto installed = loadInstalledNativeProject(*project);
    if (!installed)
      return std::unexpected(installed.error());
    std::vector<SourceOverlay> editable;
    for (const auto &overlay : overlays) {
      if (writable(overlay.path)) {
        editable.push_back(overlay);
      } else {
        auto disk = loadSource(overlay.path);
        if (!disk || (*disk)->text() != overlay.text)
          return std::unexpected(
              "Installed package sources are read-only; edit the original library instead");
      }
    }
    // Reading an installed source uses its owner in the application graph,
    // never a new workspace node with the application's dependency aliases.
    if (writable(root))
      installed->nodes.front().entry = root;
    return loadPackageModules(installed->nodes, syntaxLimits, {}, editable);
  }
  lsp::SourceGraphLoader graphLoader() const {
    return [this](const std::string &root, std::span<const SourceOverlay> overlays, bool editing) {
      return loadGraph(root, overlays, editing);
    };
  }
  std::optional<std::string> configureWorkspaces(const j::Object &params) {
    std::set<std::string> roots;
    const auto add = [&](llvm::StringRef uri) {
      auto path = pathFromUri(uri, false);
      std::error_code ec;
      if (!path || !fs::is_directory(*path, ec) || ec)
        return false;
      roots.insert(std::move(*path));
      return true;
    };
    const auto folders = params.get("workspaceFolders");
    const auto root = params.get("rootUri");
    if (folders && !folders->getAsNull()) {
      const auto array = folders->getAsArray();
      if (!array || array->size() > 16)
        return "Expected at most 16 workspace folders";
      for (const auto &value : *array) {
        const auto folder = value.getAsObject();
        const auto uri = folder ? folder->getString("uri") : std::nullopt;
        if (!uri || !add(*uri))
          return "Expected accessible local workspace directory URIs";
      }
    } else if (root && !root->getAsNull()) {
      const auto uri = root->getAsString();
      if (!uri || !add(*uri))
        return "Expected an accessible local rootUri directory";
    } else if (!folders && !root && project) {
      roots.insert(fs::path(project->manifest).parent_path().string());
    }
    workspaces.assign(roots.begin(), roots.end());
    return {};
  }
  std::expected<std::set<std::string>, std::string> searchRoots(const std::string &path) {
    std::set<std::string> roots;
    const auto workspace = lsp::containingWorkspace(workspaces, path);
    if (workspace) {
      auto discovered = lsp::discoverSources(*workspace);
      if (!discovered) {
        return std::unexpected(discovered.error());
      }
      roots.insert(discovered->begin(), discovered->end());
    }
    for (const auto &[file, document] : documents)
      if (writable(file) &&
          (workspace ? lsp::containsPath(*workspace, file) : workspaces.empty() || file == path))
        roots.insert(file);
    if (project && (workspace ? lsp::containsPath(*workspace, project->entry) : workspaces.empty()))
      roots.insert(project->entry);
    return roots;
  }
  void renameRequest(const j::Value &id, const j::Object *params, bool prepare) {
    const auto item = params ? params->getObject("textDocument") : nullptr;
    const auto uri = item ? item->getString("uri") : std::nullopt;
    const auto path = uri ? pathFromUri(*uri) : std::nullopt;
    const auto position = editPosition(params ? params->getObject("position") : nullptr);
    const auto name = params ? params->getString("newName") : std::nullopt;
    if (!path || !position || (!prepare && (!name || !lsp::validRenameName(*name)))) {
      error(id, -32602,
            "Expected a local .k URI, UTF-16 position and valid nonreserved rename identifier");
      return;
    }
    const auto respond = [&](j::Value result) {
      send(j::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };
    const auto open = documents.find(*path);
    if (open == documents.end() || open->second.uri != *uri) {
      respond(nullptr);
      return;
    }
    const auto offset = lsp::textOffset(open->second.text, *position);
    if (!offset) {
      respond(nullptr);
      return;
    }
    if (!prepare && !renameDocuments) {
      error(id, -32803, "Rename requires client support for versioned workspace documentChanges");
      return;
    }
    const auto workspace = lsp::containingWorkspace(workspaces, *path);
    if (!workspace) {
      error(id, -32803, "Rename requires a configured containing workspace");
      return;
    }
    auto roots = searchRoots(*path);
    if (!roots) {
      error(id, -32803, roots.error());
      return;
    }
    std::vector<SourceOverlay> overlays;
    for (const auto &[file, document] : documents)
      overlays.push_back({file, document.text});
    const std::vector<ModulePackage> empty;
    auto plan =
        lsp::planRename(std::vector<std::string>(roots->begin(), roots->end()), *workspace,
                        project ? project->packages : empty, overlays, *path, *offset,
                        prepare ? std::nullopt : std::optional{std::string(*name)}, options,
                        graphLoader(), [this](std::string_view file) { return writable(file); });
    if (!plan) {
      error(id, -32803, plan.error());
      return;
    }
    if (!plan->selection) {
      respond(nullptr);
      return;
    }
    if (prepare) {
      const Diagnostic location{"", "", plan->selection->span};
      auto encoded = j::parse(renderDiagnosticsJson(plan->selection->source.get(), {&location, 1}));
      if (!encoded) {
        llvm::consumeError(encoded.takeError());
        error(id, -32603, "Cannot encode rename selection");
        return;
      }
      const auto &diagnostic = *(*encoded->getAsObject()->getArray("diagnostics"))[0].getAsObject();
      respond(j::Object{{"range", *diagnostic.get("range")}, {"placeholder", plan->placeholder}});
      return;
    }
    auto currentRoots = searchRoots(*path);
    if (!currentRoots || *currentRoots != *roots) {
      error(id, -32803, "Workspace sources changed during rename");
      return;
    }
    std::map<std::string, std::vector<const lsp::SourceEdit *>> groups;
    for (const auto &edit : plan->edits)
      groups[edit.location.source->path()].push_back(&edit);
    j::Array changes;
    for (const auto &[file, edits] : groups) {
      std::vector<Diagnostic> locations;
      for (const auto *edit : edits)
        locations.push_back({"", "", edit->location.span});
      auto encoded =
          j::parse(renderDiagnosticsJson(edits.front()->location.source.get(), locations));
      if (!encoded) {
        llvm::consumeError(encoded.takeError());
        error(id, -32603, "Cannot encode rename edits");
        return;
      }
      const auto &diagnostics = *encoded->getAsObject()->getArray("diagnostics");
      j::Array textEdits;
      for (std::size_t i = 0; i < edits.size(); ++i)
        textEdits.push_back(j::Object{{"range", *diagnostics[i].getAsObject()->get("range")},
                                      {"newText", edits[i]->text}});
      const auto opened = documents.find(file);
      j::Object document{
          {"uri", opened == documents.end() ? uriFromPath(file) : opened->second.uri},
          {"version",
           opened == documents.end() ? j::Value(nullptr) : j::Value(opened->second.version)}};
      changes.push_back(
          j::Object{{"textDocument", std::move(document)}, {"edits", std::move(textEdits)}});
    }
    respond(j::Object{{"documentChanges", std::move(changes)}});
  }
  void refresh() {
    std::vector<SourceOverlay> overlays;
    std::set<std::string> roots;
    for (const auto &[path, document] : documents) {
      overlays.push_back({path, document.text});
      roots.insert(path);
    }
    if (project && !documents.empty())
      roots.insert(project->entry);
    std::map<std::string, j::Array> diagnostics;
    std::map<std::string, std::set<std::string>> seen;
    for (const auto &[path, doc] : documents)
      diagnostics.try_emplace(path);
    for (const auto &root : roots) {
      auto syntax = loadGraph(root, overlays);
      if (!syntax) {
        log("Cannot check " + root + ": " + syntax.error());
        continue;
      }
      auto checked = check(std::move(*syntax), {}, options);
      const bool synthetic = checked.source && checked.source->text().empty() &&
                             std::ranges::all_of(checked.diagnostics, [](const auto &d) {
                               return d.code.starts_with("M");
                             });
      auto report = j::parse(
          renderDiagnosticsJson(synthetic ? nullptr : checked.source.get(), checked.diagnostics));
      if (!report)
        throw std::runtime_error(llvm::toString(report.takeError()));
      for (const auto &value : *report->getAsObject()->getArray("diagnostics")) {
        const auto &d = *value.getAsObject();
        const auto file = d.getString("file");
        const auto range = d.getObject("range");
        if (!file || !range) {
          log(std::string(*d.getString("message")));
          continue;
        }
        j::Value output = j::Object{{"range", *d.get("range")},
                                    {"severity", 1},
                                    {"source", "kalandra"},
                                    {"code", *d.get("code")},
                                    {"message", *d.get("message")}};
        const auto path = std::string(*file);
        if (seen[path].insert(encode(output)).second)
          diagnostics[path].push_back(std::move(output));
      }
    }
    for (const auto &[path, uri] : published)
      diagnostics.try_emplace(path);
    std::map<std::string, std::string> next;
    for (auto &[path, list] : diagnostics) {
      auto open = documents.find(path);
      const bool active = open != documents.end() || !list.empty();
      const auto previous = published.find(path);
      const auto uri = open != documents.end()                  ? open->second.uri
                       : !active && previous != published.end() ? previous->second
                                                                : uriFromPath(path);
      if (previous != published.end() && previous->second != uri)
        send(j::Object{
            {"jsonrpc", "2.0"},
            {"method", "textDocument/publishDiagnostics"},
            {"params", j::Object{{"uri", previous->second}, {"diagnostics", j::Array{}}}}});
      j::Object params{{"uri", uri}, {"diagnostics", std::move(list)}};
      if (open != documents.end())
        params["version"] = open->second.version;
      // Retain only paths with active diagnostics or open buffers for later clearing.
      if (active)
        next.emplace(path, uri);
      send(j::Object{{"jsonrpc", "2.0"},
                     {"method", "textDocument/publishDiagnostics"},
                     {"params", std::move(params)}});
    }
    published = std::move(next);
  }
  void formattingRequest(const j::Value &id, const j::Object *params) {
    const auto item = params ? params->getObject("textDocument") : nullptr;
    const auto uri = item ? item->getString("uri") : std::nullopt;
    const auto path = uri ? pathFromUri(*uri) : std::nullopt;
    const auto format = params ? params->getObject("options") : nullptr;
    const auto tabSize = format ? protocolInteger(format->get("tabSize")) : std::nullopt;
    const auto spaces = format ? format->getBoolean("insertSpaces") : std::nullopt;
    if (!path || !tabSize || *tabSize <= 0 || !spaces) {
      error(id, -32602, "Expected a local .k URI and formatting options");
      return;
    }
    j::Array edits;
    const auto open = documents.find(*path);
    if (open != documents.end() && open->second.uri == *uri && writable(*path)) {
      const auto source = std::make_shared<const SourceFile>(*path, open->second.text);
      auto result = formatSource(source);
      if (!result.text && !result.diagnostics.empty() && result.diagnostics[0].code == "F001") {
        error(id, -32803, result.diagnostics[0].message);
        return;
      }
      if (result.text && *result.text != open->second.text) {
        const Diagnostic location{"", "", {0, open->second.text.size()}};
        auto encoded = j::parse(renderDiagnosticsJson(source.get(), {&location, 1}));
        if (!encoded) {
          llvm::consumeError(encoded.takeError());
          error(id, -32603, "Cannot encode formatting range");
          return;
        }
        const auto &diagnostic =
            *(*encoded->getAsObject()->getArray("diagnostics"))[0].getAsObject();
        edits.push_back(j::Object{{"range", *diagnostic.get("range")}, {"newText", *result.text}});
      }
    }
    send(j::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(edits)}});
  }
  void signatureRequest(const j::Value &id, const j::Object *params) {
    const auto item = params ? params->getObject("textDocument") : nullptr;
    const auto uri = item ? item->getString("uri") : std::nullopt;
    const auto path = uri ? pathFromUri(*uri) : std::nullopt;
    const auto position = editPosition(params ? params->getObject("position") : nullptr);
    if (!path || !position) {
      error(id, -32602, "Expected a local .k URI and UTF-16 position");
      return;
    }
    const auto respond = [&](j::Value result) {
      send(j::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };
    const auto open = documents.find(*path);
    if (open == documents.end() || open->second.uri != *uri) {
      respond(nullptr);
      return;
    }
    const auto offset = lsp::textOffset(open->second.text, *position);
    if (!offset) {
      respond(nullptr);
      return;
    }
    std::vector<SourceOverlay> overlays;
    for (const auto &[file, document] : documents)
      overlays.push_back({file, document.text});
    const std::vector<ModulePackage> empty;
    auto info =
        lsp::signatureAt(*path, open->second.text, *offset, project ? project->packages : empty,
                         std::move(overlays), options, graphLoader());
    if (!info) {
      respond(nullptr);
      return;
    }
    j::Array parameters;
    std::size_t next = info->label.find('(') + 1;
    const auto units = [](std::string_view text) {
      std::int64_t result = 0;
      for (const unsigned char byte : text)
        if ((byte & 0xc0) != 0x80)
          result += byte >= 0xf0 ? 2 : 1;
      return result;
    };
    for (const auto &label : info->parameters) {
      const auto begin = info->label.find(label, next);
      if (begin == std::string::npos) {
        respond(nullptr);
        return;
      }
      next = begin + label.size();
      if (signatureOffsets)
        parameters.push_back(
            j::Object{{"label", j::Array{units(std::string_view(info->label).substr(0, begin)),
                                         units(std::string_view(info->label).substr(0, next))}}});
      else
        parameters.push_back(j::Object{{"label", label}});
    }
    j::Object result{{"signatures", j::Array{j::Object{{"label", info->label},
                                                       {"parameters", std::move(parameters)}}}},
                     {"activeSignature", 0}};
    if (info->activeParameter)
      result["activeParameter"] = static_cast<std::int64_t>(*info->activeParameter);
    respond(std::move(result));
  }
  void completionRequest(const j::Value &id, const j::Object *params) {
    const auto item = params ? params->getObject("textDocument") : nullptr;
    const auto uri = item ? item->getString("uri") : std::nullopt;
    const auto path = uri ? pathFromUri(*uri) : std::nullopt;
    const auto position = editPosition(params ? params->getObject("position") : nullptr);
    if (!path || !position) {
      error(id, -32602, "Expected a local .k URI and UTF-16 position");
      return;
    }
    const auto respond = [&](j::Value result) {
      send(j::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };
    const auto open = documents.find(*path);
    if (open == documents.end() || open->second.uri != *uri) {
      respond(nullptr);
      return;
    }
    const auto offset = lsp::textOffset(open->second.text, *position);
    if (!offset) {
      respond(nullptr);
      return;
    }
    std::vector<SourceOverlay> overlays;
    for (const auto &[file, document] : documents)
      overlays.push_back({file, document.text});
    const std::vector<ModulePackage> empty;
    auto info =
        lsp::completeNames(*path, open->second.text, *offset, project ? project->packages : empty,
                           std::move(overlays), options, graphLoader());
    j::Array items;
    if (!info.names.empty()) {
      const SourceFile source(*path, open->second.text);
      const Diagnostic location{"", "", info.replacement};
      auto encoded = j::parse(renderDiagnosticsJson(&source, {&location, 1}));
      if (!encoded) {
        llvm::consumeError(encoded.takeError());
        error(id, -32603, "Cannot encode completion range");
        return;
      }
      const auto &diagnostic = *(*encoded->getAsObject()->getArray("diagnostics"))[0].getAsObject();
      for (const auto &name : info.names) {
        const auto kind = name.kind == ScopeNameKind::Function   ? 3
                          : name.kind == ScopeNameKind::Type     ? 22
                          : name.kind == ScopeNameKind::Field    ? 5
                          : name.kind == ScopeNameKind::EnumCase ? 20
                          : name.kind == ScopeNameKind::File     ? 17
                          : name.kind == ScopeNameKind::Folder   ? 19
                          : name.kind == ScopeNameKind::Keyword  ? 14
                                                                 : 6;
        items.push_back(j::Object{
            {"label", name.name},
            {"kind", kind},
            {"insertTextFormat", 1},
            {"detail", name.kind == ScopeNameKind::Parameter  ? "parameter"
                       : name.kind == ScopeNameKind::Variable ? "binding"
                       : name.kind == ScopeNameKind::Function ? "function"
                       : name.kind == ScopeNameKind::Field    ? "record field"
                       : name.kind == ScopeNameKind::EnumCase ? "enum case"
                       : name.kind == ScopeNameKind::File     ? "source file"
                       : name.kind == ScopeNameKind::Folder   ? "directory"
                       : name.kind == ScopeNameKind::Keyword  ? "expression keyword"
                                                              : "type"},
            {"textEdit", j::Object{{"range", *diagnostic.get("range")},
                                   {"newText", name.name + (info.appendFieldColon ? ": " : "")}}}});
      }
    }
    respond(j::Object{{"isIncomplete", info.incomplete}, {"items", std::move(items)}});
  }
  void symbolRequest(const j::Value &id, const j::Object *params, bool goDefinition,
                     bool findReferences = false) {
    const auto item = params ? params->getObject("textDocument") : nullptr;
    const auto uri = item ? item->getString("uri") : std::nullopt;
    const auto path = uri ? pathFromUri(*uri) : std::nullopt;
    const auto position = editPosition(params ? params->getObject("position") : nullptr);
    const auto context = params ? params->getObject("context") : nullptr;
    const auto includeDeclaration =
        context ? context->getBoolean("includeDeclaration") : std::nullopt;
    if (findReferences && !includeDeclaration) {
      error(id, -32602, "Expected references context.includeDeclaration boolean");
      return;
    }
    if (!path || !position) {
      error(id, -32602, "Expected a local .k URI and UTF-16 position");
      return;
    }
    const auto respond = [&](j::Value result) {
      send(j::Object{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
    };
    const auto open = documents.find(*path);
    if (open == documents.end() || open->second.uri != *uri) {
      respond(nullptr);
      return;
    }
    const auto offset = lsp::textOffset(open->second.text, *position);
    if (!offset) {
      respond(nullptr);
      return;
    }
    std::vector<SourceOverlay> overlays;
    for (const auto &[file, document] : documents)
      overlays.push_back({file, document.text});
    auto syntax = loadGraph(*path, overlays);
    if (!syntax) {
      respond(nullptr);
      return;
    }
    if (findReferences) {
      auto target = lsp::definition(std::move(*syntax), *path, *offset, options);
      if (!target) {
        respond(nullptr);
        return;
      }
      auto roots = searchRoots(*path);
      if (!roots) {
        error(id, -32803, roots.error());
        return;
      }
      std::map<std::tuple<std::string, std::size_t, std::size_t>, lsp::SourceLocation> found;
      std::size_t scanned = 0;
      for (const auto &root : *roots) {
        auto graph = loadGraph(root, overlays);
        if (!graph || !graph->module.source) {
          error(id, -32803, "Cannot search references: a source graph could not be loaded");
          return;
        }
        scanned += graph->module.source->text().size();
        if (scanned > 64 * 1024 * 1024) {
          error(id, -32803, "Reference search exceeds the 64 MiB graph budget");
          return;
        }
        auto locations = lsp::references(std::move(*graph), *target, *includeDeclaration, options);
        if (!locations) {
          error(id, -32803, "Cannot search references: a searched source graph has errors");
          return;
        }
        for (auto &location : *locations) {
          auto key = std::tuple{location.source->path(), location.span.begin, location.span.end};
          found.try_emplace(std::move(key), std::move(location));
        }
        if (found.size() > 100000) {
          error(id, -32803, "Reference search exceeds the 100000 location budget");
          return;
        }
      }
      j::Array result;
      for (const auto &[key, location] : found) {
        const Diagnostic range{"", "", location.span};
        auto encoded = j::parse(renderDiagnosticsJson(location.source.get(), {&range, 1}));
        if (!encoded) {
          llvm::consumeError(encoded.takeError());
          error(id, -32603, "Cannot encode reference range");
          return;
        }
        const auto &diagnostic =
            *(*encoded->getAsObject()->getArray("diagnostics"))[0].getAsObject();
        const auto opened = documents.find(location.source->path());
        result.push_back(
            j::Object{{"uri", opened == documents.end() ? uriFromPath(location.source->path())
                                                        : opened->second.uri},
                      {"range", *diagnostic.get("range")}});
      }
      respond(std::move(result));
      return;
    }
    if (goDefinition) {
      auto target = lsp::definition(std::move(*syntax), *path, *offset, options);
      if (!target) {
        respond(nullptr);
        return;
      }
      const Diagnostic location{"", "", target->span};
      auto encoded = j::parse(renderDiagnosticsJson(target->source.get(), {&location, 1}));
      if (!encoded) {
        llvm::consumeError(encoded.takeError());
        error(id, -32603, "Cannot encode definition range");
        return;
      }
      const auto &diagnostic = *(*encoded->getAsObject()->getArray("diagnostics"))[0].getAsObject();
      const auto opened = documents.find(target->source->path());
      auto targetUri =
          opened == documents.end() ? uriFromPath(target->source->path()) : opened->second.uri;
      respond(j::Object{{"uri", std::move(targetUri)}, {"range", *diagnostic.get("range")}});
      return;
    }
    auto info = lsp::hoverInfo(std::move(*syntax), *path, *offset, options);
    if (!info) {
      respond(nullptr);
      return;
    }
    // Reuse the diagnostic source-span adapter for identical original-file UTF-16 ranges.
    const Diagnostic location{"", "", info->span};
    auto encoded = j::parse(renderDiagnosticsJson(info->source.get(), {&location, 1}));
    if (!encoded) {
      llvm::consumeError(encoded.takeError());
      error(id, -32603, "Cannot encode hover range");
      return;
    }
    const auto &diagnostic = *(*encoded->getAsObject()->getArray("diagnostics"))[0].getAsObject();
    auto text = hoverMarkdown ? "```kalandra\n" + info->text + "\n```" : info->text;
    respond(j::Object{{"contents", j::Object{{"kind", hoverMarkdown ? "markdown" : "plaintext"},
                                             {"value", std::move(text)}}},
                      {"range", *diagnostic.get("range")}});
  }
  void document(llvm::StringRef method, const j::Object *params) {
    const auto item = params ? params->getObject("textDocument") : nullptr;
    const auto uri = item ? item->getString("uri") : std::nullopt;
    const auto path = uri ? pathFromUri(*uri) : std::nullopt;
    if (!path) {
      log("Expected a local file URI for a .k document");
      return;
    }
    auto found = documents.find(*path);
    if (method == "textDocument/didClose") {
      if (found == documents.end() || found->second.uri != *uri)
        return;
      bytes -= found->second.text.size() + 1;
      documents.erase(found);
      refresh();
      return;
    }
    if (method == "textDocument/didSave") {
      if (found != documents.end())
        refresh();
      return;
    }
    const auto version = protocolInteger(item->get("version"));
    if (!version || *version < std::numeric_limits<std::int32_t>::min() ||
        *version > std::numeric_limits<std::int32_t>::max()) {
      log("Expected an integer document version");
      return;
    }
    std::optional<std::string> text;
    const auto oldBytes = found == documents.end() ? 0 : found->second.text.size() + 1;
    if (method == "textDocument/didOpen") {
      if (found != documents.end()) {
        log("Document identity is already open");
        return;
      }
      if (auto value = item->getString("text"))
        text = std::string(*value);
    } else {
      if (found == documents.end() || found->second.uri != *uri) {
        log("Document is not open under this URI");
        return;
      }
      if (*version <= found->second.version)
        return;
      const auto changes = params->getArray("contentChanges");
      if (!changes || changes->size() > lsp::EditLimits{}.maxChanges) {
        log("Missing or excessive document content changes");
        return;
      }
      std::vector<lsp::TextChange> edits;
      for (const auto &change : *changes) {
        const auto event = change.getAsObject();
        const auto replacement = event ? event->getString("text") : std::nullopt;
        if (!replacement) {
          log("Expected replacement text for document edit");
          return;
        }
        lsp::TextChange edit{{}, std::string(*replacement), {}};
        if (event->get("range")) {
          const auto range = event->getObject("range");
          const auto start = editPosition(range ? range->getObject("start") : nullptr);
          const auto end = editPosition(range ? range->getObject("end") : nullptr);
          if (!start || !end) {
            log("Expected a nonnegative UTF-16 document range");
            return;
          }
          edit.range = lsp::Range{*start, *end};
        }
        if (event->get("rangeLength")) {
          const auto length = protocolInteger(event->get("rangeLength"));
          if (!length || *length < 0) {
            log("Invalid document rangeLength");
            return;
          }
          edit.rangeLength = static_cast<std::size_t>(*length);
        }
        edits.push_back(std::move(edit));
      }
      lsp::EditLimits limits;
      limits.maxBytes = SyntaxLimits{}.maxSourceBytes - 1 - (bytes - oldBytes);
      auto applied = lsp::applyChanges(found->second.text, edits, limits);
      if (!applied) {
        log(applied.error());
        return;
      }
      text = std::move(*applied);
    }
    if (!text || text->size() >= SyntaxLimits{}.maxSourceBytes - (bytes - oldBytes) ||
        (found == documents.end() && documents.size() >= ModuleLimits{}.maxFiles)) {
      log("Document text is missing or open-buffer budgets are exceeded");
      return;
    }
    bytes = bytes - oldBytes + text->size() + 1;
    documents.insert_or_assign(*path, Document{std::string(*uri), std::string(*text), *version});
    refresh();
  }
};
} // namespace
int main(int argc, char **argv) {
  if (kalandra::printVersion(argc, argv, "kalandra-lsp"))
    return 0;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  try {
    std::optional<NativeProject> project;
    bool placement = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i];
      if (arg == "--placement" && !placement)
        placement = true;
      else if (arg == "--project" && !project && i + 1 < argc) {
        auto loaded = loadNativeProject(argv[++i]);
        if (!loaded) {
          std::cerr << loaded.error() << '\n';
          return 2;
        }
        project = std::move(*loaded);
      } else {
        std::cerr << "usage: kalandra-lsp [--placement] [--project manifest.toml]\n";
        return 2;
      }
    }
    Session session(std::move(project), placement);
    while (auto body = readFrame()) {
      if (!boundedJson(*body)) {
        error(nullptr, -32700, "JSON nesting limit exceeded");
        continue;
      }
      auto parsed = j::parse(*body);
      if (!parsed) {
        llvm::consumeError(parsed.takeError());
        error(nullptr, -32700, "Invalid JSON");
        continue;
      }
      const auto object = parsed->getAsObject();
      const auto method = object ? object->getString("method") : std::nullopt;
      const auto id = object ? object->get("id") : nullptr;
      const bool request = id != nullptr;
      if (!object || !method || object->getString("jsonrpc") != "2.0" ||
          (id && !protocolInteger(id) && !id->getAsString())) {
        error(nullptr, -32600, "Invalid request");
        continue;
      }
      if (*method == "exit" && !request)
        return session.shutdown ? 0 : 1;
      if (*method == "initialize" && request && !session.initialized && !session.shutdown) {
        if (!object->getObject("params")) {
          error(*id, -32602, "Expected initialize parameters");
          continue;
        }
        const auto params = object->getObject("params");
        if (auto failure = session.configureWorkspaces(*params)) {
          error(*id, -32602, *failure);
          continue;
        }
        const auto capabilities = params->getObject("capabilities");
        const auto document = capabilities ? capabilities->getObject("textDocument") : nullptr;
        const auto workspaceCapabilities =
            capabilities ? capabilities->getObject("workspace") : nullptr;
        const auto workspaceEdit =
            workspaceCapabilities ? workspaceCapabilities->getObject("workspaceEdit") : nullptr;
        session.renameDocuments =
            workspaceEdit && workspaceEdit->getBoolean("documentChanges").value_or(false);
        const auto rename = document ? document->getObject("rename") : nullptr;
        session.renamePrepare = rename && rename->getBoolean("prepareSupport").value_or(false);
        const auto signature = document ? document->getObject("signatureHelp") : nullptr;
        const auto signatureInformation =
            signature ? signature->getObject("signatureInformation") : nullptr;
        const auto parameterInformation =
            signatureInformation ? signatureInformation->getObject("parameterInformation")
                                 : nullptr;
        session.signatureOffsets =
            parameterInformation &&
            parameterInformation->getBoolean("labelOffsetSupport").value_or(false);
        const auto hover = document ? document->getObject("hover") : nullptr;
        const auto formats = hover ? hover->getArray("contentFormat") : nullptr;
        if (formats)
          for (const auto &format : *formats) {
            const auto name = format.getAsString();
            if (name == "markdown" || name == "plaintext") {
              session.hoverMarkdown = name == "markdown";
              break;
            }
          }
        session.initialized = true;
        send(j::Object{
            {"jsonrpc", "2.0"},
            {"id", *id},
            {"result",
             j::Object{
                 {"capabilities",
                  j::Object{{"positionEncoding", "utf-16"},
                            {"hoverProvider", true},
                            {"signatureHelpProvider",
                             j::Object{{"triggerCharacters", j::Array{"(", ","}}}},
                            {"completionProvider",
                             j::Object{{"resolveProvider", false},
                                       {"triggerCharacters", j::Array{".", "{", ",", "/"}}}},
                            {"documentFormattingProvider", true},
                            {"definitionProvider", true},
                            {"referencesProvider", true},
                            {"renameProvider", !session.renameDocuments ? j::Value(false)
                                               : session.renamePrepare
                                                   ? j::Value(j::Object{{"prepareProvider", true}})
                                                   : j::Value(true)},
                            {"workspace", j::Object{{"workspaceFolders",
                                                     j::Object{{"supported", true},
                                                               {"changeNotifications", false}}}}},
                            {"textDocumentSync",
                             j::Object{{"openClose", true},
                                       {"change", 2},
                                       {"save", j::Object{{"includeText", false}}}}}}},
                 {"serverInfo", j::Object{{"name", "kalandra-native"},
                                          {"version", std::string(kalandra::version)}}}}}});
      } else if (!session.initialized) {
        if (request)
          error(*id, -32002, "Server not initialized");
      } else if (session.shutdown) {
        if (request)
          error(*id, -32600, "Server is shut down");
      } else if (*method == "shutdown" && request) {
        session.shutdown = true;
        send(j::Object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", nullptr}});
      } else if ((*method == "textDocument/rename" || *method == "textDocument/prepareRename") &&
                 request) {
        session.renameRequest(*id, object->getObject("params"),
                              *method == "textDocument/prepareRename");
      } else if (*method == "textDocument/formatting" && request) {
        session.formattingRequest(*id, object->getObject("params"));
      } else if (*method == "textDocument/signatureHelp" && request) {
        session.signatureRequest(*id, object->getObject("params"));
      } else if (*method == "textDocument/completion" && request) {
        session.completionRequest(*id, object->getObject("params"));
      } else if ((*method == "textDocument/hover" || *method == "textDocument/definition" ||
                  *method == "textDocument/references") &&
                 request) {
        session.symbolRequest(*id, object->getObject("params"),
                              *method == "textDocument/definition",
                              *method == "textDocument/references");
      } else if (request)
        error(*id, *method == "initialize" ? -32600 : -32601, "Unsupported request");
      else if (*method == "textDocument/didOpen" || *method == "textDocument/didChange" ||
               *method == "textDocument/didClose" || *method == "textDocument/didSave")
        session.document(*method, object->getObject("params"));
    }
    return session.shutdown ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << "kalandra-lsp: " << e.what() << '\n';
    return 2;
  }
}
