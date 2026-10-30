fn negate(value: float32) -> float32:
  -value

fn difference(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left - right

fn lower(values: Tensor[float32, ?, ?], bias: float32) -> Tensor[float32, ?, ?]:
  values - bias

fn reverse(bias: float32, values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  bias - values

fn shift(values: Tensor[float32, ?, ?], bias: float32) -> Tensor[float32, ?, ?]:
  values + negate(bias)

fn vectorDifference(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left - right

fn vectorLower(values: Tensor[float32, ?], bias: float32) -> Tensor[float32, ?]:
  values - bias

fn vectorReverse(bias: float32, values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  bias - values

fn vectorShift(values: Tensor[float32, ?], bias: float32) -> Tensor[float32, ?]:
  values + negate(bias)

fn total(values: Tensor[float32, ?]) -> float32:
  reduceSum(values)

fn main() -> Tensor[float32, 2, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    left: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    right: Tensor[float32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    delta = try await spawn difference(a, b) on gpu
    lowered = try await spawn lower(delta, 1) on gpu
    reversed = try await spawn reverse(0, lowered) on gpu
    shifted = try await spawn shift(reversed, 1) on gpu
    remote = try await transfer(shifted, devices.cpu())
    x: Tensor[float32, 3] = tensor([1, 2, 3])
    y: Tensor[float32, 3] = tensor([4, 5, 6])
    c = try await transfer(x, gpu)
    d = try await transfer(y, gpu)
    vd = try await spawn vectorDifference(c, d) on gpu
    vl = try await spawn vectorLower(vd, 1) on gpu
    vr = try await spawn vectorReverse(0, vl) on gpu
    vs = try await spawn vectorShift(vr, 1) on gpu
    vectorResult = try await transfer(vs, devices.cpu())
    lower(remote, total(vectorResult))
