#include "Path.h"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace {
using namespace kalandra;
using namespace kalandra::runtime;
bool healthy(Invocation &ctx) noexcept { return !ctx.poll() && ctx.usable(); }
bool checkpoint(Invocation &ctx, std::size_t i) noexcept { return (i & 1023) || healthy(ctx); }
bool separator(char ch, PathStyle style, bool verbatim = false) noexcept {
  return ch == '\\' ? style == PathStyle::Windows : ch == '/' && !verbatim;
}
bool letter(char ch) noexcept { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); }
char lower(char ch) noexcept { return ch >= 'A' && ch <= 'Z' ? char(ch - 'A' + 'a') : ch; }
enum class Anchor { None, Root, Drive, Network, Device, DeviceDrive };
struct Root {
  Anchor anchor = Anchor::None;
  std::size_t volume = 0, end = 0;
  bool absolute = false, verbatim = false, incomplete = false, hasServer = false;
};
Root root(Invocation &ctx, std::string_view text, PathStyle style) noexcept {
  Root out;
  const auto sep = [&](std::size_t i) { return separator(text[i], style, out.verbatim); };
  const auto skip = [&](std::size_t i) {
    while (i < text.size() && sep(i)) {
      if (!checkpoint(ctx, i))
        break;
      ++i;
    }
    return i;
  };
  const auto nameEnd = [&](std::size_t i) {
    while (i < text.size() && !sep(i)) {
      if (!checkpoint(ctx, i))
        break;
      ++i;
    }
    return i;
  };
  const auto drive = [&](std::size_t start) {
    out.volume = start + 2;
    out.end = out.volume;
    if (out.end < text.size() && sep(out.end)) {
      out.end = skip(out.end);
      out.absolute = true;
    }
    out.incomplete = out.verbatim && !out.absolute;
  };
  const auto network = [&](std::size_t start) {
    out.incomplete = true;
    const auto server = nameEnd(start);
    out.hasServer = server > start;
    out.volume = server;
    out.end = server;
    if (server == start || server == text.size())
      return;
    const auto share = server + 1;
    if (share == text.size() || sep(share)) {
      out.incomplete = share == text.size();
      out.end = skip(server);
      return;
    }
    out.volume = nameEnd(share);
    out.end = skip(out.volume);
    out.absolute = true;
    out.incomplete = false;
  };
  if (style == PathStyle::Windows && text.size() >= 4 && text[0] == '\\' && text[1] == '\\' &&
      (text[2] == '?' || text[2] == '.') && text[3] == '\\') {
    out.verbatim = true;
    out.anchor = Anchor::Device;
    out.volume = out.end = 4;
    out.incomplete = true;
    if (text.size() >= 6 && letter(text[4]) && text[5] == ':') {
      out.anchor = Anchor::DeviceDrive;
      drive(4);
    } else if (text.size() >= 7 && lower(text[4]) == 'u' && lower(text[5]) == 'n' &&
               lower(text[6]) == 'c' && (text.size() == 7 || text[7] == '\\')) {
      out.anchor = Anchor::Network;
      out.volume = out.end = std::min<std::size_t>(8, text.size());
      if (text.size() > 8)
        network(8);
    } else if (text.size() > 4 && !sep(4)) {
      out.volume = nameEnd(4);
      out.end = skip(out.volume);
      out.absolute = true;
      out.incomplete = false;
    }
    return out;
  }
  if (style == PathStyle::Windows && text.size() >= 2 && letter(text[0]) && text[1] == ':') {
    out.anchor = Anchor::Drive;
    drive(0);
    return out;
  }
  if (!text.empty() && sep(0)) {
    out.anchor = Anchor::Root;
    out.end = skip(0);
    out.absolute = style == PathStyle::Posix;
    if (style == PathStyle::Windows && text.size() >= 2 && sep(1)) {
      out.anchor = Anchor::Network;
      out.volume = out.end;
      out.incomplete = out.end == 2;
      if (out.end == 2 && text.size() > 2)
        network(2);
    }
  }
  return out;
}
std::string_view view(const StringDescriptor &text) noexcept {
  return {text.data ? text.data : "", std::size_t(text.byteLength)};
}
StringDescriptor cell(const StringArrayDescriptor &parts, std::size_t i) noexcept {
  StringDescriptor text;
  std::memcpy(&text, static_cast<const char *>(parts.data) + i * sizeof(text), sizeof(text));
  return text;
}
bool copy(Invocation &ctx, char *out, std::string_view input) noexcept {
  for (std::size_t i = 0; i < input.size();) {
    if (!healthy(ctx))
      return false;
    const auto n = std::min<std::size_t>(1024, input.size() - i);
    std::memcpy(out + i, input.data() + i, n);
    i += n;
  }
  return healthy(ctx);
}
void join(Invocation &ctx, StringDescriptor &out, const StringArrayDescriptor &parts,
          PathStyle style) noexcept {
  kalandra_string_array_validate_v1(&ctx, &parts);
  if (!healthy(ctx))
    return;
  // Discard components preceding the last independently absolute path before
  // reserving storage. The remaining capacity bounds every intermediate join,
  // including roots completed across multiple parts, without growing buffers.
  std::size_t start = 0;
  for (std::size_t i = 0; i < std::size_t(parts.size); ++i) {
    if (!healthy(ctx))
      return;
    if (root(ctx, view(cell(parts, i)), style).absolute)
      start = i;
  }
  std::uint64_t capacity = 0;
  for (std::size_t i = start; i < std::size_t(parts.size); ++i) {
    if (!healthy(ctx))
      return;
    const auto bytes = std::uint64_t(cell(parts, i).byteLength);
    auto sum = combinedStringBytes(capacity, bytes);
    if (sum && bytes && capacity)
      sum = combinedStringBytes(*sum, 1);
    if (!sum) {
      ctx.fault(2);
      return;
    }
    capacity = *sum;
  }
  if (!capacity || !healthy(ctx))
    return;
  auto *data = static_cast<char *>(ctx.allocateRecord(capacity));
  if (!data)
    return;
  std::size_t length = 0;
  Root current;
  for (std::size_t i = start; i < std::size_t(parts.size); ++i) {
    if (!healthy(ctx))
      return;
    auto part = view(cell(parts, i));
    if (part.empty())
      continue;
    auto next = root(ctx, part, style);
    if (!healthy(ctx))
      return;
    if (current.verbatim && !next.absolute && part[0] == '/')
      next = {};
    const bool drive = next.anchor == Anchor::Drive || next.anchor == Anchor::DeviceDrive;
    bool sameDrive =
        drive && next.anchor == current.anchor && next.volume == current.volume && length;
    if (sameDrive) {
      for (std::size_t n = 0; n < next.volume; ++n)
        sameDrive &= lower(part[n]) == lower(data[n]);
    }
    if (!length || next.absolute ||
        (next.anchor != Anchor::None && next.anchor != Anchor::Root && !sameDrive)) {
      length = 0;
      if (!copy(ctx, data, part))
        return;
      length = part.size();
      current = next;
    } else if (next.anchor == Anchor::Root) {
      length = current.volume;
      if (!copy(ctx, data + length, part))
        return;
      length += part.size();
      if (!current.volume)
        current = next;
      else if (current.anchor == Anchor::Drive || current.anchor == Anchor::DeviceDrive ||
               current.absolute) {
        current.end = current.volume + next.end;
        current.absolute = true;
        current.incomplete = false;
      } else if (part.size() == next.end) {
        current.end = length;
        current.incomplete = current.anchor == Anchor::Network && current.hasServer;
      } else
        current = root(ctx, {data, length}, style);
    } else {
      if (sameDrive)
        part.remove_prefix(next.volume);
      if (part.empty())
        continue;
      const bool bareDrive =
          (current.anchor == Anchor::Drive || current.anchor == Anchor::DeviceDrive) &&
          length == current.volume && !current.absolute;
      if (!bareDrive && !separator(data[length - 1], style, current.verbatim))
        data[length++] = style == PathStyle::Windows ? '\\' : '/';
      if (!copy(ctx, data + length, part))
        return;
      length += part.size();
      // An unfinished UNC/device root can become complete after appending its
      // server/share/device. A complete anchor never changes on relative append.
      // Thus even long root names are scanned only a bounded number of times.
      if (current.incomplete)
        current = root(ctx, {data, length}, style);
    }
    if (!healthy(ctx))
      return;
  }
  if (healthy(ctx))
    out = {data, data, std::int64_t(length)};
}
} // namespace
namespace kalandra::runtime {
std::uint64_t pathOperation(Invocation &ctx, StringDescriptor &out, PathAction action,
                            const void *input, PathStyle style) noexcept {
  StringDescriptor saved;
  const bool valid = input && unsigned(action) >= 1 && unsigned(action) <= 5;
  if (valid && action != PathAction::Join)
    saved = *static_cast<const StringDescriptor *>(input);
  out = {};
  if (!healthy(ctx))
    return 0;
  if (!valid) {
    ctx.fault(3);
    return 0;
  }
  if (action == PathAction::Join) {
    join(ctx, out, *static_cast<const StringArrayDescriptor *>(input), style);
  } else {
    if (!validateString(ctx, saved))
      return 0;
    const auto text = view(saved);
    const auto anchor = root(ctx, text, style);
    if (!healthy(ctx))
      return 0;
    if (action == PathAction::IsAbsolute)
      return anchor.absolute;
    auto end = text.size();
    while (end > anchor.end && separator(text[end - 1], style, anchor.verbatim)) {
      if (!checkpoint(ctx, end))
        return 0;
      --end;
    }
    auto begin = end;
    while (begin > anchor.end && !separator(text[begin - 1], style, anchor.verbatim)) {
      if (!checkpoint(ctx, begin))
        return 0;
      --begin;
    }
    std::string_view result;
    if (action == PathAction::FileName)
      result = text.substr(begin, end - begin);
    else if (action == PathAction::ParentPath) {
      auto parent = begin;
      while (parent > anchor.end && separator(text[parent - 1], style, anchor.verbatim)) {
        if (!checkpoint(ctx, parent))
          return 0;
        --parent;
      }
      result = parent ? text.substr(0, parent) : std::string_view(".");
    } else {
      auto leading = begin;
      while (leading < end && text[leading] == '.') {
        if (!checkpoint(ctx, leading))
          return 0;
        ++leading;
      }
      auto dot = end;
      while (dot > leading) {
        if (!checkpoint(ctx, dot))
          return 0;
        --dot;
        if (text[dot] == '.') {
          result = text.substr(dot, end - dot);
          break;
        }
      }
    }
    if (healthy(ctx))
      kalandra_string_literal_v1(&ctx, &out, result.data(), result.size());
  }
  if (!healthy(ctx))
    out = {};
  return 0;
}
} // namespace kalandra::runtime
extern "C" std::uint64_t kalandra_path_v1(kalandra::runtime::Invocation *ctx,
                                          kalandra::runtime::StringDescriptor *out,
                                          std::uint32_t action, const void *input) noexcept {
#ifdef _WIN32
  constexpr auto style = kalandra::runtime::PathStyle::Windows;
#else
  constexpr auto style = kalandra::runtime::PathStyle::Posix;
#endif
  return kalandra::runtime::pathOperation(*ctx, *out, static_cast<kalandra::PathAction>(action),
                                          input, style);
}
