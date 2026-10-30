fn multiply(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(a, b)

fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?], projection: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> float32:
  hidden = multiply(input, weights)
  shifted = adjust(hidden, bias, scale)
  reduceSum(multiply(shifted, projection)) / scale

fn adjust(values: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> Tensor[float32, ?, ?]:
  -(values + bias) / scale

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    projection: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    bias: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    offset = try await transfer(bias, gpu)
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    c = try await transfer(projection, gpu)
    work = spawn project(a, b, c, offset, 2) on gpu
    result = try await work
    cached = try await work
    cached
