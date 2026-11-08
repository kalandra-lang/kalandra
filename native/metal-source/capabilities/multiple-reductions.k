fn combine(a: Tensor[f32, ?], b: Tensor[f32, ?], scale: f32) -> f32:
  adjusted = a / scale
  return (reduceSum(adjusted) - reduceSum(b)) / scale

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 3] = tensor([2, 4, 6])
    b: Tensor[f32, 2] = tensor([1, 3])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    try await spawn combine(x, y, 2) on gpu
