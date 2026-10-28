fn cells(x: Tensor[float32, ?, ?], up: Tensor[float32, ?, ?], down: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  hidden = relu(matmul(x, up))
  matmul(hidden, down) + x

fn project(x: Tensor[float32, ?, ?], up: Tensor[float32, ?, ?], down: Tensor[float32, ?, ?]) -> float32:
  reduceSum(cells(x, up, down))

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    x: Tensor[float32, 1, 2] = tensor([[1, -2]])
    up: Tensor[float32, 2, 4] = tensor([[1, 2, -1, 0], [0, 1, 2, -1]])
    down: Tensor[float32, 4, 2] = tensor([[1, 0], [0, 1], [1, -1], [-1, 1]])
    a = try await transfer(x, gpu)
    b = try await transfer(up, gpu)
    c = try await transfer(down, gpu)
    try await spawn project(a, b, c) on gpu
