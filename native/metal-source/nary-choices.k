fn choose(a: Tensor[f32, ?], b: Tensor[f32, ?], c: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  combined = a + b + c
  return if factor > 0:
    if factor > 1:
      a * factor
    else:
      b / factor
  else:
    c + factor

fn normalize(a: Tensor[f32, ?], b: Tensor[f32, ?], c: Tensor[f32, ?], threshold: f32) -> Tensor[f32, ?]:
  total = reduceSum(a + b + c)
  return if total > threshold:
    a / total
  else:
    if total < 0:
      b * threshold
    else:
      c + threshold

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 3] = tensor([1, 2, 3])
    b: Tensor[f32, 3] = tensor([4, 5, 6])
    c: Tensor[f32, 3] = tensor([7, 8, 9])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    z = try await transfer(c, gpu)
    first = try await spawn choose(x, y, z, 2) on gpu
    result = try await spawn normalize(first, y, z, 1) on gpu
    try await transfer(result, devices.cpu())
