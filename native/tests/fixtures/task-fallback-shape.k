fn compute() -> Tensor[float32, 2]:
  input: Tensor[float32, 2] = tensor([1, 2])
  output: Tensor[float32, 2] = tensor([reduceSum(input), 4])
  output

fn attempted() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    work = spawn compute()
    try await work

fn main() -> float32:
  result = attempted()
  recovered = result catch fn(error): tensor([10, 20, 30])
  reduceSum(recovered)
