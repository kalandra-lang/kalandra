fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(input, weights)

fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn main() -> float32 ! TaskError:
  try parallel:
    input: Tensor[float32, 1, 1] = tensor([[2]])
    gpu = devices.gpu()
    a = try await transfer(input, gpu)
    work = spawn project(a, a) on gpu
    matrix = try await work
    host = try await transfer(matrix, devices.cpu())
    values: Tensor[float32, 1] = tensor([1])
    v = try await transfer(values, gpu)
    try await spawn total(v) on gpu
