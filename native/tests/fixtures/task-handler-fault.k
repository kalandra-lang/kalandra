fn total() -> f32 ! TaskError:
  return try parallel:
    work = spawn compute()
    try await work

fn compute() -> f32:
  data: Tensor[f32, 2] = tensor([1, 2])
  return reduceSum(data)

fn main() -> f32:
  result = total()
  handled = result catch fn(error): return compute()
  return handled
