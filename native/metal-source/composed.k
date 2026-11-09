fn add(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return left + right

fn combine(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  square = right * right
  return add(left, square) + 0.5

fn main() -> Tensor[f32, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu(0)
    left: Tensor[f32, 3] = tensor([1, 2, 3])
    right: Tensor[f32, 3] = tensor([4, 5, 6])
    leftCopy = transfer(left, gpu)
    rightCopy = transfer(right, gpu)
    deviceLeft = try await leftCopy
    deviceRight = try await rightCopy
    result = try await spawn combine(deviceLeft, deviceRight) on gpu
    try await transfer(result, devices.cpu())
