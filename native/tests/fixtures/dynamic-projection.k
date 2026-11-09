fn project(
  samples: Tensor[f32, ?, ?],
  weights: Tensor[f32, ?, ?],
) -> Tensor[f32, ?, ?]:
  return matmul(samples, weights)

fn summarize(values: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(values)

fn reference(
  samples: Tensor[f32, ?, ?],
  weights: Tensor[f32, ?, ?],
) -> f32:
  return summarize(project(samples, weights))

fn main() -> f32:
  samples: Tensor[f32, 2, 3] = tensor([
    [1, 2, 3],
    [4, 5, 6],
  ])
  weights: Tensor[f32, ?, ?] = tensor([
    [1, 2],
    [3, 4],
    [5, 6],
  ])
  return reference(samples, weights)
