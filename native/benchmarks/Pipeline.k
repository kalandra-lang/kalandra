fn finish(values: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> Tensor[float32, ?, ?]:
  relu((values + bias) * scale)

fn project(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> float32:
  reduceSum(finish(matmul(a, b), bias, scale))

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    x: Tensor[float32, 2, 2] = tensor([[1, -2], [3, -4]])
    y: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    z: Tensor[float32, 2, 2] = tensor([[0, 0], [0, 0]])
    a = try await transfer(x, gpu)
    b = try await transfer(y, gpu)
    bias = try await transfer(z, gpu)
    try await spawn project(a, b, bias, 1) on gpu
