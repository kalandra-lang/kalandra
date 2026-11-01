// Resolve the actual executable, independently of argv[0], PATH, symlinks and
// the caller's working directory. String paths in the language bridge are UTF-8.
inline std::expected<std::string, std::error_code> executableDirectoryUtf8() noexcept {
  try {
    std::filesystem::path executable;
#if defined(_WIN32)
    std::vector<wchar_t> buffer(1024);
    for (;;) {
      const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
      if (!length)
        return std::unexpected(std::error_code(GetLastError(), std::system_category()));
      if (length < buffer.size()) {
        executable = std::wstring_view(buffer.data(), length);
        break;
      }
      if (buffer.size() >= 1024 * 1024)
        return std::unexpected(std::make_error_code(std::errc::filename_too_long));
      buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::vector<char> buffer(1024);
    auto length = static_cast<std::uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &length)) {
      if (length > 1024 * 1024)
        return std::unexpected(std::make_error_code(std::errc::filename_too_long));
      buffer.resize(length);
      if (_NSGetExecutablePath(buffer.data(), &length))
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }
    executable = buffer.data();
#else
    std::vector<char> buffer(1024);
    for (;;) {
      const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
      if (length < 0)
        return std::unexpected(std::error_code(errno, std::generic_category()));
      if (static_cast<std::size_t>(length) < buffer.size()) {
        executable = std::string_view(buffer.data(), length);
        break;
      }
      if (buffer.size() >= 1024 * 1024)
        return std::unexpected(std::make_error_code(std::errc::filename_too_long));
      buffer.resize(buffer.size() * 2);
    }
#endif
    std::error_code error;
    const auto resolved = std::filesystem::canonical(executable, error);
    if (error)
      return std::unexpected(error);
    auto directory = resolved.parent_path().u8string();
    return std::string(reinterpret_cast<const char *>(directory.data()), directory.size());
  } catch (const std::bad_alloc &) {
    return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
  } catch (const std::filesystem::filesystem_error &error) {
    return std::unexpected(error.code());
  }
}
