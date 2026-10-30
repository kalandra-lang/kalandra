fn negative(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  -values

fn restored(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  -negative(+values)

fn positive(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  +values

fn invertedSum(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  -(left + right)

fn vectorNegative(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  -values

fn vectorRestored(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  -vectorNegative(+values)

fn vectorPositive(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  +values

fn vectorInvertedSum(left: Tensor[float32, ?], right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  -(left + right)

fn add(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left + right

fn main() -> Tensor[float32, 2, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    left: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    right: Tensor[float32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
    a = try await transfer(left, gpu)
    b = try await transfer(right, gpu)
    negated = try await spawn negative(a) on gpu
    twice = try await spawn restored(negated) on gpu
    unchanged = try await spawn positive(twice) on gpu
    inverted = try await spawn invertedSum(unchanged, b) on gpu
    observed = try await transfer(inverted, devices.cpu())
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    uploaded = try await transfer(values, gpu)
    vn = try await spawn vectorNegative(uploaded) on gpu
    vr = try await spawn vectorRestored(vn) on gpu
    vp = try await spawn vectorPositive(vr) on gpu
    vs = try await spawn vectorInvertedSum(vp, uploaded) on gpu
    vectorResult = try await transfer(vs, devices.cpu())
    remote = try await transfer(unchanged, devices.cpu())
    local = negative(left)
    add(local, remote)
