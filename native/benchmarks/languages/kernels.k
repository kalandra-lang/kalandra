fn dot(a: Tensor[float64, ?], b: Tensor[float64, ?]) -> float64:
  reduceSum(a * b)

fn axpy(a: Tensor[float64, ?], b: Tensor[float64, ?]) -> float64:
  reduceSum(a * 3 + b)

fn matrix(a: Tensor[float64, ?, ?], b: Tensor[float64, ?, ?]) -> float64:
  reduceSum(matmul(a, b))

fn main() -> float64:
  0
