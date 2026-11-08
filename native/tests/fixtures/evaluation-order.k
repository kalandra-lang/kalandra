fn first(values: Tensor[f32, ?, ?]) -> f32:
  refined: Tensor[f32, 2, 2] = values
  return reduceSum(refined)

fn second(values: Tensor[f32, ?, ?]) -> f32:
  refined: Tensor[f32, 1, 1] = values
  return reduceSum(refined)

fn combine(left: f32, right: f32) -> f32:
  return left + right

fn reference(
  samples: Tensor[f32, ?, ?],
  weights: Tensor[f32, ?, ?],
) -> f32:
  return combine(first(samples), second(weights))

fn main() -> f32:
  return 0
