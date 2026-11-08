fn choose(a: Tensor[f32, 3], b: Tensor[f32, 3], factor: f32) -> Tensor[f32, 3]:
  return if factor > 0:
    a * factor
  else:
    b / factor

fn normalize(a: Tensor[f32, 3], b: Tensor[f32, 3], threshold: f32) -> Tensor[f32, 3]:
  total = reduceSum(a)
  return if total > threshold:
    a / total
  else:
    b * threshold

fn main() -> Tensor[f32, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 3] = tensor([1, 2, 3])
    b: Tensor[f32, 3] = tensor([4, 5, 6])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    first = try await spawn choose(x, y, 2) on gpu
    result = try await spawn normalize(first, y, 1) on gpu
    try await transfer(result, devices.cpu())
