fn project(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return matmul(input, weights)

fn total(values: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(values)

fn main() -> f32 ! TaskError:
  return try parallel:
    input: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    gpu = devices.gpu()
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    work = spawn project(a, b) on gpu
    result = try await work
    summary = spawn total(result) on gpu
    try await summary
