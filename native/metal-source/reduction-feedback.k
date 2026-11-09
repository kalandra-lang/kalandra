fn center(values: Tensor[f32, ?], mean: f32) -> Tensor[f32, ?]:
  return values - mean

fn variance(values: Tensor[f32, ?], count: f32) -> f32:
  total = reduceSum(values)
  centered = center(values, total / count)
  squares = centered * centered
  return reduceSum(squares) / count

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 3] = tensor([2, 4, 6])
    input = try await transfer(values, gpu)
    work = spawn variance(input, 3) on gpu
    result = try await work
    try await work
