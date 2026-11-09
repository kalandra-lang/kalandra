fn reference(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> f32:
  result = matmul(a, b)
  fixed: Tensor[f32, 2, 2] = result
  return reduceSum(fixed)

fn main() -> f32:
  a: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
  b: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
  return reference(a, b)
