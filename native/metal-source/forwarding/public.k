from "./operations.k" import project as multiply, total as sum

fn matrixIdentity(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn scalarIdentity(value: float32) -> float32:
  value

export fn project(input: Tensor[float32, 2, 3], weights: Tensor[float32, 3, 2]) -> Tensor[float32, 2, 2]:
  result = multiply(matrixIdentity(input), weights)
  matrixIdentity(result)

export fn reversed(weights: Tensor[float32, 3, 2], input: Tensor[float32, 2, 3]) -> Tensor[float32, 2, 2]:
  project(input, weights)

export fn total(values: Tensor[float32, 2, 2]) -> float32:
  value = sum(values)
  scalarIdentity(value)

export fn square(ignored: Tensor[float32, ?, ?], input: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  multiply(input, input)
