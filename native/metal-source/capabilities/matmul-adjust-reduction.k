fn total(values: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(matmul(values, values) + bias)

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    values = try await transfer(input, gpu)
    try await spawn total(values, values) on gpu
