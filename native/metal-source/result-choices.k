fn project(a: Tensor[float32, 2, 3], b: Tensor[float32, 3, 2], bias: Tensor[float32, 2, 2], factor: float32) -> Tensor[float32, 2, 2]:
  product = matmul(a, b)
  if factor > 0:
    product * factor
  else:
    bias

fn main() -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    b: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    z = try await transfer(bias, gpu)
    work = spawn project(x, y, z, 2) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
