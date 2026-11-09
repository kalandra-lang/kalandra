fn dot(a: Tensor[f64, ?], b: Tensor[f64, ?]) -> f64:
  return reduceSum(a * b)

fn axpy(a: Tensor[f64, ?], b: Tensor[f64, ?]) -> f64:
  return reduceSum(a * 3 + b)

fn matrix(a: Tensor[f64, ?, ?], b: Tensor[f64, ?, ?]) -> f64:
  return reduceSum(matmul(a, b))

fn main() -> f64 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    v: Tensor[f64, 1] = tensor([1])
    m: Tensor[f64, 1, 1] = tensor([[1]])
    a = try await transfer(v, gpu)
    b = try await transfer(m, gpu)
    first = try await spawn dot(a, a) on gpu
    second = try await spawn axpy(a, a) on gpu
    try await spawn matrix(b, b) on gpu
