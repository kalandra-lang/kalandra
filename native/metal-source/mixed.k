fn total(values: Tensor[f32, ?]) -> f32:
  return reduceSum(values)

fn finish(value: f32, extra: f32) -> f32:
  return value + extra

fn combine(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  product = left * right
  return product + left

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    left: Tensor[f32, 3] = tensor([1, 2, 3])
    right: Tensor[f32, 3] = tensor([4, 5, 6])
    baseline = total(left)
    leftCopy = transfer(left, gpu)
    rightCopy = transfer(right, gpu)
    deviceLeft = try await leftCopy
    deviceRight = try await rightCopy
    work = spawn combine(deviceLeft, deviceRight) on gpu
    result = try await work
    host = try await transfer(result, devices.cpu())
    finish(total(host), baseline)
