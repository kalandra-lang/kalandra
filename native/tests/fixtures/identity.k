fn identity(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn main() -> f32:
  values: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
  returned = identity(values)
  return reduceSum(returned) + reduceSum(values)
