#pragma once
namespace kalandra {
enum class TensorTransformKind { Transpose, SoftmaxRows, RmsNormRows };
constexpr const char *tensorTransformName(TensorTransformKind kind) {
  switch (kind) {
  case TensorTransformKind::Transpose: return "transpose";
  case TensorTransformKind::SoftmaxRows: return "softmaxRows";
  case TensorTransformKind::RmsNormRows: return "rmsNormRows";
  }
  return "unknownTensorTransform";
}
}
