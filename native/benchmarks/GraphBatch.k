fn project(a: Tensor[f32, ?, ?], b: Tensor[f32, ?, ?]) -> f32:
  hidden = relu(matmul(a, b))
  return reduceSum(relu(matmul(hidden, b)))

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    x: Tensor[f32, 2, 2] = tensor([[1, -2], [3, 4]])
    w: Tensor[f32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(x, gpu)
    b = try await transfer(w, gpu)
    try await spawn project(a, b) on gpu
