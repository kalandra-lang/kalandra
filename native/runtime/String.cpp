#include "String.h"
#include "../include/kalandra/Utf8.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace kalandra::runtime {
namespace {
bool inspectString(Invocation &context, const StringDescriptor &value,
                   std::uint64_t *scalars = nullptr, Utf8Scalar *selected = nullptr,
                   Utf8Range *range = nullptr) noexcept {
  if (context.poll() != 0 || !context.usable())
    return false;
  const auto invalid = [&] {
    context.fault(3);
    return false;
  };
  if (value.byteLength == 0) {
    if (value.allocated || value.data)
      return invalid();
    if (scalars)
      *scalars = 0;
    if (selected) {
      selected->offset = selected->bytes = 0;
    }
    if (range) {
      range->offset = range->bytes = 0;
      range->valid = range->start == 0 && range->end == 0;
    }
    return true;
  }
  if (value.byteLength < 0 || !value.allocated || value.data != value.allocated)
    return invalid();
  auto backing = context.lease(value.allocated);
  if (!backing || static_cast<std::uint64_t>(value.byteLength) > backing->bytes().size())
    return invalid();
  auto status = validateUtf8(
      std::string_view(value.data, value.byteLength), [&] { return context.poll() == 0; }, scalars,
      selected, range);
  return status == Utf8Status::Valid || (status == Utf8Status::Invalid && invalid());
}
} // namespace
bool validateString(Invocation &context, const StringDescriptor &value) noexcept {
  return inspectString(context, value);
}
static std::int64_t stringLength(Invocation &context, const StringDescriptor &value,
                                 bool bytes) noexcept {
  std::uint64_t count = 0;
  if (!inspectString(context, value, bytes ? nullptr : &count))
    return 0;
  return bytes ? value.byteLength : static_cast<std::int64_t>(count);
}
static void indexString(Invocation &context, StringDescriptor &output,
                        const StringDescriptor &input, std::uint64_t position) noexcept {
  const auto source = input;
  output = {};
  Utf8Scalar selected{position};
  if (!inspectString(context, source, nullptr, &selected))
    return;
  if (!selected.bytes) {
    context.fault(5);
    return;
  }
  if (context.poll() != 0)
    return;
  auto *data = static_cast<char *>(context.allocateRecord(selected.bytes));
  if (!data)
    return;
  std::memcpy(data, source.data + selected.offset, selected.bytes);
  if (context.poll() == 0)
    output = {data, data, static_cast<std::int64_t>(selected.bytes)};
}
static void sliceString(Invocation &context, StringDescriptor &output,
                        const StringDescriptor &input, std::uint64_t start,
                        std::uint64_t end) noexcept {
  const auto source = input;
  output = {};
  Utf8Range range{start, end};
  if (!inspectString(context, source, nullptr, nullptr, &range))
    return;
  if (!range.valid) {
    context.fault(5);
    return;
  }
  if (!range.bytes || context.poll() != 0)
    return;
  auto *data = static_cast<char *>(context.allocateRecord(range.bytes));
  if (!data)
    return;
  for (std::size_t i = 0; i < range.bytes;) {
    if (context.poll() != 0)
      return;
    const auto count = std::min<std::size_t>(1024, range.bytes - i);
    std::memcpy(data + i, source.data + range.offset + i, count);
    i += count;
  }
  if (context.poll() == 0)
    output = {data, data, static_cast<std::int64_t>(range.bytes)};
}
} // namespace kalandra::runtime
extern "C" void kalandra_string_literal_v1(kalandra::runtime::Invocation *context,
                                           kalandra::runtime::StringDescriptor *output,
                                           const char *source, std::uint64_t length) noexcept {
  *output = {};
  if (context->poll() != 0 || !context->usable())
    return;
  if (length > std::numeric_limits<std::int64_t>::max() || (length && !source)) {
    context->fault(3);
    return;
  }
  if (!length)
    return;
  const auto status = kalandra::validateUtf8(std::string_view(source, length),
                                             [&] { return context->poll() == 0; });
  if (status != kalandra::Utf8Status::Valid) {
    if (status == kalandra::Utf8Status::Invalid)
      context->fault(3);
    return;
  }
  auto *data = static_cast<char *>(context->allocateRecord(length));
  if (!data)
    return;
  for (std::uint64_t offset = 0; offset < length;) {
    if (context->poll() != 0)
      return;
    const auto count = std::min<std::uint64_t>(1024, length - offset);
    std::memcpy(data + offset, source + offset, count);
    offset += count;
  }
  if (context->poll() == 0)
    *output = {data, data, static_cast<std::int64_t>(length)};
}
extern "C" void
kalandra_string_validate_v1(kalandra::runtime::Invocation *context,
                            const kalandra::runtime::StringDescriptor *value) noexcept {
  kalandra::runtime::validateString(*context, *value);
}
extern "C" void
kalandra_string_concat_v1(kalandra::runtime::Invocation *context,
                          kalandra::runtime::StringDescriptor *output,
                          const kalandra::runtime::StringDescriptor *left,
                          const kalandra::runtime::StringDescriptor *right) noexcept {
  // Copy descriptors first, permitting output to alias an input descriptor.
  const auto a = *left, b = *right;
  *output = {};
  if (!kalandra::runtime::validateString(*context, a) ||
      !kalandra::runtime::validateString(*context, b))
    return;
  const auto length = kalandra::runtime::combinedStringBytes(a.byteLength, b.byteLength);
  if (!length) {
    context->fault(2);
    return;
  }
  if (!*length)
    return;
  auto *data = static_cast<char *>(context->allocateRecord(*length));
  if (!data)
    return;
  std::uint64_t offset = 0;
  for (const auto &part : {a, b}) {
    for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(part.byteLength);) {
      if (context->poll() != 0)
        return;
      const auto count = std::min<std::uint64_t>(1024, part.byteLength - i);
      std::memcpy(data + offset, part.data + i, count);
      i += count;
      offset += count;
    }
  }
  if (context->poll() == 0)
    *output = {data, data, static_cast<std::int64_t>(*length)};
}
extern "C" std::int64_t
kalandra_string_equal_v1(kalandra::runtime::Invocation *context,
                         const kalandra::runtime::StringDescriptor *left,
                         const kalandra::runtime::StringDescriptor *right) noexcept {
  // Validate both complete operands before length/prefix shortcuts, even aliases.
  if (!kalandra::runtime::validateString(*context, *left) ||
      !kalandra::runtime::validateString(*context, *right))
    return 0;
  if (left->byteLength != right->byteLength)
    return 0;
  for (std::uint64_t i = 0; i < static_cast<std::uint64_t>(left->byteLength);) {
    if (context->poll() != 0)
      return 0;
    const auto count = std::min<std::uint64_t>(1024, left->byteLength - i);
    if (std::memcmp(left->data + i, right->data + i, count))
      return 0;
    i += count;
  }
  return context->poll() == 0 ? 1 : 0;
}
extern "C" std::int64_t
kalandra_string_length_v1(kalandra::runtime::Invocation *context,
                          const kalandra::runtime::StringDescriptor *value) noexcept {
  return kalandra::runtime::stringLength(*context, *value, false);
}
extern "C" std::int64_t
kalandra_string_byte_length_v1(kalandra::runtime::Invocation *context,
                               const kalandra::runtime::StringDescriptor *value) noexcept {
  return kalandra::runtime::stringLength(*context, *value, true);
}
extern "C" void kalandra_string_index_v1(kalandra::runtime::Invocation *context,
                                         kalandra::runtime::StringDescriptor *output,
                                         const kalandra::runtime::StringDescriptor *input,
                                         std::uint64_t position) noexcept {
  kalandra::runtime::indexString(*context, *output, *input, position);
}
extern "C" void kalandra_string_slice_v1(kalandra::runtime::Invocation *context,
                                         kalandra::runtime::StringDescriptor *output,
                                         const kalandra::runtime::StringDescriptor *input,
                                         std::uint64_t start, std::uint64_t end) noexcept {
  kalandra::runtime::sliceString(*context, *output, *input, start, end);
}

extern "C" bool
kalandra_string_retain_v1(kalandra::runtime::Invocation *destination,
                          const kalandra::runtime::Invocation *source,
                          const kalandra::runtime::StringDescriptor *value) noexcept {
  const auto invalid = [&] {
    destination->fault(3);
    return false;
  };
  if (destination->poll() != 0)
    return false;
  if (!destination->usable() || !source->usable() || !value)
    return invalid();
  if (!value->byteLength)
    return (!value->allocated && !value->data) || invalid();
  if (value->byteLength < 0 || !value->allocated || value->allocated != value->data)
    return invalid();
  auto lease = source->lease(value->allocated);
  if (!lease || static_cast<std::uint64_t>(value->byteLength) > lease->bytes().size())
    return invalid();
  const auto status = kalandra::validateUtf8(std::string_view(value->data, value->byteLength),
                                             [&] { return destination->poll() == 0; });
  if (status != kalandra::Utf8Status::Valid)
    return status == kalandra::Utf8Status::Invalid ? invalid() : false;
  return destination->poll() == 0 && destination->retainFrom(*source, value->allocated);
}

extern "C" const void *
kalandra_recursive_string_dependency_v1(kalandra::runtime::Invocation *context,
                                        const kalandra::runtime::StringDescriptor *value,
                                        std::uint64_t active) noexcept {
  if (context->poll() != 0 || !context->usable())
    return nullptr;
  if (active > 1 || (active && !value)) {
    context->fault(3);
    return nullptr;
  }
  if (!active || !kalandra::runtime::validateString(*context, *value))
    return nullptr;
  // Borrowed until publication: makeRecursiveValueFromPointers retains this backing.
  return value->allocated;
}
