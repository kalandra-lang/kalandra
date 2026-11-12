// The empty error family is also the uninhabited source type `never`. Its
// private identity is created only when needed, so existing modules retain
// their nominal IDs and ordinary code-generation identities.
std::optional<std::size_t> neverFamily_;
std::unordered_set<FunctionId> neverErrorFunctions_;
std::size_t neverFamily() {
  if (neverFamily_)
    return *neverFamily_;
  TypedRecord empty{"never", {}, {}};
  empty.error = empty.uninhabited = true;
  ValueType tag;
  tag.element = NumericType::Int64;
  empty.fields.push_back({"$tag", tag, {}});
  neverFamily_ = output_.records.size();
  output_.records.push_back(std::move(empty));
  return *neverFamily_;
}
bool isNeverFamily(std::size_t id) const {
  return id < output_.records.size() && output_.records[id].uninhabited;
}
ValueType eraseNeverError(ValueType value) const {
  if (value.fallible && value.errorRecord && isNeverFamily(*value.errorRecord)) {
    value.fallible = false;
    value.errorRecord.reset();
    value.emptyErrorChannel = true;
    value.identity = 0;
  }
  return value;
}
