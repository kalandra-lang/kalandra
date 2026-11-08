fn finish(values: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?], scale: f32) -> Tensor[f32, ?, ?]:
  return (values + bias) * scale

fn project(input: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?], scale: f32, weights: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  product = matmul(input, weights)
  return finish(product, bias, scale)

fn main() -> Tensor[f32, ?, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    bias: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    offset = try await transfer(bias, gpu)
    work = spawn project(a, offset, 2, b) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
