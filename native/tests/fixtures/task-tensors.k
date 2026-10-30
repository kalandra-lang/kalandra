fn project(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    work = spawn matmul(a, b)
    try await work

fn main() -> float32 ! TaskError:
  a: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
  b: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
  completed = try project(a, b)
  reduceSum(completed)
