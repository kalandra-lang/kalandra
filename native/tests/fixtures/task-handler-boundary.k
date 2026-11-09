fn total() -> f32 ! TaskError:
  return try parallel:
    work = spawn compute()
    try await work

fn compute() -> f32:
  data: Tensor[f32, 2] = tensor([1, 2])
  return reduceSum(data)

fn main() -> f32:
  result = total()
  forwarded = result catch fn(error):
    try total()
    return 99
  recovered = forwarded catch fn(error): return 40
  return recovered + 2
