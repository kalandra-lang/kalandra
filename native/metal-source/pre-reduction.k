fn adjusted(values: Tensor[f32, ?], shift: f32, factor: f32) -> Tensor[f32, ?]:
  return (values + shift) * factor

fn total(values: Tensor[f32, ?], shift: f32, factor: f32) -> f32:
  return reduceSum(adjusted(values, shift, factor))

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 5] = tensor([1, 2, 3, 4, 5])
    resident = try await transfer(values, gpu)
    work = spawn total(resident, 2, 3) on gpu
    result = try await work
    repeated = try await work
    repeated
