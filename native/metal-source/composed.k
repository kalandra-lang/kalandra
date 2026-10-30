fn add(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left + right

fn combine(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  square = right * right
  add(left, square) + 0.5

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu(0)
    left: Tensor[float32, 3] = tensor([1, 2, 3])
    right: Tensor[float32, 3] = tensor([4, 5, 6])
    leftCopy = transfer(left, gpu)
    rightCopy = transfer(right, gpu)
    deviceLeft = try await leftCopy
    deviceRight = try await rightCopy
    result = try await spawn combine(deviceLeft, deviceRight) on gpu
    try await transfer(result, devices.cpu())
