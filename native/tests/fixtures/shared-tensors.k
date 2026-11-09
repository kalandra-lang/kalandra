fn identity(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn forward(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return identity(values)

fn square(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return matmul(values, values)

fn reference(
  samples: Tensor[f32, ?, ?],
  weights: Tensor[f32, ?, ?],
) -> f32:
  alias = samples
  first = forward(square(forward(alias)))
  second = square(samples)
  return reduceSum(first) + reduceSum(second) + reduceSum(alias)

fn main() -> f32:
  values: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
  return reference(values, values)
