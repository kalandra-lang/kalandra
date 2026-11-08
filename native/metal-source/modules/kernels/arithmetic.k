fn helper(value: f32) -> f32:
  return value * 2

export fn main() -> f32:
  return 99

export fn offset(value: f32) -> f32:
  return helper(value) + 0.5

export fn combine(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return left * right + left + offset(0)
