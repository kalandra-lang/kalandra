fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(input, weights)

fn scale(values: Tensor[float32, ?, ?], factor: float32) -> Tensor[float32, ?, ?]:
  values * factor

fn adjust(values: Tensor[float32, ?, ?], factor: float32, bias: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  scale(values, factor) + bias

fn total(values: Tensor[float32, ?, ?]) -> float32:
  reduceSum(values)

fn main() -> float32 ! TaskError:
  try parallel:
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    gpu = devices.gpu()
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    c = try await transfer(bias, gpu)
    product = try await spawn project(a, b) on gpu
    adjusted = try await spawn adjust(product, 2, c) on gpu
    try await spawn total(adjusted) on gpu
