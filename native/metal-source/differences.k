fn negate(value: f32) -> f32:
  return -value

fn difference(left: Tensor[f32, ?, ?], right: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return left - right

fn lower(values: Tensor[f32, ?, ?], bias: f32) -> Tensor[f32, ?, ?]:
  return values - bias

fn reverse(bias: f32, values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return bias - values

fn shift(values: Tensor[f32, ?, ?], bias: f32) -> Tensor[f32, ?, ?]:
  return values + negate(bias)

fn vectorDifference(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return left - right

fn vectorLower(values: Tensor[f32, ?], bias: f32) -> Tensor[f32, ?]:
  return values - bias

fn vectorReverse(bias: f32, values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return bias - values

fn vectorShift(values: Tensor[f32, ?], bias: f32) -> Tensor[f32, ?]:
  return values + negate(bias)

fn total(values: Tensor[f32, ?]) -> f32:
  return reduceSum(values)

fn main() -> Tensor[f32, 2, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    left: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    right: Tensor[f32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    delta = try await spawn difference(a, b) on gpu
    lowered = try await spawn lower(delta, 1) on gpu
    reversed = try await spawn reverse(0, lowered) on gpu
    shifted = try await spawn shift(reversed, 1) on gpu
    remote = try await transfer(shifted, devices.cpu())
    x: Tensor[f32, 3] = tensor([1, 2, 3])
    y: Tensor[f32, 3] = tensor([4, 5, 6])
    c = try await transfer(x, gpu)
    d = try await transfer(y, gpu)
    vd = try await spawn vectorDifference(c, d) on gpu
    vl = try await spawn vectorLower(vd, 1) on gpu
    vr = try await spawn vectorReverse(0, vl) on gpu
    vs = try await spawn vectorShift(vr, 1) on gpu
    vectorResult = try await transfer(vs, devices.cpu())
    lower(remote, total(vectorResult))
