fn multiply(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  adjusted: Tensor[float32, 2, 2] = tensor([[reduceSum(a), 2], [3, 4]])
  matmul(adjusted, b)

fn project(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    work = spawn multiply(a, b)
    sibling = spawn multiply(a, b)
    first = try await work
    try await sibling
    first

fn main() -> float32:
  a: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
  b: Tensor[float32, 2, 2] = tensor([[5, 6], [7, 8]])
  result = project(a, b)
  recovered = result catch fn(error):
    match error:
      TaskError.Fault { kind: cause }:
        match cause:
          FaultKind.Allocation: tensor([[10, 20], [30, 40]])
          _: b
      TaskError.AdmissionRejected { reason: explanation }: a
      _: b
  reduceSum(recovered)
