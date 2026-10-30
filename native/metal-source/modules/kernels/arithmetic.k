fn helper(value: float32) -> float32:
  value * 2

export fn main() -> float32:
  99

export fn offset(value: float32) -> float32:
  helper(value) + 0.5

export fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left * right + left + offset(0)
