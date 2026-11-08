fn project(input: Tensor[f32, ?, ?], scale: f32, weights: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?]) -> f32:
  product = matmul(input + scale, weights - 1)
  return result(reduceSum(finish(product, bias, scale)), scale)

fn plain(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?], scale: f32) -> f32:
  return result(reduceSum(finish(matmul(input, weights), bias, scale)), scale)

fn finish(values: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?], scale: f32) -> Tensor[f32, ?, ?]:
  return (values + bias) * scale

fn result(value: f32, scale: f32) -> f32:
  return -(value + scale) / scale

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    offset = try await transfer(bias, gpu)
    work = spawn project(a, 2, b, offset) on gpu
    result = try await work
    cached = try await work
    cached
