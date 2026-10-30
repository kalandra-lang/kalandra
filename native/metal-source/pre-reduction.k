fn adjusted(values: Tensor[float32, ?], shift: float32, factor: float32) -> Tensor[float32, ?]:
  (values + shift) * factor

fn total(values: Tensor[float32, ?], shift: float32, factor: float32) -> float32:
  reduceSum(adjusted(values, shift, factor))

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 5] = tensor([1, 2, 3, 4, 5])
    resident = try await transfer(values, gpu)
    work = spawn total(resident, 2, 3) on gpu
    result = try await work
    repeated = try await work
    repeated
