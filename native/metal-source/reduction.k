fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn scale(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  values * factor

fn finish(value: float32) -> float32:
  value + 1

fn matrixTotal(values: Tensor[float32, ?, ?]) -> float32:
  reduceSum(values)

fn main() -> float32 ! TaskError:
  try parallel:
    values: Tensor[float32, 5] = tensor([1, 2, 3, 4, 5])
    local = total(values)
    gpu = devices.gpu()
    uploaded = try await transfer(values, gpu)
    scaled = try await spawn scale(uploaded, 2) on gpu
    work = spawn total(scaled) on gpu
    result = try await work
    finish(result)
