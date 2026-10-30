fn reference(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> float32:
  result = matmul(a, b)
  fixed: Tensor[float32, 2, 2] = result
  reduceSum(fixed)

fn main() -> float32:
  a: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
  b: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
  reference(a, b)
