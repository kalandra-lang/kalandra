fn choose(ignored: Tensor[float32, ?, ?], values: Tensor[float32, ?], other: Tensor[float32, ?]) -> Tensor[float32, ?]:
  values

fn compute(ignored: Tensor[float32, ?, ?], a: Tensor[float32, ?], other: Tensor[float32, ?, ?], b: Tensor[float32, ?], scale: float32) -> Tensor[float32, ?]:
  discarded = ignored + other
  selected = choose(ignored, a, b)
  selected * b + selected * scale

fn many(t0: Tensor[float32, ?], t1: Tensor[float32, ?], t2: Tensor[float32, ?], t3: Tensor[float32, ?], t4: Tensor[float32, ?], t5: Tensor[float32, ?], t6: Tensor[float32, ?], t7: Tensor[float32, ?], t8: Tensor[float32, ?], t9: Tensor[float32, ?], t10: Tensor[float32, ?], t11: Tensor[float32, ?], t12: Tensor[float32, ?], t13: Tensor[float32, ?], t14: Tensor[float32, ?], t15: Tensor[float32, ?], t16: Tensor[float32, ?], t17: Tensor[float32, ?], t18: Tensor[float32, ?], t19: Tensor[float32, ?], t20: Tensor[float32, ?], t21: Tensor[float32, ?], t22: Tensor[float32, ?], t23: Tensor[float32, ?], t24: Tensor[float32, ?], t25: Tensor[float32, ?], t26: Tensor[float32, ?], t27: Tensor[float32, ?], t28: Tensor[float32, ?], t29: Tensor[float32, ?], t30: Tensor[float32, ?], t31: Tensor[float32, ?], t32: Tensor[float32, ?], t33: Tensor[float32, ?], t34: Tensor[float32, ?], t35: Tensor[float32, ?], t36: Tensor[float32, ?], t37: Tensor[float32, ?], t38: Tensor[float32, ?], t39: Tensor[float32, ?]) -> Tensor[float32, ?]:
  t39

fn forward(values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn matrixChoose(ignored: Tensor[float32, ?], values: Tensor[float32, ?, ?]) -> Tensor[float32, ?, ?]:
  values

fn main() -> Tensor[float32, 3] ! TaskError:
  try parallel:
    gpu = devices.gpu()
    a: Tensor[float32, 3] = tensor([1, 2, 3])
    b: Tensor[float32, 3] = tensor([4, 5, 6])
    m: Tensor[float32, 2, 2] = tensor([[7, 8], [9, 10]])
    local = compute(m, a, m, b, 1)
    da = try await transfer(a, gpu)
    db = try await transfer(b, gpu)
    dm = try await transfer(m, gpu)
    chosen = try await spawn choose(dm, da, db) on gpu
    first = try await spawn compute(dm, chosen, dm, db, 1) on gpu
    result = try await spawn many(db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, db, first) on gpu
    matrixCopy = try await spawn matrixChoose(db, dm) on gpu
    try await transfer(result, devices.cpu())
