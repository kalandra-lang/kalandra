fn dot(a: Tensor[f64, ?], b: Tensor[f64, ?]) -> f64:
  return reduceSum(a * b)

fn axpy(a: Tensor[f64, ?], b: Tensor[f64, ?]) -> f64:
  return reduceSum(a * 3 + b)

fn matrix(a: Tensor[f64, ?, ?], b: Tensor[f64, ?, ?]) -> f64:
  return reduceSum(matmul(a, b))

fn main() -> f64:
  return 0
