export fn project(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  left = input
  right = weights
  result = matmul(left, right)
  return result

export fn total(values: Tensor[f32, ?, ?]) -> f32:
  input = values
  result = reduceSum(input)
  return result

export fn vectorTotal(values: Tensor[f32, ?]) -> f32:
  input = values
  result = reduceSum(input)
  return result
