fn finish(values: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32) -> Tensor[float32, ?, ?]:
  (values + bias) * scale

fn project(input: Tensor[float32, ?, ?], bias: Tensor[float32, ?, ?], scale: float32, weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  product = matmul(input + scale, weights - 1)
  finish(product, bias, scale)

fn main() -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    offset = try await transfer(bias, gpu)
    work = spawn project(a, offset, 2, b) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
