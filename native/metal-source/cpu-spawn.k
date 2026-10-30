fn project(input: Tensor[float32, ?, ?], weights: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  matmul(input, weights)

fn total(values: Tensor[float32, ?, ?]) -> float32:
  reduceSum(values)

fn shift(values: Tensor[float32, ?, ?], offset: float32) -> Tensor[float32, ?, ?]:
  values + offset

fn fixed(values: Tensor[float32, 2, 3]) -> float32:
  reduceSum(values)

fn scale(values: Tensor[float32, ?, ?], factor: float32) -> Tensor[float32, ?, ?]:
  values * factor

fn main() -> Tensor[float32, 2, 2] ! TaskError:
  try parallel:
    left: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    weights: Tensor[float32, 3, 2] = tensor([[1, 0], [0, 1], [1, 1]])
    cpu = devices.cpu()
    destination = cpu
    projection = spawn project(left, weights)
    sumTask = spawn total(left) on destination
    gpu = devices.gpu()
    input = try await transfer(left, gpu)
    doubled = try await spawn scale(input, fixed(left) / 10.5) on gpu
    host = try await transfer(doubled, cpu)
    projected = try await projection
    sum = try await sumTask
    adjusted = try await spawn shift(projected, sum / 21) on devices.cpu()
    uploaded = try await transfer(adjusted, gpu)
    scaled = try await spawn scale(uploaded, 2) on gpu
    result = try await transfer(scaled, cpu)
    extra = try await spawn fixed(host)
    result + extra
