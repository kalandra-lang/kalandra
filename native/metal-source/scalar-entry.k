fn scale(values: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  return values * factor

fn affine(bias: f32, values: Tensor[f32, ?], factor: f32) -> Tensor[f32, ?]:
  return scale(values, factor) + bias

fn blend(left: Tensor[f32, ?], weight: f32, right: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return scale(left, weight) + right

fn identity(values: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return values

fn many(values: Tensor[f32, ?], a: f32, b: f32, c: f32, d: f32, e: f32, f: f32, g: f32, h: f32) -> Tensor[f32, ?]:
  return values * a + b + c + d + e + f + g + h

fn main() -> Tensor[f32, 3] ! TaskError:
  return try parallel:
    values: Tensor[f32, 3] = tensor([1, 2, 3])
    factor: f32 = 2
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
