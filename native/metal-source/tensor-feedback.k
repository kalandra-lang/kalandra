fn divide(values: Tensor[float32, ?], denominator: float32) -> Tensor[float32, ?]:
  values / denominator

fn normalize(values: Tensor[float32, ?], scale: float32) -> Tensor[float32, ?]:
  total = reduceSum(values)
  divide(values, total / scale)

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([2, 4, 6])
    input = try await transfer(values, gpu)
    work = spawn normalize(input, 3) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
