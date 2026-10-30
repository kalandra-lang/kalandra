fn adjust(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  if factor > 0:
    values * factor
  else:
    -values / factor

fn normalize(values: Tensor[float32, ?], threshold: float32) -> Tensor[float32, ?]:
  total = reduceSum(values)
  if total > threshold:
    values / total
  else:
    values * threshold

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    input = try await transfer(values, gpu)
    first = try await spawn adjust(input, 2) on gpu
    result = try await spawn normalize(first, 1) on gpu
    try await transfer(result, devices.cpu())
