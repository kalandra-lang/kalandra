fn combine(a: Tensor[float32, ?], b: Tensor[float32, ?], scale: float32) -> float32:
  adjusted = a / scale
  (reduceSum(adjusted) - reduceSum(b)) / scale

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 3] = tensor([2, 4, 6])
    b: Tensor[float32, 2] = tensor([1, 3])
    x = try await transfer(a, gpu)
    y = try await transfer(b, gpu)
    try await spawn combine(x, y, 2) on gpu
