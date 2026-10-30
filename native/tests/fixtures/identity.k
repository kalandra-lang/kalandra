fn identity(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn main() -> float32:
  values: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
  returned = identity(values)
  reduceSum(returned) + reduceSum(values)
