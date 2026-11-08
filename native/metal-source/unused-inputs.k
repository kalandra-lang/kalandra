fn choose(ignored: Tensor[f32, ?, ?], values: Tensor[f32, ?], other: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return values

fn compute(ignored: Tensor[f32, ?, ?], a: Tensor[f32, ?], other: Tensor[f32, ?, ?], b: Tensor[f32, ?], scale: f32) -> Tensor[f32, ?]:
  discarded = ignored + other
  selected = choose(ignored, a, b)
  return selected * b + selected * scale

fn many(t0: Tensor[f32, ?], t1: Tensor[f32, ?], t2: Tensor[f32, ?], t3: Tensor[f32, ?], t4: Tensor[f32, ?], t5: Tensor[f32, ?], t6: Tensor[f32, ?], t7: Tensor[f32, ?], t8: Tensor[f32, ?], t9: Tensor[f32, ?], t10: Tensor[f32, ?], t11: Tensor[f32, ?], t12: Tensor[f32, ?], t13: Tensor[f32, ?], t14: Tensor[f32, ?], t15: Tensor[f32, ?], t16: Tensor[f32, ?], t17: Tensor[f32, ?], t18: Tensor[f32, ?], t19: Tensor[f32, ?], t20: Tensor[f32, ?], t21: Tensor[f32, ?], t22: Tensor[f32, ?], t23: Tensor[f32, ?], t24: Tensor[f32, ?], t25: Tensor[f32, ?], t26: Tensor[f32, ?], t27: Tensor[f32, ?], t28: Tensor[f32, ?], t29: Tensor[f32, ?], t30: Tensor[f32, ?], t31: Tensor[f32, ?], t32: Tensor[f32, ?], t33: Tensor[f32, ?], t34: Tensor[f32, ?], t35: Tensor[f32, ?], t36: Tensor[f32, ?], t37: Tensor[f32, ?], t38: Tensor[f32, ?], t39: Tensor[f32, ?]) -> Tensor[f32, ?]:
  return t39

fn forward(values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn matrixChoose(ignored: Tensor[f32, ?], values: Tensor[f32, ?, ?]) -> Tensor[f32, ?, ?]:
  return values

fn main() -> Tensor[f32, 3] ! TaskError:
  return try parallel:
    gpu = devices.gpu()
    a: Tensor[f32, 3] = tensor([1, 2, 3])
    b: Tensor[f32, 3] = tensor([4, 5, 6])
    m: Tensor[f32, 2, 2] = tensor([[7, 8], [9, 10]])
    local = compute(m, a, m, b, 1)
    da = try await transfer(a, gpu)
    db = try await transfer(b, gpu)
    dm = try await transfer(m, gpu)
    chosen = try await spawn choose(dm, da, db) on gpu
    first = try await spawn compute(dm, chosen, dm, db, 1) on gpu
    result = try await spawn many(db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, first) on gpu
    matrixCopy = try await spawn matrixChoose(db, dm) on gpu
    try await transfer(result, devices.cpu())
