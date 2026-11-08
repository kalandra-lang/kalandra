fn blend(a: Tensor[f32, ?, ?], s0: f32, b: Tensor[f32, ?, ?], s1: f32, c: Tensor[f32, ?, ?], s2: f32, s3: f32, s4: f32, s5: f32, s6: f32, s7: f32, s8: f32, s9: f32, s10: f32, s11: f32) -> Tensor[f32, ?, ?]:
  product = a * s0
  mixed = product + b * s1
  combined = mixed + c * s2
  return combined + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11

fn vectorBlend(a: Tensor[f32, ?], s0: f32, b: Tensor[f32, ?], s1: f32, c: Tensor[f32, ?], s2: f32, s3: f32, s4: f32, s5: f32, s6: f32, s7: f32, s8: f32, s9: f32, s10: f32, s11: f32) -> Tensor[f32, ?]:
  product = a * s0
  mixed = product + b * s1
  combined = mixed + c * s2
  return combined + s3 + s4 + s5 + s6 + s7 + s8 + s9 + s10 + s11


fn boundary(t0: Tensor[f32, ?], t1: Tensor[f32, ?], t2: Tensor[f32, ?], t3: Tensor[f32, ?], t4: Tensor[f32, ?], t5: Tensor[f32, ?], t6: Tensor[f32, ?], t7: Tensor[f32, ?], t8: Tensor[f32, ?], t9: Tensor[f32, ?], t10: Tensor[f32, ?], t11: Tensor[f32, ?], t12: Tensor[f32, ?], t13: Tensor[f32, ?], t14: Tensor[f32, ?], t15: Tensor[f32, ?], t16: Tensor[f32, ?], t17: Tensor[f32, ?], t18: Tensor[f32, ?], t19: Tensor[f32, ?], t20: Tensor[f32, ?], t21: Tensor[f32, ?], t22: Tensor[f32, ?], t23: Tensor[f32, ?], t24: Tensor[f32, ?], t25: Tensor[f32, ?], t26: Tensor[f32, ?], t27: Tensor[f32, ?], t28: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return t0 + t1 + t2 + t3 + t4 + t5 + t6 + t7 + t8 + t9 + t10 + t11 + t12 + t13 + t14 + t15 + t16 + t17 + t18 + t19 + t20 + t21 + t22 + t23 + t24 + t25 + t26 + t27 + t28

fn add(left: Tensor[f32, ?, ?], right: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return left + right

fn main() -> Tensor[f32, 2, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 2, 3] = tensor([[1, 2, 3], [4, 5, 6]])
    b: Tensor[f32, 2, 3] = tensor([[7, 8, 9], [10, 11, 12]])
    c: Tensor[f32, 2, 3] = tensor([[2, 3, 4], [5, 6, 7]])
    da = try await transfer(a, gpu)
    db = try await transfer(b, gpu)
    dc = try await transfer(c, gpu)
    mixed = try await spawn blend(da, 1, db, 1, dc, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) on gpu
    v: Tensor[f32, 3] = tensor([1, 2, 3])
    dv = try await transfer(v, gpu)
    vectorResult = try await spawn vectorBlend(dv, 1, dv, 1, dv, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) on gpu
    edge = try await spawn boundary(vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult, vectorResult) on gpu
    observed = try await transfer(edge, devices.cpu())
    remote = try await transfer(mixed, devices.cpu())
    local = blend(a, 1, b, 1, c, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1)
    add(local, remote)
