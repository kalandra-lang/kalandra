fn enabled(total: float32, threshold: float32) -> Bool:
  total > threshold

fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn normalize(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values / reduceSum(values)

fn adjust(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values - 1

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    resident = try await transfer(values, gpu)
    sum = try await spawn total(resident) on gpu
    selected = enabled(sum, 0) and not (sum != sum)
    result = if selected:
      try await spawn normalize(resident) on gpu
    else:
      try await spawn adjust(resident) on gpu
    try await transfer(result, devices.cpu())
