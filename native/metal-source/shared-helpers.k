fn offset(value: float32) -> float32:
  value * 2 + 0.5

fn add(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left + right

fn multiply(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left * right

fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  product = multiply(left, right)
  add(product, left) + offset(0)

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
    left: Tensor[float32, 3] = tensor([1, 2, 3])
    right: Tensor[float32, 3] = tensor([4, 5, 6])
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
