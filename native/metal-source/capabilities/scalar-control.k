fn normalize(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  total = reduceSum(values)
  denominator: f32 = if total == 0:
    1
  else:
    total
  return values / denominator

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 2] = tensor([0, 0])
    input = try await transfer(values, gpu)
    result = try await spawn normalize(input) on gpu
    try await transfer(result, devices.cpu())
