fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(input, weights)

fn total(values: Tensor[float32, ?, ?]) -> float32:
  reduceSum(values)

fn main() -> float32 ! TaskError:
  try parallel:
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    gpu = devices.gpu()
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    work = spawn project(a, b) on gpu
    result = try await work
    summary = spawn total(result) on gpu
    try await summary
