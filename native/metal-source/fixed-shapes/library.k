export fn vector(values: Tensor[float32, 3]) -> Tensor[float32, 3]:
  values * 2

export fn selected(ignored: Tensor[float32, 2], values: Tensor[float32, 3]) -> Tensor[float32, 3]:
  values + 1

export fn total(values: Tensor[float32, 2, 3]) -> float32:
  reduceSum(values)

export fn project(input: Tensor[float32, ?, 3], weights: Tensor[float32, 3, ?]) -> Tensor[float32, 2, 2]:
  matmul(input, weights)

export fn pinned(values: Tensor[float32, ?]) -> Tensor[float32, 3]:
  checked: Tensor[float32, 3] = values
  checked * 2

export fn transitive(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  vector(values) + 1

export fn pinnedResult(values: Tensor[float32, ?]) -> Tensor[float32, 3]:
  values * 2

export fn pinnedMatrix(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, 2, 2]:
  matmul(input, weights)

export fn empty(values: Tensor[float32, 0]) -> Tensor[float32, 0]:
  values + 1
