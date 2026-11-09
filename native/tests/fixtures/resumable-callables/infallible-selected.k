fn double(value:i64)->i64:
  return value*2
fn run(value:i64)->i64:
  return execute(value) catch fn(error):return 0
fn execute(value:i64)->i64 ! TaskError:
  return try parallel:
    try await spawn double(value)
fn choose(flag:bool)->fn(i64)->i64:
  return if flag:run
  else:double
fn main()->i64:
  first=choose(true)
  second=choose(false)
  return first(10)+second(11)
