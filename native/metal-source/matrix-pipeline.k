fn project(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return matmul(input, weights)

fn scale(values: Tensor[f32, ?, ?], factor: f32) -> Tensor[f32, ?, ?]:
  return values * factor

fn adjust(values: Tensor[f32, ?, ?], factor: f32, bias: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return scale(values, factor) + bias

fn total(values: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(values)

fn main() -> f32 ! TaskError:
  return try parallel:
    input: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    gpu = devices.gpu()
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    c = try await transfer(bias, gpu)
    product = try await spawn project(a, b) on gpu
    adjusted = try await spawn adjust(product, 2, c) on gpu
    try await spawn total(adjusted) on gpu
