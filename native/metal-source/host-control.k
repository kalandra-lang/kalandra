fn enabled(total: f32, threshold: f32) -> bool:
  return total > threshold

fn total(values: Tensor[f32, ?]) -> f32:
  return reduceSum(values)

fn normalize(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return values / reduceSum(values)

fn adjust(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return values - 1

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    resident = try await transfer(values, gpu)
    sum = try await spawn total(resident) on gpu
    selected = enabled(sum, 0) and not (sum != sum)
    result = if selected:
      try await spawn normalize(resident) on gpu
    else:
      try await spawn adjust(resident) on gpu
    try await transfer(result, devices.cpu())
