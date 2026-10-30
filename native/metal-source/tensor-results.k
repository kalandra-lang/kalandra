fn seed(first: float32, second: float32, third: float32) -> Tensor[float32, 3]:
  tensor([first, second, third])

fn forward(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values

fn prepare(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  forward(values)

fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  product = left * right
  product + left

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
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
