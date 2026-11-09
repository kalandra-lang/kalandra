fn compute() -> Tensor[f32, 2]:
  input: Tensor[f32, 2] = tensor([1, 2])
  output: Tensor[f32, 2] = tensor([reduceSum(input), 4])
  return output

fn attempted() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    work = spawn compute()
    try await work

fn main() -> f32:
  result = attempted()
  recovered = result catch fn(error): return tensor([10, 20, 30])
  return reduceSum(recovered)
