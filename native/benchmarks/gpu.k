fn project(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return matmul(input, weights)

fn total(values: Tensor[f32, ?]) -> f32:
  return reduceSum(values)

fn main() -> f32 ! TaskError:
  return try parallel:
    input: Tensor[f32, 1, 1] = tensor([[2]])
    gpu = devices.gpu()
    a = try await transfer(input, gpu)
    work = spawn project(a, a) on gpu
    matrix = try await work
    host = try await transfer(matrix, devices.cpu())
    values: Tensor[f32, 1] = tensor([1])
    v = try await transfer(values, gpu)
    try await spawn total(v) on gpu
