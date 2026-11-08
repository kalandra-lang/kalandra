fn finish(values: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?], scale: f32) -> Tensor[f32, ?, ?]:
  return relu((values + bias) * scale)

fn project(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?], bias: Tensor[f32, ?, ?], scale: f32) -> f32:
  return reduceSum(finish(matmul(a, b), bias, scale))

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    x: Tensor[f32, 2, 2] = tensor([[1, -2], [3, -4]])
    y: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    z: Tensor[f32, 2, 2] = tensor([[0, 0], [0, 0]])
    a = try await transfer(x, gpu)
    b = try await transfer(y, gpu)
    bias = try await transfer(z, gpu)
    try await spawn project(a, b, bias, 1) on gpu
