fn scale(values: Tensor[float32, ?, ?], factor: float32) -> Tensor[float32, ?, ?]:
  values * factor

fn add(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left + right

fn combine(left: Tensor[float32, ?, ?], factor: float32, right: Tensor[float32, ?, ?], bias: float32) -> Tensor[float32, ?, ?]:
  product = scale(left, factor)
  add(product, right) + bias

fn identity(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn main() -> Tensor[float32, 2, 3] ! TaskError:
  try parallel:
    left: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    right: Tensor[float32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
    local = combine(left, 2, right, 0.5)
    gpu = devices.gpu()
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    combined = try await spawn combine(a, 2, b, 0.5) on gpu
    scaled = try await spawn scale(combined, 1) on gpu
    copied = try await spawn identity(scaled) on gpu
    summed = try await spawn add(copied, a) on gpu
    remote = try await transfer(summed, devices.cpu())
    add(local, remote)
