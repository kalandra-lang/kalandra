fn reciprocal(value: float32) -> float32:
  1 / value

fn quotient(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left / right

fn scale(values: Tensor[float32, ?, ?], divisor: float32) -> Tensor[float32, ?, ?]:
  values / divisor

fn reverse(numerator: float32, values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  numerator / values

fn applyReciprocal(values: Tensor[float32, ?, ?], divisor: float32) -> Tensor[float32, ?, ?]:
  values * reciprocal(divisor)

fn vectorQuotient(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  left / right

fn vectorScale(values: Tensor[float32, ?], divisor: float32) -> Tensor[float32, ?]:
  values / divisor

fn vectorReverse(numerator: float32, values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  numerator / values

fn vectorApplyReciprocal(values: Tensor[float32, ?], divisor: float32) -> Tensor[float32, ?]:
  values * reciprocal(divisor)

fn add(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left + right

fn main() -> Tensor[float32, 2, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    left: Tensor[float32, 2, 3] = tensor([[2, 4, 8], [16, 32, 64]])
    right: Tensor[float32, 2, 3] = tensor([[1, 2, 4], [8, 16, 32]])
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    divided = try await spawn quotient(a, b) on gpu
    scaled = try await spawn scale(divided, 2) on gpu
    reversed = try await spawn reverse(8, scaled) on gpu
    result = try await spawn applyReciprocal(reversed, 2) on gpu
    remote = try await transfer(result, devices.cpu())
    values: Tensor[float32, 3] = tensor([1, 2, 4])
    uploaded = try await transfer(values, gpu)
    vq = try await spawn vectorQuotient(uploaded, uploaded) on gpu
    vs = try await spawn vectorScale(vq, 2) on gpu
    vr = try await spawn vectorReverse(8, vs) on gpu
    va = try await spawn vectorApplyReciprocal(vr, 2) on gpu
    vectorResult = try await transfer(va, devices.cpu())
    local = quotient(left, right)
    add(local, remote)
