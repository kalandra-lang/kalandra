// Private declarations/implementation included inside an explicit backend namespace.
// Private descriptor/transfer bridge. CPU descriptors are validated against the
// invocation ledger; retained backing is immutable and survives its producer.
std::expected<TensorValue, Error> fromCpu(Runtime &, const Invocation &, CpuTensorView);
std::expected<TensorValue, Error> hostTensor(Runtime &, NumericType, std::span<const std::byte>,
                                             std::span<const std::int64_t>);
std::expected<std::span<const std::byte>, Error> hostBytes(const TensorValue &);
std::expected<void, Error> validateTransfer(Runtime &, const TensorValue &, bool upload);
// Called by the foreign completion service, never from a scheduler worker.
std::expected<TensorValue, Error> transferTensor(Runtime &, const TensorValue &, bool upload);
template <NumericType Element, std::size_t Rank>
std::expected<CpuTensor<Element, Rank>, Error> toCpuTyped(Invocation &context,
                                                          const TensorValue &value) {
  if (value.type != Element || value.rank != Rank || value.scalar)
    return std::unexpected(Error::Invalid);
  auto bytes = hostBytes(value);
  if (!bytes)
    return std::unexpected(bytes.error());
  std::array<std::int64_t, Rank> shape{};
  for (std::size_t i = 0; i < Rank; ++i) {
    if (value.dimensions[i] > INT64_MAX)
      return std::unexpected(Error::Invalid);
    shape[i] = value.dimensions[i];
  }
  auto copied = copyCpuTensor<Element, Rank>(context, value.type, *bytes, shape);
  if (!copied)
    return std::unexpected(static_cast<Error>(copied.error()));
  return *copied;
}
