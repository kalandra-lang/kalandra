fn total(a: Tensor[float32, ?, ?]) -> float32 ! TaskError:
  try parallel:
    first = spawn reduceSum(a)
    second = spawn reduceSum(a)
    x = try await first
    y = try await second
    x + y

fn combine(a: Tensor[float32, ?, ?]) -> float32 ! TaskError:
  try parallel:
    first = spawn total(a)
    second = spawn total(a)
    x = try await first
    y = try await second
    x + y

fn main() -> float32 ! TaskError:
  values: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
  try combine(values)
