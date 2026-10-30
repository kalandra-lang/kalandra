fn wide(ignored: Tensor[float32, ?, ?], a0: Tensor[float32, ?], scale: float32, a1: Tensor[float32, ?], a2: Tensor[float32, ?], a3: Tensor[float32, ?], a4: Tensor[float32, ?], a5: Tensor[float32, ?], a6: Tensor[float32, ?], a7: Tensor[float32, ?], a8: Tensor[float32, ?], a9: Tensor[float32, ?], a10: Tensor[float32, ?], a11: Tensor[float32, ?], a12: Tensor[float32, ?], a13: Tensor[float32, ?], a14: Tensor[float32, ?], a15: Tensor[float32, ?], a16: Tensor[float32, ?], a17: Tensor[float32, ?], a18: Tensor[float32, ?], a19: Tensor[float32, ?], a20: Tensor[float32, ?], a21: Tensor[float32, ?], a22: Tensor[float32, ?], a23: Tensor[float32, ?], a24: Tensor[float32, ?], a25: Tensor[float32, ?], a26: Tensor[float32, ?], a27: Tensor[float32, ?], a28: Tensor[float32, ?], a29: Tensor[float32, ?], a30: Tensor[float32, ?], a31: Tensor[float32, ?], a32: Tensor[float32, ?], a33: Tensor[float32, ?], a34: Tensor[float32, ?], a35: Tensor[float32, ?], a36: Tensor[float32, ?], a37: Tensor[float32, ?], a38: Tensor[float32, ?], a39: Tensor[float32, ?], bias: float32) -> Tensor[float32, ?]:
  summed = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 + a32 + a33 + a34 + a35 + a36 + a37 + a38 + a39
  summed * scale + bias

fn matrixWide(ignored: Tensor[float32, ?], a0: Tensor[float32, ?, ?], scale: float32, a1: Tensor[float32, ?, ?], a2: Tensor[float32, ?, ?], a3: Tensor[float32, ?, ?], a4: Tensor[float32, ?, ?], a5: Tensor[float32, ?, ?], a6: Tensor[float32, ?, ?], a7: Tensor[float32, ?, ?], a8: Tensor[float32, ?, ?], a9: Tensor[float32, ?, ?], a10: Tensor[float32, ?, ?], a11: Tensor[float32, ?, ?], a12: Tensor[float32, ?, ?], a13: Tensor[float32, ?, ?], a14: Tensor[float32, ?, ?], a15: Tensor[float32, ?, ?], a16: Tensor[float32, ?, ?], a17: Tensor[float32, ?, ?], a18: Tensor[float32, ?, ?], a19: Tensor[float32, ?, ?], a20: Tensor[float32, ?, ?], a21: Tensor[float32, ?, ?], a22: Tensor[float32, ?, ?], a23: Tensor[float32, ?, ?], a24: Tensor[float32, ?, ?], a25: Tensor[float32, ?, ?], a26: Tensor[float32, ?, ?], a27: Tensor[float32, ?, ?], a28: Tensor[float32, ?, ?], a29: Tensor[float32, ?, ?], a30: Tensor[float32, ?, ?], a31: Tensor[float32, ?, ?], a32: Tensor[float32, ?, ?], a33: Tensor[float32, ?, ?], a34: Tensor[float32, ?, ?], a35: Tensor[float32, ?, ?], a36: Tensor[float32, ?, ?], a37: Tensor[float32, ?, ?], a38: Tensor[float32, ?, ?], a39: Tensor[float32, ?, ?], bias: float32) -> Tensor[float32, ?, ?]:
  summed = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17 + a18 + a19 + a20 + a21 + a22 + a23 + a24 + a25 + a26 + a27 + a28 + a29 + a30 + a31 + a32 + a33 + a34 + a35 + a36 + a37 + a38 + a39
  summed * scale + bias

fn add(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left + right

fn main() -> Tensor[float32, 2, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    m: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    v: Tensor[float32, 3] = tensor([1, 2, 3])
    dm = try await transfer(m, gpu)
    dv = try await transfer(v, gpu)
    vector = try await spawn wide(dm, dv, 0.5, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, dv, 1) on gpu
    observed = try await transfer(vector, devices.cpu())
    matrix = try await spawn matrixWide(dv, dm, 0.5, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, dm, 1) on gpu
    remote = try await transfer(matrix, devices.cpu())
    local = matrixWide(v, m, 0.5, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, m, 1)
    add(local, remote)
