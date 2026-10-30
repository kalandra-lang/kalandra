fn project(input: Tensor[float32, ?, ?], scale: float32, weights: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?]) -> float32:
  product = matmul(input + scale, weights - 1)
  result(reduceSum(finish(product, bias, scale)), scale)

fn plain(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> float32:
  result(reduceSum(finish(matmul(input, weights), bias, scale)), scale)

fn finish(values: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> Tensor[float32, ?, ?]:
  (values + bias) * scale

fn result(value: float32, scale: float32) -> float32:
  -(value + scale) / scale

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    offset = try await transfer(bias, gpu)
    work = spawn project(a, 2, b, offset) on gpu
    result = try await work
    cached = try await work
    cached
