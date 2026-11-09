fn seed(first: f32, second: f32, third: f32) -> Tensor[f32, 3]:
  return tensor([first, second, third])

fn forward(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return values

fn prepare(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return forward(values)

fn combine(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  product = left * right
  return product + left

fn main() -> Tensor[f32, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    left = prepare(seed(1, 2, 3))
    right = prepare(seed(4, 5, 6))
    leftCopy = transfer(left, gpu)
    rightCopy = transfer(right, gpu)
    deviceLeft = try await leftCopy
    deviceRight = try await rightCopy
    work = spawn combine(deviceLeft, deviceRight) on gpu
    result = try await work
    host = try await transfer(result, devices.cpu())
    prepare(host)
