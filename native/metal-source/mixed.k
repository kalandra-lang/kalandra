fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn finish(value: float32, extra: float32) -> float32:
  value + extra

fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  product = left * right
  product + left

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    left: Tensor[float32, 3] = tensor([1, 2, 3])
    right: Tensor[float32, 3] = tensor([4, 5, 6])
    baseline = total(left)
    leftCopy = transfer(left, gpu)
    rightCopy = transfer(right, gpu)
    deviceLeft = try await leftCopy
    deviceRight = try await rightCopy
    work = spawn combine(deviceLeft, deviceRight) on gpu
    result = try await work
    host = try await transfer(result, devices.cpu())
    finish(total(host), baseline)
