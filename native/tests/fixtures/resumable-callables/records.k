type Action:
  f:fn(i64)->i64 ! TaskError
fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn worker(action:Action)->i64 ! TaskError:
  return try action.f(21)
fn main()->i64 ! TaskError:
  action=Action {f:run}
  return try parallel:
    try await spawn worker(action)
