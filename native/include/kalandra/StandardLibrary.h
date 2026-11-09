#pragma once
#include <optional>
#include <string_view>
namespace kalandra {
enum class StandardModule { Io, Format, Process };
inline std::optional<StandardModule> standardModule(std::string_view name) {
  if (name == "std/io")
    return StandardModule::Io;
  if (name == "std/fmt")
    return StandardModule::Format;
  if (name == "std/process")
    return StandardModule::Process;
  return {};
}
inline std::string_view standardModulePath(StandardModule module) {
  switch (module) {
  case StandardModule::Io:
    return "kalandra-stdlib:std/io.k";
  case StandardModule::Format:
    return "kalandra-stdlib:std/fmt.k";
  case StandardModule::Process:
    return "kalandra-stdlib:std/process.k";
  }
  return {};
}
// Read-only compiler declarations. Intrinsic bodies are provided by lowering;
// the unresolved names deliberately fail ordinary source checking unless the
// module loader has supplied its separate standard-module identity.
inline std::string_view standardModuleSource(StandardModule module) {
  switch (module) {
  case StandardModule::Io:
    return R"k(export error IoError:
  System:
    code: i32
    message: String
  InvalidEncoding
  LineTooLong:
    limit: u64

export enum Line:
  End
  Text:
    text: String

export fn print(text: String) -> unit ! IoError:
  return __kalandra_console_print(text)
export fn println(text: String) -> unit ! IoError:
  return __kalandra_console_println(text)
export fn eprint(text: String) -> unit ! IoError:
  return __kalandra_console_eprint(text)
export fn eprintln(text: String) -> unit ! IoError:
  return __kalandra_console_eprintln(text)
export fn readLine(maxBytes: u64) -> Line ! IoError:
  return __kalandra_console_read_line(maxBytes)
)k";
  case StandardModule::Process:
    return R"k(export type ExitStatus:
  code: u8
)k";
  case StandardModule::Format:
    return R"k(export fn format(template: String) -> String:
  return __kalandra_format(template)
)k";
  }
  return {};
}
} // namespace kalandra
