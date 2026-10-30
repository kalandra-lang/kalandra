fn choose(a: Tensor[float32, ?], b: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  combined = a + b
  if factor > 0:
    combined * factor
  else:
    b / factor

fn normalize(a: Tensor[float32, ?], b: Tensor[float32, ?], threshold: float32) -> Tensor[float32, ?]:
  combined = a + b
  total = reduceSum(combined)
  if total > threshold:
    combined / total
  else:
    b * threshold

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 3] = tensor([1, 2, 3])
    b: Tensor[float32, 3] = tensor([4, 5, 6])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    first = try await spawn choose(x, y, 2) on gpu
    result = try await spawn normalize(first, y, 1) on gpu
    try await transfer(result, devices.cpu())
