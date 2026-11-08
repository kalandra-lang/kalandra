fn project(input: Tensor[f32, ?, ?], scale: f32, weights: Tensor[f32, ?, ?]) -> f32:
  product = matmul(input + scale, weights - 1)
  return reduceSum(product)

fn plain(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(matmul(input, weights))

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    work = spawn project(a, 2, b) on gpu
    result = try await work
    cached = try await work
    cached
