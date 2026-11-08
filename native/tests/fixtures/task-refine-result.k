fn multiply(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  adjusted: Tensor[f32, 2, 2] = tensor([[reduceSum(a), 2], [3, 4]])
  return matmul(adjusted, b)

fn project(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?] ! TaskError:
  return try parallel:
    work = spawn multiply(a, b)
    try await work

fn uncertain(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?] ! TaskError:
  result = project(a, b)
  return result catch fn(error):
    return match error:
      TaskError.Fault: fail error
      _: a

fn main() -> f32 ! TaskError:
  a: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
  b: Tensor[f32, 2, 2] = tensor([[5, 6], [7, 8]])
  fixed: Tensor[f32, 2, 2] = try uncertain(a, b)
  return reduceSum(fixed)
