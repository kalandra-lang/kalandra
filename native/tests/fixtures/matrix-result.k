fn reference(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(a, b)

fn shared(a: Tensor[float32, ?, ?], b: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(a, a)

fn main() -> float32:
  0
