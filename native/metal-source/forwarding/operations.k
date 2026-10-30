export fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left = input
  right = weights
  result = matmul(left, right)
  result

export fn total(values: Tensor[float32, ?, ?]) -> float32:
  input = values
  result = reduceSum(input)
  result

export fn vectorTotal(values: Tensor[float32, ?]) -> float32:
  input = values
  result = reduceSum(input)
  result
