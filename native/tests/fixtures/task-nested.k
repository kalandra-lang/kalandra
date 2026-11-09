fn total(a: Tensor[f32, ?, ?]) -> f32 ! TaskError:
  return try parallel:
    first = spawn reduceSum(a)
    second = spawn reduceSum(a)
    x = try await first
    y = try await second
    x + y

fn combine(a: Tensor[f32, ?, ?]) -> f32 ! TaskError:
  return try parallel:
    first = spawn total(a)
    second = spawn total(a)
    x = try await first
    y = try await second
    x + y

fn main() -> f32 ! TaskError:
  values: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
  return try combine(values)
