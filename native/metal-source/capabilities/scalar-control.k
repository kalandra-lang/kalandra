fn normalize(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  total = reduceSum(values)
  denominator: float32 = if total == 0:
    1
  else:
    total
  values / denominator

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 2] = tensor([0, 0])
    input = try await transfer(values, gpu)
    result = try await spawn normalize(input) on gpu
    try await transfer(result, devices.cpu())
