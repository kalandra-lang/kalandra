fn offset(value: f32) -> f32:
  return value * 2 + 0.5

fn add(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return left + right

fn multiply(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return left * right

fn combine(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  product = multiply(left, right)
  return add(product, left) + offset(0)

fn main() -> Tensor[f32, 3] ! TaskError:
  return try parallel:
    left: Tensor[f32, 3] = tensor([1, 2, 3])
    right: Tensor[f32, 3] = tensor([4, 5, 6])
    local = combine(left, right)
    gpu = devices.gpu()
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    work = spawn combine(a, b) on gpu
    remote = try await transfer(try await work, devices.cpu())
    sumWork = spawn add(a, b) on gpu
    sumResult = try await transfer(try await sumWork, devices.cpu())
    productWork = spawn multiply(a, b) on gpu
    productResult = try await transfer(try await productWork, devices.cpu())
    add(local, remote)
