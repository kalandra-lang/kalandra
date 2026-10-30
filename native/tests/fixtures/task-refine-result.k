fn multiply(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  adjusted: Tensor[float32, 2, 2] = tensor([[reduceSum(a), 2], [3, 4]])
  matmul(adjusted, b)

fn project(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    work = spawn multiply(a, b)
    try await work

fn uncertain(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?] ! TaskError:
  result = project(a, b)
  result catch fn(error):
    match error:
      TaskError.Fault: fail error
      _: a

fn main() -> float32 ! TaskError:
  a: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
  b: Tensor[float32, 2, 2] = tensor([[5, 6], [7, 8]])
  fixed: Tensor[float32, 2, 2] = try uncertain(a, b)
  reduceSum(fixed)
