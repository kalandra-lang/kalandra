fn project(a: Tensor[f32, 2, 3], b: Tensor[f32, 3, 2], bias: Tensor[f32, 2, 2], factor: f32) -> Tensor[f32, 2, 2]:
  product = matmul(a, b)
  return if factor > 0:
    product * factor
  else:
    bias

fn main() -> Tensor[f32, ?, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    b: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    z = try await transfer(bias, gpu)
    work = spawn project(x, y, z, 2) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
