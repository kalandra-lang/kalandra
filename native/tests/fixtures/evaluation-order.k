fn first(values: Tensor[float32, ?, ?]) -> float32:
  refined: Tensor[float32, 2, 2] = values
  reduceSum(refined)

fn second(values: Tensor[float32, ?, ?]) -> float32:
  refined: Tensor[float32, 1, 1] = values
  reduceSum(refined)

fn combine(left: float32, right: float32) -> float32:
  left + right

fn reference(
  samples: Tensor[float32, ?, ?],
  weights: Tensor[float32, ?, ?],
) -> float32:
  combine(first(samples), second(weights))

fn main() -> float32:
  0
