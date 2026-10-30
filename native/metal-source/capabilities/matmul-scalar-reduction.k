fn total(v: Tensor[float32, ?, ?], count: float32) -> float32:
  reduceSum(matmul(v, v)) / count

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    values = try await transfer(input, gpu)
    try await spawn total(values, 4) on gpu
