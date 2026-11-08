fn cells(x: Tensor[f32, ?, ?], up: Tensor[f32, ?, ?], down: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  hidden = relu(matmul(x, up))
  return matmul(hidden, down) + x

fn project(x: Tensor[f32, ?, ?], up: Tensor[f32, ?, ?], down: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(cells(x, up, down))

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    x: Tensor[f32, 1, 2] = tensor([[1, -2]])
    up: Tensor[f32, 2, 4] = tensor([[1, 2, -1, 0], [0, 1, 2, -1]])
    down: Tensor[f32, 4, 2] = tensor([[1, 0], [0, 1], [1, -1], [-1, 1]])
    a = try await transfer(x, gpu)
    b = try await transfer(up, gpu)
    c = try await transfer(down, gpu)
    try await spawn project(a, b, c) on gpu
