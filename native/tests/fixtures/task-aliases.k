fn identity(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn main() -> f32 ! TaskError:
  input: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
  output = try parallel:
    first = spawn identity(input)
    second = spawn identity(input)
    a = try await first
    b = try await second
    reduceSum(a) + reduceSum(b)
    a
  return reduceSum(output) + reduceSum(input)
