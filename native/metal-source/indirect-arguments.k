fn wide(ignored: Tensor[f32, ?, ?], a0: Tensor[f32, ?], scale: f32, a1: Tensor[f32, ?], a2: Tensor[f32, ?], a3: Tensor[f32, ?], a4: Tensor[f32, ?], a5: Tensor[f32, ?], a6: Tensor[f32, ?], a7: Tensor[f32, ?], a8: Tensor[f32, ?], a9: Tensor[f32, ?], a10: Tensor[f32, ?], a11: Tensor[f32, ?], a12: Tensor[f32, ?], a13: Tensor[f32, ?], a14: Tensor[f32, ?], a15: Tensor[f32, ?], a16: Tensor[f32, ?], a17: Tensor[f32, ?], a18: Tensor[f32, ?], a19: Tensor[f32, ?], a20: Tensor[f32, ?], a21: Tensor[f32, ?], a22: Tensor[f32, ?], a23: Tensor[f32, ?], a24: Tensor[f32, ?], a25: Tensor[f32, ?], a26: Tensor[f32, ?], a27: Tensor[f32, ?], a28: Tensor[f32, ?], a29: Tensor[f32, ?], a30: Tensor[f32, ?], a31: Tensor[f32, ?], a32: Tensor[f32, ?], a33: Tensor[f32, ?], a34: Tensor[f32, ?], a35: Tensor[f32, ?], a36: Tensor[f32, ?], a37: Tensor[f32, ?], a38: Tensor[f32, ?], a39: Tensor[f32, ?], bias: f32) -> Tensor[f32, ?]:
  summed = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 + a32 + a33 + a34 + a35 + a36 + a37 + a38 + a39
  return summed * scale + bias

fn matrixWide(ignored: Tensor[f32, ?], a0: Tensor[f32, ?, ?], scale: f32, a1: Tensor[f32, ?, ?], a2: Tensor[f32, ?, ?], a3: Tensor[f32, ?, ?], a4: Tensor[f32, ?, ?], a5: Tensor[f32, ?, ?], a6: Tensor[f32, ?, ?], a7: Tensor[f32, ?, ?], a8: Tensor[f32, ?, ?], a9: Tensor[f32, ?, ?], a10: Tensor[f32, ?, ?], a11: Tensor[f32, ?, ?], a12: Tensor[f32, ?, ?], a13: Tensor[f32, ?, ?], a14: Tensor[f32, ?, ?], a15: Tensor[f32, ?, ?], a16: Tensor[f32, ?, ?], a17: Tensor[f32, ?, ?], a18: Tensor[f32, ?, ?], a19: Tensor[f32, ?, ?], a20: Tensor[f32, ?, ?], a21: Tensor[f32, ?, ?], a22: Tensor[f32, ?, ?], a23: Tensor[f32, ?, ?], a24: Tensor[f32, ?, ?], a25: Tensor[f32, ?, ?], a26: Tensor[f32, ?, ?], a27: Tensor[f32, ?, ?], a28: Tensor[f32, ?, ?], a29: Tensor[f32, ?, ?], a30: Tensor[f32, ?, ?], a31: Tensor[f32, ?, ?], a32: Tensor[f32, ?, ?], a33: Tensor[f32, ?, ?], a34: Tensor[f32, ?, ?], a35: Tensor[f32, ?, ?], a36: Tensor[f32, ?, ?], a37: Tensor[f32, ?, ?], a38: Tensor[f32, ?, ?], a39: Tensor[f32, ?, ?], bias: f32) -> Tensor[f32, ?, ?]:
  summed = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 + a32 + a33 + a34 + a35 + a36 + a37 + a38 + a39
  return summed * scale + bias

fn add(left: Tensor[f32, ?, ?], right: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return left + right

fn main() -> Tensor[f32, 2, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    m: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    v: Tensor[f32, 3] = tensor([1, 2, 3])
    dm = try await transfer(m, gpu)
    dv = try await transfer(v, gpu)
    vector = try await spawn wide(dm, dv, 0.5, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, 1) on gpu
    observed = try await transfer(vector, devices.cpu())
    matrix = try await spawn matrixWide(dv, dm, 0.5, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, 1) on gpu
    remote = try await transfer(matrix, devices.cpu())
    local = matrixWide(v, m, 0.5, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, 1)
    add(local, remote)
