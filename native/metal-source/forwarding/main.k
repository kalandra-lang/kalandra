from "./public.k" import project, reversed, total, square
from "./operations.k" import vectorTotal

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    input: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 0], [0, 1], [1, 1]])
    vector: Tensor[float32, 3] = tensor([1, 2, 3])
    a = try await transfer(input, gpu)
    b = try await transfer(weights, gpu)
    v = try await transfer(vector, gpu)
    first = try await spawn project(a, b) on gpu
    second = try await spawn reversed(b, a) on gpu
    alias = try await spawn square(a, second) on gpu
    sumTask = spawn total(first) on gpu
    sum = try await sumTask
    repeated = try await sumTask
    swapped = try await spawn total(second) on gpu
    squared = try await spawn total(alias) on gpu
    vectorSum = try await spawn vectorTotal(v) on gpu
    repeated + swapped + squared + vectorSum
