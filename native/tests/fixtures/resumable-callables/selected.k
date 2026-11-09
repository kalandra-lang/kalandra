fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn ordinary(value:i64)->i64 ! TaskError:
  return value*2
fn choose(flag:bool)->(fn(i64)->i64 ! TaskError):
  return if flag:run
  else:ordinary
fn main()->i64 ! TaskError:
  first=choose(true)
  second=choose(false)
  left=try first(10)
  right=try second(11)
  return left+right
