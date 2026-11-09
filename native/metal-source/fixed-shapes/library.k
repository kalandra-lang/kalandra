export fn vector(values: Tensor[f32, 3]) -> Tensor[f32, 3]:
  return values * 2

export fn selected(ignored: Tensor[f32, 2], values: Tensor[f32, 3]) -> Tensor[f32, 3]:
  return values + 1

export fn total(values: Tensor[f32, 2, 3]) -> f32:
  return reduceSum(values)

export fn project(input: Tensor[f32, ?, 3], weights: Tensor[f32, 3, ?]) -> Tensor[f32, 2, 2]:
  return matmul(input, weights)

export fn pinned(values: Tensor[f32, ?]) -> Tensor[f32, 3]:
  checked: Tensor[f32, 3] = values
  return checked * 2

export fn transitive(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return vector(values) + 1

export fn pinnedResult(values: Tensor[f32, ?]) -> Tensor[f32, 3]:
  return values * 2

export fn pinnedMatrix(input: Tensor[f32, ?, ?], weights: Tensor[f32, ?, ?]) -> Tensor[f32, 2, 2]:
  return matmul(input, weights)

export fn empty(values: Tensor[f32, 0]) -> Tensor[f32, 0]:
  return values + 1
