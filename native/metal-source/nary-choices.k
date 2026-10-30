fn choose(a: Tensor[float32, ?], b: Tensor[float32, ?], c: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  combined = a + b + c
  if factor > 0:
    if factor > 1:
      a * factor
    else:
      b / factor
  else:
    c + factor

fn normalize(a: Tensor[float32, ?], b: Tensor[float32, ?], c: Tensor[float32, ?], threshold: float32) -> Tensor[float32, ?]:
  total = reduceSum(a + b + c)
  if total > threshold:
    a / total
  else:
    if total < 0:
      b * threshold
    else:
      c + threshold

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 3] = tensor([1, 2, 3])
    b: Tensor[float32, 3] = tensor([4, 5, 6])
    c: Tensor[float32, 3] = tensor([7, 8, 9])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    z = try await transfer(c, gpu)
    first = try await spawn choose(x, y, z, 2) on gpu
    result = try await spawn normalize(first, y, z, 1) on gpu
    try await transfer(result, devices.cpu())
