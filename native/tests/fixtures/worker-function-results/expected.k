error E:
  Bad:
    text:String
    values:[i64]
fn make(flag:bool)->(fn(i64)->i64) ! E:
  if flag:fail E.Bad {text:"é!",values:[20,20]}
  values=[40]
  return fn(value:i64):return values[0]+value
fn run(flag:bool)->(fn(i64)->i64) ! (E | TaskError):
  return try parallel:
    try await spawn make(flag)
fn main()->i64 ! TaskError:
  f=run(false) catch fn(error):return fn(value:i64):return 0
  return f(2)
