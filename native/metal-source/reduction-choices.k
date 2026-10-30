fn finish(total: float32, threshold: float32) -> float32:
  valid = total > threshold and total == total
  if valid:
    total * 2
  else:
    threshold

fn score(values: Tensor[float32, ?], threshold: float32) -> float32:
  total = reduceSum(values)
  finish(total, threshold)

fn main() -> float32 ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    input = try await transfer(values, gpu)
    work = spawn score(input, 1) on gpu
    result = try await work
    cached = try await work
    cached
