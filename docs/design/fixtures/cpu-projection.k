fn project(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> Tensor[float32, ?, 2]:
  matmul(samples, weights)

fn summarize(values: Tensor[float32, ?, 2]) -> float32:
  reduceSum(values)

fn reference(
  samples: Tensor[float32, ?, 3],
  weights: Tensor[float32, 3, 2],
) -> float32:
  summarize(project(samples, weights))

fn main() -> float32:
  samples: Tensor[float32, 2, 3] = tensor([
    [1, 2, 3],
    [4, 5, 6],
  ])
  weights: Tensor[float32, 3, 2] = tensor([
    [1, 2],
    [3, 4],
    [5, 6],
  ])
  reference(samples, weights)
