fn divide(values: Tensor[f32, ?], denominator: f32) -> Tensor[f32, ?]:
  return values / denominator

fn normalize(values: Tensor[f32, ?], scale: f32) -> Tensor[f32, ?]:
  total = reduceSum(values)
  return divide(values, total / scale)

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 3] = tensor([2, 4, 6])
    input = try await transfer(values, gpu)
    work = spawn normalize(input, 3) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
