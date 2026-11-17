fn ordinary(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return (values + f32(1)) * f32(2)

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    input: Tensor[f32, 4] = tensor([1, 2, 3, 4])
    values = try await transfer(input, gpu)
    result = try await spawn ordinary(values) on gpu
    try await transfer(result, devices.cpu())
