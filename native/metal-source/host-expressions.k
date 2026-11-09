fn scale(values: Tensor[f32, ?, ?], factor: f32) -> Tensor[f32, ?, ?]:
  return values * factor

fn finish(value: f32, extra: f32) -> f32:
  return value + extra

fn __host_expression_3(value: f32) -> f32:
  return value

fn main() -> f32 ! TaskError:
  return try parallel:
    left: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[f32, 3, 2] = tensor([[1, 0], [0, 1], [1, 1]])
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
