fn total(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(matmul(a, b))

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    x = try await transfer(a, gpu)
    try await spawn total(x, x) on gpu
