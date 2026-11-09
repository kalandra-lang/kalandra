fn multiply(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  adjusted: Tensor[f32, 2, 2] = tensor([[reduceSum(a), 2], [3, 4]])
  return matmul(adjusted, b)

fn project(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?] ! TaskError:
  return try parallel:
    work = spawn multiply(a, b)
    sibling = spawn multiply(a, b)
    first = try await work
    try await sibling
    first

fn main() -> f32:
  a: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
  b: Tensor[f32, 2, 2] = tensor([[5, 6], [7, 8]])
  result = project(a, b)
  recovered = result catch fn(error):
    return match error:
      TaskError.Fault { kind: cause }:
        match cause:
          FaultKind.Allocation: a
          _: b
      TaskError.AdmissionRejected { reason: explanation }: a
      _: b
  return reduceSum(recovered) + reduceSum(a)
