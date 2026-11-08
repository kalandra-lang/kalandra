fn project(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?] ! TaskError:
  return try parallel:
    work = spawn matmul(a, b)
    try await work

fn main() -> f32 ! TaskError:
  a: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
  b: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
  completed = try project(a, b)
  return reduceSum(completed)
