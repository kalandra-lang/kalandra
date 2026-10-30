fn total(values: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?]) -> float32:
  reduceSum(matmul(values, values) + bias)

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    values = try await transfer(input, gpu)
    try await spawn total(values, values) on gpu
