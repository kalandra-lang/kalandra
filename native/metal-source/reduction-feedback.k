fn center(values: Tensor[float32, ?], mean: float32) -> Tensor[float32, ?]:
  values - mean

fn variance(values: Tensor[float32, ?], count: float32) -> float32:
  total = reduceSum(values)
  centered = center(values, total / count)
  squares = centered * centered
  reduceSum(squares) / count

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([2, 4, 6])
    input = try await transfer(values, gpu)
    work = spawn variance(input, 3) on gpu
    result = try await work
    try await work
