fn dot(a: Tensor[float64, ?], b: Tensor[float64, ?]) -> float64:
  reduceSum(a * b)

fn axpy(a: Tensor[float64, ?], b: Tensor[float64, ?]) -> float64:
  reduceSum(a * 3 + b)

fn matrix(a: Tensor[float64, ?, ?], b: Tensor[float64, ?, ?]) -> float64:
  reduceSum(matmul(a, b))

fn main() -> float64 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    v: Tensor[float64, 1] = tensor([1])
    m: Tensor[float64, 1, 1] = tensor([[1]])
    a = try await transfer(v, gpu)
    b = try await transfer(m, gpu)
    first = try await spawn dot(a, a) on gpu
    second = try await spawn axpy(a, a) on gpu
    try await spawn matrix(b, b) on gpu
