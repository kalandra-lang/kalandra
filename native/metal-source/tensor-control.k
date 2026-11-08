fn adjust(values: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  return if factor > 0:
    values * factor
  else:
    -values / factor

fn normalize(values: Tensor[f32, ?], threshold: f32) -> Tensor[f32, ?]:
  total = reduceSum(values)
  return if total > threshold:
    values / total
  else:
    values * threshold

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    input = try await transfer(values, gpu)
    first = try await spawn adjust(input, 2) on gpu
    result = try await spawn normalize(first, 1) on gpu
    try await transfer(result, devices.cpu())
