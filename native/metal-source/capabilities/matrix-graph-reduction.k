fn multiply(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(a, b)

fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?], projection: Tensor[float32, ?, ?], count: float32) -> float32:
  hidden = multiply(input, weights)
  reduceSum(multiply(hidden, projection))

fn finish(value: float32, count: float32) -> float32:
  -(value + count) / count

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    projection: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    c = try await transfer(projection, gpu)
    work = spawn project(a, b, c, 2) on gpu
    result = try await work
    cached = try await work
    cached
