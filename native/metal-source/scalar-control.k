fn classify(left: float32, right: float32) -> float32:
  eq: float32 = if left == right:
    1
  else:
    0
  ne: float32 = if left != right:
    1
  else:
    0
  lt: float32 = if left < right:
    1
  else:
    0
  le: float32 = if left <= right:
    1
  else:
    0
  gt: float32 = if left > right:
    1
  else:
    0
  ge: float32 = if left >= right:
    1
  else:
    0
  eq + ne * 2 + lt * 4 + le * 8 + gt * 16 + ge * 32

fn project(values: Tensor[float32, ?], left: float32, right: float32) -> Tensor[float32, ?]:
  values * classify(left, right)

fn choose(total: float32, threshold: float32) -> float32:
  valid = total > threshold and not (total != total)
  if valid:
    total
  else:
    threshold

fn normalize(values: Tensor[float32, ?], threshold: float32) -> Tensor[float32, ?]:
  total = reduceSum(values)
  denominator = choose(total, threshold)
  values / denominator

fn main() -> Tensor[float32, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    input = try await transfer(values, gpu)
    classified = try await spawn project(input, 0, 0) on gpu
    result = try await spawn normalize(classified, 1) on gpu
    try await transfer(result, devices.cpu())
