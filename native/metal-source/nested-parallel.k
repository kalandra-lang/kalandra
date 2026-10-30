fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn shift(values: Tensor[float32, ?], offset: float32) -> Tensor[float32, ?]:
  values + offset

fn scale(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  values * factor

fn main() -> float32 ! TaskError:
  try parallel:
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    pending = spawn total(values)
    gpu = devices.gpu()
    resident = try parallel:
      values: Tensor[float32, 3] = tensor([4, 5, 6])
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
