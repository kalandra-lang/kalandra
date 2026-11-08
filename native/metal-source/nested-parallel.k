fn total(values: Tensor[f32, ?]) -> f32:
  return reduceSum(values)

fn shift(values: Tensor[f32, ?], offset: f32) -> Tensor[f32, ?]:
  return values + offset

fn scale(values: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  return values * factor

fn main() -> f32 ! TaskError:
  return try parallel:
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    pending = spawn total(values)
    gpu = devices.gpu()
    resident = try parallel:
      values: Tensor[f32, 3] = tensor([4, 5, 6])
      host = try parallel:
        moved = spawn shift(values, 1)
        try await moved
      input = try await transfer(host, gpu)
      try await spawn scale(input, 2) on gpu
    host = try await transfer(resident, devices.cpu())
    result = try parallel:
      prior = try await pending
      inner = try parallel:
        try await spawn total(host)
      prior + inner
    result + reduceSum(values)
