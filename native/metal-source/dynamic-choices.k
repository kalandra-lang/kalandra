fn choose(a: Tensor[f32, ?], b: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  combined = a + b
  return if factor > 0:
    combined * factor
  else:
    b / factor

fn normalize(a: Tensor[f32, ?], b: Tensor[f32, ?], threshold: f32) -> Tensor[f32, ?]:
  combined = a + b
  total = reduceSum(combined)
  return if total > threshold:
    combined / total
  else:
    b * threshold

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 3] = tensor([1, 2, 3])
    b: Tensor[f32, 3] = tensor([4, 5, 6])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    first = try await spawn choose(x, y, 2) on gpu
    result = try await spawn normalize(first, y, 1) on gpu
    try await transfer(result, devices.cpu())
