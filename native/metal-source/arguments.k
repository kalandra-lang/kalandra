fn blend(a: Tensor[float32, ?, ?], s0: float32, b: Tensor[float32, ?, ?], s1: float32, c: Tensor[float32, ?, ?], s2: float32, s3: float32, s4: float32, s5: float32, s6: float32, s7: float32, s8: float32, s9: float32, s10: float32, s11: float32) -> Tensor[float32, ?, ?]:
  product = a * s0
  mixed = product + b * s1
  combined = mixed + c * s2
  combined + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11

fn vectorBlend(a: Tensor[float32, ?], s0: float32, b: Tensor[float32, ?], s1: float32, c: Tensor[float32, ?], s2: float32, s3: float32, s4: float32, s5: float32, s6: float32, s7: float32, s8: float32, s9: float32, s10: float32, s11: float32) -> Tensor[float32, ?]:
  product = a * s0
  mixed = product + b * s1
  combined = mixed + c * s2
  combined + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11


fn boundary(t0: Tensor[float32, ?], t1: Tensor[float32, ?], t2: Tensor[float32, ?], t3: Tensor[float32, ?], t4: Tensor[float32, ?], t5: Tensor[float32, ?], t6: Tensor[float32, ?], t7: Tensor[float32, ?], t8: Tensor[float32, ?], t9: Tensor[float32, ?], t10: Tensor[float32, ?], t11: Tensor[float32, ?], t12: Tensor[float32, ?], t13: Tensor[float32, ?], t14: Tensor[float32, ?], t15: Tensor[float32, ?], t16: Tensor[float32, ?], t17: Tensor[float32, ?], t18: Tensor[float32, ?], t19: Tensor[float32, ?], t20: Tensor[float32, ?], t21: Tensor[float32, ?], t22: Tensor[float32, ?], t23: Tensor[float32, ?], t24: Tensor[float32, ?], t25: Tensor[float32, ?], t26: Tensor[float32, ?], t27: Tensor[float32, ?], t28: Tensor[float32, ?]) -> Tensor[float32, ?]:
  t0 + t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + t10 + t11 + t12 + t13 + t14 + t15 + t16 + t17 + t18 + t19 + t20 + t21 + t22 + t23 + t24 + t25 + t26 + t27 + t28

fn add(left: Tensor[float32, ?, ?], right: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  left + right

fn main() -> Tensor[float32, 2, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    b: Tensor[float32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
    c: Tensor[float32, 2, 3] = tensor([[2, 3, 4], [5, 6, 7]])
    da = try await transfer(a, gpu)
    db = try await transfer(b, gpu)
    dc = try await transfer(c, gpu)
    mixed = try await spawn blend(da, 1, db, 1, dc, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) on gpu
    v: Tensor[float32, 3] = tensor([1, 2, 3])
    dv = try await transfer(v, gpu)
    vectorResult = try await spawn vectorBlend(dv, 1, dv, 1, dv, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) on gpu
    edge = try await spawn boundary(vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult) on gpu
    observed = try await transfer(edge, devices.cpu())
    remote = try await transfer(mixed, devices.cpu())
    local = blend(a, 1, b, 1, c, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1)
    add(local, remote)
