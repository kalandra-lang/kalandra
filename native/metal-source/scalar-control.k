fn classify(left: f32, right: f32) -> f32:
  eq: f32 = if left == right:
    1
  else:
    0
  ne: f32 = if left != right:
    1
  else:
    0
  lt: f32 = if left < right:
    1
  else:
    0
  le: f32 = if left <= right:
    1
  else:
    0
  gt: f32 = if left > right:
    1
  else:
    0
  ge: f32 = if left >= right:
    1
  else:
    0
  return eq + ne * 2 + lt * 4 + le * 8 + gt * 16 + ge * 32

fn project(values: Tensor[f32, ?], left: f32, right: f32) -> Tensor[f32, ?]:
  return values * classify(left, right)

fn choose(total: f32, threshold: f32) -> f32:
  valid = total > threshold and not (total != total)
  return if valid:
    total
  else:
    threshold

fn normalize(values: Tensor[f32, ?], threshold: f32) -> Tensor[f32, ?]:
  total = reduceSum(values)
  denominator = choose(total, threshold)
  return values / denominator

fn main() -> Tensor[f32, ?] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    input = try await transfer(values, gpu)
    classified = try await spawn project(input, 0, 0) on gpu
    result = try await spawn normalize(classified, 1) on gpu
    try await transfer(result, devices.cpu())
