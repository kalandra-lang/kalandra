#pragma once
#include "DeviceArrayResultWire.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace kalandra::runtime {
struct DeviceAggregateResultField {
  enum Kind { Record, Scalar, Array };
  static constexpr std::size_t None = SIZE_MAX;
  Kind kind;
  std::size_t parent, guard, offset;
  std::uint64_t tag, variants;
  NumericType element;
  bool boolean;
};

// Schema is generated from the checked nominal type and target native layout.
// The caller keeps the result scope selected while advancing this decoder.
// No part of the record is published until the entire envelope is consumed.
template <class Policy> class DeviceAggregateResultDecode {
  using F = DeviceAggregateResultField;
  using A = DeviceAggregateEnvelopeABI;
  using W = DeviceArrayWireABI;
  static constexpr auto count = Policy::fields.size();
  struct Region {
    std::uint64_t offset, length, layout;
    std::size_t nativeOffset;
  };
  std::array<bool, count> active_{};
  std::array<std::uint64_t, count> tags_{};
  std::array<Region, count> regions_{};
  std::size_t node_ = 0, leaf_ = 0, regionCount_ = 0;
  std::uint64_t nextRegion_ = 0, cursor_ = 0, length_ = 0, payloadOffset_ = 0;
  std::size_t width_ = 0;
  bool boolean_ = false;
  std::byte *output_ = nullptr;
  std::optional<RetainedCpuTensor> envelope_;
  std::span<const std::uint64_t> words_;

  static bool invalid(Invocation &context) noexcept {
    context.fault(3);
    return false;
  }

  template <std::size_t... I>
  static void descriptor(std::byte *out, void *data, std::uint64_t size,
                         NumericType element, std::index_sequence<I...>) noexcept {
    const auto write = [&]<NumericType E>() {
      auto *p = static_cast<NumericValue<E> *>(data);
      CpuTensor<E, 1> value{p, p, 0, {std::int64_t(size)}, {1}};
      static_assert(sizeof(value) == 40);
      std::memcpy(out, &value, sizeof(value));
    };
    (void)((element == numericTypes[I].type
               ? (write.template operator()<numericTypes[I].type>(), true) : false) || ...);
  }

public:
  using Result = typename Policy::Result;
  using Wire = CpuTensor<NumericType::UInt64, 1>;

  bool initialize(Invocation &context, const Wire &wire) noexcept {
    if (context.poll())
      return false;
    if (envelope_ || wire.sizes[0] < std::int64_t(W::HeaderWords + A::HeaderWords) ||
        std::uint64_t(wire.sizes[0]) > W::MaxWords || wire.strides[0] != 1)
      return invalid(context);
    auto retained = retainCpuTensor(context, wire.view());
    if (!retained)
      return invalid(context);
    const auto all = std::span(reinterpret_cast<const std::uint64_t *>(retained->data().data()),
                               std::size_t(wire.sizes[0]));
    if (all[W::Version] != W::ResultMagic || all[W::Layout] != W::layout(NumericType::UInt64, false) ||
        all[W::Origin] || all[W::Length] != all.size() - W::HeaderWords)
      return invalid(context);
    auto words = all.subspan(W::HeaderWords);
    constexpr auto leaves = std::count_if(Policy::fields.begin(), Policy::fields.end(),
                                         [](const auto &field) { return field.kind != F::Record; });
    nextRegion_ = A::HeaderWords + A::LeafWords * leaves;
    if (words[0] != A::Magic || words[1] != leaves || nextRegion_ > words.size())
      return invalid(context);
    envelope_.emplace(std::move(*retained));
    words_ = words;
    return true;
  }

  template <class Resolve>
  bool advance(Invocation &context, Result &result, Resolve resolve) noexcept {
    if (context.poll())
      return false;
    if (!envelope_)
      return invalid(context);
    if (output_) {
      const auto end = cursor_ + std::min<std::uint64_t>(W::PageWords, length_ - cursor_);
      for (; cursor_ < end; ++cursor_) {
        if (context.poll())
          return false;
        const auto word = words_[payloadOffset_ + cursor_];
        if ((boolean_ && word > 1) || (width_ < 8 && (word >> (8 * width_))))
          return invalid(context);
        std::memcpy(output_ + cursor_ * width_, &word, width_);
      }
      if (cursor_ == length_)
        output_ = nullptr;
      return false;
    }
    if (node_ == count)
      return nextRegion_ == words_.size() || invalid(context);
    const auto index = node_++;
    const auto &field = Policy::fields[index];
    const bool active = field.parent == F::None ||
        (active_[field.parent] && (field.guard == F::None || tags_[field.guard] == field.tag));
    active_[index] = active;
    if (field.kind == F::Record)
      return false;

    const auto data = words_.subspan(A::HeaderWords + A::LeafWords * leaf_++, A::LeafWords);
    if (!active)
      return (data[0] || data[1] || data[2]) ? invalid(context) : false;
    const auto description = describeNumeric(field.element);
    if (!description)
      return invalid(context);
    auto *out = result.data.data() + field.offset;
    const auto width = description->bytes();
    if (field.kind == F::Scalar) {
      const auto word = data[A::Scalar];
      if (data[1] || data[2] || (field.boolean && word > 1) ||
          (width < 8 && (word >> (8 * width))) ||
          (field.variants && (!word || word > field.variants)))
        return invalid(context);
      tags_[index] = word;
      std::memcpy(out, &word, width);
      return false;
    }

    const auto length = data[A::Length], origin = data[A::Origin], offset = data[A::Offset];
    if (origin) {
      if (offset || !resolve(origin, field.element, field.boolean, length, out))
        return invalid(context);
      return false;
    }
    if (length) {
      const auto layout = W::layout(field.element, field.boolean);
      if (offset < nextRegion_) {
        const auto end = regions_.begin() + regionCount_;
        const auto prior = std::lower_bound(regions_.begin(), end, offset,
            [](const Region &region, auto key) { return region.offset < key; });
        if (prior == end || prior->offset != offset || prior->length != length ||
            prior->layout != layout)
          return invalid(context);
        std::memcpy(out, result.data.data() + prior->nativeOffset, 40);
        return false;
      }
      if (offset != nextRegion_ || length > words_.size() - offset || length > SIZE_MAX / width)
        return invalid(context);
      output_ = static_cast<std::byte *>(context.allocate(length * width));
      if (!output_)
        return false;
      regions_[regionCount_++] = {offset, length, layout, field.offset};
      nextRegion_ += length;
      cursor_ = 0;
      length_ = length;
      payloadOffset_ = offset;
      width_ = width;
      boolean_ = field.boolean;
    } else if (offset)
      return invalid(context);
    descriptor(out, output_, length, field.element,
               std::make_index_sequence<numericTypes.size()>{});
    return false;
  }
};
} // namespace kalandra::runtime
