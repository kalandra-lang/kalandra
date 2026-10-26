fn project(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> float32:
  hidden = relu(matmul(a, b))
  reduceSum(relu(matmul(hidden, b)))

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    x: Tensor[float32, 2, 2] = tensor([[1, -2], [3, 4]])
    w: Tensor[float32, 2, 2] = tensor([[1, 2], [3, 4]])
    a = try await transfer(x, gpu)
    b = try await transfer(w, gpu)
    try await spawn project(a, b) on gpu
