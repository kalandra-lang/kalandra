#pragma once
#include "../include/kalandra/FileOperations.h"
#include "String.h"
#include "Tensor.h"
namespace kalandra::runtime {
using FileBytes = CpuTensor<NumericType::UInt8, 1>;
// Private ABI: source File/BufferedReader/BufferedWriter are opaque nominal
// records with one inaccessible backing field. No OS descriptor is exposed.
struct FileDescriptor {
  FileBytes storage;
};
using kalandra::FileAction;
enum class FileMode : std::uint64_t { Read, Write, Create, CreateNew, Append, ReadWrite };
enum class FileKind : std::uint32_t {
  Success,
  End,
  System,
  InvalidEncoding,
  LineTooLong,
  UnexpectedEnd,
  InvalidArgument,
  ResourceLimit
};
struct FileArguments {
  FileDescriptor file;
  StringDescriptor path;
  FileBytes bytes;
  std::uint64_t value = 0, mode = 0;
};
struct FileResult {
  FileKind kind = FileKind::Success;
  std::int32_t code = 0;
  StringDescriptor text;
  FileDescriptor file;
  FileBytes bytes;
  std::uint64_t count = 0;
};
struct FileServiceStats {
  std::uint32_t reserved = 0, pendingClose = 0, workers = 0;
};
// Private diagnostics for qualification, not a source-language API.
FileServiceStats fileServiceStats() noexcept;
class Task;
} // namespace kalandra::runtime
extern "C" {
bool kalandra_file_step_v1(kalandra::runtime::Invocation *, kalandra::runtime::FileResult *,
                           std::uint32_t action, const kalandra::runtime::FileArguments *,
                           void **state) noexcept;
void kalandra_file_v1(kalandra::runtime::Invocation *, kalandra::runtime::FileResult *,
                      std::uint32_t action, const kalandra::runtime::FileArguments *) noexcept;
void kalandra_file_pause_v1(kalandra::runtime::Task *, void *state) noexcept;
void *kalandra_file_scope_enter_v1(kalandra::runtime::Invocation *,
                                   const kalandra::runtime::FileDescriptor *const *,
                                   std::uint64_t count) noexcept;
void kalandra_file_scope_leave_v1(kalandra::runtime::Invocation *, void *scope,
                                  const kalandra::runtime::FileDescriptor *escaping) noexcept;
bool kalandra_file_receive_v1(kalandra::runtime::Invocation *,
                              const kalandra::runtime::FileDescriptor *) noexcept;
}
