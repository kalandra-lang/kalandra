fn total(v: Tensor[f32, ?, ?], count: f32) -> f32:
  return reduceSum(matmul(v, v)) / count

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    values = try await transfer(input, gpu)
    try await spawn total(values, 4) on gpu
