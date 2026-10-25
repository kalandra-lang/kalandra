# Tensor ReLU

`relu(input)` returns a tensor with the same numeric identity and complete shape, applying rectified linear activation to each element. It supports all ten numeric types and ranks zero through eight on CPU and Apple Metal. Scalars, arrays and nonnumeric inputs are rejected. CPU host code must explicitly transfer device tensors back before applying host tensor operations; placed GPU functions operate on their resident inputs.

```kalandra
fn main() -> float32:
  values: Tensor[float32, 2, 2] = tensor([[-3, 2], [-1, 4]])
  reduceSum(relu(values))
```

This returns `6`. The original tensor remains immutable. Unsigned elements retain their values. Signed integer negatives, including the minimum integer, become zero without a negation or overflow. Positive elements are preserved.

Floating behavior is explicit: both zeros become positive zero, negative finite values and negative infinity become positive zero, positive infinity remains positive infinity, and all NaN bit patterns (including sign/payload) are preserved. Positive subnormal values are preserved by this operation. Arithmetic around it retains the target's existing IEEE/Metal flush-to-zero contract; ReLU does not undo earlier rounding or flushing. It does not compute an addition or maximum whose NaN rules could silently discard NaNs.

Lowering uses sign/exponent classification and integer select, preserving floating bits without floating arithmetic. Therefore float64 ReLU itself is admitted under the Metal hardware policy. This does not add binary64 hardware arithmetic to Apple GPUs: surrounding float64 arithmetic, reductions and matrix products keep their explicit software-fallback requirement. Hardware and software arithmetic policies agree on ReLU selection.

ReLU participates in the existing fused elementwise lowering. `relu((matmul(a,b)+bias)*scale)` requires bias to match the full matrix output shape; this API does not add implicit broadcasting, autodiff, scalar ReLU, GELU, sigmoid or an LLM runtime. On the qualified hardware float32 path, a matrix and its fused post-map/reduction may share one command submission while retaining every buffer owner through completion. Tight byte budgets retain staged acknowledgement and earlier reclamation.

Conformance checks raw input/output bits against a separate C++ numeric-comparison oracle, including NaN payloads, signed zero, subnormals, infinities, signed minima, strides, empty axes, immutable inputs, allocation failures and cancellation. CPU and GPU qualification covers debug, optimized, ASan/UBSan and TSan runtimes. Source checks cover rank-zero and higher-rank placement; the editor exposes the typed signature and prevents renaming the builtin.
