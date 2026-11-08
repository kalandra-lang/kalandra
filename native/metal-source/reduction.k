fn total(values: Tensor[f32, ?]) -> f32:
  return reduceSum(values)

fn scale(values: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  return values * factor

fn finish(value: f32) -> f32:
  return value + 1

fn matrixTotal(values: Tensor[f32, ?, ?]) -> f32:
  return reduceSum(values)

fn main() -> f32 ! TaskError:
  return try parallel:
    values: Tensor[f32, 5] = tensor([1, 2, 3, 4, 5])
    local = total(values)
    gpu = devices.gpu()
    uploaded = try await transfer(values, gpu)
    scaled = try await spawn scale(uploaded, 2) on gpu
    work = spawn total(scaled) on gpu
    result = try await work
    finish(result)
