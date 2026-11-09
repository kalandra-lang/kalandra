fn finish(total: f32, threshold: f32) -> f32:
  valid = total > threshold and total == total
  return if valid:
    total * 2
  else:
    threshold

fn score(values: Tensor[f32, ?], threshold: f32) -> f32:
  total = reduceSum(values)
  return finish(total, threshold)

fn main() -> f32 ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    input = try await transfer(values, gpu)
    work = spawn score(input, 1) on gpu
    result = try await work
    cached = try await work
    cached
