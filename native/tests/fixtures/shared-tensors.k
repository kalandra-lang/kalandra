fn identity(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn forward(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  identity(values)

fn square(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(values, values)

fn reference(
  samples: Tensor[float32, ?, ?],
  weights: Tensor[float32, ?, ?],
) -> float32:
  alias = samples
  first = forward(square(forward(alias)))
  second = square(samples)
  reduceSum(first) + reduceSum(second) + reduceSum(alias)

fn main() -> float32:
  values: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
  reference(values, values)
