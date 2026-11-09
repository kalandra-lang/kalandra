fn reciprocal(value: f32) -> f32:
  return 1 / value

fn quotient(left: Tensor[f32, ?, ?], right: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return left / right

fn scale(values: Tensor[f32, ?, ?], divisor: f32) -> Tensor[f32, ?, ?]:
  return values / divisor

fn reverse(numerator: f32, values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return numerator / values

fn applyReciprocal(values: Tensor[f32, ?, ?], divisor: f32) -> Tensor[f32, ?, ?]:
  return values * reciprocal(divisor)

fn vectorQuotient(left: Tensor[f32, ?], right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return left / right

fn vectorScale(values: Tensor[f32, ?], divisor: f32) -> Tensor[f32, ?]:
  return values / divisor

fn vectorReverse(numerator: f32, values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return numerator / values

fn vectorApplyReciprocal(values: Tensor[f32, ?], divisor: f32) -> Tensor[f32, ?]:
  return values * reciprocal(divisor)

fn add(left: Tensor[f32, ?, ?], right: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return left + right

fn main() -> Tensor[f32, 2, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    left: Tensor[f32, 2, 3] = tensor([[2, 4, 8], [16, 32, 64]])
    right: Tensor[f32, 2, 3] = tensor([[1, 2, 4], [8, 16, 32]])
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    divided = try await spawn quotient(a, b) on gpu
    scaled = try await spawn scale(divided, 2) on gpu
    reversed = try await spawn reverse(8, scaled) on gpu
    result = try await spawn applyReciprocal(reversed, 2) on gpu
    remote = try await transfer(result, devices.cpu())
    values: Tensor[f32, 3] = tensor([1, 2, 4])
    uploaded = try await transfer(values, gpu)
    vq = try await spawn vectorQuotient(uploaded, uploaded) on gpu
    vs = try await spawn vectorScale(vq, 2) on gpu
    vr = try await spawn vectorReverse(8, vs) on gpu
    va = try await spawn vectorApplyReciprocal(vr, 2) on gpu
    vectorResult = try await transfer(va, devices.cpu())
    local = quotient(left, right)
    add(local, remote)
