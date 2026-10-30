fn choose(a: Tensor[float32, 3], b: Tensor[float32, 3], factor: float32) -> Tensor[float32, 3]:
  if factor > 0:
    a * factor
  else:
    b / factor

fn normalize(a: Tensor[float32, 3], b: Tensor[float32, 3], threshold: float32) -> Tensor[float32, 3]:
  total = reduceSum(a)
  if total > threshold:
    a / total
  else:
    b * threshold

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 3] = tensor([1, 2, 3])
    b: Tensor[float32, 3] = tensor([4, 5, 6])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    first = try await spawn choose(x, y, 2) on gpu
    result = try await spawn normalize(first, y, 1) on gpu
    try await transfer(result, devices.cpu())
