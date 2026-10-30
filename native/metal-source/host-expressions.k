fn scale(values: Tensor[float32, ?, ?], factor: float32) -> Tensor[float32, ?, ?]:
  values * factor

fn finish(value: float32, extra: float32) -> float32:
  value + extra

fn __host_expression_3(value: float32) -> float32:
  value

fn main() -> float32 ! TaskError:
  try parallel:
    left: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 0], [0, 1], [1, 1]])
    gpu = devices.gpu()
    # CPU expressions can appear in transfer arguments.
    deviceInput = try await transfer((left + left) / 2, gpu)
    # Scalar arguments can themselves introduce compiled host operations.
    work = spawn scale(deviceInput, (reduceSum(left) - 19) / 2) on gpu
    resident = try await work
    host = try await transfer(resident, devices.cpu())
    projected = matmul(+host, weights)
    centered = -(projected - 1)
    scaled = 2 * centered / (1 + 1)
    reversed = 180 / -scaled
    # Nested arguments may grow the typed function/symbol arenas.
    finish(reduceSum(reversed + projected), __host_expression_3(-(reduceSum(host) - 21)))
