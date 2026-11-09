fn double(value:i64)->i64:
  return value*2
fn apply(value:i64,f:fn(i64)->i64)->i64:
  return f(value)
fn forward(functions:[fn(i64)->i64])->[fn(i64)->i64]:
  return functions
fn wrap(functions:[fn(i64)->i64])->fn(i64)->i64:
  return fn(value:i64):return functions[0](value)
fn main()->i64 ! TaskError:
  functions=try parallel:
    try await spawn forward([double])
  return try parallel:
    try await spawn apply(21,wrap(functions))
