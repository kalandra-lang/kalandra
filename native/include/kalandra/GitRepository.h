#pragma once
#include <algorithm>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
namespace kalandra {
enum class GitTransport { Local, HTTPS, LoopbackHTTP };
// One strict spelling is used for both manifest identities and snapshot keys.
// Remote credentials, helpers, percent escapes and ambiguous URL paths are not
// supported. HTTP is restricted to the numeric loopback development server.
inline std::expected<GitTransport, std::string> gitTransport(std::string_view repository) {
  const auto invalid = [] {
    return std::unexpected(std::string("dependency git requires a normalized absolute local path, "
                                       "HTTPS URL, or http://127.0.0.1:<port>/ URL"));
  };
  if (repository.empty() || repository.size() > 4096 ||
      repository.find_first_of("\r\n") != std::string_view::npos ||
      repository.find('\0') != std::string_view::npos)
    return invalid();
#ifdef _WIN32
  // Windows local identities use normalized drive-qualified paths. Reject
  // UNC/device paths so the local transport cannot access a network share.
  if (repository.size() >= 3 &&
      ((repository[0] >= 'A' && repository[0] <= 'Z') ||
       (repository[0] >= 'a' && repository[0] <= 'z')) &&
      repository[1] == ':' && repository[2] == '\\') {
    const std::filesystem::path path(repository);
    if (!path.is_absolute() || path.lexically_normal().string() != repository ||
        repository.find(':', 2) != std::string_view::npos)
      return invalid();
    return GitTransport::Local;
  }
#else
  if (repository.starts_with('/')) {
    if (std::filesystem::path(repository).lexically_normal().string() != repository)
      return invalid();
    return GitTransport::Local;
  }
#endif
  const bool secure = repository.starts_with("https://");
  if (!secure && !repository.starts_with("http://"))
    return invalid();
  const auto rest = repository.substr(secure ? 8 : 7);
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos)
    return invalid();
  auto host = rest.substr(0, slash);
  const auto colon = host.find(':');
  if (colon != std::string_view::npos) {
    const auto port = host.substr(colon + 1);
    if (port.empty() || port.size() > 5 || port.front() == '0')
      return invalid();
    unsigned number = 0;
    for (char c : port) {
      if (c < '0' || c > '9')
        return invalid();
      number = number * 10 + unsigned(c - '0');
    }
    if (number > 65535 || (secure && number == 443))
      return invalid();
    host = host.substr(0, colon);
  }
  if (host.empty() || (!secure && (host != "127.0.0.1" || colon == std::string_view::npos)))
    return invalid();
  for (std::size_t start = 0; start < host.size();) {
    const auto end = host.find('.', start);
    const auto label = host.substr(start, end == std::string_view::npos ? end : end - start);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-' ||
        !std::ranges::all_of(label, [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        }))
      return invalid();
    if (end == std::string_view::npos)
      break;
    start = end + 1;
    if (start == host.size())
      return invalid();
  }
  const auto path = rest.substr(slash + 1);
  if (path.empty())
    return invalid();
  for (std::size_t start = 0; start < path.size();) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string_view::npos ? end : end - start);
    if (part.empty() || part == "." || part == ".." || !std::ranges::all_of(part, [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '~';
        }))
      return invalid();
    if (end == std::string_view::npos)
      break;
    start = end + 1;
    if (start == path.size())
      return invalid();
  }
  return secure ? GitTransport::HTTPS : GitTransport::LoopbackHTTP;
}
} // namespace kalandra
