fn scale(values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  values * factor

fn affine(bias: float32, values: Tensor[float32, ?], factor: float32) -> Tensor[float32, ?]:
  scale(values, factor) + bias

fn blend(left: Tensor[float32, ?], weight: float32, right: Tensor[float32, ?]) -> Tensor[float32, ?]:
  scale(left, weight) + right

fn identity(values: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values

fn many(values: Tensor[float32, ?], a: float32, b: float32, c: float32, d: float32, e: float32, f: float32, g: float32, h: float32) -> Tensor[float32, ?]:
  values * a + b + c + d + e + f + g + h

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
    values: Tensor[float32, 3] = tensor([1, 2, 3])
    factor: float32 = 2
    local = affine(0.5, values, factor)
    gpu = devices.gpu()
    uploaded = try await transfer(values, gpu)
    first = spawn scale(uploaded, factor) on gpu
    scaled = try await first
    second = spawn affine(0.5, scaled, factor) on gpu
    shifted = try await second
    third = spawn blend(shifted, factor, uploaded) on gpu
    blended = try await third
    fourth = spawn identity(blended) on gpu
    copied = try await fourth
    fifth = spawn many(copied, 1, 1, 1, 1, 1, 1, 1, 1) on gpu
    result = try await fifth
    try await transfer(result, devices.cpu())
