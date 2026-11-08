fn scale(values: Tensor[f32, ?, ?], factor: f32) -> Tensor[f32, ?, ?]:
  return values * factor

fn add(left: Tensor[f32, ?, ?], right: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return left + right

fn combine(left: Tensor[f32, ?, ?], factor: f32, right: Tensor[f32, ?, ?], bias: f32) -> Tensor[f32, ?, ?]:
  product = scale(left, factor)
  return add(product, right) + bias

fn identity(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn main() -> Tensor[f32, 2, 3] ! TaskError:
  return try parallel:
    left: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    right: Tensor[f32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
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
