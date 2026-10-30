fn choose(a: Tensor[float32, 2, 3], b: Tensor[float32, 2, 3], c: Tensor[float32, 2, 3], factor: float32) -> Tensor[float32, 2, 3]:
  if factor > 1:
    a
  else:
    if factor > 0:
      b
    else:
      c

fn project(a: Tensor[float32, 2, 3], b: Tensor[float32, 2, 3], c: Tensor[float32, 2, 3], x: Tensor[float32, 3, 2], y: Tensor[float32, 3, 2], factor: float32) -> Tensor[float32, 2, 2]:
  left = choose(a, b, c, factor)
  right = if factor > 0:
    x
  else:
    y
  matmul(left, right)

fn main() -> Tensor[float32, ?, ?] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    b: Tensor[float32, 2, 3] = tensor([[2, 3, 4], [5, 6, 7]])
    c: Tensor[float32, 2, 3] = tensor([[3, 4, 5], [6, 7, 8]])
    x: Tensor[float32, 3, 2] = tensor([[1, 2], [3, 4], [5, 6]])
    y: Tensor[float32, 3, 2] = tensor([[2, 1], [4, 3], [6, 5]])
    aa = try await transfer(a, gpu)
    bb = try await transfer(b, gpu)
    cc = try await transfer(c, gpu)
    xx = try await transfer(x, gpu)
    yy = try await transfer(y, gpu)
    work = spawn project(aa, bb, cc, xx, yy, 2) on gpu
    result = try await work
    cached = try await work
    try await transfer(cached, devices.cpu())
