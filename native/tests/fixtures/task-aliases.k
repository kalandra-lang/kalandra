fn identity(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn main() -> float32 ! TaskError:
  input: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
  output = try parallel:
    first = spawn identity(input)
    second = spawn identity(input)
    a = try await first
    b = try await second
    reduceSum(a) + reduceSum(b)
    a
  reduceSum(output) + reduceSum(input)
