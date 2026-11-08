from "./operations.k" import project as multiply, total as sum

fn matrixIdentity(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn scalarIdentity(value: f32) -> f32:
  return value

export fn project(input: Tensor[f32, 2, 3], weights: Tensor[f32, 3, 2]) -> Tensor[f32, 2, 2]:
  result = multiply(matrixIdentity(input), weights)
  return matrixIdentity(result)

export fn reversed(weights: Tensor[f32, 3, 2], input: Tensor[f32, 2, 3]) -> Tensor[f32, 2, 2]:
  return project(input, weights)

export fn total(values: Tensor[f32, 2, 2]) -> f32:
  value = sum(values)
  return scalarIdentity(value)

export fn square(ignored: Tensor[f32, ?, ?], input: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return multiply(input, input)
