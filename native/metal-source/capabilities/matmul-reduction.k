fn total(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> float32:
  reduceSum(matmul(a, b))

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    x = try await transfer(a, gpu)
    try await spawn total(x, x) on gpu
